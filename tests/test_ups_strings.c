#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <cmocka.h>
#include <string.h>

#include "ups/ups_format.h"
#include "ups/ups.h"

/* --- ups_transfer_reason_str --- */

static void test_transfer_reason_known(void **state)
{
    (void)state;
    assert_string_equal(ups_transfer_reason_str(0), "SystemInitialization");
    assert_string_equal(ups_transfer_reason_str(1), "HighInputVoltage");
    assert_string_equal(ups_transfer_reason_str(2), "LowInputVoltage");
    assert_string_equal(ups_transfer_reason_str(8), "AcceptableInput");
    assert_string_equal(ups_transfer_reason_str(30), "FailureBypassExpired");
}

static void test_transfer_reason_unknown(void **state)
{
    (void)state;
    assert_string_equal(ups_transfer_reason_str(31), "Unknown");
    assert_string_equal(ups_transfer_reason_str(999), "Unknown");
    assert_string_equal(ups_transfer_reason_str(0xFFFF), "Unknown");
}

/* --- ups_status_str --- */

static void test_status_str_single_bit(void **state)
{
    (void)state;
    char buf[256];

    ups_status_str(UPS_ST_ONLINE, buf, sizeof(buf));
    assert_string_equal(buf, "Online");

    ups_status_str(UPS_ST_ON_BATTERY, buf, sizeof(buf));
    assert_string_equal(buf, "OnBattery");

    ups_status_str(UPS_ST_FAULT, buf, sizeof(buf));
    assert_string_equal(buf, "Fault");

    ups_status_str(UPS_ST_OVERLOAD, buf, sizeof(buf));
    assert_string_equal(buf, "Overload");

    ups_status_str(UPS_ST_HE_MODE, buf, sizeof(buf));
    assert_string_equal(buf, "HighEfficiency");
}

static void test_status_str_multiple_bits(void **state)
{
    (void)state;
    char buf[256];

    ups_status_str(UPS_ST_ONLINE | UPS_ST_HE_MODE, buf, sizeof(buf));
    assert_string_equal(buf, "Online HighEfficiency");

    ups_status_str(UPS_ST_ON_BATTERY | UPS_ST_OVERLOAD | UPS_ST_FAULT, buf, sizeof(buf));
    assert_string_equal(buf, "OnBattery Fault Overload");
}

static void test_status_str_zero(void **state)
{
    (void)state;
    char buf[256];
    ups_status_str(0, buf, sizeof(buf));
    assert_string_equal(buf, "Unknown");
}

static void test_status_str_truncation(void **state)
{
    (void)state;
    char buf[8];
    ups_status_str(UPS_ST_ONLINE | UPS_ST_ON_BATTERY, buf, sizeof(buf));
    /* Should not overflow — content is truncated but null-terminated */
    assert_true(strlen(buf) < sizeof(buf));
}

/* --- ups_efficiency_str --- */

static void test_efficiency_str_positive(void **state)
{
    (void)state;
    char buf[64];

    /* 128 raw = 1.0% (128/128) */
    ups_efficiency_str(128, buf, sizeof(buf));
    assert_string_equal(buf, "1.0%");

    /* 12800 raw = 100.0% */
    ups_efficiency_str(12800, buf, sizeof(buf));
    assert_string_equal(buf, "100.0%");

    /* 12160 raw = 95.0% */
    ups_efficiency_str(12160, buf, sizeof(buf));
    assert_string_equal(buf, "95.0%");
}

static void test_efficiency_str_negative_reasons(void **state)
{
    (void)state;
    char buf[64];

    ups_efficiency_str(-1, buf, sizeof(buf));
    assert_string_equal(buf, "NotAvailable");

    ups_efficiency_str(-2, buf, sizeof(buf));
    assert_string_equal(buf, "LoadTooLow");

    ups_efficiency_str(-3, buf, sizeof(buf));
    assert_string_equal(buf, "OutputOff");

    ups_efficiency_str(-4, buf, sizeof(buf));
    assert_string_equal(buf, "OnBattery");

    ups_efficiency_str(-8, buf, sizeof(buf));
    assert_string_equal(buf, "BatteryDisconnected");
}

static void test_efficiency_str_unknown_negative(void **state)
{
    (void)state;
    char buf[64];
    ups_efficiency_str(-9, buf, sizeof(buf));
    assert_true(strstr(buf, "Unknown") != NULL);

    ups_efficiency_str(-100, buf, sizeof(buf));
    assert_true(strstr(buf, "Unknown") != NULL);
}

/* --- ups_decode_general_errors --- */

static void test_decode_general_errors_none(void **state)
{
    (void)state;
    const char *out[16];
    int n = ups_decode_general_errors(0, out, 16);
    assert_int_equal(n, 0);
}

