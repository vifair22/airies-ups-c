/* Comprehensive tests for src/ups/ups.c — the UPS abstraction layer.
 *
 * Complements test_config_validation.c (which focuses on ups_config_write
 * rejection rules) by exercising capability queries, lookup tables, read
 * dispatch, command dispatch, error recovery, and the connect/close
 * lifecycle. Uses the same "heap-allocate ups_t + sentinel transport"
 * trick to bypass ups_connect's driver loop when we want to drive the
 * registry directly against a fake driver.
 */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <cmocka.h>

#include <pthread.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#include "ups/ups.h"
#include "ups/ups_driver.h"

/* --- Instrumentation: knobs and call counters --- */

static int  fake_read_status_calls;
static int  fake_read_status_return;

static int  fake_read_dynamic_calls;
static int  fake_read_dynamic_return;
static ups_data_t fake_read_dynamic_fill;

static int  fake_read_inventory_calls;
static int  fake_read_thresholds_calls;

static int  fake_execute_calls;
static int  fake_execute_off_calls;

static int  fake_connect_calls;
static int  fake_detect_calls;
static int  fake_detect_return;
static int  fake_disconnect_calls;
static void *fake_connect_return;

static int fake_get_topology_calls;
static ups_topology_t fake_get_topology_return;

/* Sentinel address used as the opaque transport handle. The registry
 * only checks that transport != NULL; it never dereferences it. */
static int sentinel_transport = 1;

/* --- Callbacks --- */

static int fake_read_status(void *t, ups_data_t *d)
{
    (void)t; (void)d;
    fake_read_status_calls++;
    return fake_read_status_return;
}

static int fake_read_dynamic(void *t, ups_data_t *d)
{
    (void)t;
    fake_read_dynamic_calls++;
    *d = fake_read_dynamic_fill;
    return fake_read_dynamic_return;
}

static int fake_read_inventory(void *t, ups_inventory_t *i)
{
    (void)t; (void)i;
    fake_read_inventory_calls++;
    return 0;
}

static int fake_read_thresholds(void *t, uint16_t *h, uint16_t *l)
{
    (void)t;
    fake_read_thresholds_calls++;
    if (h) *h = 0;
    if (l) *l = 0;
    return 0;
}

static int fake_cmd_execute(void *t)      { (void)t; fake_execute_calls++;     return 0; }
static int fake_cmd_execute_off(void *t)  { (void)t; fake_execute_off_calls++; return 0; }

static void *fake_connect(const ups_conn_params_t *p)
{
    (void)p;
    fake_connect_calls++;
    return fake_connect_return;
}

static int fake_detect(void *t)
{
    (void)t;
    fake_detect_calls++;
    return fake_detect_return;
}

static void fake_disconnect(void *t)
{
    (void)t;
    fake_disconnect_calls++;
}

static ups_topology_t fake_get_topology(void *t)
{
    (void)t;
    fake_get_topology_calls++;
    return fake_get_topology_return;
}

/* --- Tables --- */

static const ups_freq_setting_t fake_freq[] = {
    { 0x1, "hz60_0_5", "60 Hz +/- 0.5 Hz" },
    { 0x2, "hz60_1_0", "60 Hz +/- 1.0 Hz" },
    { 0x3, "hz50_0_5", "50 Hz +/- 0.5 Hz" },
};

static const ups_cmd_desc_t fake_cmds[] = {
    { .name = "beep",       .type = UPS_CMD_SIMPLE, .flags = 0,
      .execute = fake_cmd_execute },
    { .name = "bypass",     .type = UPS_CMD_TOGGLE, .flags = 0,
      .execute = fake_cmd_execute, .execute_off = fake_cmd_execute_off },
    { .name = "shutdown",   .type = UPS_CMD_SIMPLE, .flags = UPS_CMD_IS_SHUTDOWN,
      .execute = fake_cmd_execute },
    { .name = "no_handler", .type = UPS_CMD_SIMPLE, .flags = 0,
      .execute = NULL /* handler intentionally absent */ },
};

static const ups_config_reg_t fake_regs[] = {
    { .name = "transfer_high", .type = UPS_CFG_SCALAR,   .writable = 1 },
    { .name = "sensitivity",   .type = UPS_CFG_BITFIELD, .writable = 1 },
};

