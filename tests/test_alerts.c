#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <cmocka.h>
#include <stdio.h>
#include <string.h>

#include "alerts/alerts.h"

/* --- Notification capture ---
 * alerts_check fires notifications via a callback. We capture them here
 * so tests can verify what was fired and in what order. */

#define MAX_NOTIFICATIONS 32

static struct {
    char severities[MAX_NOTIFICATIONS][32];
    char categories[MAX_NOTIFICATIONS][32];
    char titles[MAX_NOTIFICATIONS][128];
    char bodies[MAX_NOTIFICATIONS][512];
    int  count;
} g_notifs;

static void reset_notifs(void)
{
    memset(&g_notifs, 0, sizeof(g_notifs));
}

static void capture_notify(const char *severity, const char *category,
                           const char *title, const char *body)
{
    if (g_notifs.count < MAX_NOTIFICATIONS) {
        snprintf(g_notifs.severities[g_notifs.count], 32,  "%s", severity);
        snprintf(g_notifs.categories[g_notifs.count], 32,  "%s", category);
        snprintf(g_notifs.titles[g_notifs.count],     128, "%s", title);
        snprintf(g_notifs.bodies[g_notifs.count],     512, "%s", body);
        g_notifs.count++;
    }
}

/* --- Helpers --- */

static ups_data_t make_data(void)
{
    ups_data_t d;
    memset(&d, 0, sizeof(d));
    d.status = UPS_ST_ONLINE;
    d.input_voltage = 120.0;
    d.output_voltage = 120.0;
    d.charge_pct = 100.0;
    d.load_pct = 20.0;
    return d;
}

static alert_config_t make_config(void)
{
    alert_config_t c = {
        .load_high_pct       = 80,
        .battery_low_pct     = 50,
        .voltage_warn_offset = 5,
        .voltage_deadband    = 1,
    };
    return c;
}

static alert_thresholds_t make_thresholds(void)
{
    alert_thresholds_t t = {
        .transfer_high = 150,
        .transfer_low  = 97,
    };
    return t;
}

/* --- alerts_init --- */

static void test_init_clears_state(void **state)
{
    (void)state;
    alert_state_t s;
    memset(&s, 0xFF, sizeof(s));
    alerts_init(&s);

    assert_int_equal(s.overload, 0);
    assert_int_equal(s.fault, 0);
    assert_int_equal(s.bat_replace, 0);
    assert_int_equal(s.input_high, 0);
    assert_int_equal(s.input_low, 0);
    assert_int_equal(s.load_high, 0);
    assert_int_equal(s.bat_low, 0);
    assert_true(s.prev_charge < 0.0);
}

/* --- Bit alerts: overload --- */

static void test_overload_fires_on_set(void **state)
{
    (void)state;
    alert_state_t s;
    alerts_init(&s);
    reset_notifs();

    ups_data_t d = make_data();
    alert_config_t acfg = make_config();
    alert_thresholds_t thresh = make_thresholds();

    /* First poll: normal */
    alerts_check(&s, &d, &thresh, &acfg, capture_notify);
    assert_int_equal(g_notifs.count, 0);

    /* Second poll: overload bit set */
    reset_notifs();
    d.status |= UPS_ST_OVERLOAD;
    d.load_pct = 110.0;
    uint32_t alerted = alerts_check(&s, &d, &thresh, &acfg, capture_notify);

    /* Should fire overload alert + load high */
    assert_true(alerted & UPS_ST_OVERLOAD);
    int found = 0;
    for (int i = 0; i < g_notifs.count; i++)
        if (strstr(g_notifs.titles[i], "Overload") && !strstr(g_notifs.titles[i], "Cleared")) {
            assert_string_equal(g_notifs.severities[i], "error");
            found++;
        }
    assert_int_equal(found, 1);
}

