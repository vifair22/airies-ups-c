#include "ups/ups_format.h"
#include "ups/ups.h"

#include <stdio.h>
#include <string.h>

/* --- Transfer reason --- */

static const char *transfer_reasons[] = {
    "SystemInitialization", "HighInputVoltage", "LowInputVoltage",
    "DistortedInput", "RapidChangeOfInputVoltage", "HighInputFrequency",
    "LowInputFrequency", "FreqAndOrPhaseDifference", "AcceptableInput",
    "AutomaticTest", "TestEnded", "LocalUICommand", "ProtocolCommand",
    "LowBatteryVoltage", "GeneralError", "PowerSystemError",
    "BatterySystemError", "ErrorCleared", "AutomaticRestart",
    "DistortedInverterOutput", "InverterOutputAcceptable", "EPOInterface",
    "InputPhaseDeltaOutOfRange", "InputNeutralNotConnected", "ATSTransfer",
    "ConfigurationChange", "AlertAsserted", "AlertCleared",
    "PlugRatingExceeded", "OutletGroupStateChange", "FailureBypassExpired",
};

const char *ups_transfer_reason_str(uint16_t reason)
{
    if (reason < sizeof(transfer_reasons) / sizeof(transfer_reasons[0]))
        return transfer_reasons[reason];
    return "Unknown";
}

/* --- Status string --- */

const char *ups_status_str(uint32_t status, char *buf, size_t len)
{
    buf[0] = '\0';
    struct { uint32_t bit; const char *name; } flags[] = {
        { UPS_ST_ONLINE,         "Online" },
        { UPS_ST_ON_BATTERY,     "OnBattery" },
        { UPS_ST_BYPASS,         "Bypass" },
        { UPS_ST_OUTPUT_OFF,     "OutputOff" },
        { UPS_ST_FAULT,          "Fault" },
        { UPS_ST_INPUT_BAD,      "InputBad" },
        { UPS_ST_TEST,           "SelfTest" },
        { UPS_ST_PENDING_ON,     "PendingOn" },
        { UPS_ST_SHUT_PENDING,   "ShutdownPending" },
        { UPS_ST_COMMANDED,      "Commanded" },
        { UPS_ST_HE_MODE,        "HighEfficiency" },
        { UPS_ST_INFO_ALERT,     "InfoAlert" },
        { UPS_ST_FAULT_STATE,    "FaultState" },
        { UPS_ST_MAINS_BAD,      "MainsBad" },
        { UPS_ST_FAULT_RECOVERY, "FaultRecovery" },
        { UPS_ST_OVERLOAD,       "Overload" },
    };
    for (size_t i = 0; i < sizeof(flags) / sizeof(flags[0]); i++) {
        if (status & flags[i].bit) {
            if (buf[0]) strncat(buf, " ", len - strlen(buf) - 1);
            strncat(buf, flags[i].name, len - strlen(buf) - 1);
        }
    }
    if (!buf[0]) strncpy(buf, "Unknown", len - 1);
    return buf;
}

/* --- Efficiency string --- */

const char *ups_efficiency_str(int reason, double pct, char *buf, size_t len)
{
    /* Index 0 is UPS_EFF_OK; rows 1..8 map to reason codes. Keep in sync
     * with ups_eff_reason_t in ups.h — ordering is load-bearing. */
    static const char *reasons[] = {
        NULL,  /* UPS_EFF_OK → handled below */
        "NotAvailable", "LoadTooLow", "OutputOff", "OnBattery",
        "InBypass", "BatteryCharging", "PoorACInput", "BatteryDisconnected",
    };
    if (reason == 0) {
        snprintf(buf, len, "%.1f%%", pct);
    } else if (reason > 0 && reason < (int)(sizeof(reasons) / sizeof(reasons[0]))) {
        snprintf(buf, len, "%s", reasons[reason]);
    } else {
        snprintf(buf, len, "Unknown(%d)", reason);
    }
    return buf;
}

/* --- Error bitfield decoders --- */

typedef struct { uint32_t bit; const char *name; } bit_label_t;

static int decode_bits(uint32_t raw, const char **out, int max,
                       const bit_label_t *table, size_t table_len)
{
    int n = 0;
    for (size_t i = 0; i < table_len && n < max; i++) {
        if (raw & table[i].bit)
            out[n++] = table[i].name;
    }
    return n;
}

int ups_decode_general_errors(uint16_t raw, const char **out, int max)
{
    static const bit_label_t flags[] = {
        { UPS_GENERR_SITE_WIRING,   "SiteWiring" },
        { UPS_GENERR_EEPROM,        "EEPROM" },
        { UPS_GENERR_AD_CONV,       "ADConverter" },
        { UPS_GENERR_LOGIC_PSU,     "LogicPowerSupply" },
        { UPS_GENERR_INTERNAL_COMM, "InternalComm" },
        { UPS_GENERR_UI_BUTTON,     "UIButton" },
        { UPS_GENERR_EPO_ACTIVE,    "EPOActive" },
        { UPS_GENERR_FW_MISMATCH,   "FirmwareMismatch" },
    };
    return decode_bits(raw, out, max, flags,
                       sizeof(flags) / sizeof(flags[0]));
}

int ups_decode_power_errors(uint32_t raw, const char **out, int max)
{
    static const bit_label_t flags[] = {
        { UPS_PWRERR_OVERLOAD,      "Overload" },
        { UPS_PWRERR_SHORT_CIRCUIT, "ShortCircuit" },
        { UPS_PWRERR_OVERVOLTAGE,   "Overvoltage" },
        { UPS_PWRERR_OVERTEMP,      "Overtemperature" },
        { UPS_PWRERR_FAN,           "Fan" },
        { UPS_PWRERR_INVERTER,      "Inverter" },
    };
    return decode_bits(raw, out, max, flags,
                       sizeof(flags) / sizeof(flags[0]));
}

int ups_decode_battery_errors(uint16_t raw, const char **out, int max)
{
    static const bit_label_t flags[] = {
        { UPS_BATERR_DISCONNECTED,  "Disconnected" },
        { UPS_BATERR_OVERVOLTAGE,   "Overvoltage" },
        { UPS_BATERR_REPLACE,       "NeedsReplacement" },
        { UPS_BATERR_OVERTEMP_CRIT, "OvertemperatureCritical" },
        { UPS_BATERR_CHARGER,       "ChargerFault" },
        { UPS_BATERR_TEMP_SENSOR,   "TempSensorFault" },
        { UPS_BATERR_OVERTEMP_WARN, "OvertemperatureWarning" },
        { UPS_BATERR_GENERAL,       "GeneralError" },
        { UPS_BATERR_COMM,          "CommunicationError" },
    };
    return decode_bits(raw, out, max, flags,
                       sizeof(flags) / sizeof(flags[0]));
}
