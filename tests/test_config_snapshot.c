/* Diff-aware decision for the daily config register snapshot.
 *
 * The daily pass reads every config register and compares against the most
 * recent ups_config row for that register. This test exercises the three
 * outcomes of that comparison — no prior row (baseline), same value (skip),
 * different value (external). */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <cmocka.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <cutils/db.h>

#include "monitor/config_snapshot.h"

#define TEST_DB "/tmp/test_config_snapshot.db"

/* Mirror of migrations 003 + 012 — kept inline so the test doesn't depend on
 * the migration bundle and runs fully self-contained. */
static const char *SCHEMA =
    "CREATE TABLE IF NOT EXISTS ups_config ("
    "    id           INTEGER PRIMARY KEY AUTOINCREMENT,"
    "    register_name TEXT   NOT NULL,"
    "    raw_value    INTEGER NOT NULL,"
    "    display_value TEXT,"
    "    timestamp    TEXT    NOT NULL,"
    "    source       TEXT"
    ");";

typedef struct {
    cutils_db_t *db;
} ctx_t;

static int setup(void **state)
{
    unlink(TEST_DB);
    cutils_db_t *db = NULL;
    if (db_open(&db, TEST_DB) != 0 || !db) return -1;
    if (db_exec_raw(db, SCHEMA) != 0) { db_close(db); return -1; }
    ctx_t *c = calloc(1, sizeof(*c));
    c->db = db;
    *state = c;
    return 0;
}

static int teardown(void **state)
{
    ctx_t *c = *state;
    db_close(c->db);
    free(c);
    unlink(TEST_DB);
    return 0;
}

static void insert_row(cutils_db_t *db, const char *name, uint16_t raw,
                       const char *source)
{
    char raw_s[16];
    snprintf(raw_s, sizeof(raw_s), "%u", raw);
    const char *params[] = { name, raw_s, "", "2026-04-23 00:00:00", source, NULL };
    assert_int_equal(db_execute_non_query(db,
        "INSERT INTO ups_config "
        "(register_name, raw_value, display_value, timestamp, source) "
        "VALUES (?, ?, ?, ?, ?)", params, NULL), 0);
}

/* No prior row — the register has never been snapshotted, so this is the
 * seed value. The caller will insert it with source='baseline'. */
static void test_no_prior_row_returns_baseline(void **state)
{
    ctx_t *c = *state;
    assert_int_equal(
        monitor_config_snapshot_decide(c->db, "transfer_low", 103),
        CONFIG_SNAPSHOT_BASELINE);
}

/* Prior row exists and matches current reading — no change, skip the insert
 * so history only ever contains real deltas. */
static void test_equal_to_prior_returns_skip(void **state)
{
    ctx_t *c = *state;
    insert_row(c->db, "transfer_low", 103, "baseline");
    assert_int_equal(
        monitor_config_snapshot_decide(c->db, "transfer_low", 103),
        CONFIG_SNAPSHOT_SKIP);
}

/* Prior row exists and differs — an external change (LCD / front panel).
 * The monitor will insert a new row with source='external'. */
static void test_differs_from_prior_returns_external(void **state)
{
    ctx_t *c = *state;
    insert_row(c->db, "transfer_low", 103, "baseline");
    assert_int_equal(
        monitor_config_snapshot_decide(c->db, "transfer_low", 110),
        CONFIG_SNAPSHOT_EXTERNAL);
}

/* Most-recent-row-wins semantics: if there's a history of changes, compare
 * against the latest one, not the earliest. */
static void test_uses_most_recent_row(void **state)
{
    ctx_t *c = *state;
    insert_row(c->db, "transfer_low", 103, "baseline");
    insert_row(c->db, "transfer_low", 110, "external");
    insert_row(c->db, "transfer_low", 115, "api");

    /* 115 is the current reading → matches latest → skip. */
    assert_int_equal(
        monitor_config_snapshot_decide(c->db, "transfer_low", 115),
        CONFIG_SNAPSHOT_SKIP);

    /* An older value reappearing (say, operator reverted via LCD) is a
     * change against the most recent row, so external. */
    assert_int_equal(
        monitor_config_snapshot_decide(c->db, "transfer_low", 103),
        CONFIG_SNAPSHOT_EXTERNAL);
}

/* Per-register isolation — a row for one register must not affect the
 * decision for a different register. */
static void test_other_register_doesnt_interfere(void **state)
{
    ctx_t *c = *state;
    insert_row(c->db, "transfer_low", 103, "baseline");
    assert_int_equal(
        monitor_config_snapshot_decide(c->db, "transfer_high", 140),
        CONFIG_SNAPSHOT_BASELINE);
}

/* Legacy rows (pre-migration-012) have source=NULL. The diff ignores source
 * entirely — decision is purely on raw_value — so a NULL-source prior row
 * still gates the next snapshot correctly. */
static void test_null_source_legacy_row_still_gates(void **state)
{
    ctx_t *c = *state;
    const char *params[] = { "transfer_low", "103", "", "2025-01-01 00:00:00", NULL };
    assert_int_equal(db_execute_non_query(c->db,
        "INSERT INTO ups_config "
        "(register_name, raw_value, display_value, timestamp) "
        "VALUES (?, ?, ?, ?)", params, NULL), 0);

    assert_int_equal(
        monitor_config_snapshot_decide(c->db, "transfer_low", 103),
        CONFIG_SNAPSHOT_SKIP);
    assert_int_equal(
        monitor_config_snapshot_decide(c->db, "transfer_low", 110),
        CONFIG_SNAPSHOT_EXTERNAL);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(test_no_prior_row_returns_baseline,   setup, teardown),
        cmocka_unit_test_setup_teardown(test_equal_to_prior_returns_skip,     setup, teardown),
        cmocka_unit_test_setup_teardown(test_differs_from_prior_returns_external, setup, teardown),
        cmocka_unit_test_setup_teardown(test_uses_most_recent_row,            setup, teardown),
        cmocka_unit_test_setup_teardown(test_other_register_doesnt_interfere, setup, teardown),
        cmocka_unit_test_setup_teardown(test_null_source_legacy_row_still_gates, setup, teardown),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
