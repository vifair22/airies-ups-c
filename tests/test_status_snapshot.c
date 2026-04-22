/* Round-trip tests for the persisted UPS status snapshot used by the
 * monitor and alert engine to detect transitions across daemon restarts. */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <cmocka.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <cutils/db.h>

#include "monitor/status_snapshot.h"

#define TEST_DB "/tmp/test_status_snapshot.db"

/* Mirror of migration 011 — kept inline so the test isn't sensitive to
 * the migrations bundle and runs fully self-contained. */
static const char *SCHEMA =
    "CREATE TABLE IF NOT EXISTS ups_status_snapshot ("
    "    id                  INTEGER PRIMARY KEY CHECK (id = 1),"
    "    status              INTEGER NOT NULL,"
    "    bat_system_error    INTEGER NOT NULL,"
    "    general_error       INTEGER NOT NULL,"
    "    power_system_error  INTEGER NOT NULL,"
    "    bat_lifetime_status INTEGER NOT NULL,"
    "    updated_at          TEXT    NOT NULL"
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

/* Empty-DB load → -1, leaves out untouched. */
static void test_load_when_empty_returns_minus_one(void **state)
{
    ctx_t *c = *state;
    status_snapshot_t snap;
    memset(&snap, 0xCC, sizeof(snap));  /* sentinel — must not be overwritten */

    assert_int_equal(status_snapshot_load(c->db, &snap), -1);

    /* The fields stay at the sentinel — load must not partially populate. */
    assert_int_equal(snap.status,           0xCCCCCCCCu);
    assert_int_equal(snap.bat_system_error, 0xCCCCu);
}

/* Save then load returns the same fields. */
static void test_save_then_load_round_trip(void **state)
{
    ctx_t *c = *state;
    status_snapshot_t in = {
        .status              = 0x00002022u,  /* Online + Fault + HE — like the live SMT */
        .bat_system_error    = 0x0001u,      /* Disconnected */
        .general_error       = 0x0008u,      /* LogicPowerSupply */
        .power_system_error  = 0x00002000u,  /* Inverter */
        .bat_lifetime_status = 0x0002u,      /* LifeTimeNearEnd */
        .updated_at          = "",            /* save() fills this */
    };
    assert_int_equal(status_snapshot_save(c->db, &in), 0);
    assert_true(in.updated_at[0] != '\0');  /* save stamped a timestamp */

    status_snapshot_t out;
    memset(&out, 0, sizeof(out));
    assert_int_equal(status_snapshot_load(c->db, &out), 0);
    assert_int_equal(out.status,              in.status);
    assert_int_equal(out.bat_system_error,    in.bat_system_error);
    assert_int_equal(out.general_error,       in.general_error);
    assert_int_equal(out.power_system_error,  in.power_system_error);
    assert_int_equal(out.bat_lifetime_status, in.bat_lifetime_status);
    assert_string_equal(out.updated_at, in.updated_at);
}

/* Repeated saves UPSERT against the singleton row — last write wins,
 * and there's still exactly one row. */
static void test_repeated_save_upserts_singleton(void **state)
{
    ctx_t *c = *state;

    status_snapshot_t s = { .status = 0x00000002u };  /* Online only */
    assert_int_equal(status_snapshot_save(c->db, &s), 0);

    s.status = 0x00002022u;  /* now also Fault + HE */
    s.bat_system_error = 0x0001u;
    assert_int_equal(status_snapshot_save(c->db, &s), 0);

    s.status = 0x00000004u;  /* OnBattery only */
    s.bat_system_error = 0;
    assert_int_equal(status_snapshot_save(c->db, &s), 0);

    /* Load reflects the last save. */
    status_snapshot_t out;
    assert_int_equal(status_snapshot_load(c->db, &out), 0);
    assert_int_equal(out.status, 0x00000004u);
    assert_int_equal(out.bat_system_error, 0u);

    /* And there's exactly one row. */
    db_result_t *r = NULL;
    assert_int_equal(db_execute(c->db,
        "SELECT COUNT(*) FROM ups_status_snapshot", NULL, &r), 0);
    assert_non_null(r);
    assert_int_equal(r->nrows, 1);
    assert_string_equal(r->rows[0][0], "1");
    db_result_free(r);
}

/* Wide values (uint32 fields) survive the INTEGER round-trip without
 * sign-extension or truncation. */
static void test_wide_uint32_values_round_trip(void **state)
{
    ctx_t *c = *state;
    status_snapshot_t in = {
        .status             = 0xDEADBEEFu,
        .power_system_error = 0xFEEDFACEu,
    };
    assert_int_equal(status_snapshot_save(c->db, &in), 0);

    status_snapshot_t out;
    assert_int_equal(status_snapshot_load(c->db, &out), 0);
    assert_int_equal(out.status,             0xDEADBEEFu);
    assert_int_equal(out.power_system_error, 0xFEEDFACEu);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(test_load_when_empty_returns_minus_one, setup, teardown),
        cmocka_unit_test_setup_teardown(test_save_then_load_round_trip,         setup, teardown),
        cmocka_unit_test_setup_teardown(test_repeated_save_upserts_singleton,   setup, teardown),
        cmocka_unit_test_setup_teardown(test_wide_uint32_values_round_trip,     setup, teardown),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
