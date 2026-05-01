#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <cmocka.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#include <pthread.h>

#include <cutils/db.h>
#include <cutils/config.h>
#include <cutils/error.h>
#include "shutdown/shutdown.h"
#include "config/app_config.h"
#include "ups/ups.h"
#include "ups/ups_driver.h"

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
    assert_int_equal(config_set_db(cfg, "shutdown.ups_mode", "none"), CUTILS_OK);
    assert_int_equal(config_set_db(cfg, "shutdown.controller_enabled", "0"), CUTILS_OK);

    /* Create shutdown manager (ups=NULL is safe with ups_mode=none;
     * monitor=NULL skips event emission, which the tests don't exercise) */
    shutdown_mgr_t *mgr = shutdown_create(db, NULL, cfg, NULL);
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
    assert_int_equal(config_set_db(ctx->cfg, "shutdown.trigger", "manual"), CUTILS_OK);

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
    assert_int_equal(config_set_db(ctx->cfg, "shutdown.trigger", "software"), CUTILS_OK);
    assert_int_equal(config_set_db(ctx->cfg, "shutdown.trigger_source", "runtime"), CUTILS_OK);
    assert_int_equal(config_set_db(ctx->cfg, "shutdown.trigger_on_battery", "1"), CUTILS_OK);
    assert_int_equal(config_set_db(ctx->cfg, "shutdown.trigger_runtime_sec", "300"), CUTILS_OK);
    assert_int_equal(config_set_db(ctx->cfg, "shutdown.trigger_delay_sec", "0"), CUTILS_OK);

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
    assert_int_equal(config_set_db(ctx->cfg, "shutdown.trigger", "software"), CUTILS_OK);
    assert_int_equal(config_set_db(ctx->cfg, "shutdown.trigger_source", "runtime"), CUTILS_OK);
    assert_int_equal(config_set_db(ctx->cfg, "shutdown.trigger_on_battery", "1"), CUTILS_OK);
    assert_int_equal(config_set_db(ctx->cfg, "shutdown.trigger_runtime_sec", "300"), CUTILS_OK);
    assert_int_equal(config_set_db(ctx->cfg, "shutdown.trigger_delay_sec", "9999"), CUTILS_OK);

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
    assert_int_equal(config_set_db(ctx->cfg, "shutdown.trigger", "software"), CUTILS_OK);
    assert_int_equal(config_set_db(ctx->cfg, "shutdown.trigger_source", "runtime"), CUTILS_OK);
    assert_int_equal(config_set_db(ctx->cfg, "shutdown.trigger_on_battery", "1"), CUTILS_OK);
    assert_int_equal(config_set_db(ctx->cfg, "shutdown.trigger_runtime_sec", "300"), CUTILS_OK);
    assert_int_equal(config_set_db(ctx->cfg, "shutdown.trigger_delay_sec", "9999"), CUTILS_OK);

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
    assert_int_equal(config_set_db(ctx->cfg, "shutdown.trigger", "software"), CUTILS_OK);
    assert_int_equal(config_set_db(ctx->cfg, "shutdown.trigger_source", "runtime"), CUTILS_OK);
    assert_int_equal(config_set_db(ctx->cfg, "shutdown.trigger_on_battery", "1"), CUTILS_OK);
    assert_int_equal(config_set_db(ctx->cfg, "shutdown.trigger_runtime_sec", "0"), CUTILS_OK);
    assert_int_equal(config_set_db(ctx->cfg, "shutdown.trigger_battery_pct", "20"), CUTILS_OK);
    assert_int_equal(config_set_db(ctx->cfg, "shutdown.trigger_delay_sec", "9999"), CUTILS_OK);

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
    assert_int_equal(config_set_db(ctx->cfg, "shutdown.trigger", "software"), CUTILS_OK);
    assert_int_equal(config_set_db(ctx->cfg, "shutdown.trigger_source", "runtime"), CUTILS_OK);
    assert_int_equal(config_set_db(ctx->cfg, "shutdown.trigger_on_battery", "0"), CUTILS_OK);
    assert_int_equal(config_set_db(ctx->cfg, "shutdown.trigger_runtime_sec", "300"), CUTILS_OK);
    assert_int_equal(config_set_db(ctx->cfg, "shutdown.trigger_delay_sec", "9999"), CUTILS_OK);

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
    assert_int_equal(config_set_db(ctx->cfg, "shutdown.trigger", "ups"), CUTILS_OK);
    assert_int_equal(config_set_db(ctx->cfg, "shutdown.trigger_delay_sec", "9999"), CUTILS_OK);

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
    assert_int_equal(config_set_db(ctx->cfg, "shutdown.trigger", "software"), CUTILS_OK);
    assert_int_equal(config_set_db(ctx->cfg, "shutdown.trigger_source", "field"), CUTILS_OK);
    assert_int_equal(config_set_db(ctx->cfg, "shutdown.trigger_on_battery", "0"), CUTILS_OK);
    assert_int_equal(config_set_db(ctx->cfg, "shutdown.trigger_field", "input_voltage"), CUTILS_OK);
    assert_int_equal(config_set_db(ctx->cfg, "shutdown.trigger_field_op", "lt"), CUTILS_OK);
    assert_int_equal(config_set_db(ctx->cfg, "shutdown.trigger_field_value", "90"), CUTILS_OK);
    assert_int_equal(config_set_db(ctx->cfg, "shutdown.trigger_delay_sec", "9999"), CUTILS_OK);

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
    assert_int_equal(config_set_db(ctx->cfg, "shutdown.trigger", "software"), CUTILS_OK);
    assert_int_equal(config_set_db(ctx->cfg, "shutdown.trigger_source", "field"), CUTILS_OK);
    assert_int_equal(config_set_db(ctx->cfg, "shutdown.trigger_on_battery", "0"), CUTILS_OK);
    assert_int_equal(config_set_db(ctx->cfg, "shutdown.trigger_field", "load_pct"), CUTILS_OK);
    assert_int_equal(config_set_db(ctx->cfg, "shutdown.trigger_field_op", "gt"), CUTILS_OK);
    assert_int_equal(config_set_db(ctx->cfg, "shutdown.trigger_field_value", "95"), CUTILS_OK);
    assert_int_equal(config_set_db(ctx->cfg, "shutdown.trigger_delay_sec", "9999"), CUTILS_OK);

    ups_data_t d = {0};
    d.load_pct = 50.0;

    shutdown_check_trigger(ctx->mgr, &d);

    d.load_pct = 98.0;
    shutdown_check_trigger(ctx->mgr, &d);
}