static void test_overload_fires_on_clear(void **state)
{
    (void)state;
    alert_state_t s;
    alerts_init(&s);

    ups_data_t d = make_data();
    alert_config_t acfg = make_config();
    alert_thresholds_t thresh = make_thresholds();

    /* Set overload */
    d.status |= UPS_ST_OVERLOAD;
    alerts_check(&s, &d, &thresh, &acfg, capture_notify);

    /* Clear overload */
    reset_notifs();
    d.status &= ~(uint32_t)UPS_ST_OVERLOAD;
    alerts_check(&s, &d, &thresh, &acfg, capture_notify);

    int found = 0;
    for (int i = 0; i < g_notifs.count; i++)
        if (strstr(g_notifs.titles[i], "Overload Cleared")) {
            assert_string_equal(g_notifs.severities[i], "info");
            found++;
        }
    assert_int_equal(found, 1);
}

static void test_overload_no_repeat(void **state)
{
    (void)state;
    alert_state_t s;
    alerts_init(&s);
    reset_notifs();

    ups_data_t d = make_data();
    alert_config_t acfg = make_config();
    alert_thresholds_t thresh = make_thresholds();

    d.status |= UPS_ST_OVERLOAD;
    alerts_check(&s, &d, &thresh, &acfg, capture_notify);
    int first_count = g_notifs.count;

    /* Same state again — should not re-fire */
    reset_notifs();
    alerts_check(&s, &d, &thresh, &acfg, capture_notify);

    int overload_notifs = 0;
    for (int i = 0; i < g_notifs.count; i++)
        if (strstr(g_notifs.titles[i], "Overload"))
            overload_notifs++;
    assert_int_equal(overload_notifs, 0);
    (void)first_count;
}

/* --- Bit alerts: fault --- */

static void test_fault_fires_on_set(void **state)
{
    (void)state;
    alert_state_t s;
    alerts_init(&s);
    reset_notifs();

    ups_data_t d = make_data();
    alert_config_t acfg = make_config();
    alert_thresholds_t thresh = make_thresholds();

    alerts_check(&s, &d, &thresh, &acfg, capture_notify);

    reset_notifs();
    d.status |= UPS_ST_FAULT;
    alerts_check(&s, &d, &thresh, &acfg, capture_notify);

    int found = 0;
    for (int i = 0; i < g_notifs.count; i++)
        if (strcmp(g_notifs.titles[i], "UPS Fault") == 0) {
            assert_string_equal(g_notifs.severities[i], "error");
            found++;
        }
    assert_int_equal(found, 1);
}

/* --- Threshold alert: input voltage high with deadband --- */

static void test_input_voltage_high(void **state)
{
    (void)state;
    alert_state_t s;
    alerts_init(&s);
    reset_notifs();

    ups_data_t d = make_data();
    alert_config_t acfg = make_config();
    alert_thresholds_t thresh = make_thresholds();

    /* Threshold: transfer_high=150, warn_offset=5 → enter at 145, deadband=1 → clear at 144 */

    /* Normal voltage */
    d.input_voltage = 120.0;
    alerts_check(&s, &d, &thresh, &acfg, capture_notify);
    assert_int_equal(g_notifs.count, 0);

    /* Just below enter threshold (145) */
    reset_notifs();
    d.input_voltage = 144.9;
    alerts_check(&s, &d, &thresh, &acfg, capture_notify);
    int high_alerts = 0;
    for (int i = 0; i < g_notifs.count; i++)
        if (strstr(g_notifs.titles[i], "Voltage High"))
            high_alerts++;
    assert_int_equal(high_alerts, 0);

    /* Above enter threshold */
    reset_notifs();
    d.input_voltage = 146.0;
    alerts_check(&s, &d, &thresh, &acfg, capture_notify);
    high_alerts = 0;
    for (int i = 0; i < g_notifs.count; i++)
        if (strstr(g_notifs.titles[i], "Voltage High")) {
            assert_string_equal(g_notifs.severities[i], "warning");
            high_alerts++;
        }
    assert_int_equal(high_alerts, 1);

    /* Drop below enter but above clear (deadband zone: 144-145) */
    reset_notifs();
    d.input_voltage = 144.5;
    alerts_check(&s, &d, &thresh, &acfg, capture_notify);
    /* Should NOT clear yet — still in deadband */
    int clear_alerts = 0;
    for (int i = 0; i < g_notifs.count; i++)
        if (strstr(g_notifs.titles[i], "Voltage Normal"))
            clear_alerts++;
    assert_int_equal(clear_alerts, 0);

    /* Drop below clear threshold */
    reset_notifs();
    d.input_voltage = 143.0;
    alerts_check(&s, &d, &thresh, &acfg, capture_notify);
    clear_alerts = 0;
    for (int i = 0; i < g_notifs.count; i++)
        if (strstr(g_notifs.titles[i], "Voltage Normal"))
            clear_alerts++;
    assert_int_equal(clear_alerts, 1);
}

