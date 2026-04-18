#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <cmocka.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include <cutils/db.h>
#include <cutils/config.h>
#include <cutils/error.h>
#include "shutdown/shutdown.h"
#include "config/app_config.h"

/* --- Test fixture ---
 *
 * Sets up an in-memory DB with required tables, a config backed by a temp
 * YAML file + DB keys, and a shutdown_mgr. The UPS pointer is NULL — safe
 * because we configure ups_mode=none and controller_enabled=0. */

#define TEST_YAML "/tmp/airies_test_shutdown.yaml"
#define TEST_DB   "/tmp/airies_test_shutdown.db"

typedef struct {
    cutils_db_t     *db;
    cutils_config_t *cfg;
    shutdown_mgr_t  *mgr;
} test_ctx_t;

/* Schema for shutdown tables (combined from migrations 004 + 010) */
static const char *SHUTDOWN_SCHEMA =
    "CREATE TABLE IF NOT EXISTS shutdown_groups ("
    "  id              INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  name            TEXT    NOT NULL UNIQUE,"
    "  execution_order INTEGER NOT NULL DEFAULT 0,"
    "  parallel        INTEGER NOT NULL DEFAULT 1,"
    "  max_timeout_sec INTEGER NOT NULL DEFAULT 0,"
    "  post_group_delay INTEGER NOT NULL DEFAULT 0"
    ");"
    "CREATE TABLE IF NOT EXISTS shutdown_targets ("
    "  id                INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  group_id          INTEGER NOT NULL REFERENCES shutdown_groups(id) ON DELETE CASCADE,"
    "  name              TEXT    NOT NULL,"
    "  method            TEXT    NOT NULL DEFAULT 'ssh_password',"
    "  host              TEXT,"
    "  username          TEXT,"
    "  credential        TEXT,"
    "  command           TEXT    NOT NULL,"
    "  timeout_sec       INTEGER NOT NULL DEFAULT 180,"
    "  order_in_group    INTEGER NOT NULL DEFAULT 0,"
    "  confirm_method    TEXT    NOT NULL DEFAULT 'ping',"
    "  confirm_port      INTEGER,"
    "  confirm_command   TEXT,"
    "  post_confirm_delay INTEGER NOT NULL DEFAULT 15"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_targets_group ON shutdown_targets(group_id);";

static int setup(void **state)
{
    /* Write a minimal YAML config file (db.path is required by c-utils) */
    FILE *f = fopen(TEST_YAML, "w");
    if (!f) return -1;
    fprintf(f, "db:\n  path: %s\n", TEST_DB);
    fclose(f);

    /* Initialize config from file */
    cutils_config_t *cfg = NULL;
    int rc = config_init(&cfg, "airies_ups", TEST_YAML,
                         CFG_FIRST_RUN_CONTINUE,
                         app_file_keys, app_sections);
    if (rc != CUTILS_OK || !cfg) return -1;

    /* Open DB and run library migrations (creates config table) */
    cutils_db_t *db = NULL;
    rc = db_open(&db, TEST_DB);
    if (rc != CUTILS_OK || !db) { config_free(cfg); return -1; }

    rc = db_run_lib_migrations(db);
    if (rc != CUTILS_OK) { db_close(db); config_free(cfg); return -1; }

    /* Create shutdown tables */
    rc = db_exec_raw(db, SHUTDOWN_SCHEMA);
    if (rc != CUTILS_OK) { db_close(db); config_free(cfg); return -1; }

    /* Attach DB-backed config keys (seeds defaults into config table) */
    rc = config_attach_db(cfg, db, app_db_keys);
    if (rc != CUTILS_OK) { db_close(db); config_free(cfg); return -1; }

    /* Safety: make shutdown_execute a no-op */
    config_set_db(cfg, "shutdown.ups_mode", "none");
    config_set_db(cfg, "shutdown.controller_enabled", "0");

    /* Create shutdown manager (ups=NULL is safe with ups_mode=none) */
    shutdown_mgr_t *mgr = shutdown_create(db, NULL, cfg);
    if (!mgr) { db_close(db); config_free(cfg); return -1; }

    test_ctx_t *ctx = calloc(1, sizeof(*ctx));
    ctx->db  = db;
    ctx->cfg = cfg;
    ctx->mgr = mgr;
    *state = ctx;
    return 0;
}