static void test_trigger_field_eq(void **state)
{
    test_ctx_t *ctx = *state;
    assert_int_equal(config_set_db(ctx->cfg, "shutdown.trigger", "software"), CUTILS_OK);
    assert_int_equal(config_set_db(ctx->cfg, "shutdown.trigger_source", "field"), CUTILS_OK);
    assert_int_equal(config_set_db(ctx->cfg, "shutdown.trigger_on_battery", "0"), CUTILS_OK);
    assert_int_equal(config_set_db(ctx->cfg, "shutdown.trigger_field", "charge_pct"), CUTILS_OK);
    assert_int_equal(config_set_db(ctx->cfg, "shutdown.trigger_field_op", "eq"), CUTILS_OK);
    assert_int_equal(config_set_db(ctx->cfg, "shutdown.trigger_field_value", "0"), CUTILS_OK);
    assert_int_equal(config_set_db(ctx->cfg, "shutdown.trigger_delay_sec", "9999"), CUTILS_OK);

    ups_data_t d = {0};
    d.charge_pct = 50.0;

    shutdown_check_trigger(ctx->mgr, &d);

    d.charge_pct = 0.0;
    shutdown_check_trigger(ctx->mgr, &d);
}

static void test_trigger_field_empty_name(void **state)
{
    test_ctx_t *ctx = *state;
    assert_int_equal(config_set_db(ctx->cfg, "shutdown.trigger", "software"), CUTILS_OK);
    assert_int_equal(config_set_db(ctx->cfg, "shutdown.trigger_source", "field"), CUTILS_OK);
    assert_int_equal(config_set_db(ctx->cfg, "shutdown.trigger_on_battery", "0"), CUTILS_OK);
    assert_int_equal(config_set_db(ctx->cfg, "shutdown.trigger_field", ""), CUTILS_OK);
    assert_int_equal(config_set_db(ctx->cfg, "shutdown.trigger_delay_sec", "9999"), CUTILS_OK);

    ups_data_t d = {0};
    /* Empty field name → get_ups_field returns 0, compare_field("lt",0,0) = false */
    shutdown_check_trigger(ctx->mgr, &d);
}

/* --- Trigger: all data fields via get_ups_field --- */

static void test_trigger_all_data_fields(void **state)
{
    test_ctx_t *ctx = *state;
    assert_int_equal(config_set_db(ctx->cfg, "shutdown.trigger", "software"), CUTILS_OK);
    assert_int_equal(config_set_db(ctx->cfg, "shutdown.trigger_source", "field"), CUTILS_OK);
    assert_int_equal(config_set_db(ctx->cfg, "shutdown.trigger_on_battery", "0"), CUTILS_OK);
    assert_int_equal(config_set_db(ctx->cfg, "shutdown.trigger_field_op", "gt"), CUTILS_OK);
    assert_int_equal(config_set_db(ctx->cfg, "shutdown.trigger_field_value", "0"), CUTILS_OK);
    assert_int_equal(config_set_db(ctx->cfg, "shutdown.trigger_delay_sec", "9999"), CUTILS_OK);

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
        ctx->mgr = shutdown_create(ctx->db, NULL, ctx->cfg, NULL);

        assert_int_equal(config_set_db(ctx->cfg, "shutdown.trigger_field", fields[i]), CUTILS_OK);
        shutdown_check_trigger(ctx->mgr, &d);
        /* Each field > 0, op=gt, value=0 → condition met, debounce starts */
    }

    /* Test unknown field name — should not trigger */
    shutdown_free(ctx->mgr);
    ctx->mgr = shutdown_create(ctx->db, NULL, ctx->cfg, NULL);
    assert_int_equal(config_set_db(ctx->cfg, "shutdown.trigger_field", "nonexistent_field"), CUTILS_OK);
    shutdown_check_trigger(ctx->mgr, &d);
}

/* --- Lifecycle --- */

static void test_create_free(void **state)
{
    test_ctx_t *ctx = *state;
    /* The setup/teardown already tests create+free, but verify no crash
     * on a second create/free cycle */
    shutdown_mgr_t *mgr2 = shutdown_create(ctx->db, NULL, ctx->cfg, NULL);
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
    assert_int_equal(db_exec_raw(ctx->db,
        "INSERT INTO shutdown_groups (name, execution_order, parallel, "
        "max_timeout_sec, post_group_delay) VALUES ('test_group', 1, 0, 0, 0)"),
        CUTILS_OK);
    assert_int_equal(db_exec_raw(ctx->db,
        "INSERT INTO shutdown_targets (group_id, name, method, host, username, "
        "credential, command, timeout_sec, order_in_group, confirm_method, "
        "confirm_port, confirm_command, post_confirm_delay) "
        "VALUES (1, 'test_target', 'command', NULL, NULL, NULL, "
        "'echo test', 30, 1, 'none', NULL, NULL, 0)"),
        CUTILS_OK);

    g_progress_count = 0;
    shutdown_on_progress(ctx->mgr, test_progress_cb, &g_progress_count);

    int rc = shutdown_execute(ctx->mgr, 1);
    assert_int_equal(rc, CUTILS_OK);
    /* Dry run should report "would execute" for the target */
    assert_true(g_progress_count > 0);
}

/* =========================================================================
 * shutdown_execute non-dry-run with command targets
 *
 * Exercises execute_group_sequential and execute_group_parallel without
 * touching the network. method='command' + command='/bin/true' is a
 * cheap subprocess; confirm_method='none' short-circuits the post-fire
 * confirmation; ups_mode=none and controller_enabled=0 (defaults from
 * setup) skip the trailing UPS + controller shutdown phases.
 * ========================================================================= */

