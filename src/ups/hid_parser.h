#ifndef HID_PARSER_H
#define HID_PARSER_H

#include <stdint.h>
#include <stddef.h>

/* HID report descriptor parser.
 *
 * Parses a raw USB HID report descriptor into a flat array of resolved fields.
 * Each field carries its full collection path (e.g., UPS → Battery → Voltage),
 * report ID, bit position, and unit scaling — everything needed to read a value
 * and convert it to SI units without any hardcoded report IDs or magic divisors.
 *
 * Usage:
 *   hid_report_map_t map;
 *   hid_parse_descriptor(raw, len, &map);
 *
 *   // Look up input voltage by its collection-qualified usage path
 *   uint32_t path[] = { HID_UP(0x84, 0x04), HID_UP(0x84, 0x1A), HID_UP(0x84, 0x30) };
 *   const hid_field_t *f = hid_find_field(&map, path, 3, HID_FIELD_FEATURE);
 *   double volts = hid_field_read_scaled(fd, f);
 *
 *   hid_report_map_free(&map);
 */

/* Maximum collection nesting depth */
#define HID_MAX_PATH 8

/* Maximum fields we'll track from a single descriptor */
#define HID_MAX_FIELDS 256

/* Pack a usage page and usage ID into a single path element */
#define HID_UP(page, usage) (((uint32_t)(page) << 16) | (uint32_t)(usage))

/* Extract page/usage from a path element */
#define HID_UP_PAGE(up)  ((uint16_t)((up) >> 16))
#define HID_UP_USAGE(up) ((uint16_t)((up) & 0xFFFF))

/* Standard HID usage pages (USB HID Power Device Class v1.1) */
#define HID_PAGE_POWER    0x0084
#define HID_PAGE_BATTERY  0x0085

/* Power Device page (0x84) — structure (§4.1.1) */
#define HID_USAGE_INAME                  0x0001
#define HID_USAGE_PRESENT_STATUS         0x0002
#define HID_USAGE_CHANGED_STATUS         0x0003
#define HID_USAGE_UPS                    0x0004
#define HID_USAGE_POWER_SUPPLY           0x0005
#define HID_USAGE_BATTERY_SYSTEM         0x0010
#define HID_USAGE_BATTERY_SYSTEM_ID      0x0011
#define HID_USAGE_BATTERY                0x0012
#define HID_USAGE_BATTERY_ID             0x0013
#define HID_USAGE_CHARGER                0x0014
#define HID_USAGE_CHARGER_ID             0x0015
#define HID_USAGE_POWER_CONVERTER        0x0016
#define HID_USAGE_POWER_CONVERTER_ID     0x0017
#define HID_USAGE_OUTLET_SYSTEM          0x0018
#define HID_USAGE_OUTLET_SYSTEM_ID       0x0019
#define HID_USAGE_INPUT                  0x001A
#define HID_USAGE_INPUT_ID               0x001B
#define HID_USAGE_OUTPUT                 0x001C
#define HID_USAGE_OUTPUT_ID              0x001D
#define HID_USAGE_FLOW                   0x001E
#define HID_USAGE_FLOW_ID                0x001F
#define HID_USAGE_OUTLET                 0x0020
#define HID_USAGE_OUTLET_ID              0x0021
#define HID_USAGE_GANG                   0x0022
#define HID_USAGE_GANG_ID                0x0023
#define HID_USAGE_POWER_SUMMARY          0x0024
#define HID_USAGE_POWER_SUMMARY_ID       0x0025

/* Power Device page (0x84) — measures (§4.1.2) */
#define HID_USAGE_VOLTAGE                0x0030
#define HID_USAGE_CURRENT                0x0031
#define HID_USAGE_FREQUENCY              0x0032
#define HID_USAGE_APPARENT_POWER         0x0033
#define HID_USAGE_ACTIVE_POWER           0x0034
#define HID_USAGE_PERCENT_LOAD           0x0035
#define HID_USAGE_TEMPERATURE            0x0036
#define HID_USAGE_HUMIDITY               0x0037
#define HID_USAGE_BAD_COUNT              0x0038