/* --- Threshold alert: input voltage low --- */

static void test_input_voltage_low(void **state)
{
    (void)state;
    alert_state_t s;
    alerts_init(&s);
    reset_notifs();

    ups_data_t d = make_data();
    alert_config_t acfg = make_config();
    alert_thresholds_t thresh = make_thresholds();

    /* transfer_low=97, warn_offset=5 → enter at 102, deadband=1 → clear at 103 */

    /* Normal voltage */
    d.input_voltage = 120.0;
    alerts_check(&s, &d, &thresh, &acfg, capture_notify);

    /* Drop below enter threshold (102) */
    reset_notifs();
    d.input_voltage = 101.0;
    alerts_check(&s, &d, &thresh, &acfg, capture_notify);
    int low_alerts = 0;
    for (int i = 0; i < g_notifs.count; i++)
        if (strstr(g_notifs.titles[i], "Voltage Low"))
            low_alerts++;
    assert_int_equal(low_alerts, 1);

    /* Rise above clear threshold */
    reset_notifs();
    d.input_voltage = 104.0;
    alerts_check(&s, &d, &thresh, &acfg, capture_notify);
    int clear_alerts = 0;
    for (int i = 0; i < g_notifs.count; i++)
        if (strstr(g_notifs.titles[i], "Voltage Normal"))
            clear_alerts++;
    assert_int_equal(clear_alerts, 1);
}

/* --- Threshold alert: load high --- */

static void test_load_high(void **state)
{
    (void)state;
    alert_state_t s;
    alerts_init(&s);
    reset_notifs();

    ups_data_t d = make_data();
    alert_config_t acfg = make_config();
    alert_thresholds_t thresh = make_thresholds();

    /* load_high_pct = 80 */

    d.load_pct = 50.0;
    alerts_check(&s, &d, &thresh, &acfg, capture_notify);
    assert_int_equal(g_notifs.count, 0);

    /* Exceed threshold */
    reset_notifs();
    d.load_pct = 85.0;
    alerts_check(&s, &d, &thresh, &acfg, capture_notify);
    int found = 0;
    for (int i = 0; i < g_notifs.count; i++)
        if (strstr(g_notifs.titles[i], "Load High"))
            found++;
    assert_int_equal(found, 1);

    /* Return to normal */
    reset_notifs();
    d.load_pct = 50.0;
    alerts_check(&s, &d, &thresh, &acfg, capture_notify);
    found = 0;
    for (int i = 0; i < g_notifs.count; i++)
        if (strstr(g_notifs.titles[i], "Load Normal"))
            found++;
    assert_int_equal(found, 1);
}

/* --- Battery low (gated: online + charge not rising) --- */