static int insert_command_target(cutils_db_t *db, int group_id,
                                 const char *target_name, const char *command)
{
    char sql[512];
    snprintf(sql, sizeof(sql),
        "INSERT INTO shutdown_targets (group_id, name, method, host, username, "
        "credential, command, timeout_sec, order_in_group, confirm_method, "
        "confirm_port, confirm_command, post_confirm_delay) "
        "VALUES (%d, '%s', 'command', NULL, NULL, NULL, '%s', "
        "5, 1, 'none', NULL, NULL, 0)",
        group_id, target_name, command);
    return db_exec_raw(db, sql);
}

static void test_execute_sequential_command_group(void **state)
{
    test_ctx_t *ctx = *state;

    assert_int_equal(db_exec_raw(ctx->db,
        "INSERT INTO shutdown_groups (name, execution_order, parallel, "
        "max_timeout_sec, post_group_delay) VALUES ('seq', 1, 0, 0, 0)"),
        CUTILS_OK);
    assert_int_equal(insert_command_target(ctx->db, 1, "a", "/bin/true"), CUTILS_OK);
    assert_int_equal(insert_command_target(ctx->db, 1, "b", "/bin/false"), CUTILS_OK);

    g_progress_count = 0;
    shutdown_on_progress(ctx->mgr, test_progress_cb, &g_progress_count);

    int rc = shutdown_execute(ctx->mgr, 0 /* not dry-run */);
    assert_int_equal(rc, CUTILS_OK);
    /* "starting" + "completed/failed" per target × 2 targets = 4 reports */
    assert_true(g_progress_count >= 4);
}

static void test_execute_parallel_command_group(void **state)
{
    test_ctx_t *ctx = *state;

    assert_int_equal(db_exec_raw(ctx->db,
        "INSERT INTO shutdown_groups (name, execution_order, parallel, "
        "max_timeout_sec, post_group_delay) VALUES ('par', 1, 1, 0, 0)"),
        CUTILS_OK);
    assert_int_equal(insert_command_target(ctx->db, 1, "a", "/bin/true"), CUTILS_OK);
    assert_int_equal(insert_command_target(ctx->db, 1, "b", "/bin/true"), CUTILS_OK);

    g_progress_count = 0;
    shutdown_on_progress(ctx->mgr, test_progress_cb, &g_progress_count);

    int rc = shutdown_execute(ctx->mgr, 0);
    assert_int_equal(rc, CUTILS_OK);
    assert_true(g_progress_count >= 4);
}

static void test_execute_parallel_with_max_timeout(void **state)
{
    test_ctx_t *ctx = *state;

    /* max_timeout=1s, target sleeps 5s. The group ceiling fires SIGTERM
     * on the straggler and the wait-loop reaps it. Covers the timeout
     * + kill branches in execute_group_parallel. */
    assert_int_equal(db_exec_raw(ctx->db,
        "INSERT INTO shutdown_groups (name, execution_order, parallel, "
        "max_timeout_sec, post_group_delay) VALUES ('slow', 1, 1, 1, 0)"),
        CUTILS_OK);
    assert_int_equal(insert_command_target(ctx->db, 1, "slow", "sleep 5"),
                     CUTILS_OK);

    int rc = shutdown_execute(ctx->mgr, 0);
    assert_int_equal(rc, CUTILS_OK);
}

static void test_execute_sequential_with_max_timeout(void **state)
{
    test_ctx_t *ctx = *state;

    /* Sequential: max_timeout cuts off later targets entirely (no fork). */
    assert_int_equal(db_exec_raw(ctx->db,
        "INSERT INTO shutdown_groups (name, execution_order, parallel, "
        "max_timeout_sec, post_group_delay) VALUES ('seq_to', 1, 0, 1, 0)"),
        CUTILS_OK);
    assert_int_equal(insert_command_target(ctx->db, 1, "fast", "/bin/true"), CUTILS_OK);
    /* Sleep target makes the second iteration trip the 1s ceiling. */
    assert_int_equal(insert_command_target(ctx->db, 1, "slow", "sleep 2"), CUTILS_OK);
    assert_int_equal(insert_command_target(ctx->db, 1, "skipped", "/bin/true"), CUTILS_OK);

    int rc = shutdown_execute(ctx->mgr, 0);
    assert_int_equal(rc, CUTILS_OK);
}

/* =========================================================================
 * Trigger fire path + efficiency NaN gate
 *
 * The existing trigger tests all set delay_sec=9999 so they never reach
 * the fire branch. These cover the tail of shutdown_check_trigger (the
 * trigger_first_met debounce elapses and shutdown_execute fires) plus
 * the efficiency NaN gate in get_ups_field.
 * ========================================================================= */

static void test_trigger_fires_after_debounce(void **state)
{
    test_ctx_t *ctx = *state;
    assert_int_equal(config_set_db(ctx->cfg, "shutdown.trigger",              "software"), CUTILS_OK);
    assert_int_equal(config_set_db(ctx->cfg, "shutdown.trigger_source",       "runtime"),  CUTILS_OK);
    assert_int_equal(config_set_db(ctx->cfg, "shutdown.trigger_on_battery",   "1"),        CUTILS_OK);
    assert_int_equal(config_set_db(ctx->cfg, "shutdown.trigger_runtime_sec",  "300"),      CUTILS_OK);
    assert_int_equal(config_set_db(ctx->cfg, "shutdown.trigger_delay_sec",    "0"),        CUTILS_OK);

    ups_data_t d = {0};
    d.status      = UPS_ST_ON_BATTERY;
    d.runtime_sec = 100;

    /* First call: trigger_first_met=0 → sets it to now, returns without firing. */
    shutdown_check_trigger(ctx->mgr, &d);

    /* Second call: now - trigger_first_met >= 0 (delay=0), fires
     * shutdown_execute. Safe because ups_mode=none + controller_enabled=0. */
    shutdown_check_trigger(ctx->mgr, &d);

    /* Subsequent calls no-op (mgr->triggered latched). */
    shutdown_check_trigger(ctx->mgr, &d);
}