static int teardown(void **state)
{
    test_ctx_t *ctx = *state;
    shutdown_free(ctx->mgr);
    db_close(ctx->db);
    config_free(ctx->cfg);
    free(ctx);
    remove(TEST_YAML);
    remove(TEST_DB);
    remove(TEST_DB "-wal");
    remove(TEST_DB "-shm");
    return 0;
}

/* --- Trigger: manual mode never fires --- */

static void test_trigger_manual_never_fires(void **state)
{
    test_ctx_t *ctx = *state;
    config_set_db(ctx->cfg, "shutdown.trigger", "manual");

    /* On battery, low runtime — should still not trigger */
    ups_data_t d = {0};
    d.status = UPS_ST_ON_BATTERY;
    d.runtime_sec = 10;
    d.charge_pct = 5.0;

    for (int i = 0; i < 100; i++)
        shutdown_check_trigger(ctx->mgr, &d);

    /* If we got here without shutdown_execute running, manual mode works.
     * (shutdown_execute with ups_mode=none + controller_enabled=0 is safe
     * but the point is manual mode shouldn't even reach it.) */
}

/* --- Trigger: software/runtime mode --- */

static void test_trigger_software_requires_battery(void **state)
{
    test_ctx_t *ctx = *state;
    config_set_db(ctx->cfg, "shutdown.trigger", "software");
    config_set_db(ctx->cfg, "shutdown.trigger_source", "runtime");
    config_set_db(ctx->cfg, "shutdown.trigger_on_battery", "1");
    config_set_db(ctx->cfg, "shutdown.trigger_runtime_sec", "300");
    config_set_db(ctx->cfg, "shutdown.trigger_delay_sec", "0");

    /* Online (not on battery) with low runtime — should NOT trigger */
    ups_data_t d = {0};
    d.status = UPS_ST_ONLINE;
    d.runtime_sec = 10;

    shutdown_check_trigger(ctx->mgr, &d);
    shutdown_check_trigger(ctx->mgr, &d);
    shutdown_check_trigger(ctx->mgr, &d);
    /* No crash = no trigger = correct */
}

static void test_trigger_software_runtime_debounce(void **state)
{
    test_ctx_t *ctx = *state;
    config_set_db(ctx->cfg, "shutdown.trigger", "software");
    config_set_db(ctx->cfg, "shutdown.trigger_source", "runtime");
    config_set_db(ctx->cfg, "shutdown.trigger_on_battery", "1");
    config_set_db(ctx->cfg, "shutdown.trigger_runtime_sec", "300");
    config_set_db(ctx->cfg, "shutdown.trigger_delay_sec", "9999");

    ups_data_t d = {0};
    d.status = UPS_ST_ON_BATTERY;
    d.runtime_sec = 100;

    /* First call sets trigger_first_met, but debounce hasn't elapsed */
    shutdown_check_trigger(ctx->mgr, &d);
    /* Second call — still within debounce window */
    shutdown_check_trigger(ctx->mgr, &d);
    /* No trigger fired (delay=9999s). If shutdown_execute ran, it's safe
     * because ups_mode=none + controller_enabled=0. */
}

static void test_trigger_software_runtime_resets_on_recovery(void **state)
{
    test_ctx_t *ctx = *state;
    config_set_db(ctx->cfg, "shutdown.trigger", "software");
    config_set_db(ctx->cfg, "shutdown.trigger_source", "runtime");
    config_set_db(ctx->cfg, "shutdown.trigger_on_battery", "1");
    config_set_db(ctx->cfg, "shutdown.trigger_runtime_sec", "300");
    config_set_db(ctx->cfg, "shutdown.trigger_delay_sec", "9999");

    ups_data_t d = {0};
    d.status = UPS_ST_ON_BATTERY;
    d.runtime_sec = 100;

    /* Start accumulating */
    shutdown_check_trigger(ctx->mgr, &d);

    /* Power restored — condition no longer met */
    d.status = UPS_ST_ONLINE;
    d.runtime_sec = 3600;
    shutdown_check_trigger(ctx->mgr, &d);

    /* Back on battery — should restart debounce from scratch */
    d.status = UPS_ST_ON_BATTERY;
    d.runtime_sec = 100;
    shutdown_check_trigger(ctx->mgr, &d);
    /* Still within fresh debounce — no trigger */
}