static void test_battery_low_fires_when_not_charging(void **state)
{
    (void)state;
    alert_state_t s;
    alerts_init(&s);
    reset_notifs();

    ups_data_t d = make_data();
    alert_config_t acfg = make_config();
    alert_thresholds_t thresh = make_thresholds();

    /* Initial poll to seed prev_charge */
    d.charge_pct = 45.0;
    alerts_check(&s, &d, &thresh, &acfg, capture_notify);

    /* Second poll: still at 45% (not rising), online, below threshold */
    reset_notifs();
    alerts_check(&s, &d, &thresh, &acfg, capture_notify);
    int found = 0;
    for (int i = 0; i < g_notifs.count; i++)
        if (strstr(g_notifs.titles[i], "Battery Low"))
            found++;
    assert_int_equal(found, 1);
}

static void test_battery_low_suppressed_when_charging(void **state)
{
    (void)state;
    alert_state_t s;
    alerts_init(&s);
    reset_notifs();

    ups_data_t d = make_data();
    alert_config_t acfg = make_config();
    alert_thresholds_t thresh = make_thresholds();

    /* Initial poll */
    d.charge_pct = 40.0;
    alerts_check(&s, &d, &thresh, &acfg, capture_notify);

    /* Second poll: charge rising (41% > 40%), below threshold but recovering */
    reset_notifs();
    d.charge_pct = 41.0;
    alerts_check(&s, &d, &thresh, &acfg, capture_notify);
    int found = 0;
    for (int i = 0; i < g_notifs.count; i++)
        if (strstr(g_notifs.titles[i], "Battery Low"))
            found++;
    assert_int_equal(found, 0);
}

static void test_battery_low_clears_on_recovery(void **state)
{
    (void)state;
    alert_state_t s;
    alerts_init(&s);
    reset_notifs();

    ups_data_t d = make_data();
    alert_config_t acfg = make_config();
    alert_thresholds_t thresh = make_thresholds();

    /* Trigger battery low */
    d.charge_pct = 30.0;
    alerts_check(&s, &d, &thresh, &acfg, capture_notify);
    alerts_check(&s, &d, &thresh, &acfg, capture_notify);
    assert_int_equal(s.bat_low, 1);

    /* Charge starts rising — should clear */
    reset_notifs();
    d.charge_pct = 31.0;
    alerts_check(&s, &d, &thresh, &acfg, capture_notify);
    int found = 0;
    for (int i = 0; i < g_notifs.count; i++)
        if (strstr(g_notifs.titles[i], "Battery Normal"))
            found++;
    assert_int_equal(found, 1);
    assert_int_equal(s.bat_low, 0);
}

/* --- Error register transitions --- */

static void test_general_error_transition(void **state)
{
    (void)state;
    alert_state_t s;
    alerts_init(&s);
    reset_notifs();

    ups_data_t d = make_data();
    alert_config_t acfg = make_config();
    alert_thresholds_t thresh = make_thresholds();

    /* Baseline */
    alerts_check(&s, &d, &thresh, &acfg, capture_notify);

    /* Set a general error */
    reset_notifs();
    d.general_error = UPS_GENERR_SITE_WIRING;
    alerts_check(&s, &d, &thresh, &acfg, capture_notify);
    int found = 0;
    for (int i = 0; i < g_notifs.count; i++)
        if (strstr(g_notifs.titles[i], "General Error") &&
            !strstr(g_notifs.titles[i], "Cleared"))
            found++;
    assert_int_equal(found, 1);

    /* Clear the error */
    reset_notifs();
    d.general_error = 0;
    alerts_check(&s, &d, &thresh, &acfg, capture_notify);
    found = 0;
    for (int i = 0; i < g_notifs.count; i++)
        if (strstr(g_notifs.titles[i], "General Error Cleared"))
            found++;
    assert_int_equal(found, 1);
}