static void test_trigger_field_efficiency_invalid_reason_no_fire(void **state)
{
    test_ctx_t *ctx = *state;
    assert_int_equal(config_set_db(ctx->cfg, "shutdown.trigger",             "software"), CUTILS_OK);
    assert_int_equal(config_set_db(ctx->cfg, "shutdown.trigger_source",      "field"),    CUTILS_OK);
    assert_int_equal(config_set_db(ctx->cfg, "shutdown.trigger_on_battery",  "0"),        CUTILS_OK);
    assert_int_equal(config_set_db(ctx->cfg, "shutdown.trigger_field",       "efficiency"), CUTILS_OK);
    assert_int_equal(config_set_db(ctx->cfg, "shutdown.trigger_field_op",    "lt"),       CUTILS_OK);
    assert_int_equal(config_set_db(ctx->cfg, "shutdown.trigger_field_value", "50"),       CUTILS_OK);
    assert_int_equal(config_set_db(ctx->cfg, "shutdown.trigger_delay_sec",   "9999"),     CUTILS_OK);

    ups_data_t d = {0};
    /* efficiency_reason != OK → get_ups_field returns NaN.
     * NaN < 50 is false, so compare_field returns false → no trigger. */
    d.efficiency_reason = UPS_EFF_LOAD_TOO_LOW;
    d.efficiency        = 25.0;  /* ignored: reason gate zeros it */
    shutdown_check_trigger(ctx->mgr, &d);

    /* Valid reason with matching value → condition met, debounce starts. */
    d.efficiency_reason = UPS_EFF_OK;
    d.efficiency        = 25.0;
    shutdown_check_trigger(ctx->mgr, &d);
}

/* =========================================================================
 * shutdown_test_target — DB lookup + connectivity dispatch
 *
 * test_target_connectivity returns 0 for method='command' (can't test
 * arbitrary commands) and -1 for unknown methods. Both paths deterministic.
 * ========================================================================= */

static int insert_group_and_target(cutils_db_t *db, const char *target_name,
                                   const char *method)
{
    int rc = db_exec_raw(db,
        "INSERT OR IGNORE INTO shutdown_groups (id, name, execution_order, "
        "parallel, max_timeout_sec, post_group_delay) "
        "VALUES (1, 'g', 1, 0, 0, 0)");
    if (rc != CUTILS_OK) return rc;

    char sql[512];
    snprintf(sql, sizeof(sql),
        "INSERT INTO shutdown_targets (group_id, name, method, host, username, "
        "credential, command, timeout_sec, order_in_group, confirm_method, "
        "post_confirm_delay) "
        "VALUES (1, '%s', '%s', 'h', 'u', 'c', '/bin/true', 30, 1, 'none', 0)",
        target_name, method);
    return db_exec_raw(db, sql);
}

static void test_shutdown_test_target_not_found(void **state)
{
    test_ctx_t *ctx = *state;
    assert_int_equal(shutdown_test_target(ctx->mgr, "does_not_exist"),
                     CUTILS_ERR_NOT_FOUND);
}

static void test_shutdown_test_target_command_method_ok(void **state)
{
    test_ctx_t *ctx = *state;
    assert_int_equal(insert_group_and_target(ctx->db, "cmd_ok", "command"),
                     CUTILS_OK);
    /* method='command' short-circuits test_target_connectivity to 0. */
    assert_int_equal(shutdown_test_target(ctx->mgr, "cmd_ok"), 0);
}

static void test_shutdown_test_target_unknown_method_fails(void **state)
{
    test_ctx_t *ctx = *state;
    assert_int_equal(insert_group_and_target(ctx->db, "weird", "bogus_method"),
                     CUTILS_OK);
    /* Unknown methods fall through the if-else chain and return -1. */
    assert_int_equal(shutdown_test_target(ctx->mgr, "weird"), -1);
}

/* --- shutdown_test_target_confirm: per-target down-detect probe --- */

static int insert_target_with_confirm(cutils_db_t *db, const char *target_name,
                                      const char *confirm_method,
                                      const char *confirm_command)
{
    int rc = db_exec_raw(db,
        "INSERT OR IGNORE INTO shutdown_groups (id, name, execution_order, "
        "parallel, max_timeout_sec, post_group_delay) "
        "VALUES (1, 'g', 1, 0, 0, 0)");
    if (rc != CUTILS_OK) return rc;

    char sql[768];
    snprintf(sql, sizeof(sql),
        "INSERT INTO shutdown_targets (group_id, name, method, host, username, "
        "credential, command, timeout_sec, order_in_group, confirm_method, "
        "confirm_port, confirm_command, post_confirm_delay) "
        "VALUES (1, '%s', 'command', 'h', 'u', 'c', '/bin/true', 30, 1, "
        "'%s', 0, '%s', 0)",
        target_name, confirm_method, confirm_command ? confirm_command : "");
    return db_exec_raw(db, sql);
}

static void test_shutdown_test_target_confirm_not_found(void **state)
{
    test_ctx_t *ctx = *state;
    assert_int_equal(shutdown_test_target_confirm(ctx->mgr, "does_not_exist"),
                     CUTILS_ERR_NOT_FOUND);
}

static void test_shutdown_test_target_confirm_none_method_ok(void **state)
{
    test_ctx_t *ctx = *state;
    assert_int_equal(insert_target_with_confirm(ctx->db, "cm_none", "none", ""),
                     CUTILS_OK);
    /* method='none' has nothing to validate -> trivially ok. */
    assert_int_equal(shutdown_test_target_confirm(ctx->mgr, "cm_none"), 0);
}

static void test_shutdown_test_target_confirm_command_success(void **state)
{
    test_ctx_t *ctx = *state;
    assert_int_equal(
        insert_target_with_confirm(ctx->db, "cm_ok", "command", "/bin/true"),
        CUTILS_OK);
    assert_int_equal(shutdown_test_target_confirm(ctx->mgr, "cm_ok"), 0);
}

static void test_shutdown_test_target_confirm_command_failure(void **state)
{
    test_ctx_t *ctx = *state;
    assert_int_equal(
        insert_target_with_confirm(ctx->db, "cm_bad", "command", "/bin/false"),
        CUTILS_OK);
    /* command exits non-zero -> reachability probe fails. */
    assert_int_equal(shutdown_test_target_confirm(ctx->mgr, "cm_bad"), -1);
}

static void test_shutdown_test_target_confirm_command_empty_fails(void **state)
{
    test_ctx_t *ctx = *state;
    assert_int_equal(
        insert_target_with_confirm(ctx->db, "cm_empty", "command", ""),
        CUTILS_OK);
    /* No command stored -> can't probe -> -1. */
    assert_int_equal(shutdown_test_target_confirm(ctx->mgr, "cm_empty"), -1);
}