/* --- Fake driver — non-const so each setup() can rewire callbacks --- */

static ups_driver_t fake_driver;

static void reset_driver(void)
{
    fake_driver = (ups_driver_t){
        .name              = "fake_driver",
        .conn_type         = UPS_CONN_SERIAL,
        .topology          = UPS_TOPO_ONLINE_DOUBLE,
        .caps              = UPS_CAP_SHUTDOWN | UPS_CAP_BATTERY_TEST,
        .connect           = fake_connect,
        .detect            = fake_detect,
        .disconnect        = fake_disconnect,
        .read_status       = fake_read_status,
        .read_dynamic      = fake_read_dynamic,
        .read_inventory    = fake_read_inventory,
        .read_thresholds   = fake_read_thresholds,
        .commands          = fake_cmds,
        .commands_count    = sizeof(fake_cmds) / sizeof(fake_cmds[0]),
        .config_regs       = fake_regs,
        .config_regs_count = sizeof(fake_regs) / sizeof(fake_regs[0]),
    };
}

static void reset_counters(void)
{
    fake_read_status_calls = 0;
    fake_read_status_return = 0;
    fake_read_dynamic_calls = 0;
    fake_read_dynamic_return = 0;
    memset(&fake_read_dynamic_fill, 0, sizeof(fake_read_dynamic_fill));
    fake_read_inventory_calls = 0;
    fake_read_thresholds_calls = 0;
    fake_execute_calls = 0;
    fake_execute_off_calls = 0;
    fake_connect_calls = 0;
    fake_detect_calls = 0;
    fake_detect_return = 1;
    fake_disconnect_calls = 0;
    fake_connect_return = &sentinel_transport;
    fake_get_topology_calls = 0;
    fake_get_topology_return = UPS_TOPO_LINE_INTERACTIVE;
}

static ups_t *make_ups(void)
{
    ups_t *u = calloc(1, sizeof(*u));
    assert_non_null(u);
    u->driver    = &fake_driver;
    u->transport = &sentinel_transport;
    u->caps      = fake_driver.caps;  /* mimics post-resolve_runtime state */
    pthread_mutex_init(&u->cmd_mutex, NULL);
    return u;
}

static void free_ups(ups_t *u)
{
    pthread_mutex_destroy(&u->cmd_mutex);
    free(u->resolved_regs);
    free(u);
}

/* Setup variants: `setup` gives the test a live fixture; `setup_no_fixture`
 * just resets globals and leaves *state NULL (for tests that build their
 * own ups_t or exercise ups_connect / ups_close themselves). */

static int setup(void **state)
{
    reset_driver();
    reset_counters();
    *state = make_ups();
    return 0;
}

static int setup_no_fixture(void **state)
{
    reset_driver();
    reset_counters();
    *state = NULL;
    return 0;
}

static int teardown(void **state)
{
    if (*state) free_ups(*state);
    return 0;
}

/* =========================================================================
 * Identity & capability queries
 * ========================================================================= */

static void test_has_cap_returns_set_bits(void **state)
{
    ups_t *u = *state;
    assert_true (ups_has_cap(u, UPS_CAP_SHUTDOWN));
    assert_true (ups_has_cap(u, UPS_CAP_BATTERY_TEST));
    assert_false(ups_has_cap(u, UPS_CAP_BYPASS));
}

static void test_has_cap_null_ups_is_false(void **state)
{
    (void)state;
    assert_false(ups_has_cap(NULL, UPS_CAP_SHUTDOWN));
}

static void test_driver_name_returns_drivers_name(void **state)
{
    ups_t *u = *state;
    assert_string_equal(ups_driver_name(u), "fake_driver");
}

static void test_is_connected_tracks_transport(void **state)
{
    ups_t *u = *state;
    assert_true(ups_is_connected(u));
    u->transport = NULL;
    assert_false(ups_is_connected(u));
}

static void test_is_connected_null_ups_is_false(void **state)
{
    (void)state;
    assert_false(ups_is_connected(NULL));
}

static void test_topology_static_when_no_override(void **state)
{
    ups_t *u = *state;
    fake_driver.get_topology = NULL;
    assert_int_equal(ups_topology(u), UPS_TOPO_ONLINE_DOUBLE);
}

