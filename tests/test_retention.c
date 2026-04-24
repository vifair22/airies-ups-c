/* Tests for the telemetry retention sweep — delete + downsample + vacuum.
 * Inserts rows at controlled ages via SQLite datetime() modifiers and
 * verifies each phase independently. */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <cmocka.h>

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include <cutils/db.h>
#include <cutils/error.h>

#include "monitor/retention.h"

#define TEST_DB "/tmp/test_retention.db"

/* Subset of the telemetry schema retention actually touches. The real
 * migrations add more columns (outlet states, energy) which aren't
 * relevant here — only timestamp + id matter for the sweep. */
static const char *SCHEMA =
    "CREATE TABLE telemetry ("
    "    id              INTEGER PRIMARY KEY AUTOINCREMENT,"
    "    timestamp       TEXT    NOT NULL,"
    "    status          INTEGER NOT NULL,"
    "    charge_pct      REAL,"
    "    runtime_sec     INTEGER,"
    "    battery_voltage REAL,"
    "    load_pct        REAL,"
    "    output_voltage  REAL,"
    "    output_frequency REAL,"
    "    output_current  REAL,"
    "    input_voltage   REAL,"
    "    efficiency      REAL,"
    "    temperature     REAL"
    ");"
    "CREATE INDEX idx_telemetry_ts ON telemetry(timestamp);";

static int setup(void **state)
{
    unlink(TEST_DB);
    cutils_db_t *db = NULL;
    if (db_open(&db, TEST_DB) != 0 || !db) return -1;
    if (db_exec_raw(db, SCHEMA) != 0) { db_close(db); return -1; }
    *state = db;
    return 0;
}

static int teardown(void **state)
{
    cutils_db_t *db = *state;
    db_close(db);
    unlink(TEST_DB);
    return 0;
}

/* --- Helpers --- */

/* Insert a telemetry row at (now - minutes). Everything is expressed in
 * minutes because SQLite's datetime() takes a single modifier at a time,
 * and the downsampling test needs sub-hour resolution within a single
 * window. */
static void insert_at_minutes(cutils_db_t *db, int minutes_ago)
{
    char sql[256];
    snprintf(sql, sizeof(sql),
        "INSERT INTO telemetry (timestamp, status) "
        "VALUES (datetime('now', '-%d minutes'), 0)", minutes_ago);
    assert_int_equal(db_exec_raw(db, sql), CUTILS_OK);
}

/* Insert at a fixed timestamp. Used for downsample tests so multiple rows
 * land in exactly the same window regardless of how the current wall-clock
 * "now" aligns to window boundaries — avoids flaky failures when a
 * relative-offset cluster happens to straddle a window cut. */
static void insert_at_ts(cutils_db_t *db, const char *ts)
{
    const char *params[] = { ts, NULL };
    assert_int_equal(db_execute_non_query(db,
        "INSERT INTO telemetry (timestamp, status) VALUES (?, 0)",
        params, NULL), CUTILS_OK);
}

#define DAYS(n)  ((n) * 1440)
#define HOURS(n) ((n) * 60)

static int count_rows(cutils_db_t *db)
{
    CUTILS_AUTO_DBRES db_result_t *result = NULL;
    assert_int_equal(db_execute(db,
        "SELECT COUNT(*) FROM telemetry", NULL, &result), CUTILS_OK);
    assert_non_null(result);
    return (int)strtol(result->rows[0][0], NULL, 10);
}

/* --- Phase 1: delete old rows --- */

static void test_deletes_rows_older_than_retention(void **state)
{
    cutils_db_t *db = *state;

    insert_at_minutes(db, DAYS(100));  /* deleted */
    insert_at_minutes(db, DAYS(50));   /* deleted */
    insert_at_minutes(db, DAYS(10));   /* survives (< 30d retention) */
    insert_at_minutes(db, HOURS(1));   /* survives */
    assert_int_equal(count_rows(db), 4);

    /* retention_days=30, downsampling disabled so this test only exercises phase 1 */
    retention_config_t cfg = { .full_res_hours = 0, .downsample_minutes = 0, .retention_days = 30 };
    assert_int_equal(retention_run(db, &cfg), 0);

    assert_int_equal(count_rows(db), 2);
}

static void test_retention_zero_disables_delete(void **state)
{
    cutils_db_t *db = *state;

    insert_at_minutes(db, DAYS(1000));
    insert_at_minutes(db, HOURS(1));
    assert_int_equal(count_rows(db), 2);

    retention_config_t cfg = { .full_res_hours = 0, .downsample_minutes = 0, .retention_days = 0 };
    assert_int_equal(retention_run(db, &cfg), 0);

    /* Both rows survive — phase 1 gated on retention_days > 0 */
    assert_int_equal(count_rows(db), 2);
}

/* --- Phase 2: downsample --- */