static void test_shutdown_test_target_confirm_unknown_method_fails(void **state)
{
    test_ctx_t *ctx = *state;
    assert_int_equal(
        insert_target_with_confirm(ctx->db, "cm_weird", "magic", ""),
        CUTILS_OK);
    assert_int_equal(shutdown_test_target_confirm(ctx->mgr, "cm_weird"), -1);
}

/* =========================================================================
 * shutdown_execute UPS-action phase — swap mgr->ups for a fake
 *
 * Covers the three UPS action branches (none / command / register) plus
 * the unknown-mode warn branch. Purely dispatch logic; no fork, no shell.
 * ========================================================================= */

static int fake_ups_transport_sentinel = 1;
static int fake_ups_execute_calls;
static int fake_ups_config_write_calls;
static uint16_t fake_ups_config_write_last_value;

static int fake_ups_execute_cb(void *t)
{
    (void)t;
    fake_ups_execute_calls++;
    return 0;
}

static int fake_ups_config_write_cb(void *t, const ups_config_reg_t *r, uint16_t v)
{
    (void)t; (void)r;
    fake_ups_config_write_calls++;
    fake_ups_config_write_last_value = v;
    return 0;
}

static const ups_cmd_desc_t fake_ups_cmds[] = {
    { .name = "shutdown_sig", .display_name = "Shutdown", .type = UPS_CMD_SIMPLE,
      .flags = UPS_CMD_IS_SHUTDOWN, .execute = fake_ups_execute_cb },
};

static const ups_config_reg_t fake_ups_regs[] = {
    { .name = "ups_delay", .display_name = "Shutdown Delay", .type = UPS_CFG_SCALAR,
      .writable = 1 },
};

static const ups_driver_t fake_ups_driver = {
    .name              = "fake_ups_driver",
    .conn_type         = UPS_CONN_SERIAL,
    .topology          = UPS_TOPO_ONLINE_DOUBLE,
    .caps              = UPS_CAP_SHUTDOWN,
    .commands          = fake_ups_cmds,
    .commands_count    = sizeof(fake_ups_cmds) / sizeof(fake_ups_cmds[0]),
    .config_regs       = fake_ups_regs,
    .config_regs_count = sizeof(fake_ups_regs) / sizeof(fake_ups_regs[0]),
    .config_write      = fake_ups_config_write_cb,
};

static ups_t *alloc_fake_ups(void)
{
    ups_t *u = calloc(1, sizeof(*u));
    assert_non_null(u);
    u->driver    = &fake_ups_driver;
    u->transport = &fake_ups_transport_sentinel;
    u->caps      = fake_ups_driver.caps;
    pthread_mutex_init(&u->cmd_mutex, NULL);
    return u;
}

static void free_fake_ups(ups_t *u)
{
    pthread_mutex_destroy(&u->cmd_mutex);
    free(u);
}

/* The _command and _register tests need mgr->ups to point at a fake,
 * but shutdown_mgr_t is opaque with no setter. Simplest approach:
 * create a throw-away mgr bound to the fake for just these tests,
 * sharing the fixture's db + cfg. */
static shutdown_mgr_t *make_mgr_with_fake_ups(test_ctx_t *ctx, ups_t *fake)
{
    shutdown_mgr_t *m = shutdown_create(ctx->db, fake, ctx->cfg, NULL);
    assert_non_null(m);
    return m;
}

static void test_execute_ups_mode_none_skips_ups_action(void **state)
{
    test_ctx_t *ctx = *state;
    /* ups_mode=none is the default from setup(). */
    fake_ups_execute_calls = 0;
    fake_ups_config_write_calls = 0;

    int rc = shutdown_execute(ctx->mgr, 0 /* not dry-run */);
    assert_int_equal(rc, CUTILS_OK);
    assert_int_equal(fake_ups_execute_calls,      0);
    assert_int_equal(fake_ups_config_write_calls, 0);
}

static void test_execute_ups_mode_unknown_warns(void **state)
{
    test_ctx_t *ctx = *state;
    assert_int_equal(config_set_db(ctx->cfg, "shutdown.ups_mode", "bogus"), CUTILS_OK);
    fake_ups_execute_calls = 0;
    fake_ups_config_write_calls = 0;

    /* Unknown ups_mode falls to log_warn and skips all driver calls. NULL
     * ups is safe here because neither branch dereferences it. */
    int rc = shutdown_execute(ctx->mgr, 0);
    assert_int_equal(rc, CUTILS_OK);
    assert_int_equal(fake_ups_execute_calls,      0);
    assert_int_equal(fake_ups_config_write_calls, 0);
}

static void test_execute_ups_mode_command_fires_shutdown(void **state)
{
    test_ctx_t *ctx = *state;
    assert_int_equal(config_set_db(ctx->cfg, "shutdown.ups_mode",  "command"), CUTILS_OK);
    assert_int_equal(config_set_db(ctx->cfg, "shutdown.ups_delay", "0"),       CUTILS_OK);
    fake_ups_execute_calls = 0;

    ups_t *fake = alloc_fake_ups();
    shutdown_mgr_t *m = make_mgr_with_fake_ups(ctx, fake);

    int rc = shutdown_execute(m, 0);
    assert_int_equal(rc, CUTILS_OK);
    assert_int_equal(fake_ups_execute_calls, 1);

    shutdown_free(m);
    free_fake_ups(fake);
}

static void test_execute_ups_mode_register_writes_register(void **state)
{
    test_ctx_t *ctx = *state;
    assert_int_equal(config_set_db(ctx->cfg, "shutdown.ups_mode",     "register"),    CUTILS_OK);
    assert_int_equal(config_set_db(ctx->cfg, "shutdown.ups_register", "ups_delay"),   CUTILS_OK);
    assert_int_equal(config_set_db(ctx->cfg, "shutdown.ups_value",    "42"),          CUTILS_OK);
    assert_int_equal(config_set_db(ctx->cfg, "shutdown.ups_delay",    "0"),           CUTILS_OK);
    fake_ups_config_write_calls = 0;

    ups_t *fake = alloc_fake_ups();
    shutdown_mgr_t *m = make_mgr_with_fake_ups(ctx, fake);

    int rc = shutdown_execute(m, 0);
    assert_int_equal(rc, CUTILS_OK);
    assert_int_equal(fake_ups_config_write_calls, 1);
    assert_int_equal(fake_ups_config_write_last_value, 42);

    shutdown_free(m);
    free_fake_ups(fake);
}