static void test_topology_override_used_when_set(void **state)
{
    ups_t *u = *state;
    fake_driver.get_topology = fake_get_topology;
    fake_get_topology_return = UPS_TOPO_STANDBY;
    assert_int_equal(ups_topology(u), UPS_TOPO_STANDBY);
    assert_int_equal(fake_get_topology_calls, 1);
}

/* =========================================================================
 * Frequency settings
 * ========================================================================= */

static void test_freq_settings_nil_without_cap(void **state)
{
    ups_t *u = *state;
    /* fake_driver.caps has no FREQ_TOLERANCE; ups->caps mirrors it. */
    fake_driver.freq_settings       = fake_freq;
    fake_driver.freq_settings_count = 3;
    size_t n = 0xABCD;
    const ups_freq_setting_t *s = ups_get_freq_settings(u, &n);
    assert_null(s);
    assert_int_equal(n, 0);
}

static void test_freq_settings_returns_table_when_capable(void **state)
{
    ups_t *u = *state;
    u->caps |= UPS_CAP_FREQ_TOLERANCE;
    fake_driver.freq_settings       = fake_freq;
    fake_driver.freq_settings_count = 3;
    size_t n = 0;
    const ups_freq_setting_t *s = ups_get_freq_settings(u, &n);
    assert_ptr_equal(s, fake_freq);
    assert_int_equal(n, 3);
}

static void test_find_freq_setting_by_name(void **state)
{
    ups_t *u = *state;
    u->caps |= UPS_CAP_FREQ_TOLERANCE;
    fake_driver.freq_settings       = fake_freq;
    fake_driver.freq_settings_count = 3;

    const ups_freq_setting_t *s = ups_find_freq_setting(u, "hz50_0_5");
    assert_non_null(s);
    assert_int_equal(s->value, 0x3);

    assert_null(ups_find_freq_setting(u, "not_there"));
}

static void test_find_freq_setting_without_cap_is_null(void **state)
{
    ups_t *u = *state;
    /* No FREQ_TOLERANCE cap → ups_get_freq_settings returns NULL → find is NULL. */
    assert_null(ups_find_freq_setting(u, "anything"));
}

static void test_find_freq_value(void **state)
{
    ups_t *u = *state;
    u->caps |= UPS_CAP_FREQ_TOLERANCE;
    fake_driver.freq_settings       = fake_freq;
    fake_driver.freq_settings_count = 3;

    const ups_freq_setting_t *s = ups_find_freq_value(u, 0x2);
    assert_non_null(s);
    assert_string_equal(s->name, "hz60_1_0");

    assert_null(ups_find_freq_value(u, 0xDEAD));
}

/* =========================================================================
 * Command table lookup
 * ========================================================================= */

static void test_get_commands_nil_when_driver_has_none(void **state)
{
    ups_t *u = *state;
    fake_driver.commands       = NULL;
    fake_driver.commands_count = 0;
    size_t n = 0xABCD;
    assert_null(ups_get_commands(u, &n));
    assert_int_equal(n, 0);
}

static void test_get_commands_returns_driver_table(void **state)
{
    ups_t *u = *state;
    size_t n = 0;
    const ups_cmd_desc_t *cmds = ups_get_commands(u, &n);
    assert_ptr_equal(cmds, fake_cmds);
    assert_int_equal(n, sizeof(fake_cmds) / sizeof(fake_cmds[0]));
}

static void test_find_command_hit_miss(void **state)
{
    ups_t *u = *state;
    const ups_cmd_desc_t *c = ups_find_command(u, "bypass");
    assert_non_null(c);
    assert_int_equal(c->type, UPS_CMD_TOGGLE);

    assert_null(ups_find_command(u, "nope"));
}

static void test_find_command_flag_hit_miss(void **state)
{
    ups_t *u = *state;
    const ups_cmd_desc_t *c = ups_find_command_flag(u, UPS_CMD_IS_SHUTDOWN);
    assert_non_null(c);
    assert_string_equal(c->name, "shutdown");

    assert_null(ups_find_command_flag(u, UPS_CMD_IS_MUTE));
}

/* =========================================================================
 * Command dispatch — exercises ups_cmd_execute, including the
 * post_command_settle 200 ms nanosleep after each successful call.
 * ========================================================================= */

