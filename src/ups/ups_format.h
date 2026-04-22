#ifndef UPS_FORMAT_H
#define UPS_FORMAT_H

#include <stdint.h>
#include <stddef.h>

/* Human-readable strings for UPS status fields.
 * Shared across all APC Modbus models — no driver dependency. */

/* Transfer reason enum → string. Returns "Unknown" for out-of-range values. */
const char *ups_transfer_reason_str(uint16_t reason);

/* Status bitfield → space-separated flag names.
 * Writes into caller's buffer, returns buf. */
const char *ups_status_str(uint32_t status, char *buf, size_t len);

/* Efficiency → human-readable string.
 *
 * When reason == UPS_EFF_OK, renders "99.9%". Otherwise renders the reason
 * name ("LoadTooLow", "OnBattery", etc.). Writes into caller's buffer and
 * returns buf. The reason parameter is typed as int so this header stays
 * free of ups.h's enum; callers pass ups_data.efficiency_reason directly. */
const char *ups_efficiency_str(int reason, double pct, char *buf, size_t len);

/* Error bitfield decoders — return number of active errors, write string
 * pointers into the provided array. Caller must not free the strings. */
int ups_decode_general_errors(uint16_t raw, const char **out, int max);
int ups_decode_power_errors(uint32_t raw, const char **out, int max);
int ups_decode_battery_errors(uint16_t raw, const char **out, int max);

#endif
