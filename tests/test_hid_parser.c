#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <cmocka.h>
#include <string.h>

#include "ups/hid_parser.h"

/* Minimal UPS-like HID report descriptor.
 *
 * Structure:
 *   Power Device (0x84) / UPS (0x04) [Application Collection]
 *     Power Device / Input (0x1A) [Physical Collection]
 *       Report ID 1, 16-bit features, voltage unit (0xF0D121), exponent 7
 *       - Voltage (0x30): bits 0-15
 *       - Frequency (0x32): bits 16-31
 *     Battery System (0x85) / Battery (0x12) [Physical Collection]
 *       Report ID 2, 8-bit features, no unit
 *       - RemainingCapacity (0x66): bits 0-7
 */
static const uint8_t TEST_DESC[] = {
    /* Usage Page (Power Device = 0x0084) */
    0x06, 0x84, 0x00,
    /* Usage (UPS = 0x04) */
    0x09, 0x04,
    /* Collection (Application = 0x01) */
    0xA1, 0x01,

        /* --- Input collection --- */
        /* Usage (Input = 0x1A) */
        0x09, 0x1A,
        /* Collection (Physical = 0x00) */
        0xA1, 0x00,
            /* Report ID (1) */
            0x85, 0x01,
            /* Logical Minimum (0) */
            0x15, 0x00,
            /* Logical Maximum (2400 = 0x0960) */
            0x26, 0x60, 0x09,
            /* Unit Exponent (7) */
            0x55, 0x07,
            /* Unit (voltage: 0x00F0D121) */
            0x67, 0x21, 0xD1, 0xF0, 0x00,
            /* Report Size (16) */
            0x75, 0x10,
            /* Report Count (1) */
            0x95, 0x01,
            /* Usage (Voltage = 0x30) */
            0x09, 0x30,
            /* Feature (Data, Variable) */
            0xB1, 0x02,
            /* Usage (Frequency = 0x32) */
            0x09, 0x32,
            /* Feature (Data, Variable) */
            0xB1, 0x02,
        /* End Collection */
        0xC0,

        /* --- Battery collection --- */
        /* Usage Page (Battery System = 0x0085) */
        0x06, 0x85, 0x00,
        /* Usage (Battery = 0x12) */
        0x09, 0x12,
        /* Collection (Physical) */
        0xA1, 0x00,
            /* Report ID (2) */
            0x85, 0x02,
            /* Logical Minimum (0) */
            0x15, 0x00,
            /* Logical Maximum (100) */
            0x25, 0x64,
            /* Unit Exponent (0) */
            0x55, 0x00,
            /* Unit (none) */
            0x65, 0x00,
            /* Report Size (8) */
            0x75, 0x08,
            /* Report Count (1) */
            0x95, 0x01,
            /* Usage (RemainingCapacity = 0x66) */
            0x09, 0x66,
            /* Feature (Data, Variable) */
            0xB1, 0x02,
        /* End Collection */
        0xC0,

    /* End Collection */
    0xC0,
};

/* --- Parse basic descriptor --- */

static void test_parse_field_count(void **state)
{
    (void)state;
    hid_report_map_t map;
    int rc = hid_parse_descriptor(TEST_DESC, sizeof(TEST_DESC), &map);
    assert_int_equal(rc, 0);
    assert_int_equal(map.count, 3);
    hid_report_map_free(&map);
}

static void test_parse_voltage_field(void **state)
{
    (void)state;
    hid_report_map_t map;
    hid_parse_descriptor(TEST_DESC, sizeof(TEST_DESC), &map);

    const hid_field_t *f = &map.fields[0];
    assert_int_equal(f->usage_page, HID_PAGE_POWER);
    assert_int_equal(f->usage_id, HID_USAGE_VOLTAGE);
    assert_int_equal(f->report_id, 1);
    assert_int_equal(f->bit_offset, 0);
    assert_int_equal(f->bit_size, 16);
    assert_int_equal(f->type, HID_FIELD_FEATURE);
    assert_int_equal(f->logical_min, 0);
    assert_int_equal(f->logical_max, 2400);
    assert_int_equal(f->unit_exponent, 7);
    assert_true(f->unit == 0x00F0D121);

    /* Path: [Power.UPS, Power.Input, Power.Voltage] */
    assert_int_equal(f->path_depth, 3);
    assert_true(f->usage_path[0] == HID_UP(HID_PAGE_POWER, HID_USAGE_UPS));
    assert_true(f->usage_path[1] == HID_UP(HID_PAGE_POWER, HID_USAGE_INPUT));
    assert_true(f->usage_path[2] == HID_UP(HID_PAGE_POWER, HID_USAGE_VOLTAGE));

    hid_report_map_free(&map);
}

