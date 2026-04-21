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

/* Efficiency raw value → string.
 * Positive values: percentage (raw / 128).
 * Negative values: reason string (NotAvailable, LoadTooLow, etc.). */
const char *ups_efficiency_str(int16_t raw, char *buf, size_t len);

/* Error bitfield decoders — return number of active errors, write string
 * pointers into the provided array. Caller must not free the strings. */
int ups_decode_general_errors(uint16_t raw, const char **out, int max);
int ups_decode_power_errors(uint32_t raw, const char **out, int max);
int ups_decode_battery_errors(uint16_t raw, const char **out, int max);

#endif