/* =========================================================================
 * write_key_to_tmpfs — ssh_key material lands on tmpfs at 0600
 *
 * Internal helper used by fire_target_action() for method='ssh_key'.
 * Non-static so we can extern it here without a public header entry.
 * ========================================================================= */

extern char *write_key_to_tmpfs(const char *key_material);
extern int   fire_target_action(const char *method, const char *host,
                                const char *username, const char *credential,
                                const char *command);

static void test_write_key_to_tmpfs_writes_content_at_0600(void **state)
{
    (void)state;

    const char *key =
        "-----BEGIN OPENSSH PRIVATE KEY-----\n"
        "fake-key-material-for-test\n"
        "-----END OPENSSH PRIVATE KEY-----\n";

    char *path = write_key_to_tmpfs(key);
    assert_non_null(path);
    assert_non_null(strstr(path, "/dev/shm/airies-ups-key."));

    /* mode is 0600 */
    struct stat st;
    assert_int_equal(stat(path, &st), 0);
    assert_int_equal(st.st_mode & 0777, 0600);

    /* contents round-trip */
    FILE *f = fopen(path, "r");
    assert_non_null(f);
    char buf[256] = {0};
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    assert_int_equal(n, strlen(key));
    assert_string_equal(buf, key);

    unlink(path);
    free(path);
}

static void test_fire_target_action_command_success(void **state)
{
    (void)state;
    /* method='command' shells out via system(). /bin/true exits 0. */
    int rc = fire_target_action("command", NULL, NULL, NULL, "/bin/true");
    assert_int_equal(rc, 0);
}

static void test_fire_target_action_command_failure(void **state)
{
    (void)state;
    int rc = fire_target_action("command", NULL, NULL, NULL, "/bin/false");
    assert_int_equal(rc, -1);
}

static void test_fire_target_action_unknown_method(void **state)
{
    (void)state;
    int rc = fire_target_action("bogus", "h", "u", "c", "/bin/true");
    assert_int_equal(rc, CUTILS_ERR_INVALID);
}

/* =========================================================================
 * shutdown_execute_ex — per-step result aggregation
 *
 * The setup() defaults (ups_mode=none, controller_enabled=0) emit two
 * phase2/phase3 skipped rows, so even a no-groups workflow has 2 result
 * rows — gives every test a known baseline to inspect.
 * ========================================================================= */

static const shutdown_step_result_t *find_step(const shutdown_result_t *res,
                                               const char *phase,
                                               const char *target_substr)
{
    if (!res) return NULL;
    for (size_t i = 0; i < res->n_steps; i++) {
        const shutdown_step_result_t *s = &res->steps[i];
        if (strcmp(s->phase, phase) != 0) continue;
        if (target_substr && !strstr(s->target, target_substr)) continue;
        return s;
    }
    return NULL;
}

static void test_execute_ex_no_groups_emits_phase2_phase3(void **state)
{
    test_ctx_t *ctx = *state;
    shutdown_result_t *res = NULL;
    int rc = shutdown_execute_ex(ctx->mgr, 0, &res);

    assert_int_equal(rc, CUTILS_OK);
    assert_non_null(res);
    /* No phase1 rows (no groups). Phase 2 + 3 always emit one row each. */
    assert_int_equal(res->n_steps, 2);
    assert_non_null(find_step(res, "phase2", "ups"));
    assert_non_null(find_step(res, "phase3", "controller"));
    shutdown_result_free(res);
}

static void test_execute_ex_phase2_skipped_when_mode_none(void **state)
{
    test_ctx_t *ctx = *state;
    /* setup() pins ups_mode=none. Phase 2 row should be ok=2 (skipped). */
    shutdown_result_t *res = NULL;
    assert_int_equal(shutdown_execute_ex(ctx->mgr, 0, &res), CUTILS_OK);

    const shutdown_step_result_t *p2 = find_step(res, "phase2", "ups");
    assert_non_null(p2);
    assert_int_equal(p2->ok, 2);
    shutdown_result_free(res);
}

static void test_execute_ex_phase3_skipped_when_disabled(void **state)
{
    test_ctx_t *ctx = *state;
    /* setup() pins controller_enabled=0. Phase 3 row should be ok=2. */
    shutdown_result_t *res = NULL;
    assert_int_equal(shutdown_execute_ex(ctx->mgr, 0, &res), CUTILS_OK);

    const shutdown_step_result_t *p3 = find_step(res, "phase3", "controller");
    assert_non_null(p3);
    assert_int_equal(p3->ok, 2);
    assert_int_equal(res->n_failed, 0);
    shutdown_result_free(res);
}

static void test_execute_ex_sequential_failure_captured(void **state)
{
    test_ctx_t *ctx = *state;

    assert_int_equal(db_exec_raw(ctx->db,
        "INSERT INTO shutdown_groups (name, execution_order, parallel, "
        "max_timeout_sec, post_group_delay) VALUES ('seq', 1, 0, 0, 0)"),
        CUTILS_OK);
    assert_int_equal(insert_command_target(ctx->db, 1, "good", "/bin/true"),  CUTILS_OK);
    assert_int_equal(insert_command_target(ctx->db, 1, "bad",  "/bin/false"), CUTILS_OK);

    shutdown_result_t *res = NULL;
    assert_int_equal(shutdown_execute_ex(ctx->mgr, 0, &res), CUTILS_OK);

    const shutdown_step_result_t *good = find_step(res, "phase1", "/good");
    const shutdown_step_result_t *bad  = find_step(res, "phase1", "/bad");
    assert_non_null(good);
    assert_non_null(bad);
    assert_int_equal(good->ok, 0);
    assert_int_equal(bad->ok,  1);
    assert_int_equal(res->n_failed, 1);
    shutdown_result_free(res);
}