static void test_parse_frequency_field(void **state)
{
    (void)state;
    hid_report_map_t map;
    hid_parse_descriptor(TEST_DESC, sizeof(TEST_DESC), &map);

    const hid_field_t *f = &map.fields[1];
    assert_int_equal(f->usage_page, HID_PAGE_POWER);
    assert_int_equal(f->usage_id, HID_USAGE_FREQUENCY);
    assert_int_equal(f->report_id, 1);
    assert_int_equal(f->bit_offset, 16);
    assert_int_equal(f->bit_size, 16);
    assert_int_equal(f->type, HID_FIELD_FEATURE);

    hid_report_map_free(&map);
}

static void test_parse_battery_field(void **state)
{
    (void)state;
    hid_report_map_t map;
    hid_parse_descriptor(TEST_DESC, sizeof(TEST_DESC), &map);

    const hid_field_t *f = &map.fields[2];
    assert_int_equal(f->usage_page, HID_PAGE_BATTERY);
    assert_int_equal(f->usage_id, HID_USAGE_BAT_REMAINING_CAPACITY);
    assert_int_equal(f->report_id, 2);
    assert_int_equal(f->bit_offset, 0);
    assert_int_equal(f->bit_size, 8);
    assert_int_equal(f->type, HID_FIELD_FEATURE);
    assert_int_equal(f->unit, 0);

    /* Path: [Power.UPS, Battery.Battery, Battery.RemainingCapacity] */
    assert_int_equal(f->path_depth, 3);
    assert_true(f->usage_path[0] == HID_UP(HID_PAGE_POWER, HID_USAGE_UPS));
    assert_true(f->usage_path[1] == HID_UP(HID_PAGE_BATTERY, HID_USAGE_BATTERY));
    assert_true(f->usage_path[2] == HID_UP(HID_PAGE_BATTERY, HID_USAGE_BAT_REMAINING_CAPACITY));

    hid_report_map_free(&map);
}

/* --- Empty descriptor --- */

static void test_parse_empty(void **state)
{
    (void)state;
    hid_report_map_t map;
    int rc = hid_parse_descriptor(NULL, 0, &map);
    assert_int_equal(rc, 0);
    assert_int_equal(map.count, 0);
}

/* --- hid_find_field --- */

static void test_find_field_full_path(void **state)
{
    (void)state;
    hid_report_map_t map;
    hid_parse_descriptor(TEST_DESC, sizeof(TEST_DESC), &map);

    uint32_t path[] = {
        HID_UP(HID_PAGE_POWER, HID_USAGE_UPS),
        HID_UP(HID_PAGE_POWER, HID_USAGE_INPUT),
        HID_UP(HID_PAGE_POWER, HID_USAGE_VOLTAGE),
    };
    const hid_field_t *f = hid_find_field(&map, path, 3, HID_FIELD_FEATURE);
    assert_non_null(f);
    assert_int_equal(f->usage_id, HID_USAGE_VOLTAGE);

    hid_report_map_free(&map);
}

static void test_find_field_suffix_match(void **state)
{
    (void)state;
    hid_report_map_t map;
    hid_parse_descriptor(TEST_DESC, sizeof(TEST_DESC), &map);

    /* Two-element suffix: [Input, Voltage] */
    uint32_t path[] = {
        HID_UP(HID_PAGE_POWER, HID_USAGE_INPUT),
        HID_UP(HID_PAGE_POWER, HID_USAGE_VOLTAGE),
    };
    const hid_field_t *f = hid_find_field(&map, path, 2, HID_FIELD_FEATURE);
    assert_non_null(f);
    assert_int_equal(f->usage_id, HID_USAGE_VOLTAGE);

    hid_report_map_free(&map);
}

static void test_find_field_single_usage(void **state)
{
    (void)state;
    hid_report_map_t map;
    hid_parse_descriptor(TEST_DESC, sizeof(TEST_DESC), &map);

    /* Single-element match: [Frequency] */
    uint32_t path[] = { HID_UP(HID_PAGE_POWER, HID_USAGE_FREQUENCY) };
    const hid_field_t *f = hid_find_field(&map, path, 1, HID_FIELD_FEATURE);
    assert_non_null(f);
    assert_int_equal(f->usage_id, HID_USAGE_FREQUENCY);
    assert_int_equal(f->bit_offset, 16);

    hid_report_map_free(&map);
}