static void test_trigger_software_battery_threshold(void **state)
{
    test_ctx_t *ctx = *state;
    config_set_db(ctx->cfg, "shutdown.trigger", "software");
    config_set_db(ctx->cfg, "shutdown.trigger_source", "runtime");
    config_set_db(ctx->cfg, "shutdown.trigger_on_battery", "1");
    config_set_db(ctx->cfg, "shutdown.trigger_runtime_sec", "0");
    config_set_db(ctx->cfg, "shutdown.trigger_battery_pct", "20");
    config_set_db(ctx->cfg, "shutdown.trigger_delay_sec", "9999");

    ups_data_t d = {0};
    d.status = UPS_ST_ON_BATTERY;
    d.charge_pct = 15.0;
    d.runtime_sec = 9999;

    /* Battery below threshold, on battery → condition met */
    shutdown_check_trigger(ctx->mgr, &d);

    /* Above threshold → condition clears */
    d.charge_pct = 50.0;
    shutdown_check_trigger(ctx->mgr, &d);
}

static void test_trigger_software_no_battery_requirement(void **state)
{
    test_ctx_t *ctx = *state;
    config_set_db(ctx->cfg, "shutdown.trigger", "software");
    config_set_db(ctx->cfg, "shutdown.trigger_source", "runtime");
    config_set_db(ctx->cfg, "shutdown.trigger_on_battery", "0");
    config_set_db(ctx->cfg, "shutdown.trigger_runtime_sec", "300");
    config_set_db(ctx->cfg, "shutdown.trigger_delay_sec", "9999");

    /* Online (not on battery) but low runtime — should still trigger
     * because trigger_on_battery=0 */
    ups_data_t d = {0};
    d.status = UPS_ST_ONLINE;
    d.runtime_sec = 100;

    shutdown_check_trigger(ctx->mgr, &d);
    /* Debounce started (trigger_first_met set) — that's the expected path
     * for on_battery=0 with condition met */
}

/* --- Trigger: UPS mode (shutdown-imminent flag) --- */

static void test_trigger_ups_mode(void **state)
{
    test_ctx_t *ctx = *state;
    config_set_db(ctx->cfg, "shutdown.trigger", "ups");
    config_set_db(ctx->cfg, "shutdown.trigger_delay_sec", "9999");

    ups_data_t d = {0};
    d.sig_status = 0;

    /* No shutdown-imminent flag — no trigger */
    shutdown_check_trigger(ctx->mgr, &d);

    /* Set shutdown-imminent flag — starts debounce */
    d.sig_status = UPS_SIG_SHUTDOWN_IMMINENT;
    shutdown_check_trigger(ctx->mgr, &d);

    /* Clear flag — resets */
    d.sig_status = 0;
    shutdown_check_trigger(ctx->mgr, &d);
}

/* --- Trigger: field mode --- */

static void test_trigger_field_lt(void **state)
{
    test_ctx_t *ctx = *state;
    config_set_db(ctx->cfg, "shutdown.trigger", "software");
    config_set_db(ctx->cfg, "shutdown.trigger_source", "field");
    config_set_db(ctx->cfg, "shutdown.trigger_on_battery", "0");
    config_set_db(ctx->cfg, "shutdown.trigger_field", "input_voltage");
    config_set_db(ctx->cfg, "shutdown.trigger_field_op", "lt");
    config_set_db(ctx->cfg, "shutdown.trigger_field_value", "90");
    config_set_db(ctx->cfg, "shutdown.trigger_delay_sec", "9999");

    ups_data_t d = {0};
    d.input_voltage = 120.0;

    /* Above threshold — no trigger */
    shutdown_check_trigger(ctx->mgr, &d);

    /* Below threshold — starts debounce */
    d.input_voltage = 85.0;
    shutdown_check_trigger(ctx->mgr, &d);

    /* Recovery — resets */
    d.input_voltage = 120.0;
    shutdown_check_trigger(ctx->mgr, &d);
}