/* Power Device page (0x84) — config controls (§4.1.3) */
#define HID_USAGE_CONFIG_VOLTAGE         0x0040
#define HID_USAGE_CONFIG_CURRENT         0x0041
#define HID_USAGE_CONFIG_FREQUENCY       0x0042
#define HID_USAGE_CONFIG_APPARENT_POWER  0x0043
#define HID_USAGE_CONFIG_ACTIVE_POWER    0x0044
#define HID_USAGE_CONFIG_PERCENT_LOAD    0x0045
#define HID_USAGE_CONFIG_TEMPERATURE     0x0046
#define HID_USAGE_CONFIG_HUMIDITY        0x0047

/* Power Device page (0x84) — power controls (§4.1.4) */
#define HID_USAGE_SWITCH_ON_CONTROL      0x0050
#define HID_USAGE_SWITCH_OFF_CONTROL     0x0051
#define HID_USAGE_TOGGLE_CONTROL         0x0052
#define HID_USAGE_LOW_VOLTAGE_TRANSFER   0x0053
#define HID_USAGE_HIGH_VOLTAGE_TRANSFER  0x0054
#define HID_USAGE_DELAY_BEFORE_REBOOT    0x0055
#define HID_USAGE_DELAY_BEFORE_STARTUP   0x0056
#define HID_USAGE_DELAY_BEFORE_SHUTDOWN  0x0057
#define HID_USAGE_TEST                   0x0058
#define HID_USAGE_MODULE_RESET           0x0059
#define HID_USAGE_AUDIBLE_ALARM_CTRL     0x005A

/* Power Device page (0x84) — generic status (§4.1.5) */
#define HID_USAGE_PRESENT                0x0060
#define HID_USAGE_GOOD                   0x0061
#define HID_USAGE_INTERNAL_FAILURE       0x0062
#define HID_USAGE_VOLTAGE_OUT_OF_RANGE   0x0063
#define HID_USAGE_FREQ_OUT_OF_RANGE      0x0064
#define HID_USAGE_OVERLOAD               0x0065
#define HID_USAGE_OVER_CHARGED           0x0066
#define HID_USAGE_OVER_TEMPERATURE       0x0067
#define HID_USAGE_SHUTDOWN_REQUESTED     0x0068
#define HID_USAGE_SHUTDOWN_IMMINENT      0x0069
#define HID_USAGE_SWITCH_ON_OFF          0x006B
#define HID_USAGE_SWITCHABLE             0x006C
#define HID_USAGE_USED                   0x006D
#define HID_USAGE_BOOST                  0x006E
#define HID_USAGE_BUCK                   0x006F
#define HID_USAGE_INITIALIZED            0x0070
#define HID_USAGE_TESTED                 0x0071
#define HID_USAGE_AWAITING_POWER         0x0072
#define HID_USAGE_COMMUNICATIONS_LOST    0x0073

/* Power Device page (0x84) — identification (§4.1.6) */
#define HID_USAGE_IMANUFACTURER          0x00FD
#define HID_USAGE_IPRODUCT               0x00FE
#define HID_USAGE_ISERIAL_NUMBER         0x00FF

/* Battery System page (0x85) — settings (§4.2.2) */
#define HID_USAGE_BAT_REMAINING_CAP_LIM  0x0029
#define HID_USAGE_BAT_REMAINING_TIME_LIM 0x002A
#define HID_USAGE_BAT_CAPACITY_MODE      0x002C

/* Battery System page (0x85) — status (§4.2.4) */
#define HID_USAGE_BAT_BELOW_CAP_LIMIT   0x0042
#define HID_USAGE_BAT_REMAINING_TIME_EXP 0x0043
#define HID_USAGE_BAT_CHARGING           0x0044
#define HID_USAGE_BAT_DISCHARGING        0x0045
#define HID_USAGE_BAT_NEED_REPLACEMENT   0x004B