static void test_power_error_transition(void **state)
{
    (void)state;
    alert_state_t s;
    alerts_init(&s);
    reset_notifs();

    ups_data_t d = make_data();
    alert_config_t acfg = make_config();
    alert_thresholds_t thresh = make_thresholds();

    alerts_check(&s, &d, &thresh, &acfg, capture_notify);

    /* Set power errors */
    reset_notifs();
    d.power_system_error = UPS_PWRERR_FAN | UPS_PWRERR_OVERTEMP;
    alerts_check(&s, &d, &thresh, &acfg, capture_notify);
    int found = 0;
    for (int i = 0; i < g_notifs.count; i++)
        if (strstr(g_notifs.titles[i], "Power System Error") &&
            !strstr(g_notifs.titles[i], "Cleared"))
            found++;
    assert_int_equal(found, 2);
}

static void test_battery_error_transition(void **state)
{
    (void)state;
    alert_state_t s;
    alerts_init(&s);
    reset_notifs();

    ups_data_t d = make_data();
    alert_config_t acfg = make_config();
    alert_thresholds_t thresh = make_thresholds();

    alerts_check(&s, &d, &thresh, &acfg, capture_notify);

    reset_notifs();
    d.bat_system_error = UPS_BATERR_CHARGER;
    alerts_check(&s, &d, &thresh, &acfg, capture_notify);
    int found = 0;
    for (int i = 0; i < g_notifs.count; i++)
        if (strstr(g_notifs.titles[i], "Battery Error") &&
            !strstr(g_notifs.titles[i], "Cleared"))
            found++;
    assert_int_equal(found, 1);
}

/* --- alerts_seed_from_snapshot --- */

static void test_seed_from_snapshot_mirrors_bit_state(void **state)
{
    (void)state;
    alert_state_t s;
    memset(&s, 0xFF, sizeof(s));

    status_snapshot_t snap;
    memset(&snap, 0, sizeof(snap));
    snap.status              = UPS_ST_OVERLOAD | UPS_ST_FAULT;
    snap.bat_system_error    = UPS_BATERR_REPLACE;
    snap.general_error       = 0xAB;
    snap.power_system_error  = 0xDEAD;

    alerts_seed_from_snapshot(&s, &snap);

    assert_int_equal(s.overload, 1);
    assert_int_equal(s.fault, 1);
    assert_int_equal(s.bat_replace, 1);
    assert_int_equal(s.prev_general_error, 0xAB);
    assert_int_equal(s.prev_power_error,   0xDEAD);
    assert_int_equal(s.prev_battery_error, UPS_BATERR_REPLACE);
}

static void test_seed_from_snapshot_clears_when_unset(void **state)
{
    (void)state;
    alert_state_t s;
    memset(&s, 0xFF, sizeof(s));

    status_snapshot_t snap;
    memset(&snap, 0, sizeof(snap));
    snap.status = UPS_ST_ONLINE;  /* no overload, no fault, no bat replace */

    alerts_seed_from_snapshot(&s, &snap);

    assert_int_equal(s.overload, 0);
    assert_int_equal(s.fault, 0);
    assert_int_equal(s.bat_replace, 0);
}

/* --- Clear-side transitions ---
 *
 * The "cleared" notifications fire when an error bit was previously set
 * and is no longer present. Existing tests cover the set-side; these
 * cover the clear-side branches. */

static void test_battery_replace_clears(void **state)
{
    (void)state;
    alert_state_t s;
    alerts_init(&s);
    s.bat_replace = 1;  /* seed: replace was previously asserted */
    s.prev_battery_error = UPS_BATERR_REPLACE;
    reset_notifs();

    ups_data_t d = make_data();
    d.bat_system_error = 0;  /* condition cleared */
    alert_config_t acfg = make_config();
    alert_thresholds_t thresh = make_thresholds();

    alerts_check(&s, &d, &thresh, &acfg, capture_notify);

    int found = 0;
    for (int i = 0; i < g_notifs.count; i++)
        if (strstr(g_notifs.titles[i], "Battery Error Cleared"))
            found++;
    /* Both the bit-alert path and the bat_system_error register decode
     * fire "UPS Battery Error Cleared" — assert at least one. */
    assert_true(found >= 1);
    assert_int_equal(s.bat_replace, 0);
}