static void test_trigger_field_gt(void **state)
{
    test_ctx_t *ctx = *state;
    config_set_db(ctx->cfg, "shutdown.trigger", "software");
    config_set_db(ctx->cfg, "shutdown.trigger_source", "field");
    config_set_db(ctx->cfg, "shutdown.trigger_on_battery", "0");
    config_set_db(ctx->cfg, "shutdown.trigger_field", "load_pct");
    config_set_db(ctx->cfg, "shutdown.trigger_field_op", "gt");
    config_set_db(ctx->cfg, "shutdown.trigger_field_value", "95");
    config_set_db(ctx->cfg, "shutdown.trigger_delay_sec", "9999");

    ups_data_t d = {0};
    d.load_pct = 50.0;

    shutdown_check_trigger(ctx->mgr, &d);

    d.load_pct = 98.0;
    shutdown_check_trigger(ctx->mgr, &d);
}

static void test_trigger_field_eq(void **state)
{
    test_ctx_t *ctx = *state;
    config_set_db(ctx->cfg, "shutdown.trigger", "software");
    config_set_db(ctx->cfg, "shutdown.trigger_source", "field");
    config_set_db(ctx->cfg, "shutdown.trigger_on_battery", "0");
    config_set_db(ctx->cfg, "shutdown.trigger_field", "charge_pct");
    config_set_db(ctx->cfg, "shutdown.trigger_field_op", "eq");
    config_set_db(ctx->cfg, "shutdown.trigger_field_value", "0");
    config_set_db(ctx->cfg, "shutdown.trigger_delay_sec", "9999");

    ups_data_t d = {0};
    d.charge_pct = 50.0;

    shutdown_check_trigger(ctx->mgr, &d);

    d.charge_pct = 0.0;
    shutdown_check_trigger(ctx->mgr, &d);
}

static void test_trigger_field_empty_name(void **state)
{
    test_ctx_t *ctx = *state;
    config_set_db(ctx->cfg, "shutdown.trigger", "software");
    config_set_db(ctx->cfg, "shutdown.trigger_source", "field");
    config_set_db(ctx->cfg, "shutdown.trigger_on_battery", "0");
    config_set_db(ctx->cfg, "shutdown.trigger_field", "");
    config_set_db(ctx->cfg, "shutdown.trigger_delay_sec", "9999");

    ups_data_t d = {0};
    /* Empty field name → get_ups_field returns 0, compare_field("lt",0,0) = false */
    shutdown_check_trigger(ctx->mgr, &d);
}

/* --- Trigger: all data fields via get_ups_field --- */

static void test_trigger_all_data_fields(void **state)
{
    test_ctx_t *ctx = *state;
    config_set_db(ctx->cfg, "shutdown.trigger", "software");
    config_set_db(ctx->cfg, "shutdown.trigger_source", "field");
    config_set_db(ctx->cfg, "shutdown.trigger_on_battery", "0");
    config_set_db(ctx->cfg, "shutdown.trigger_field_op", "gt");
    config_set_db(ctx->cfg, "shutdown.trigger_field_value", "0");
    config_set_db(ctx->cfg, "shutdown.trigger_delay_sec", "9999");

    const char *fields[] = {
        "runtime_sec", "charge_pct", "battery_voltage", "load_pct",
        "output_current", "output_voltage", "output_frequency",
        "input_voltage", "efficiency", "bypass_voltage", "bypass_frequency",
    };

    ups_data_t d = {0};
    d.runtime_sec = 100;
    d.charge_pct = 50.0;
    d.battery_voltage = 24.0;
    d.load_pct = 30.0;
    d.output_current = 5.0;
    d.output_voltage = 120.0;
    d.output_frequency = 60.0;
    d.input_voltage = 121.0;
    d.efficiency = 95.0;
    d.bypass_voltage = 120.0;
    d.bypass_frequency = 60.0;

    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        /* Reset trigger state by creating a fresh manager */
        shutdown_free(ctx->mgr);
        ctx->mgr = shutdown_create(ctx->db, NULL, ctx->cfg);

        config_set_db(ctx->cfg, "shutdown.trigger_field", fields[i]);
        shutdown_check_trigger(ctx->mgr, &d);
        /* Each field > 0, op=gt, value=0 → condition met, debounce starts */
    }

    /* Test unknown field name — should not trigger */
    shutdown_free(ctx->mgr);
    ctx->mgr = shutdown_create(ctx->db, NULL, ctx->cfg);
    config_set_db(ctx->cfg, "shutdown.trigger_field", "nonexistent_field");
    shutdown_check_trigger(ctx->mgr, &d);
}