/* Battery System page (0x85) — measures (§4.2.5) */
#define HID_USAGE_BAT_REMAINING_CAPACITY 0x0066
#define HID_USAGE_BAT_FULL_CHARGE_CAP    0x0067
#define HID_USAGE_BAT_RUNTIME_TO_EMPTY   0x0068

/* Battery System page (0x85) — battery info (§4.2.6) */
#define HID_USAGE_BAT_DESIGN_CAPACITY    0x0083
#define HID_USAGE_BAT_MANUFACTURE_DATE   0x0085
#define HID_USAGE_BAT_RECHARGEABLE       0x008B
#define HID_USAGE_BAT_WARNING_CAP_LIMIT  0x008C
#define HID_USAGE_BAT_CAP_GRANULARITY_1  0x008D
#define HID_USAGE_BAT_CAP_GRANULARITY_2  0x008E
#define HID_USAGE_BAT_OEM_INFORMATION    0x008F

/* Battery System page (0x85) — charger/power status */
#define HID_USAGE_BAT_AC_PRESENT         0x00D0
#define HID_USAGE_BAT_BATTERY_PRESENT    0x00D1
#define HID_USAGE_BAT_POWER_FAIL         0x00D2
#define HID_USAGE_BAT_VOLTAGE_NOT_REG    0x00DB

/* Field type (HID main item) */
typedef enum {
    HID_FIELD_INPUT   = 0,
    HID_FIELD_OUTPUT  = 1,
    HID_FIELD_FEATURE = 2,
} hid_field_type_t;

/* A single resolved field from the HID report descriptor */
typedef struct {
    /* Collection-qualified usage path.
     * e.g., [Power.UPS, Power.Input, Power.Voltage] */
    uint32_t         usage_path[HID_MAX_PATH];
    int              path_depth;

    /* The field's own usage (last element of path, but stored separately
     * because the page may differ from the collection page) */
    uint16_t         usage_page;
    uint16_t         usage_id;

    /* Report location */
    uint8_t          report_id;
    uint16_t         bit_offset;  /* offset within the report (after report ID byte) */
    uint16_t         bit_size;    /* size in bits */
    hid_field_type_t type;        /* input/output/feature */

    /* Scaling */
    int32_t          logical_min;
    int32_t          logical_max;
    int32_t          physical_min;
    int32_t          physical_max;
    int8_t           unit_exponent;
    uint32_t         unit;
} hid_field_t;

/* Parsed descriptor — array of resolved fields */
typedef struct {
    hid_field_t fields[HID_MAX_FIELDS];
    size_t      count;
} hid_report_map_t;

/* Parse a raw HID report descriptor into a field map.
 * Returns 0 on success, -1 on parse error. */
int hid_parse_descriptor(const uint8_t *desc, size_t len,
                         hid_report_map_t *map);

/* Find a field by collection-qualified usage path.
 * Path matching: each element of `path` is matched against the field's
 * usage_path. The match is suffix-based — the path elements must appear
 * in order at the END of the field's collection path.
 *
 * Example: path = [Power.Input, Power.Voltage] matches a field with
 * collection path [Power.UPS, Power.Input, Power.Voltage].
 *
 * Returns NULL if no match. */
const hid_field_t *hid_find_field(const hid_report_map_t *map,
                                   const uint32_t *path, int path_len,
                                   hid_field_type_t type);

/* Read a field's raw integer value from a hidraw fd.
 * Handles multi-byte reads and bit extraction.
 * Returns the raw logical value (before scaling). */
int32_t hid_field_read_raw(int fd, const hid_field_t *field);

/* Read a field and apply unit exponent scaling.
 * Converts from the descriptor's CGS-based unit to SI.
 *
 * For voltage (unit 0xF0D121 = cm²·g·s⁻³·A⁻¹):
 *   CGS→SI offset = 10^-7
 *   Result = raw * 10^(unit_exponent - 7)
 *
 * For fields with no unit (unit=0): returns raw value as-is. */
double hid_field_read_scaled(int fd, const hid_field_t *field);

/* Free internal allocations (currently a no-op since we use fixed arrays,
 * but call it for forward compatibility). */
void hid_report_map_free(hid_report_map_t *map);

#endif
