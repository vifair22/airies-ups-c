/* Tests for the transfer-reason ring buffer used by the monitor's
 * fast-poll thread. The ring stores recent register-2 transitions so
 * status-bit events fired by the slow poll can pick up the actual cause
 * even when register 2 has already reverted to AcceptableInput. */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <cmocka.h>

#include <string.h>

#include "monitor/xfer_ring.h"
#include "ups/ups_format.h"

/* Symbolic non-AcceptableInput values. The decoder treats anything in
 * range 0..30 as a known reason; pick a few that exercise the lookup
 * without overlapping XFER_REASON_ACCEPTABLE_INPUT (8). */
#define R_DISTORTED  3u   /* DistortedInput */
#define R_HIGH_VOLT  1u   /* HighInputVoltage */
#define R_LOW_VOLT   2u   /* LowInputVoltage */

static void test_init_zeros_all_state(void **state)
{
    (void)state;
    xfer_ring_t r;
    /* Pre-fill with garbage so init has something to actually clear. */
    memset(&r, 0xCC, sizeof(r));
    xfer_ring_init(&r);

    assert_int_equal(r.head, 0);
    assert_int_equal(r.count, 0);
    assert_int_equal(r.last_seen_valid, 0);
}

static void test_push_first_value_records_and_returns_one(void **state)
{
    (void)state;
    xfer_ring_t r;
    xfer_ring_init(&r);

    int rc = xfer_ring_push(&r, 1000, R_DISTORTED);
    assert_int_equal(rc, 1);
    assert_int_equal(r.count, 1);
    assert_int_equal(r.entries[0].reason, R_DISTORTED);
    assert_int_equal(r.entries[0].timestamp_ms, 1000);
    assert_int_equal(r.last_seen_valid, 1);
    assert_int_equal(r.last_seen, R_DISTORTED);
}

static void test_push_duplicate_returns_zero_and_does_not_record(void **state)
{
    (void)state;
    xfer_ring_t r;
    xfer_ring_init(&r);

    assert_int_equal(xfer_ring_push(&r, 1000, R_DISTORTED), 1);
    assert_int_equal(xfer_ring_push(&r, 1100, R_DISTORTED), 0);
    /* Only the first push counts. */
    assert_int_equal(r.count, 1);
    assert_int_equal(r.entries[0].timestamp_ms, 1000);
}

static void test_push_change_returns_one_and_records(void **state)
{
    (void)state;
    xfer_ring_t r;
    xfer_ring_init(&r);

    assert_int_equal(xfer_ring_push(&r, 1000, R_DISTORTED), 1);
    assert_int_equal(xfer_ring_push(&r, 1100, XFER_REASON_ACCEPTABLE_INPUT), 1);
    assert_int_equal(r.count, 2);
}

static void test_push_wraps_at_capacity(void **state)
{
    (void)state;
    xfer_ring_t r;
    xfer_ring_init(&r);

    /* Alternating values so each push is a real transition. We push
     * SIZE+3 entries; count caps at SIZE and head wraps. */
    for (size_t i = 0; i < XFER_RING_SIZE + 3; i++) {
        uint16_t v = (i % 2 == 0) ? R_DISTORTED : R_HIGH_VOLT;
        assert_int_equal(xfer_ring_push(&r, 1000 + i, v), 1);
    }
    assert_int_equal(r.count, XFER_RING_SIZE);
    /* head wraps to 3 after SIZE+3 writes. */
    assert_int_equal(r.head, 3);
}

static void test_recent_cause_empty_returns_unknown(void **state)
{
    (void)state;
    xfer_ring_t r;
    xfer_ring_init(&r);

    assert_int_equal(xfer_ring_recent_cause(&r, 5000, 5000),
                     UPS_TRANSFER_REASON_UNKNOWN);
}

static void test_recent_cause_finds_only_non_acceptable_entry(void **state)
{
    (void)state;
    xfer_ring_t r;
    xfer_ring_init(&r);
    xfer_ring_push(&r, 1000, R_DISTORTED);

    assert_int_equal(xfer_ring_recent_cause(&r, /*now*/2000, /*back*/5000),
                     R_DISTORTED);
}

static void test_recent_cause_outside_window_returns_unknown(void **state)
{
    (void)state;
    xfer_ring_t r;
    xfer_ring_init(&r);
    xfer_ring_push(&r, 1000, R_DISTORTED);

    /* now=10000, lookback=5000 → cutoff=5000; entry at 1000 is too old. */
    assert_int_equal(xfer_ring_recent_cause(&r, 10000, 5000),
                     UPS_TRANSFER_REASON_UNKNOWN);
}

static void test_recent_cause_skips_acceptable_input(void **state)
{
    (void)state;
    xfer_ring_t r;
    xfer_ring_init(&r);
    /* Only AcceptableInput in window — caller should get UNKNOWN so it
     * doesn't annotate events with a misleading "(reason: Acceptable)". */
    xfer_ring_push(&r, 1000, XFER_REASON_ACCEPTABLE_INPUT);
    /* Push something else to make it a real transition recordable. */
    xfer_ring_push(&r, 1100, R_DISTORTED);
    xfer_ring_push(&r, 1200, XFER_REASON_ACCEPTABLE_INPUT);

    /* Even though AcceptableInput is the most recent push, the lookup
     * should return DistortedInput because that's the meaningful cause. */
    assert_int_equal(xfer_ring_recent_cause(&r, 2000, 5000), R_DISTORTED);
}

static void test_recent_cause_returns_most_recent_when_multiple(void **state)
{
    (void)state;
    xfer_ring_t r;
    xfer_ring_init(&r);
    xfer_ring_push(&r, 1000, R_HIGH_VOLT);
    xfer_ring_push(&r, 1500, R_LOW_VOLT);
    xfer_ring_push(&r, 2000, R_DISTORTED);

    assert_int_equal(xfer_ring_recent_cause(&r, 3000, 5000), R_DISTORTED);
}

static void test_recent_cause_now_smaller_than_lookback(void **state)
{
    (void)state;
    /* Edge case: cutoff would underflow without the guard. now=100,
     * lookback=5000 → cutoff should clamp to 0, so all entries are
     * in-window. */
    xfer_ring_t r;
    xfer_ring_init(&r);
    xfer_ring_push(&r, 50, R_DISTORTED);

    assert_int_equal(xfer_ring_recent_cause(&r, 100, 5000), R_DISTORTED);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_init_zeros_all_state),
        cmocka_unit_test(test_push_first_value_records_and_returns_one),
        cmocka_unit_test(test_push_duplicate_returns_zero_and_does_not_record),
        cmocka_unit_test(test_push_change_returns_one_and_records),
        cmocka_unit_test(test_push_wraps_at_capacity),
        cmocka_unit_test(test_recent_cause_empty_returns_unknown),
        cmocka_unit_test(test_recent_cause_finds_only_non_acceptable_entry),
        cmocka_unit_test(test_recent_cause_outside_window_returns_unknown),
        cmocka_unit_test(test_recent_cause_skips_acceptable_input),
        cmocka_unit_test(test_recent_cause_returns_most_recent_when_multiple),
        cmocka_unit_test(test_recent_cause_now_smaller_than_lookback),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