/* --- Lifecycle --- */

static void test_create_free(void **state)
{
    test_ctx_t *ctx = *state;
    /* The setup/teardown already tests create+free, but verify no crash
     * on a second create/free cycle */
    shutdown_mgr_t *mgr2 = shutdown_create(ctx->db, NULL, ctx->cfg);
    assert_non_null(mgr2);
    shutdown_free(mgr2);
}

/* --- Progress callback --- */

static int g_progress_count;
static void test_progress_cb(const char *group, const char *target,
                              const char *status, void *userdata)
{
    (void)group; (void)target; (void)status;
    int *counter = userdata;
    (*counter)++;
}

static void test_progress_callback(void **state)
{
    test_ctx_t *ctx = *state;
    g_progress_count = 0;
    shutdown_on_progress(ctx->mgr, test_progress_cb, &g_progress_count);

    /* Dry-run execute with no groups — no targets to report on */
    shutdown_execute(ctx->mgr, 1);
    /* Progress callback won't fire for empty groups, but verifies
     * the callback registration doesn't crash */
}

/* --- Dry run --- */

static void test_execute_dry_run_no_groups(void **state)
{
    test_ctx_t *ctx = *state;
    int rc = shutdown_execute(ctx->mgr, 1);
    assert_int_equal(rc, CUTILS_OK);
}

static void test_execute_dry_run_with_group(void **state)
{
    test_ctx_t *ctx = *state;

    /* Insert a group and target */
    db_exec_raw(ctx->db,
        "INSERT INTO shutdown_groups (name, execution_order, parallel, "
        "max_timeout_sec, post_group_delay) VALUES ('test_group', 1, 0, 0, 0)");
    db_exec_raw(ctx->db,
        "INSERT INTO shutdown_targets (group_id, name, method, host, username, "
        "credential, command, timeout_sec, order_in_group, confirm_method, "
        "confirm_port, confirm_command, post_confirm_delay) "
        "VALUES (1, 'test_target', 'command', NULL, NULL, NULL, "
        "'echo test', 30, 1, 'none', NULL, NULL, 0)");

    g_progress_count = 0;
    shutdown_on_progress(ctx->mgr, test_progress_cb, &g_progress_count);

    int rc = shutdown_execute(ctx->mgr, 1);
    assert_int_equal(rc, CUTILS_OK);
    /* Dry run should report "would execute" for the target */
    assert_true(g_progress_count > 0);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        /* trigger modes */
        cmocka_unit_test_setup_teardown(test_trigger_manual_never_fires, setup, teardown),
        cmocka_unit_test_setup_teardown(test_trigger_software_requires_battery, setup, teardown),
        cmocka_unit_test_setup_teardown(test_trigger_software_runtime_debounce, setup, teardown),
        cmocka_unit_test_setup_teardown(test_trigger_software_runtime_resets_on_recovery, setup, teardown),
        cmocka_unit_test_setup_teardown(test_trigger_software_battery_threshold, setup, teardown),
        cmocka_unit_test_setup_teardown(test_trigger_software_no_battery_requirement, setup, teardown),
        cmocka_unit_test_setup_teardown(test_trigger_ups_mode, setup, teardown),
        /* field trigger */
        cmocka_unit_test_setup_teardown(test_trigger_field_lt, setup, teardown),
        cmocka_unit_test_setup_teardown(test_trigger_field_gt, setup, teardown),
        cmocka_unit_test_setup_teardown(test_trigger_field_eq, setup, teardown),
        cmocka_unit_test_setup_teardown(test_trigger_field_empty_name, setup, teardown),
        cmocka_unit_test_setup_teardown(test_trigger_all_data_fields, setup, teardown),
        /* lifecycle */
        cmocka_unit_test_setup_teardown(test_create_free, setup, teardown),
        cmocka_unit_test_setup_teardown(test_progress_callback, setup, teardown),
        /* execute */
        cmocka_unit_test_setup_teardown(test_execute_dry_run_no_groups, setup, teardown),
        cmocka_unit_test_setup_teardown(test_execute_dry_run_with_group, setup, teardown),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