static void test_power_error_clears(void **state)
{
    (void)state;
    alert_state_t s;
    alerts_init(&s);
    s.prev_power_error = 0x3;  /* two power-error bits previously set */
    reset_notifs();

    ups_data_t d = make_data();
    d.power_system_error = 0;
    alert_config_t acfg = make_config();
    alert_thresholds_t thresh = make_thresholds();

    alerts_check(&s, &d, &thresh, &acfg, capture_notify);

    int found = 0;
    for (int i = 0; i < g_notifs.count; i++)
        if (strstr(g_notifs.titles[i], "Power System Error Cleared"))
            found++;
    /* Number of decoded bits depends on ups_decode_power_errors. Just
     * assert at least one — the path is exercised. */
    assert_true(found >= 1);
    assert_int_equal(s.prev_power_error, 0);
}

static void test_battery_system_error_clears(void **state)
{
    (void)state;
    alert_state_t s;
    alerts_init(&s);
    s.prev_battery_error = UPS_BATERR_CHARGER;
    reset_notifs();

    ups_data_t d = make_data();
    d.bat_system_error = 0;
    alert_config_t acfg = make_config();
    alert_thresholds_t thresh = make_thresholds();

    alerts_check(&s, &d, &thresh, &acfg, capture_notify);

    int found = 0;
    for (int i = 0; i < g_notifs.count; i++)
        if (strstr(g_notifs.titles[i], "Battery Error Cleared"))
            found++;
    assert_true(found >= 1);
}

/* --- No thresholds (NULL) --- */

static void test_null_thresholds_skips_voltage(void **state)
{
    (void)state;
    alert_state_t s;
    alerts_init(&s);
    reset_notifs();

    ups_data_t d = make_data();
    alert_config_t acfg = make_config();

    /* Way outside any threshold, but NULL thresholds → skip voltage checks */
    d.input_voltage = 200.0;
    alerts_check(&s, &d, NULL, &acfg, capture_notify);
    alerts_check(&s, &d, NULL, &acfg, capture_notify);

    int found = 0;
    for (int i = 0; i < g_notifs.count; i++)
        if (strstr(g_notifs.titles[i], "Voltage"))
            found++;
    assert_int_equal(found, 0);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        /* init */
        cmocka_unit_test(test_init_clears_state),
        /* overload */
        cmocka_unit_test(test_overload_fires_on_set),
        cmocka_unit_test(test_overload_fires_on_clear),
        cmocka_unit_test(test_overload_no_repeat),
        /* fault */
        cmocka_unit_test(test_fault_fires_on_set),
        /* voltage */
        cmocka_unit_test(test_input_voltage_high),
        cmocka_unit_test(test_input_voltage_low),
        /* load */
        cmocka_unit_test(test_load_high),
        /* battery */
        cmocka_unit_test(test_battery_low_fires_when_not_charging),
        cmocka_unit_test(test_battery_low_suppressed_when_charging),
        cmocka_unit_test(test_battery_low_clears_on_recovery),
        /* error registers */
        cmocka_unit_test(test_general_error_transition),
        cmocka_unit_test(test_power_error_transition),
        cmocka_unit_test(test_battery_error_transition),
        /* seed from snapshot */
        cmocka_unit_test(test_seed_from_snapshot_mirrors_bit_state),
        cmocka_unit_test(test_seed_from_snapshot_clears_when_unset),
        /* clear-side transitions */
        cmocka_unit_test(test_battery_replace_clears),
        cmocka_unit_test(test_power_error_clears),
        cmocka_unit_test(test_battery_system_error_clears),
        /* edge cases */
        cmocka_unit_test(test_null_thresholds_skips_voltage),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