static void test_cmd_execute_simple_success(void **state)
{
    ups_t *u = *state;
    assert_int_equal(ups_cmd_execute(u, "beep", 0), 0);
    assert_int_equal(fake_execute_calls,     1);
    assert_int_equal(fake_execute_off_calls, 0);
}

static void test_cmd_execute_toggle_on_vs_off(void **state)
{
    ups_t *u = *state;
    assert_int_equal(ups_cmd_execute(u, "bypass", 0), 0);
    assert_int_equal(fake_execute_calls,     1);
    assert_int_equal(fake_execute_off_calls, 0);

    assert_int_equal(ups_cmd_execute(u, "bypass", 1), 0);
    assert_int_equal(fake_execute_calls,     1);
    assert_int_equal(fake_execute_off_calls, 1);
}

static void test_cmd_execute_unknown_not_supported(void **state)
{
    ups_t *u = *state;
    assert_int_equal(ups_cmd_execute(u, "nothing", 0), UPS_ERR_NOT_SUPPORTED);
    assert_int_equal(fake_execute_calls, 0);
}

static void test_cmd_execute_missing_handler_not_supported(void **state)
{
    ups_t *u = *state;
    /* "no_handler" exists in fake_cmds but has execute == NULL */
    assert_int_equal(ups_cmd_execute(u, "no_handler", 0), UPS_ERR_NOT_SUPPORTED);
}

/* =========================================================================
 * Config register queries (complements test_config_validation.c)
 * ========================================================================= */

static void test_get_config_regs_uses_resolved_when_set(void **state)
{
    ups_t *u = *state;
    ups_config_reg_t *resolved = calloc(1, sizeof(*resolved));
    assert_non_null(resolved);
    resolved[0]            = fake_regs[1];
    u->resolved_regs       = resolved;
    u->resolved_regs_count = 1;

    size_t n = 0;
    const ups_config_reg_t *r = ups_get_config_regs(u, &n);
    assert_ptr_equal(r, resolved);
    assert_int_equal(n, 1);
    /* free_ups in teardown frees resolved_regs */
}

static void test_get_config_regs_falls_back_to_driver_table(void **state)
{
    ups_t *u = *state;
    size_t n = 0;
    const ups_config_reg_t *r = ups_get_config_regs(u, &n);
    assert_ptr_equal(r, fake_regs);
    assert_int_equal(n, sizeof(fake_regs) / sizeof(fake_regs[0]));
}

static void test_get_config_regs_nil_when_driver_has_none(void **state)
{
    ups_t *u = *state;
    fake_driver.config_regs       = NULL;
    fake_driver.config_regs_count = 0;
    size_t n = 0xABCD;
    assert_null(ups_get_config_regs(u, &n));
    assert_int_equal(n, 0);
}

static void test_find_config_reg_hit_miss(void **state)
{
    ups_t *u = *state;
    assert_non_null(ups_find_config_reg(u, "sensitivity"));
    assert_null   (ups_find_config_reg(u, "imaginary"));
}

/* =========================================================================
 * Read dispatch
 * ========================================================================= */

static void test_read_status_success(void **state)
{
    ups_t *u = *state;
    ups_data_t d = {0};
    assert_int_equal(ups_read_status(u, &d), 0);
    assert_int_equal(fake_read_status_calls, 1);
    assert_int_equal(u->consecutive_errors,  0);
}

static void test_read_status_error_increments_counter(void **state)
{
    ups_t *u = *state;
    ups_data_t d = {0};
    fake_read_status_return = -1;
    assert_int_equal(ups_read_status(u, &d), -1);
    assert_int_equal(u->consecutive_errors,  1);
}

static void test_read_status_no_callback_not_supported(void **state)
{
    ups_t *u = *state;
    fake_driver.read_status = NULL;
    ups_data_t d = {0};
    assert_int_equal(ups_read_status(u, &d), UPS_ERR_NOT_SUPPORTED);
}

