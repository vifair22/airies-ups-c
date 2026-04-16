#include "hid_parser.h"

#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/hidraw.h>
#include <math.h>

/* --- Descriptor item parsing --- */

/* Decode a signed integer from 1/2/4 bytes */
static int32_t signed_val(uint32_t val, int size)
{
    switch (size) {
    case 1: return (val & 0x80)       ? (int32_t)val - 256         : (int32_t)val;
    case 2: return (val & 0x8000)     ? (int32_t)val - 65536       : (int32_t)val;
    case 4: return (int32_t)val;
    default: return (int32_t)val;
    }
}

/* HID unit exponent: 4-bit signed nibble (0-7 = positive, 8-15 = -8 to -1).
 * The descriptor stores it as a full byte but only the low nibble matters
 * for the standard encoding. Values > 7 wrap negative. */
static int8_t decode_unit_exponent(int32_t raw)
{
    int8_t nibble = (int8_t)(raw & 0x0F);
    return (nibble > 7) ? (int8_t)(nibble - 16) : nibble;
}

/* --- Descriptor parser --- */

int hid_parse_descriptor(const uint8_t *desc, size_t len,
                         hid_report_map_t *map)
{
    memset(map, 0, sizeof(*map));

    /* Collection stack */
    uint32_t coll_stack[HID_MAX_PATH];
    int      coll_depth = 0;

    /* Global state */
    uint16_t current_page = 0;
    int32_t  logical_min  = 0;
    int32_t  logical_max  = 0;
    int32_t  physical_min = 0;
    int32_t  physical_max = 0;
    int32_t  unit_exp_raw = 0;
    uint32_t unit         = 0;
    uint32_t report_size  = 0;
    uint32_t report_count = 0;
    uint8_t  report_id    = 0;

    /* Per-report-ID, per-type bit offset tracker.
     * HID spec: Input, Output, Feature each have independent bit counters
     * even when sharing the same report ID. [report_id][type] */
    uint16_t bit_offsets[256][3];
    memset(bit_offsets, 0, sizeof(bit_offsets));

    /* Local usage queue */
    uint16_t usage_queue[64];
    int      usage_count = 0;

    size_t i = 0;
    while (i < len) {
        uint8_t b = desc[i];
        int bsize = b & 0x03;
        if (bsize == 3) bsize = 4;
        int btype = (b >> 2) & 0x03;
        int btag  = (b >> 4) & 0x0F;

        if (i + 1 + (size_t)bsize > len)
            break;

        uint32_t val = 0;
        if (bsize == 1) val = desc[i + 1];
        else if (bsize == 2) val = (uint32_t)desc[i + 1] | ((uint32_t)desc[i + 2] << 8);
        else if (bsize == 4) val = (uint32_t)desc[i + 1] | ((uint32_t)desc[i + 2] << 8) |
                                   ((uint32_t)desc[i + 3] << 16) | ((uint32_t)desc[i + 4] << 24);

        if (btype == 1) {
            /* Global item */
            switch (btag) {
            case 0: current_page = (uint16_t)val; break;
            case 1: logical_min  = signed_val(val, bsize); break;
            case 2: logical_max  = signed_val(val, bsize); break;
            case 3: physical_min = signed_val(val, bsize); break;
            case 4: physical_max = signed_val(val, bsize); break;
            case 5: unit_exp_raw = signed_val(val, bsize); break;
            case 6: unit         = val; break;
            case 7: report_size  = val; break;
            case 8: report_id    = (uint8_t)val; break;
            case 9: report_count = val; break;
            }
        } else if (btype == 2) {
            /* Local item */
            if (btag == 0 && usage_count < 64) {
                usage_queue[usage_count++] = (uint16_t)val;
            }
        } else if (btype == 0) {
            /* Main item */
            if (btag == 0x0A) {
                /* Collection — push onto stack */
                uint16_t u = (usage_count > 0) ? usage_queue[0] : 0;
                if (coll_depth < HID_MAX_PATH)
                    coll_stack[coll_depth++] = HID_UP(current_page, u);
                usage_count = 0;
            } else if (btag == 0x0C) {
                /* End Collection — pop */
                if (coll_depth > 0) coll_depth--;
            } else if (btag == 0x08 || btag == 0x09 || btag == 0x0B) {
                /* Input (0x08), Output (0x09), Feature (0x0B) */
                hid_field_type_t ftype = (btag == 0x08) ? HID_FIELD_INPUT :
                                         (btag == 0x09) ? HID_FIELD_OUTPUT :
                                                          HID_FIELD_FEATURE;

                for (uint32_t fi = 0; fi < report_count && map->count < HID_MAX_FIELDS; fi++) {
                    uint16_t u = (fi < (uint32_t)usage_count)
                        ? usage_queue[fi]
                        : (usage_count > 0 ? usage_queue[usage_count - 1] : 0);

                    hid_field_t *f = &map->fields[map->count];
                    memset(f, 0, sizeof(*f));

                    /* Build usage path: collection stack + this field's usage */
                    int depth = 0;
                    for (int ci = 0; ci < coll_depth && depth < HID_MAX_PATH; ci++)
                        f->usage_path[depth++] = coll_stack[ci];
                    if (depth < HID_MAX_PATH)
                        f->usage_path[depth++] = HID_UP(current_page, u);
                    f->path_depth = depth;

                    f->usage_page  = current_page;
                    f->usage_id    = u;
                    f->report_id   = report_id;
                    f->bit_offset  = bit_offsets[report_id][ftype];
                    f->bit_size    = (uint16_t)report_size;
                    f->type        = ftype;
                    f->logical_min = logical_min;
                    f->logical_max = logical_max;
                    f->physical_min = physical_min;
                    f->physical_max = physical_max;
                    f->unit_exponent = decode_unit_exponent(unit_exp_raw);
                    f->unit = unit;

                    bit_offsets[report_id][ftype] += (uint16_t)report_size;
                    map->count++;
                }
                usage_count = 0;
            }
        }

        i += 1 + (size_t)bsize;
    }

    return 0;
}