static void test_downsamples_collapses_window(void **state)
{
    cutils_db_t *db = *state;

    /* Three rows at the exact same old timestamp — guaranteed to fall in
     * the same 15-minute downsample window. Only MIN(id) survives. */
    insert_at_ts(db, "2024-01-01 12:00:00");
    insert_at_ts(db, "2024-01-01 12:00:00");
    insert_at_ts(db, "2024-01-01 12:00:00");
    /* Recent row: within full_res_hours, must not be touched */
    insert_at_minutes(db, HOURS(1));
    assert_int_equal(count_rows(db), 4);

    retention_config_t cfg = {
        .full_res_hours = 24, .downsample_minutes = 15, .retention_days = 0,
    };
    assert_int_equal(retention_run(db, &cfg), 0);

    /* 1 from the old window + 1 recent = 2 */
    assert_int_equal(count_rows(db), 2);
}

static void test_downsample_preserves_full_resolution_window(void **state)
{
    cutils_db_t *db = *state;

    /* All rows within full_res_hours — phase 2 must not touch them
     * regardless of how close in time they are. */
    insert_at_minutes(db, HOURS(1));
    insert_at_minutes(db, HOURS(1) + 5);
    insert_at_minutes(db, HOURS(1) + 10);
    assert_int_equal(count_rows(db), 3);

    retention_config_t cfg = {
        .full_res_hours = 24, .downsample_minutes = 15, .retention_days = 0,
    };
    assert_int_equal(retention_run(db, &cfg), 0);

    assert_int_equal(count_rows(db), 3);
}

static void test_downsample_zero_disables_phase(void **state)
{
    cutils_db_t *db = *state;

    /* Three old rows in the same would-be window */
    insert_at_minutes(db, HOURS(48));
    insert_at_minutes(db, HOURS(48) + 5);
    insert_at_minutes(db, HOURS(48) + 10);

    /* downsample_minutes=0 disables phase 2 even if full_res_hours > 0 */
    retention_config_t cfg = {
        .full_res_hours = 24, .downsample_minutes = 0, .retention_days = 0,
    };
    assert_int_equal(retention_run(db, &cfg), 0);

    assert_int_equal(count_rows(db), 3);
}

static void test_full_res_hours_zero_disables_phase(void **state)
{
    cutils_db_t *db = *state;

    insert_at_minutes(db, HOURS(48));
    insert_at_minutes(db, HOURS(48) + 5);

    /* full_res_hours=0 disables phase 2 even with a downsample interval */
    retention_config_t cfg = {
        .full_res_hours = 0, .downsample_minutes = 15, .retention_days = 0,
    };
    assert_int_equal(retention_run(db, &cfg), 0);

    assert_int_equal(count_rows(db), 2);
}

/* --- Both phases together --- */

static void test_delete_and_downsample_combined(void **state)
{
    cutils_db_t *db = *state;

    /* Very old — deleted by phase 1 */
    insert_at_ts(db, "2020-01-01 00:00:00");
    /* Middle-aged, same timestamp → one downsample window → collapses to 1.
     * Must be older than full_res_hours=24 but within retention_days=30. */
    char middle_ts[32];
    time_t middle = time(NULL) - 2 * 86400;  /* 2 days ago */
    struct tm tm;
    gmtime_r(&middle, &tm);
    strftime(middle_ts, sizeof(middle_ts), "%Y-%m-%d %H:%M:%S", &tm);
    insert_at_ts(db, middle_ts);
    insert_at_ts(db, middle_ts);
    insert_at_ts(db, middle_ts);
    /* Recent — untouched */
    insert_at_minutes(db, HOURS(1));
    assert_int_equal(count_rows(db), 5);

    retention_config_t cfg = {
        .full_res_hours = 24, .downsample_minutes = 15, .retention_days = 30,
    };
    assert_int_equal(retention_run(db, &cfg), 0);

    /* 1 downsampled + 1 recent = 2 */
    assert_int_equal(count_rows(db), 2);
}

/* --- Edge cases --- */

static void test_empty_db_no_crash(void **state)
{
    cutils_db_t *db = *state;
    assert_int_equal(count_rows(db), 0);

    retention_config_t cfg = {
        .full_res_hours = 24, .downsample_minutes = 15, .retention_days = 30,
    };
    assert_int_equal(retention_run(db, &cfg), 0);

    assert_int_equal(count_rows(db), 0);
}

static void test_all_phases_disabled_is_noop(void **state)
{
    cutils_db_t *db = *state;

    insert_at_minutes(db, DAYS(100));
    insert_at_minutes(db, HOURS(1));

    retention_config_t cfg = {0};
    assert_int_equal(retention_run(db, &cfg), 0);

    assert_int_equal(count_rows(db), 2);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(test_deletes_rows_older_than_retention, setup, teardown),
        cmocka_unit_test_setup_teardown(test_retention_zero_disables_delete, setup, teardown),
        cmocka_unit_test_setup_teardown(test_downsamples_collapses_window, setup, teardown),
        cmocka_unit_test_setup_teardown(test_downsample_preserves_full_resolution_window, setup, teardown),
        cmocka_unit_test_setup_teardown(test_downsample_zero_disables_phase, setup, teardown),
        cmocka_unit_test_setup_teardown(test_full_res_hours_zero_disables_phase, setup, teardown),
        cmocka_unit_test_setup_teardown(test_delete_and_downsample_combined, setup, teardown),
        cmocka_unit_test_setup_teardown(test_empty_db_no_crash, setup, teardown),
        cmocka_unit_test_setup_teardown(test_all_phases_disabled_is_noop, setup, teardown),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