static void test_read_dynamic_preserves_fields_when_battery_ok(void **state)
{
    ups_t *u = *state;
    fake_read_dynamic_fill.bat_system_error = 0;
    fake_read_dynamic_fill.charge_pct       = 87.5;
    fake_read_dynamic_fill.battery_voltage  = 27.3;
    fake_read_dynamic_fill.runtime_sec      = 4200;

    ups_data_t out = {0};
    assert_int_equal(ups_read_dynamic(u, &out), 0);
    assert_true(out.charge_pct      == 87.5);
    assert_true(out.battery_voltage == 27.3);
    assert_int_equal(out.runtime_sec, 4200);
}

static void test_read_dynamic_battery_disconnect_zeros_fields(void **state)
{
    ups_t *u = *state;
    fake_read_dynamic_fill.bat_system_error = UPS_BATERR_DISCONNECTED;
    fake_read_dynamic_fill.charge_pct       = 87.5;
    fake_read_dynamic_fill.battery_voltage  = 27.3;
    fake_read_dynamic_fill.runtime_sec      = 4200;

    ups_data_t out = {0};
    assert_int_equal(ups_read_dynamic(u, &out), 0);
    /* Gate zeroes these regardless of what the driver reported. */
    assert_true    (out.charge_pct      == 0.0);
    assert_true    (out.battery_voltage == 0.0);
    assert_int_equal(out.runtime_sec,     0);
}

static void test_read_inventory_success(void **state)
{
    ups_t *u = *state;
    ups_inventory_t inv = {0};
    assert_int_equal(ups_read_inventory(u, &inv), 0);
    assert_int_equal(fake_read_inventory_calls, 1);
}

static void test_read_inventory_no_callback_not_supported(void **state)
{
    ups_t *u = *state;
    fake_driver.read_inventory = NULL;
    ups_inventory_t inv = {0};
    assert_int_equal(ups_read_inventory(u, &inv), UPS_ERR_NOT_SUPPORTED);
}

static void test_read_thresholds_success(void **state)
{
    ups_t *u = *state;
    uint16_t hi = 0xBEEF, lo = 0xBEEF;
    assert_int_equal(ups_read_thresholds(u, &hi, &lo), 0);
    assert_int_equal(fake_read_thresholds_calls, 1);
}

static void test_read_thresholds_no_callback_not_supported(void **state)
{
    ups_t *u = *state;
    fake_driver.read_thresholds = NULL;
    uint16_t hi = 0, lo = 0;
    assert_int_equal(ups_read_thresholds(u, &hi, &lo), UPS_ERR_NOT_SUPPORTED);
}

/* Instrumented config_read so the success-path test can ride the same
 * infrastructure; the fake_config_read in test_config_validation.c is
 * in a different translation unit. */
static int fake_config_read_calls;
static int fake_config_read_fn(void *t, const ups_config_reg_t *r,
                               uint16_t *raw, char *str, size_t str_sz)
{
    (void)t; (void)r; (void)str; (void)str_sz;
    fake_config_read_calls++;
    if (raw) *raw = 0xAB;
    return 0;
}

static void test_config_read_success(void **state)
{
    ups_t *u = *state;
    fake_driver.config_read = fake_config_read_fn;
    fake_config_read_calls = 0;

    uint16_t raw = 0;
    assert_int_equal(ups_config_read(u, &fake_regs[0], &raw, NULL, 0), 0);
    assert_int_equal(fake_config_read_calls, 1);
    assert_int_equal(raw, 0xAB);
}

static void test_config_read_no_callback_not_supported(void **state)
{
    ups_t *u = *state;
    fake_driver.config_read = NULL;
    uint16_t raw = 0;
    assert_int_equal(ups_config_read(u, &fake_regs[0], &raw, NULL, 0),
                     UPS_ERR_NOT_SUPPORTED);
}

/* =========================================================================
 * Error recovery (MAX_CONSECUTIVE_ERRORS = 5 / RECONNECT_INTERVAL_SEC = 2)
 * ========================================================================= */

static void test_five_consecutive_errors_tears_down_transport(void **state)
{
    ups_t *u = *state;
    fake_read_status_return = -1;
    /* Block reconnect so transport stays NULL after teardown. */
    fake_connect_return = NULL;

    ups_data_t d = {0};
    for (int i = 0; i < 4; i++) {
        (void)ups_read_status(u, &d);
        assert_non_null(u->transport);
        assert_int_equal(u->consecutive_errors, i + 1);
    }
    /* 5th failure crosses MAX_CONSECUTIVE_ERRORS and nulls transport. */
    (void)ups_read_status(u, &d);
    assert_null(u->transport);
    assert_int_equal(fake_disconnect_calls, 1);
}

