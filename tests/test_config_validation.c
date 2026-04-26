/* Validation contract for ups_config_write — rejects scalar
 * out-of-range and strict-bitfield non-listed values BEFORE the driver
 * sees them. Exercises the registry directly against a fake driver
 * that records calls so we can assert the boundary held. */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <cmocka.h>

#include <pthread.h>
#include <string.h>
#include <stdlib.h>

#include "ups/ups.h"
#include "ups/ups_driver.h"

/* --- Fake driver: counts config_write calls, returns 0 (success) --- */

static int fake_driver_writes;
static uint16_t fake_driver_last_value;

static int fake_config_write(void *transport, const ups_config_reg_t *reg, uint16_t value)
{
    (void)transport;
    (void)reg;
    fake_driver_writes++;
    fake_driver_last_value = value;
    return 0;
}

static int fake_config_read(void *transport, const ups_config_reg_t *reg,
                            uint32_t *raw, char *str, size_t str_sz)
{
    (void)transport; (void)reg; (void)str; (void)str_sz;
    if (raw) *raw = 0;
    return 0;
}

static const ups_driver_t fake_driver = {
    .name         = "fake",
    .conn_type    = UPS_CONN_SERIAL,
    .topology     = UPS_TOPO_LINE_INTERACTIVE,
    .config_read  = fake_config_read,
    .config_write = fake_config_write,
};

/* --- Test fixtures --- */

static int sentinel_transport = 1;  /* address-only sentinel */

/* Build a ups_t on the heap with the fake driver wired in. The
 * registry's ups_ensure_transport() returns truthy iff transport is
 * non-NULL — pointing at a sentinel keeps us out of the reconnect
 * path entirely. */
static ups_t *make_ups(void)
{
    ups_t *u = calloc(1, sizeof(*u));
    assert_non_null(u);
    u->driver    = &fake_driver;
    u->transport = &sentinel_transport;
    pthread_mutex_init(&u->cmd_mutex, NULL);
    return u;
}

static void free_ups(ups_t *u)
{
    pthread_mutex_destroy(&u->cmd_mutex);
    free(u);
}

static int setup(void **state)
{
    fake_driver_writes = 0;
    fake_driver_last_value = 0xFFFF;
    *state = make_ups();
    return 0;
}

static int teardown(void **state)
{
    free_ups(*state);
    return 0;
}

/* --- Bitfield options for the test descriptors --- */

static const ups_bitfield_opt_t test_opts[] = {
    { 0x01, "alpha", "Alpha" },
    { 0x02, "beta",  "Beta"  },
    { 0x10, "gamma", "Gamma" },
};

/* --- Scalar validation --- */

static void test_scalar_in_range_accepted(void **state)
{
    ups_t *u = *state;
    ups_config_reg_t reg = {
        .name = "transfer_high", .writable = 1,
        .type = UPS_CFG_SCALAR,
        .meta.scalar = { .min = 110, .max = 150 },
    };
    assert_int_equal(ups_config_write(u, &reg, 130), UPS_OK);
    assert_int_equal(fake_driver_writes, 1);
    assert_int_equal(fake_driver_last_value, 130);
}

static void test_scalar_below_min_rejected(void **state)
{
    ups_t *u = *state;
    ups_config_reg_t reg = {
        .name = "transfer_high", .writable = 1,
        .type = UPS_CFG_SCALAR,
        .meta.scalar = { .min = 110, .max = 150 },
    };
    assert_int_equal(ups_config_write(u, &reg, 100), UPS_ERR_INVALID_VALUE);
    assert_int_equal(fake_driver_writes, 0);
}

static void test_scalar_above_max_rejected(void **state)
{
    ups_t *u = *state;
    ups_config_reg_t reg = {
        .name = "transfer_high", .writable = 1,
        .type = UPS_CFG_SCALAR,
        .meta.scalar = { .min = 110, .max = 150 },
    };
    assert_int_equal(ups_config_write(u, &reg, 151), UPS_ERR_INVALID_VALUE);
    assert_int_equal(fake_driver_writes, 0);
}

static void test_scalar_at_boundaries_accepted(void **state)
{
    ups_t *u = *state;
    ups_config_reg_t reg = {
        .name = "x", .writable = 1, .type = UPS_CFG_SCALAR,
        .meta.scalar = { .min = 110, .max = 150 },
    };
    assert_int_equal(ups_config_write(u, &reg, 110), UPS_OK);
    assert_int_equal(ups_config_write(u, &reg, 150), UPS_OK);
    assert_int_equal(fake_driver_writes, 2);
}