static void test_find_field_wrong_type(void **state)
{
    (void)state;
    hid_report_map_t map;
    hid_parse_descriptor(TEST_DESC, sizeof(TEST_DESC), &map);

    /* Voltage exists as Feature but not as Input */
    uint32_t path[] = { HID_UP(HID_PAGE_POWER, HID_USAGE_VOLTAGE) };
    const hid_field_t *f = hid_find_field(&map, path, 1, HID_FIELD_INPUT);
    assert_null(f);

    hid_report_map_free(&map);
}

static void test_find_field_not_found(void **state)
{
    (void)state;
    hid_report_map_t map;
    hid_parse_descriptor(TEST_DESC, sizeof(TEST_DESC), &map);

    /* Usage that doesn't exist in the descriptor */
    uint32_t path[] = { HID_UP(HID_PAGE_POWER, 0x99) };
    const hid_field_t *f = hid_find_field(&map, path, 1, HID_FIELD_FEATURE);
    assert_null(f);

    hid_report_map_free(&map);
}

static void test_find_field_cross_page(void **state)
{
    (void)state;
    hid_report_map_t map;
    hid_parse_descriptor(TEST_DESC, sizeof(TEST_DESC), &map);

    /* Find battery remaining capacity via suffix */
    uint32_t path[] = {
        HID_UP(HID_PAGE_BATTERY, HID_USAGE_BATTERY),
        HID_UP(HID_PAGE_BATTERY, HID_USAGE_BAT_REMAINING_CAPACITY),
    };
    const hid_field_t *f = hid_find_field(&map, path, 2, HID_FIELD_FEATURE);
    assert_non_null(f);
    assert_int_equal(f->usage_id, HID_USAGE_BAT_REMAINING_CAPACITY);
    assert_int_equal(f->report_id, 2);

    hid_report_map_free(&map);
}

/* --- Multi-count report --- */

static void test_parse_multi_count(void **state)
{
    (void)state;

    /* Descriptor with Report Count = 2, two usages.
     * Should produce 2 fields with consecutive bit offsets. */
    static const uint8_t desc[] = {
        0x06, 0x84, 0x00,          /* Usage Page (Power Device) */
        0x09, 0x04,                /* Usage (UPS) */
        0xA1, 0x01,                /* Collection (Application) */
            0x85, 0x03,            /* Report ID (3) */
            0x15, 0x00,            /* Logical Minimum (0) */
            0x25, 0x01,            /* Logical Maximum (1) */
            0x75, 0x01,            /* Report Size (1) — single bit */
            0x95, 0x02,            /* Report Count (2) */
            0x09, 0x65,            /* Usage (Overload) */
            0x09, 0x68,            /* Usage (ShutdownRequested) */
            0x81, 0x02,            /* Input (Data, Variable) */
        0xC0,                      /* End Collection */
    };

    hid_report_map_t map;
    hid_parse_descriptor(desc, sizeof(desc), &map);
    assert_int_equal(map.count, 2);

    /* Field 0: Overload at bit 0, 1-bit */
    assert_int_equal(map.fields[0].usage_id, HID_USAGE_OVERLOAD);
    assert_int_equal(map.fields[0].bit_offset, 0);
    assert_int_equal(map.fields[0].bit_size, 1);
    assert_int_equal(map.fields[0].type, HID_FIELD_INPUT);

    /* Field 1: ShutdownRequested at bit 1, 1-bit */
    assert_int_equal(map.fields[1].usage_id, HID_USAGE_SHUTDOWN_REQUESTED);
    assert_int_equal(map.fields[1].bit_offset, 1);
    assert_int_equal(map.fields[1].bit_size, 1);
    assert_int_equal(map.fields[1].type, HID_FIELD_INPUT);

    hid_report_map_free(&map);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        /* parsing */
        cmocka_unit_test(test_parse_field_count),
        cmocka_unit_test(test_parse_voltage_field),
        cmocka_unit_test(test_parse_frequency_field),
        cmocka_unit_test(test_parse_battery_field),
        cmocka_unit_test(test_parse_empty),
        cmocka_unit_test(test_parse_multi_count),
        /* field lookup */
        cmocka_unit_test(test_find_field_full_path),
        cmocka_unit_test(test_find_field_suffix_match),
        cmocka_unit_test(test_find_field_single_usage),
        cmocka_unit_test(test_find_field_wrong_type),
        cmocka_unit_test(test_find_field_not_found),
        cmocka_unit_test(test_find_field_cross_page),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