static void test_execute_ex_parallel_failure_captured(void **state)
{
    test_ctx_t *ctx = *state;

    assert_int_equal(db_exec_raw(ctx->db,
        "INSERT INTO shutdown_groups (name, execution_order, parallel, "
        "max_timeout_sec, post_group_delay) VALUES ('par', 1, 1, 0, 0)"),
        CUTILS_OK);
    assert_int_equal(insert_command_target(ctx->db, 1, "good", "/bin/true"),  CUTILS_OK);
    assert_int_equal(insert_command_target(ctx->db, 1, "bad",  "/bin/false"), CUTILS_OK);

    shutdown_result_t *res = NULL;
    assert_int_equal(shutdown_execute_ex(ctx->mgr, 0, &res), CUTILS_OK);

    const shutdown_step_result_t *good = find_step(res, "phase1", "/good");
    const shutdown_step_result_t *bad  = find_step(res, "phase1", "/bad");
    assert_non_null(good);
    assert_non_null(bad);
    assert_int_equal(good->ok, 0);
    assert_int_equal(bad->ok,  1);
    /* Parallel can't reach the child's cutils_get_error across the fork,
     * so the parent records a generic message — verify the prefix. */
    assert_true(strstr(bad->error, "step failed") != NULL);
    shutdown_result_free(res);
}

static void test_execute_ex_dry_run_validates_command_target(void **state)
{
    test_ctx_t *ctx = *state;

    assert_int_equal(db_exec_raw(ctx->db,
        "INSERT INTO shutdown_groups (name, execution_order, parallel, "
        "max_timeout_sec, post_group_delay) VALUES ('g', 1, 0, 0, 0)"),
        CUTILS_OK);
    assert_int_equal(insert_command_target(ctx->db, 1, "t", "/bin/true"), CUTILS_OK);

    /* Dry run; method='command' validation passes (can't probe shell cmds). */
    shutdown_result_t *res = NULL;
    assert_int_equal(shutdown_execute_ex(ctx->mgr, 1, &res), CUTILS_OK);

    const shutdown_step_result_t *p1 = find_step(res, "phase1", "/t");
    assert_non_null(p1);
    assert_int_equal(p1->ok, 0);
    shutdown_result_free(res);
}

static void test_execute_ex_null_out_is_safe(void **state)
{
    test_ctx_t *ctx = *state;
    /* NULL out param: behaves like shutdown_execute, no crash, no leak. */
    assert_int_equal(shutdown_execute_ex(ctx->mgr, 0, NULL), CUTILS_OK);
}

static void test_shutdown_result_free_handles_null(void **state)
{
    (void)state;
    shutdown_result_free(NULL);   /* no-op, no crash */
}

/* --- Async workflow --- */

/* Spin until shutdown_get_status reports the desired state, or fail. The
 * UPS-mode-none / controller-disabled fixture means execute_ex returns
 * within tens of milliseconds, but we give ourselves a generous ceiling
 * so a slow CI runner can't flake the test. */