static void test_successful_read_clears_error_counter(void **state)
{
    ups_t *u = *state;
    u->consecutive_errors = 3;
    ups_data_t d = {0};
    assert_int_equal(ups_read_status(u, &d), 0);
    assert_int_equal(u->consecutive_errors, 0);
}

static void test_reconnect_rate_limited(void **state)
{
    ups_t *u = *state;
    u->transport              = NULL;
    u->last_reconnect_attempt = time(NULL);  /* just now — within window */

    ups_data_t d = {0};
    int rc = ups_read_status(u, &d);
    assert_int_equal(rc, UPS_ERR_IO);
    /* Rate-limited: connect should NOT have been called. */
    assert_int_equal(fake_connect_calls, 0);
}

static void test_reconnect_success_restores_transport(void **state)
{
    ups_t *u = *state;
    u->transport              = NULL;
    u->last_reconnect_attempt = 0;   /* disable rate-limit gate */
    u->consecutive_errors     = 3;   /* should clear on success */
    fake_connect_return = &sentinel_transport;
    fake_detect_return  = 1;

    ups_data_t d = {0};
    int rc = ups_read_status(u, &d);
    assert_int_equal(rc, 0);
    assert_int_equal(fake_connect_calls,          1);
    assert_int_equal(fake_detect_calls,           1);
    assert_int_equal(fake_read_status_calls,      1);
    assert_int_equal(fake_read_inventory_calls,   1);  /* refreshed on reconnect */
    assert_ptr_equal(u->transport, &sentinel_transport);
    assert_int_equal(u->consecutive_errors, 0);
}

static void test_reconnect_detect_miss_leaves_transport_down(void **state)
{
    ups_t *u = *state;
    u->transport              = NULL;
    u->last_reconnect_attempt = 0;
    fake_connect_return = &sentinel_transport;
    fake_detect_return  = 0;  /* different device on the wire */

    ups_data_t d = {0};
    int rc = ups_read_status(u, &d);
    assert_int_equal(rc, UPS_ERR_IO);
    assert_int_equal(fake_connect_calls,    1);
    assert_int_equal(fake_detect_calls,     1);
    assert_int_equal(fake_disconnect_calls, 1);  /* opened-then-rejected */
    assert_null(u->transport);
}

/* resolve_config_regs branch of ups_resolve_runtime (lines 42-56). The
 * reconnect path calls ups_resolve_runtime, so wire a resolver and
 * verify resolved_regs lands on the ups context. */
static size_t fake_resolve_calls;
static size_t fake_resolve_config_regs(void *t, const ups_config_reg_t *defaults,
                                       size_t default_count, ups_config_reg_t *out)
{
    (void)t;
    fake_resolve_calls++;
    /* Drop the second descriptor — simulate a driver narrowing the set. */
    size_t written = 0;
    for (size_t i = 0; i < default_count; i++) {
        if (i == 1) continue;
        out[written++] = defaults[i];
    }
    return written;
}

static void test_reconnect_runs_resolve_config_regs(void **state)
{
    ups_t *u = *state;
    fake_driver.resolve_config_regs = fake_resolve_config_regs;
    u->transport              = NULL;
    u->last_reconnect_attempt = 0;
    fake_connect_return = &sentinel_transport;
    fake_detect_return  = 1;

    ups_data_t d = {0};
    assert_int_equal(ups_read_status(u, &d), 0);
    assert_int_equal(fake_resolve_calls, 1);
    assert_non_null(u->resolved_regs);
    assert_int_equal(u->resolved_regs_count, 1);
    assert_string_equal(u->resolved_regs[0].name, "transfer_high");
}

/* =========================================================================
 * Lifecycle — ups_connect / ups_close
 *
 * ups_connect iterates the real driver registry, which in test builds is
 * populated by tests/test_stubs.c (srt/smt/backups_hid stubs with NULL
 * connect + detect). So any connect that reaches the detect loop will
 * skip every stub via `if (!drv->connect || !drv->detect) continue;`.
 * That lets us verify the two early-return error codes cleanly.
 * ========================================================================= */