static void test_decode_general_errors_single(void **state)
{
    (void)state;
    const char *out[16];

    int n = ups_decode_general_errors(UPS_GENERR_SITE_WIRING, out, 16);
    assert_int_equal(n, 1);
    assert_string_equal(out[0], "SiteWiring");

    n = ups_decode_general_errors(UPS_GENERR_EPO_ACTIVE, out, 16);
    assert_int_equal(n, 1);
    assert_string_equal(out[0], "EPOActive");
}

static void test_decode_general_errors_multiple(void **state)
{
    (void)state;
    const char *out[16];
    uint16_t raw = UPS_GENERR_SITE_WIRING | UPS_GENERR_EEPROM | UPS_GENERR_FW_MISMATCH;
    int n = ups_decode_general_errors(raw, out, 16);
    assert_int_equal(n, 3);
    assert_string_equal(out[0], "SiteWiring");
    assert_string_equal(out[1], "EEPROM");
    assert_string_equal(out[2], "FirmwareMismatch");
}

static void test_decode_general_errors_max_limit(void **state)
{
    (void)state;
    const char *out[2];
    uint16_t raw = UPS_GENERR_SITE_WIRING | UPS_GENERR_EEPROM | UPS_GENERR_FW_MISMATCH;
    int n = ups_decode_general_errors(raw, out, 2);
    assert_int_equal(n, 2);
}

/* --- ups_decode_power_errors --- */

static void test_decode_power_errors_none(void **state)
{
    (void)state;
    const char *out[16];
    assert_int_equal(ups_decode_power_errors(0, out, 16), 0);
}

static void test_decode_power_errors_all(void **state)
{
    (void)state;
    const char *out[16];
    uint32_t raw = UPS_PWRERR_OVERLOAD | UPS_PWRERR_SHORT_CIRCUIT |
                   UPS_PWRERR_OVERVOLTAGE | UPS_PWRERR_OVERTEMP |
                   UPS_PWRERR_FAN | UPS_PWRERR_INVERTER;
    int n = ups_decode_power_errors(raw, out, 16);
    assert_int_equal(n, 6);
    assert_string_equal(out[0], "Overload");
    assert_string_equal(out[1], "ShortCircuit");
    assert_string_equal(out[2], "Overvoltage");
    assert_string_equal(out[3], "Overtemperature");
    assert_string_equal(out[4], "Fan");
    assert_string_equal(out[5], "Inverter");
}

/* --- ups_decode_battery_errors --- */

static void test_decode_battery_errors_none(void **state)
{
    (void)state;
    const char *out[16];
    assert_int_equal(ups_decode_battery_errors(0, out, 16), 0);
}

static void test_decode_battery_errors_single(void **state)
{
    (void)state;
    const char *out[16];
    int n = ups_decode_battery_errors(UPS_BATERR_REPLACE, out, 16);
    assert_int_equal(n, 1);
    assert_string_equal(out[0], "NeedsReplacement");
}

static void test_decode_battery_errors_multiple(void **state)
{
    (void)state;
    const char *out[16];
    uint16_t raw = UPS_BATERR_DISCONNECTED | UPS_BATERR_OVERTEMP_WARN | UPS_BATERR_COMM;
    int n = ups_decode_battery_errors(raw, out, 16);
    assert_int_equal(n, 3);
    assert_string_equal(out[0], "Disconnected");
    assert_string_equal(out[1], "OvertemperatureWarning");
    assert_string_equal(out[2], "CommunicationError");
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        /* transfer reason */
        cmocka_unit_test(test_transfer_reason_known),
        cmocka_unit_test(test_transfer_reason_unknown),
        /* status string */
        cmocka_unit_test(test_status_str_single_bit),
        cmocka_unit_test(test_status_str_multiple_bits),
        cmocka_unit_test(test_status_str_zero),
        cmocka_unit_test(test_status_str_truncation),
        /* efficiency string */
        cmocka_unit_test(test_efficiency_str_positive),
        cmocka_unit_test(test_efficiency_str_negative_reasons),
        cmocka_unit_test(test_efficiency_str_unknown_negative),
        /* general errors */
        cmocka_unit_test(test_decode_general_errors_none),
        cmocka_unit_test(test_decode_general_errors_single),
        cmocka_unit_test(test_decode_general_errors_multiple),
        cmocka_unit_test(test_decode_general_errors_max_limit),
        /* power errors */
        cmocka_unit_test(test_decode_power_errors_none),
        cmocka_unit_test(test_decode_power_errors_all),
        /* battery errors */
        cmocka_unit_test(test_decode_battery_errors_none),
        cmocka_unit_test(test_decode_battery_errors_single),
        cmocka_unit_test(test_decode_battery_errors_multiple),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