/* The {0,0} convention means "no range declared" — any value passes
 * through. Critical for backward-compatibility with existing
 * descriptors that didn't bother declaring a range. */
static void test_scalar_zero_zero_unconstrained(void **state)
{
    ups_t *u = *state;
    ups_config_reg_t reg = {
        .name = "anything", .writable = 1, .type = UPS_CFG_SCALAR,
        .meta.scalar = { .min = 0, .max = 0 },
    };
    assert_int_equal(ups_config_write(u, &reg, 0),     UPS_OK);
    assert_int_equal(ups_config_write(u, &reg, 65535), UPS_OK);
    assert_int_equal(fake_driver_writes, 2);
}

/* --- Bitfield validation --- */

static void test_bitfield_strict_listed_accepted(void **state)
{
    ups_t *u = *state;
    ups_config_reg_t reg = {
        .name = "x", .writable = 1, .type = UPS_CFG_BITFIELD,
        .meta.bitfield = { test_opts, 3, 1 /* strict */ },
    };
    assert_int_equal(ups_config_write(u, &reg, 0x01), UPS_OK);
    assert_int_equal(ups_config_write(u, &reg, 0x02), UPS_OK);
    assert_int_equal(ups_config_write(u, &reg, 0x10), UPS_OK);
    assert_int_equal(fake_driver_writes, 3);
}

static void test_bitfield_strict_unlisted_rejected(void **state)
{
    ups_t *u = *state;
    ups_config_reg_t reg = {
        .name = "x", .writable = 1, .type = UPS_CFG_BITFIELD,
        .meta.bitfield = { test_opts, 3, 1 /* strict */ },
    };
    /* 0x04 is not in opts[] (alpha=1, beta=2, gamma=16) */
    assert_int_equal(ups_config_write(u, &reg, 0x04), UPS_ERR_INVALID_VALUE);
    /* 0x03 (alpha|beta) is also not a single listed value */
    assert_int_equal(ups_config_write(u, &reg, 0x03), UPS_ERR_INVALID_VALUE);
    assert_int_equal(fake_driver_writes, 0);
}

static void test_bitfield_non_strict_passes_anything(void **state)
{
    ups_t *u = *state;
    ups_config_reg_t reg = {
        .name = "x", .writable = 1, .type = UPS_CFG_BITFIELD,
        .meta.bitfield = { test_opts, 3, 0 /* not strict */ },
    };
    assert_int_equal(ups_config_write(u, &reg, 0x04), UPS_OK);
    assert_int_equal(ups_config_write(u, &reg, 0xFFFF), UPS_OK);
    assert_int_equal(fake_driver_writes, 2);
}

/* --- Non-validation guards still take precedence --- */

static void test_readonly_returns_not_supported(void **state)
{
    ups_t *u = *state;
    ups_config_reg_t reg = {
        .name = "x", .writable = 0, .type = UPS_CFG_SCALAR,
        .meta.scalar = { .min = 110, .max = 150 },
    };
    /* Even with an in-range value, RO must reject with NOT_SUPPORTED
     * before validation runs. */
    assert_int_equal(ups_config_write(u, &reg, 130), UPS_ERR_NOT_SUPPORTED);
    assert_int_equal(fake_driver_writes, 0);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(test_scalar_in_range_accepted,        setup, teardown),
        cmocka_unit_test_setup_teardown(test_scalar_below_min_rejected,       setup, teardown),
        cmocka_unit_test_setup_teardown(test_scalar_above_max_rejected,       setup, teardown),
        cmocka_unit_test_setup_teardown(test_scalar_at_boundaries_accepted,   setup, teardown),
        cmocka_unit_test_setup_teardown(test_scalar_zero_zero_unconstrained,  setup, teardown),
        cmocka_unit_test_setup_teardown(test_bitfield_strict_listed_accepted, setup, teardown),
        cmocka_unit_test_setup_teardown(test_bitfield_strict_unlisted_rejected, setup, teardown),
        cmocka_unit_test_setup_teardown(test_bitfield_non_strict_passes_anything, setup, teardown),
        cmocka_unit_test_setup_teardown(test_readonly_returns_not_supported,  setup, teardown),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