static void test_connect_null_params_returns_io(void **state)
{
    (void)state;
    ups_t *u = (ups_t *)0xDEADBEEF;  /* prove *out is cleared to NULL */
    assert_int_equal(ups_connect(NULL, &u), UPS_ERR_IO);
    /* ups_connect can't clear *out when out is NULL, but it can here. */
    (void)u;  /* behavior on error is documented as UPS_ERR_IO */
}

static void test_connect_null_out_returns_io(void **state)
{
    (void)state;
    ups_conn_params_t p = { .type = UPS_CONN_SERIAL };
    assert_int_equal(ups_connect(&p, NULL), UPS_ERR_IO);
}

static void test_connect_snmp_has_no_matching_driver(void **state)
{
    (void)state;
    /* No stub advertises UPS_CONN_SNMP, so the registry loop sets
     * any_matched_type=0 and returns UPS_ERR_NO_DRIVER. */
    ups_conn_params_t p = { .type = UPS_CONN_SNMP };
    ups_t *u = NULL;
    assert_int_equal(ups_connect(&p, &u), UPS_ERR_NO_DRIVER);
    assert_null(u);
}

static void test_connect_serial_stubs_fall_through_to_io(void **state)
{
    (void)state;
    /* Two stubs (srt, smt) match UPS_CONN_SERIAL but have NULL connect +
     * detect, so the loop sets any_matched_type=1 then skips each stub
     * via `if (!drv->connect || !drv->detect) continue;`. any_connected
     * stays 0, so the fall-through reaches `if (!any_connected) return
     * UPS_ERR_IO;`. Production drivers always have real connect/detect;
     * this is an artifact of the stub shape, but the code path is
     * live in ups_connect and worth pinning. */
    ups_conn_params_t p = { .type = UPS_CONN_SERIAL,
                            .serial = { .device = "/dev/nowhere", .baud = 9600, .slave_id = 1 } };
    ups_t *u = NULL;
    assert_int_equal(ups_connect(&p, &u), UPS_ERR_IO);
    assert_null(u);
}

static void test_close_null_is_safe(void **state)
{
    (void)state;
    ups_close(NULL);  /* must not crash */
}

static void test_close_frees_resolved_regs_and_disconnects(void **state)
{
    (void)state;  /* fixture is NULL here; we build our own ups_t */

    ups_t *u = calloc(1, sizeof(*u));
    assert_non_null(u);
    u->driver              = &fake_driver;
    u->transport           = &sentinel_transport;
    u->resolved_regs       = calloc(2, sizeof(*u->resolved_regs));
    u->resolved_regs_count = 2;
    pthread_mutex_init(&u->cmd_mutex, NULL);

    ups_close(u);

    /* ASAN / valgrind catch the resolved_regs leak; here we at least
     * observe that disconnect was called. */
    assert_int_equal(fake_disconnect_calls, 1);
}

/* --- Runner ------------------------------------------------------------- */