/* --- Field lookup --- */

const hid_field_t *hid_find_field(const hid_report_map_t *map,
                                   const uint32_t *path, int path_len,
                                   hid_field_type_t type)
{
    for (size_t i = 0; i < map->count; i++) {
        const hid_field_t *f = &map->fields[i];
        if (f->type != type) continue;
        if (f->path_depth < path_len) continue;

        /* Suffix match: the search path must match the last path_len
         * elements of the field's usage path */
        int offset = f->path_depth - path_len;
        int match = 1;
        for (int j = 0; j < path_len; j++) {
            if (f->usage_path[offset + j] != path[j]) {
                match = 0;
                break;
            }
        }
        if (match) return f;
    }
    return NULL;
}

/* --- Reading values --- */

int32_t hid_field_read_raw(int fd, const hid_field_t *field)
{
    if (!field) return 0;

    /* Read the full report via HIDIOCGFEATURE.
     * Report size: 1 byte (report ID) + ceil(total_bits / 8).
     * We don't know the total report size, so read a generous buffer. */
    uint8_t buf[64];
    memset(buf, 0, sizeof(buf));
    buf[0] = field->report_id;

    int rc = ioctl(fd, HIDIOCGFEATURE(sizeof(buf)), buf);
    if (rc < 0) return 0;

    /* Extract value from the report.
     * buf[0] is the report ID, data starts at buf[1].
     * bit_offset is relative to the data portion. */
    uint16_t byte_off = (uint16_t)(field->bit_offset / 8);
    uint16_t bit_off  = (uint16_t)(field->bit_offset % 8);
    uint16_t bits     = field->bit_size;

    /* Simple fast paths for byte-aligned common sizes */
    if (bit_off == 0 && bits == 8) {
        return (int32_t)buf[1 + byte_off];
    }
    if (bit_off == 0 && bits == 16) {
        return (int32_t)((uint32_t)buf[1 + byte_off] |
                         ((uint32_t)buf[2 + byte_off] << 8));
    }
    if (bit_off == 0 && bits == 32) {
        return (int32_t)((uint32_t)buf[1 + byte_off] |
                         ((uint32_t)buf[2 + byte_off] << 8) |
                         ((uint32_t)buf[3 + byte_off] << 16) |
                         ((uint32_t)buf[4 + byte_off] << 24));
    }

    /* General bit extraction for non-aligned fields (e.g., status bits) */
    uint32_t result = 0;
    for (uint16_t b = 0; b < bits; b++) {
        uint16_t abs_bit = (uint16_t)(field->bit_offset + b);
        uint16_t byte_idx = (uint16_t)(1 + abs_bit / 8);
        uint16_t bit_idx  = (uint16_t)(abs_bit % 8);
        if (byte_idx < sizeof(buf) && (buf[byte_idx] & (1u << bit_idx)))
            result |= (1u << b);
    }

    /* Sign-extend if logical_min is negative */
    if (field->logical_min < 0 && bits < 32) {
        if (result & (1u << (bits - 1)))
            result |= ~((1u << bits) - 1);
    }

    return (int32_t)result;
}

double hid_field_read_scaled(int fd, const hid_field_t *field)
{
    if (!field) return 0.0;

    int32_t raw = hid_field_read_raw(fd, field);

    /* No unit — return raw value (percentages, enums, etc.) */
    if (field->unit == 0)
        return (double)raw;

    /* Apply unit exponent.
     *
     * HID units in the Power Device class use CGS base (cm, g, s, A).
     * The relationship to SI varies by unit type:
     *
     * Voltage (unit 0xF0D121 = cm²·g·s⁻³·A⁻¹):
     *   CGS→SI: 10^-4 (cm²→m²) * 10^-3 (g→kg) = 10^-7
     *   SI value = raw * 10^(exponent - 7)
     *
     * Time (unit 0x1001 = seconds):
     *   No CGS offset needed, time is the same in both systems.
     *   SI value = raw * 10^exponent
     *
     * For simplicity, we compute the CGS→SI offset from the unit nibbles
     * and apply it together with the exponent. */

    /* Decode the unit nibbles to find the CGS→SI offset.
     * Unit encoding: nibbles [system, length, mass, time, temp, current, lum, ...]
     * SI: length=m(exp), mass=kg(exp), time=s(exp), current=A(exp)
     * CGS: length=cm, mass=g → multiply by 10^(-2*len_exp) * 10^(-3*mass_exp)
     */
    int8_t len_exp  = (int8_t)((field->unit >> 4)  & 0x0F);
    int8_t mass_exp = (int8_t)((field->unit >> 8)  & 0x0F);
    if (len_exp  > 7) len_exp  -= 16;  /* sign-extend nibble */
    if (mass_exp > 7) mass_exp -= 16;

    /* CGS→SI correction: cm^n → m^n is *10^(-2n), g^n → kg^n is *10^(-3n) */
    int cgs_offset = (-2 * len_exp) + (-3 * mass_exp);

    /* Total exponent: unit_exponent + cgs_offset */
    int total_exp = (int)field->unit_exponent + cgs_offset;

    return (double)raw * pow(10.0, (double)total_exp);
}

void hid_report_map_free(hid_report_map_t *map)
{
    (void)map;
    /* Fixed-size arrays, nothing to free */
}