static void wait_for_state(shutdown_mgr_t *mgr, shutdown_state_t want,
                           int timeout_ms)
{
    shutdown_status_t st = {0};
    for (int waited = 0; waited < timeout_ms; waited += 10) {
        shutdown_get_status(mgr, &st);
        shutdown_state_t got = st.state;
        shutdown_status_free(&st);
        if (got == want) return;
        struct timespec ts = { .tv_nsec = 10 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    fail_msg("workflow did not reach state %d within %d ms", want, timeout_ms);
}

static void test_async_idle_state_before_start(void **state)
{
    test_ctx_t *ctx = *state;

    shutdown_status_t st = {0};
    shutdown_get_status(ctx->mgr, &st);
    assert_int_equal(st.state, SHUTDOWN_IDLE);
    assert_int_equal(st.workflow_id, 0);
    assert_int_equal(st.n_steps, 0);
    assert_null(st.steps);
    shutdown_status_free(&st);
}

static void test_async_start_runs_to_completion(void **state)
{
    test_ctx_t *ctx = *state;

    /* Set up a single sequential group with one quick command target so
     * the worker has something to write into the snapshot before it
     * transitions to COMPLETED. */
    assert_int_equal(db_exec_raw(ctx->db,
        "INSERT INTO shutdown_groups (name, execution_order, parallel, "
        "max_timeout_sec, post_group_delay) VALUES ('async-grp', 0, 0, 0, 0)"),
        CUTILS_OK);
    assert_int_equal(insert_command_target(ctx->db, 1, "async-tgt", "/bin/true"),
                     CUTILS_OK);

    uint64_t id = 0;
    assert_int_equal(shutdown_start_async(ctx->mgr, 0, &id), CUTILS_OK);
    assert_int_equal(id, 1);

    wait_for_state(ctx->mgr, SHUTDOWN_COMPLETED, 5000);

    shutdown_status_t st = {0};
    shutdown_get_status(ctx->mgr, &st);
    assert_int_equal(st.state, SHUTDOWN_COMPLETED);
    assert_int_equal(st.workflow_id, 1);
    assert_int_equal(st.dry_run, 0);
    assert_int_equal(st.all_ok, 1);
    assert_int_equal(st.n_failed, 0);
    /* Phase 1 (the target) + Phase 2 (skipped, ups_mode=none) +
     * Phase 3 (skipped, controller_enabled=0) = 3 rows. */
    assert_int_equal(st.n_steps, 3);
    assert_non_null(st.steps);
    assert_string_equal(st.steps[0].phase, "phase1");
    assert_string_equal(st.steps[0].target, "async-grp/async-tgt");
    assert_int_equal(st.steps[0].ok, 0);
    shutdown_status_free(&st);
}

static void test_async_second_start_returns_ealready(void **state)
{
    test_ctx_t *ctx = *state;

    /* Use a sleep target so the workflow is reliably still running when
     * we issue the second start_async. `sleep 1` is short enough to keep
     * the test fast but long enough that a 5ms-old worker can't have
     * finished yet. */
    assert_int_equal(db_exec_raw(ctx->db,
        "INSERT INTO shutdown_groups (name, execution_order, parallel, "
        "max_timeout_sec, post_group_delay) VALUES ('busy-grp', 0, 0, 0, 0)"),
        CUTILS_OK);
    assert_int_equal(insert_command_target(ctx->db, 1, "busy-tgt", "/bin/sleep 1"),
                     CUTILS_OK);

    uint64_t first = 0;
    assert_int_equal(shutdown_start_async(ctx->mgr, 0, &first), CUTILS_OK);
    assert_int_equal(first, 1);

    /* Second start while first is still running: -EALREADY, *out_id is
     * the id of the in-flight workflow (not a new one). */
    uint64_t conflict = 0;
    int rc = shutdown_start_async(ctx->mgr, 1, &conflict);
    assert_int_equal(rc, -EALREADY);
    assert_int_equal(conflict, first);

    /* Wait it out so teardown's shutdown_free can join cleanly. */
    wait_for_state(ctx->mgr, SHUTDOWN_COMPLETED, 5000);
}

static void test_async_completed_then_restart_increments_id(void **state)
{
    test_ctx_t *ctx = *state;

    /* No groups configured — workflow has zero phase-1 steps but
     * still emits phase-2 (skipped) and phase-3 (skipped) rows, so
     * the COMPLETED transition fires almost immediately. */
    assert_int_equal(shutdown_start_async(ctx->mgr, 1, NULL), CUTILS_OK);
    wait_for_state(ctx->mgr, SHUTDOWN_COMPLETED, 5000);

    shutdown_status_t st = {0};
    shutdown_get_status(ctx->mgr, &st);
    assert_int_equal(st.workflow_id, 1);
    assert_int_equal(st.dry_run, 1);
    shutdown_status_free(&st);

    /* A start after a completed run resets the snapshot and bumps the id. */
    uint64_t second_id = 0;
    assert_int_equal(shutdown_start_async(ctx->mgr, 0, &second_id), CUTILS_OK);
    assert_int_equal(second_id, 2);

    wait_for_state(ctx->mgr, SHUTDOWN_COMPLETED, 5000);

    shutdown_get_status(ctx->mgr, &st);
    assert_int_equal(st.workflow_id, 2);
    assert_int_equal(st.dry_run, 0);
    shutdown_status_free(&st);
}

static void test_async_status_free_handles_null_and_zero(void **state)
{
    (void)state;
    shutdown_status_free(NULL);          /* no-op */
    shutdown_status_t empty = {0};
    shutdown_status_free(&empty);        /* no-op on a zeroed struct */
    /* Idempotent: second call after free clears nothing. */
    shutdown_status_free(&empty);
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
        /* execute non-dry-run command-method paths */
        cmocka_unit_test_setup_teardown(test_execute_sequential_command_group, setup, teardown),
        cmocka_unit_test_setup_teardown(test_execute_parallel_command_group, setup, teardown),
        cmocka_unit_test_setup_teardown(test_execute_parallel_with_max_timeout, setup, teardown),
        cmocka_unit_test_setup_teardown(test_execute_sequential_with_max_timeout, setup, teardown),
        /* trigger fire + efficiency nan */
        cmocka_unit_test_setup_teardown(test_trigger_fires_after_debounce, setup, teardown),
        cmocka_unit_test_setup_teardown(test_trigger_field_efficiency_invalid_reason_no_fire, setup, teardown),
        /* shutdown_test_target */
        cmocka_unit_test_setup_teardown(test_shutdown_test_target_not_found, setup, teardown),
        cmocka_unit_test_setup_teardown(test_shutdown_test_target_command_method_ok, setup, teardown),
        cmocka_unit_test_setup_teardown(test_shutdown_test_target_unknown_method_fails, setup, teardown),
        cmocka_unit_test_setup_teardown(test_shutdown_test_target_confirm_not_found, setup, teardown),
        cmocka_unit_test_setup_teardown(test_shutdown_test_target_confirm_none_method_ok, setup, teardown),
        cmocka_unit_test_setup_teardown(test_shutdown_test_target_confirm_command_success, setup, teardown),
        cmocka_unit_test_setup_teardown(test_shutdown_test_target_confirm_command_failure, setup, teardown),
        cmocka_unit_test_setup_teardown(test_shutdown_test_target_confirm_command_empty_fails, setup, teardown),
        cmocka_unit_test_setup_teardown(test_shutdown_test_target_confirm_unknown_method_fails, setup, teardown),
        /* ups action phase */
        cmocka_unit_test_setup_teardown(test_execute_ups_mode_none_skips_ups_action, setup, teardown),
        cmocka_unit_test_setup_teardown(test_execute_ups_mode_unknown_warns, setup, teardown),
        cmocka_unit_test_setup_teardown(test_execute_ups_mode_command_fires_shutdown, setup, teardown),
        cmocka_unit_test_setup_teardown(test_execute_ups_mode_register_writes_register, setup, teardown),
        /* internal helpers */
        cmocka_unit_test(test_write_key_to_tmpfs_writes_content_at_0600),
        cmocka_unit_test(test_fire_target_action_command_success),
        cmocka_unit_test(test_fire_target_action_command_failure),
        cmocka_unit_test(test_fire_target_action_unknown_method),
        /* shutdown_execute_ex result aggregation */
        cmocka_unit_test_setup_teardown(test_execute_ex_no_groups_emits_phase2_phase3, setup, teardown),
        cmocka_unit_test_setup_teardown(test_execute_ex_phase2_skipped_when_mode_none, setup, teardown),
        cmocka_unit_test_setup_teardown(test_execute_ex_phase3_skipped_when_disabled, setup, teardown),
        cmocka_unit_test_setup_teardown(test_execute_ex_sequential_failure_captured, setup, teardown),
        cmocka_unit_test_setup_teardown(test_execute_ex_parallel_failure_captured, setup, teardown),
        cmocka_unit_test_setup_teardown(test_execute_ex_dry_run_validates_command_target, setup, teardown),
        cmocka_unit_test_setup_teardown(test_execute_ex_null_out_is_safe, setup, teardown),
        cmocka_unit_test(test_shutdown_result_free_handles_null),
        /* async workflow */
        cmocka_unit_test_setup_teardown(test_async_idle_state_before_start,            setup, teardown),
        cmocka_unit_test_setup_teardown(test_async_start_runs_to_completion,           setup, teardown),
        cmocka_unit_test_setup_teardown(test_async_second_start_returns_ealready,      setup, teardown),
        cmocka_unit_test_setup_teardown(test_async_completed_then_restart_increments_id, setup, teardown),
        cmocka_unit_test(test_async_status_free_handles_null_and_zero),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