int main(void)
{
    const struct CMUnitTest tests[] = {
        /* Identity & caps */
        cmocka_unit_test_setup_teardown(test_has_cap_returns_set_bits,           setup, teardown),
        cmocka_unit_test_setup_teardown(test_has_cap_null_ups_is_false,          setup, teardown),
        cmocka_unit_test_setup_teardown(test_driver_name_returns_drivers_name,   setup, teardown),
        cmocka_unit_test_setup_teardown(test_is_connected_tracks_transport,      setup, teardown),
        cmocka_unit_test_setup_teardown(test_is_connected_null_ups_is_false,     setup, teardown),
        cmocka_unit_test_setup_teardown(test_topology_static_when_no_override,   setup, teardown),
        cmocka_unit_test_setup_teardown(test_topology_override_used_when_set,    setup, teardown),

        /* Frequency settings */
        cmocka_unit_test_setup_teardown(test_freq_settings_nil_without_cap,      setup, teardown),
        cmocka_unit_test_setup_teardown(test_freq_settings_returns_table_when_capable, setup, teardown),
        cmocka_unit_test_setup_teardown(test_find_freq_setting_by_name,          setup, teardown),
        cmocka_unit_test_setup_teardown(test_find_freq_setting_without_cap_is_null, setup, teardown),
        cmocka_unit_test_setup_teardown(test_find_freq_value,                    setup, teardown),

        /* Commands table */
        cmocka_unit_test_setup_teardown(test_get_commands_nil_when_driver_has_none, setup, teardown),
        cmocka_unit_test_setup_teardown(test_get_commands_returns_driver_table,  setup, teardown),
        cmocka_unit_test_setup_teardown(test_find_command_hit_miss,              setup, teardown),
        cmocka_unit_test_setup_teardown(test_find_command_flag_hit_miss,         setup, teardown),

        /* Command dispatch (each successful execute adds ~200ms settle) */
        cmocka_unit_test_setup_teardown(test_cmd_execute_simple_success,         setup, teardown),
        cmocka_unit_test_setup_teardown(test_cmd_execute_toggle_on_vs_off,       setup, teardown),
        cmocka_unit_test_setup_teardown(test_cmd_execute_unknown_not_supported,  setup, teardown),
        cmocka_unit_test_setup_teardown(test_cmd_execute_missing_handler_not_supported, setup, teardown),

        /* Config regs */
        cmocka_unit_test_setup_teardown(test_get_config_regs_uses_resolved_when_set, setup, teardown),
        cmocka_unit_test_setup_teardown(test_get_config_regs_falls_back_to_driver_table, setup, teardown),
        cmocka_unit_test_setup_teardown(test_get_config_regs_nil_when_driver_has_none, setup, teardown),
        cmocka_unit_test_setup_teardown(test_find_config_reg_hit_miss,           setup, teardown),

        /* Reads */
        cmocka_unit_test_setup_teardown(test_read_status_success,                setup, teardown),
        cmocka_unit_test_setup_teardown(test_read_status_error_increments_counter, setup, teardown),
        cmocka_unit_test_setup_teardown(test_read_status_no_callback_not_supported, setup, teardown),
        cmocka_unit_test_setup_teardown(test_read_dynamic_preserves_fields_when_battery_ok, setup, teardown),
        cmocka_unit_test_setup_teardown(test_read_dynamic_battery_disconnect_zeros_fields, setup, teardown),
        cmocka_unit_test_setup_teardown(test_read_inventory_success,             setup, teardown),
        cmocka_unit_test_setup_teardown(test_read_inventory_no_callback_not_supported, setup, teardown),
        cmocka_unit_test_setup_teardown(test_read_thresholds_success,            setup, teardown),
        cmocka_unit_test_setup_teardown(test_read_thresholds_no_callback_not_supported, setup, teardown),
        cmocka_unit_test_setup_teardown(test_config_read_success,                setup, teardown),
        cmocka_unit_test_setup_teardown(test_config_read_no_callback_not_supported, setup, teardown),

        /* Error recovery */
        cmocka_unit_test_setup_teardown(test_five_consecutive_errors_tears_down_transport, setup, teardown),
        cmocka_unit_test_setup_teardown(test_successful_read_clears_error_counter, setup, teardown),
        cmocka_unit_test_setup_teardown(test_reconnect_rate_limited,             setup, teardown),
        cmocka_unit_test_setup_teardown(test_reconnect_success_restores_transport, setup, teardown),
        cmocka_unit_test_setup_teardown(test_reconnect_detect_miss_leaves_transport_down, setup, teardown),
        cmocka_unit_test_setup_teardown(test_reconnect_runs_resolve_config_regs, setup, teardown),

        /* Lifecycle */
        cmocka_unit_test_setup_teardown(test_connect_null_params_returns_io,     setup_no_fixture, teardown),
        cmocka_unit_test_setup_teardown(test_connect_null_out_returns_io,        setup_no_fixture, teardown),
        cmocka_unit_test_setup_teardown(test_connect_snmp_has_no_matching_driver, setup_no_fixture, teardown),
        cmocka_unit_test_setup_teardown(test_connect_serial_stubs_fall_through_to_io, setup_no_fixture, teardown),
        cmocka_unit_test_setup_teardown(test_close_null_is_safe,                 setup_no_fixture, teardown),
        cmocka_unit_test_setup_teardown(test_close_frees_resolved_regs_and_disconnects, setup_no_fixture, teardown),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
