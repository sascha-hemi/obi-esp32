/**
 * Open Battery Information - Seeed XIAO ESP32-S3 Firmware
 *
 * FUNCTIONAL REQUIREMENTS:
 * 1. Web server mode: Browser-based interface for standalone diagnostics
 * 2. Support Makita LXT 18V battery protocol via modified OneWire
 * 3. WiFi Access Point for direct device connection
 *
 * HARDWARE CONFIGURATION:
 * - Seeed XIAO ESP32-S3
 * - GPIO3: OneWire data line (4.7kΩ pull-up to 3.3V)  [use GPIO5+ on S3 Sense]
 * - GPIO4: Enable pin (4.7kΩ pull-up to 3.3V)         [use GPIO5+ on S3 Sense]
 * - Battery Pin 2: OneWire data
 * - Battery Pin 6: Enable (must be HIGH during communication)
 *
 * USAGE:
 * - Connect to WiFi network "OBI-Battery" (open, no password)
 * - Open http://obi-battery.local or http://192.168.4.1 in any browser
 * - On mobile: connecting to the network opens the page automatically (captive portal)
 *
 * AI-generated on 2025-12-16
 */

#include <Arduino.h>
#include "OneWire2.h"
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <DNSServer.h>
#include <ArduinoJson.h>
#include <ArduinoOTA.h>
#include "web_interface.h"
#if __has_include("secrets.h")
#include "secrets.h"
#endif

// Version
#define OBI_VERSION_MAJOR 1
#define OBI_VERSION_MINOR 0
#define OBI_VERSION_PATCH 0

// Pin definitions (can be overridden via build flags)
#ifndef ONEWIRE_PIN
#define ONEWIRE_PIN 3
#endif

#ifndef ENABLE_PIN
#define ENABLE_PIN 4
#endif

// WiFi Access Point configuration (override in secrets.h)
#ifndef AP_SSID
#define AP_SSID "OBI-Battery"
#endif

#ifndef AP_PASS
#define AP_PASS ""
#endif

// mDNS hostname (http://obi-battery.local)
#ifndef MDNS_HOSTNAME
#define MDNS_HOSTNAME "obi-battery"
#endif

// Nibble swap helper
#define SWAP_NIBBLES(x) (((x) & 0x0F) << 4 | ((x) & 0xF0) >> 4)

// ------------------------------------------------------------------
// Ring-buffer debug log (accessible via /api/log)
// ------------------------------------------------------------------
#define LOG_LINES     40
#define LOG_LINE_LEN  96

static char logBuffer[LOG_LINES][LOG_LINE_LEN];
static uint8_t logHead = 0;   // index of oldest entry
static uint8_t logCount = 0;  // how many entries are filled

void debugLog(const char *fmt, ...) {
    char tmp[LOG_LINE_LEN];
    va_list args;
    va_start(args, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, args);
    va_end(args);

    // Write to Serial as well
    Serial.println(tmp);

    // Store in ring buffer
    uint8_t idx = (logHead + logCount) % LOG_LINES;
    strncpy(logBuffer[idx], tmp, LOG_LINE_LEN - 1);
    logBuffer[idx][LOG_LINE_LEN - 1] = '\0';
    if (logCount < LOG_LINES) {
        logCount++;
    } else {
        logHead = (logHead + 1) % LOG_LINES; // overwrite oldest
    }
}

// Instantiate OneWire with template pin
OneWire<ONEWIRE_PIN> makita;

WebServer server(80);
DNSServer dnsServer;

// Battery data structure
struct BatteryData {
    bool valid;
    char model[16];
    bool locked;
    uint8_t batteryType;    // 0, 2, 3, 5, 6; 0xFF = unknown
    uint16_t chargeCount;
    char mfgDate[16];
    float capacity;
    uint8_t errorCode;
    uint8_t romId[8];
    float packVoltage;
    float cellVoltage[5];
    float cellDiff;
    float tempCell;
    float tempMosfet;
    uint8_t healthRating;   // 0-4; 0xFF = not available
    uint8_t stateOfCharge;  // 0-7 (type 0 only); 0xFF = not available
    float overdischargePct;
    float overloadPct;
};

BatteryData batteryData;

// Forward declarations
bool cmdAndRead33(byte *cmd, uint8_t cmd_len, byte *rsp, uint8_t rsp_len);
bool cmdAndReadCC(byte *cmd, uint8_t cmd_len, byte *rsp, uint8_t rsp_len);
bool cmdDirect(byte *cmd, uint8_t cmd_len, byte *rsp, uint8_t rsp_len);
void exitTestMode();
uint8_t detectBatteryType(byte msgByte17);
void setEnable(bool high);
void triggerPower();
bool readBatteryInfo();
bool readBatteryVoltages();
bool readBatteryModel();
bool readBatteryHealth();
bool readBatteryStateOfCharge();
bool readBatteryOverdischarge();
bool readBatteryOverload();
void setupWebServer();
void setupMDNS();
void setupOTA();
void handleApiLog();

// ------------------------------------------------------------------
// Setup
// ------------------------------------------------------------------
void setup() {
    Serial.begin(115200);

    // Configure pins
    pinMode(ENABLE_PIN, OUTPUT);
    digitalWrite(ENABLE_PIN, LOW);

    // Initialise battery data
    memset(&batteryData, 0, sizeof(batteryData));

    debugLog("=================================");
    debugLog("OBI ESP32-S3  - Open Battery Info");
    debugLog("=================================");
    debugLog("Version: %d.%d.%d", OBI_VERSION_MAJOR, OBI_VERSION_MINOR, OBI_VERSION_PATCH);
    debugLog("OneWire Pin: GPIO%d", ONEWIRE_PIN);
    debugLog("Enable Pin:  GPIO%d", ENABLE_PIN);

    debugLog("Mode: WiFi Access Point + Web Server");
    WiFi.softAP(AP_SSID, AP_PASS[0] ? AP_PASS : nullptr);
    IPAddress apIP = WiFi.softAPIP();
    debugLog("AP SSID: %s", AP_SSID);
    debugLog("AP IP:   %s", apIP.toString().c_str());

    // Captive portal: redirect all DNS queries to our IP
    dnsServer.start(53, "*", apIP);
    debugLog("Captive portal DNS started");

    setupOTA();
    setupWebServer();
    setupMDNS();

    debugLog("Ready.");
}

// ------------------------------------------------------------------
// Main Loop
// ------------------------------------------------------------------
void loop() {
    dnsServer.processNextRequest();
    ArduinoOTA.handle();
    server.handleClient();
}

// ------------------------------------------------------------------
// Enable pin control
// ------------------------------------------------------------------
void setEnable(bool high) {
    digitalWrite(ENABLE_PIN, high ? HIGH : LOW);
}

void triggerPower() {
    setEnable(false);
    delay(200);
    setEnable(true);
    delay(500);
}

// ------------------------------------------------------------------
// OneWire command functions
// ------------------------------------------------------------------

bool cmdAndRead33(byte *cmd, uint8_t cmd_len, byte *rsp, uint8_t rsp_len) {
    int i;

    for (int retry = 0; retry < 3; retry++) {
        if (!makita.reset()) {
            triggerPower();
            debugLog("OneWire reset failed (33), power-cycling");
            continue;
        }

        delayMicroseconds(310);
        makita.write(0x33);

        // Read 8-byte ROM ID
        for (i = 0; i < 8; i++) {
            delayMicroseconds(90);
            rsp[i] = makita.read();
        }

        // Write command
        for (i = 0; i < cmd_len; i++) {
            delayMicroseconds(90);
            makita.write(cmd[i]);
        }

        // Read response
        for (i = 8; i < rsp_len + 8; i++) {
            delayMicroseconds(90);
            rsp[i] = makita.read();
        }

        // Check if valid (not all 0xFF)
        bool valid = false;
        for (i = 0; i < rsp_len + 8; i++) {
            if (rsp[i] != 0xFF) {
                valid = true;
                break;
            }
        }
        if (valid) return true;
    }

    memset(rsp, 0xFF, rsp_len + 8);
    return false;
}

bool cmdAndReadCC(byte *cmd, uint8_t cmd_len, byte *rsp, uint8_t rsp_len) {
    int i;

    for (int retry = 0; retry < 3; retry++) {
        if (!makita.reset()) {
            triggerPower();
            debugLog("OneWire reset failed (CC), power-cycling");
            continue;
        }

        delayMicroseconds(310);
        makita.write(0xCC);

        // Write command
        for (i = 0; i < cmd_len; i++) {
            delayMicroseconds(90);
            makita.write(cmd[i]);
        }

        // Read response
        for (i = 0; i < rsp_len; i++) {
            delayMicroseconds(90);
            rsp[i] = makita.read();
        }

        // Check if valid
        bool valid = false;
        for (i = 0; i < rsp_len; i++) {
            if (rsp[i] != 0xFF) {
                valid = true;
                break;
            }
        }
        if (valid) return true;
    }

    memset(rsp, 0xFF, rsp_len);
    return false;
}

// ------------------------------------------------------------------
// High-level battery functions
// ------------------------------------------------------------------

// Send a command directly after reset, without any CC/33 ROM prefix.
// Used for type-5 (F0513) batteries whose commands have no ROM prefix.
bool cmdDirect(byte *cmd, uint8_t cmd_len, byte *rsp, uint8_t rsp_len) {
    int i;
    for (int retry = 0; retry < 3; retry++) {
        if (!makita.reset()) {
            triggerPower();
            debugLog("OneWire reset failed (direct), power-cycling");
            continue;
        }
        delayMicroseconds(310);
        for (i = 0; i < cmd_len; i++) {
            delayMicroseconds(90);
            makita.write(cmd[i]);
        }
        for (i = 0; i < rsp_len; i++) {
            delayMicroseconds(90);
            rsp[i] = makita.read();
        }
        bool valid = false;
        for (i = 0; i < rsp_len; i++) {
            if (rsp[i] != 0xFF) { valid = true; break; }
        }
        if (valid) return true;
    }
    memset(rsp, 0xFF, rsp_len);
    return false;
}

// Exit test mode (applies to type 0, 2, 3 batteries)
void exitTestMode() {
    byte cmd[] = {0xD9, 0xFF, 0xFF};
    byte rsp[1];
    cmdAndReadCC(cmd, 3, rsp, 1);
}

// Probe the battery type while enable pin is HIGH and romId is populated.
// msgByte17: raw byte 17 of the 32-byte basic info (cc aa 00) response.
uint8_t detectBatteryType(byte msgByte17) {
    byte rsp[17];

    // Type 6: 10-cell (BL36xx) - identified by flags byte in basic info
    if (msgByte17 == 30) return 6;

    // Type 5 (F0513): ROM ID byte 3 < 100
    if (batteryData.romId[3] < 100) return 5;

    // Type 0: supports cc dc 0b (17-byte response, last byte == 0x06)
    {
        byte cmd[] = {0xDC, 0x0B};
        if (cmdAndReadCC(cmd, 2, rsp, 17) && rsp[16] == 0x06) return 0;
    }

    // Type 2: after entering test mode, supports cc dc 0a
    {
        byte tmcmd[] = {0xD9, 0x96, 0xA5};
        cmdAndReadCC(tmcmd, 3, rsp, 1);
        byte cmd[] = {0xDC, 0x0A};
        bool is2 = cmdAndReadCC(cmd, 2, rsp, 17) && rsp[16] == 0x06;
        exitTestMode();
        if (is2) return 2;
    }

    // Type 3: supports cc d4 2c 00 02 (3-byte response, last byte == 0x06)
    {
        byte cmd[] = {0xD4, 0x2C, 0x00, 0x02};
        if (cmdAndReadCC(cmd, 4, rsp, 3) && rsp[2] == 0x06) return 3;
    }

    return 0xFF; // unknown
}

bool readBatteryInfo() {
    byte rsp[40];  // 8 bytes ROM ID + 32 bytes response
    byte cmd[] = {0xAA, 0x00};

    batteryData.batteryType = 0xFF;
    batteryData.healthRating = 0xFF;
    batteryData.stateOfCharge = 0xFF;
    batteryData.overdischargePct = 0.0f;
    batteryData.overloadPct = 0.0f;

    setEnable(true);
    delay(400);

    // Response is 32 bytes (nibble-oriented, LSN first)
    bool success = cmdAndRead33(cmd, 2, rsp, 32);

    if (success) {
        // Copy ROM ID
        memcpy(batteryData.romId, rsp, 8);

        // Message data starts at offset 8
        byte *msg = &rsp[8];

        // Manufacturing date from ROM ID bytes
        snprintf(batteryData.mfgDate, sizeof(batteryData.mfgDate),
                 "20%02d-%02d-%02d",
                 batteryData.romId[0],
                 batteryData.romId[1],
                 batteryData.romId[2]);

        // Cycle count: nybbles 52..55 (13-bit) at msg[26..27]
        // nybble 52 (bit 0 = cycle bit 12), 53 (bits 8..11), 54 (bits 4..7), 55 (bits 0..3)
        batteryData.chargeCount =
            ((uint16_t)(msg[26] & 0x01) << 12) |
            ((uint16_t)((msg[26] >> 4) & 0x0F) << 8) |
            ((uint16_t)(msg[27] & 0x0F) << 4) |
            ((uint16_t)((msg[27] >> 4) & 0x0F));

        // Locked state: verify checksums at nybbles 41, 42, 43.
        // checksum = sum(nybbles in range) & 0x0F
        // Also dead/locked if bytes msg[20] and msg[21] are all 0xFF.
        uint8_t cs0 = 0, cs1 = 0, cs2 = 0;
        for (int i = 0; i <= 15; i++) cs0 += (msg[i / 2] >> ((i % 2) * 4)) & 0x0F;
        for (int i = 16; i <= 31; i++) cs1 += (msg[i / 2] >> ((i % 2) * 4)) & 0x0F;
        for (int i = 32; i <= 40; i++) cs2 += (msg[i / 2] >> ((i % 2) * 4)) & 0x0F;
        batteryData.locked =
            (msg[20] == 0xFF && msg[21] == 0xFF) ||
            ((cs0 & 0x0F) != ((msg[20] >> 4) & 0x0F)) ||  // nybble 41
            ((cs1 & 0x0F) != (msg[21] & 0x0F)) ||          // nybble 42
            ((cs2 & 0x0F) != ((msg[21] >> 4) & 0x0F));     // nybble 43

        // Failure/error code: nybble 40 = low nibble of msg[20]
        batteryData.errorCode = msg[20] & 0x0F;

        // Capacity: nybbles 32/33 at msg[16], swap nibbles to assemble byte
        batteryData.capacity = SWAP_NIBBLES(msg[16]) / 10.0f;

        // Probe battery type while enable pin is still HIGH
        batteryData.batteryType = detectBatteryType(msg[17]);
        debugLog("Battery type detected: %d", batteryData.batteryType);

        batteryData.valid = true;
    }

    setEnable(false);
    return success;
}

bool readBatteryModel() {
    byte rsp[16];
    byte cmd[] = {0xDC, 0x0C};

    setEnable(true);
    delay(400);

    // Response is 16 bytes: null-terminated ASCII model string
    bool success = cmdAndReadCC(cmd, 2, rsp, 16);

    if (success && rsp[0] != 0xFF) {
        // Copy model string and ensure null-termination
        memcpy(batteryData.model, rsp, 15);
        batteryData.model[15] = '\0';
    } else {
        // Try F0513 method for older batteries
        makita.reset();
        delayMicroseconds(400);
        makita.write(0xCC);
        delayMicroseconds(90);
        makita.write(0x99);
        delay(400);
        makita.reset();
        delayMicroseconds(400);
        makita.write(0x31);
        delayMicroseconds(90);
        byte b1 = makita.read();
        delayMicroseconds(90);
        byte b0 = makita.read();

        if (b0 != 0xFF && b1 != 0xFF) {
            snprintf(batteryData.model, sizeof(batteryData.model), "BL%02X%02X", b1, b0);
            success = true;
        }
    }

    setEnable(false);
    return success;
}

bool readBatteryVoltages() {
    byte rsp[16];
    // Protocol: cc d7 00 00 0c -> 13 bytes (pack + 5 cells in mV LE + 0x06)
    byte cmd[] = {0xD7, 0x00, 0x00, 0x0C};

    setEnable(true);
    delay(400);

    bool success = cmdAndReadCC(cmd, 4, rsp, 13);

    if (success && rsp[0] != 0xFF) {
        batteryData.packVoltage = ((uint16_t)rsp[0] | ((uint16_t)rsp[1] << 8)) / 1000.0f;

        float maxV = 0, minV = 5;
        for (int i = 0; i < 5; i++) {
            float v = ((uint16_t)rsp[2 + i*2] | ((uint16_t)rsp[3 + i*2] << 8)) / 1000.0f;
            batteryData.cellVoltage[i] = v;
            if (v > maxV) maxV = v;
            if (v < minV) minV = v;
        }
        batteryData.cellDiff = maxV - minV;

        // Temperature is a separate command: cc d7 0e 00 02 -> 3 bytes, unit: 1/10 K
        byte tcmd[] = {0xD7, 0x0E, 0x00, 0x02};
        byte trsp[3];
        if (cmdAndReadCC(tcmd, 4, trsp, 3) && trsp[2] == 0x06) {
            uint16_t tempRaw = (uint16_t)trsp[0] | ((uint16_t)trsp[1] << 8);
            batteryData.tempCell = tempRaw / 10.0f - 273.15f;
        }
        batteryData.tempMosfet = 0;  // No separate MOSFET temp in type 0/2/3 protocol
    } else {
        // Try type-5 (F0513) method:
        // Cell voltage commands 31..35 are sent directly (no CC/33 ROM prefix)
        byte vcmd[1];
        bool f0513_ok = true;

        for (int i = 0; i < 5 && f0513_ok; i++) {
            vcmd[0] = 0x31 + i;
            if (cmdDirect(vcmd, 1, rsp, 2)) {
                batteryData.cellVoltage[i] = ((uint16_t)rsp[0] | ((uint16_t)rsp[1] << 8)) / 1000.0f;
            } else {
                f0513_ok = false;
            }
        }

        if (f0513_ok) {
            float sum = 0, maxV = 0, minV = 5;
            for (int i = 0; i < 5; i++) {
                sum += batteryData.cellVoltage[i];
                if (batteryData.cellVoltage[i] > maxV) maxV = batteryData.cellVoltage[i];
                if (batteryData.cellVoltage[i] < minV) minV = batteryData.cellVoltage[i];
            }
            batteryData.packVoltage = sum;
            batteryData.cellDiff = maxV - minV;

            // Temperature: cc 52 -> 2 bytes, unit: 1/10 K
            vcmd[0] = 0x52;
            if (cmdAndReadCC(vcmd, 1, rsp, 2)) {
                uint16_t tempRaw = (uint16_t)rsp[0] | ((uint16_t)rsp[1] << 8);
                batteryData.tempCell = tempRaw / 10.0f - 273.15f;
                batteryData.tempMosfet = 0;
            }
            success = true;
        }
    }

    setEnable(false);
    return success;
}

// ------------------------------------------------------------------
// Additional battery diagnostics (type-dependent)
// ------------------------------------------------------------------

// Health rating 0-4. Requires readBatteryInfo() to have been called first.
bool readBatteryHealth() {
    byte rsp[3];
    bool success = false;

    setEnable(true);
    delay(400);

    switch (batteryData.batteryType) {
        case 0: {
            byte cmd[] = {0xD4, 0x50, 0x01, 0x02};
            success = cmdAndReadCC(cmd, 4, rsp, 3) && rsp[2] == 0x06;
            break;
        }
        case 2: {
            byte cmd[] = {0xD6, 0x04, 0x05, 0x02};
            success = cmdAndReadCC(cmd, 4, rsp, 3) && rsp[2] == 0x06;
            break;
        }
        case 3: {
            byte cmd[] = {0xD6, 0x38, 0x02, 0x02};
            success = cmdAndReadCC(cmd, 4, rsp, 3) && rsp[2] == 0x06;
            break;
        }
        default:
            setEnable(false);
            return false;
    }

    if (success) {
        uint16_t healthRaw = (uint16_t)rsp[0] | ((uint16_t)rsp[1] << 8);
        float capacityRaw = batteryData.capacity * 10.0f;
        float ratio = (capacityRaw > 0.0f) ? (float)healthRaw / capacityRaw : 0.0f;
        if (ratio > 80.0f) {
            batteryData.healthRating = 4;
        } else {
            int h = (int)(ratio / 10.0f) - 5;
            batteryData.healthRating = (uint8_t)(h < 0 ? 0 : h);
        }
    }

    setEnable(false);
    return success;
}

// State of charge 0-7 (type 0 only). Requires readBatteryInfo() first.
bool readBatteryStateOfCharge() {
    if (batteryData.batteryType != 0) return false;

    byte rsp[5];
    byte cmd[] = {0xD7, 0x19, 0x00, 0x04};

    setEnable(true);
    delay(400);

    bool success = cmdAndReadCC(cmd, 4, rsp, 5) && rsp[4] == 0x06;

    if (success) {
        uint32_t chargeLevel = (uint32_t)rsp[0] | ((uint32_t)rsp[1] << 8) |
                               ((uint32_t)rsp[2] << 16) | ((uint32_t)rsp[3] << 24);
        float capacityRaw = batteryData.capacity * 10.0f;
        float ratio = (capacityRaw > 0.0f) ? (float)chargeLevel / capacityRaw / 2880.0f : 0.0f;
        if (ratio == 0.0f) {
            batteryData.stateOfCharge = 0;
        } else if (ratio < 10.0f) {
            batteryData.stateOfCharge = 1;
        } else {
            uint8_t s = (uint8_t)(ratio / 10.0f);
            batteryData.stateOfCharge = (s > 7) ? 7 : s;
        }
    }

    setEnable(false);
    return success;
}

// Overdischarge event count as percentage. Requires readBatteryInfo() first.
bool readBatteryOverdischarge() {
    byte rsp[2];
    bool success = false;

    setEnable(true);
    delay(400);

    switch (batteryData.batteryType) {
        case 0: {
            byte cmd[] = {0xD4, 0xBA, 0x00, 0x01};
            success = cmdAndReadCC(cmd, 4, rsp, 2) && rsp[1] == 0x06;
            break;
        }
        case 2: {
            byte cmd[] = {0xD6, 0x8D, 0x05, 0x01};
            success = cmdAndReadCC(cmd, 4, rsp, 2) && rsp[1] == 0x06;
            break;
        }
        case 3: {
            byte cmd[] = {0xD6, 0x09, 0x03, 0x01};
            success = cmdAndReadCC(cmd, 4, rsp, 2) && rsp[1] == 0x06;
            break;
        }
        default:
            setEnable(false);
            return false;
    }

    if (success) {
        uint8_t count = rsp[0];
        if (count > 0 && batteryData.chargeCount > 0) {
            batteryData.overdischargePct = 4.0f + 100.0f * count / batteryData.chargeCount;
        } else {
            batteryData.overdischargePct = 0.0f;
        }
    }

    setEnable(false);
    return success;
}

// Overload event counters as percentage. Requires readBatteryInfo() first.
bool readBatteryOverload() {
    byte rsp[8];
    bool success = false;
    uint32_t sum = 0;

    setEnable(true);
    delay(400);

    switch (batteryData.batteryType) {
        case 0: {
            // Three 10-bit counters packed in bytes 0-6
            byte cmd[] = {0xD4, 0x8D, 0x00, 0x07};
            success = cmdAndReadCC(cmd, 4, rsp, 8) && rsp[7] == 0x06;
            if (success) {
                uint16_t cA = ((uint16_t)rsp[1] << 2) | ((rsp[0] >> 6) & 0x03);
                uint16_t cB = ((uint16_t)(rsp[4] & 0x03) << 8) | rsp[3];
                uint16_t cC = ((uint16_t)(rsp[6] & 0x3F) << 4) | ((rsp[5] >> 4) & 0x0F);
                sum = (uint32_t)cA + cB + cC;
            }
            break;
        }
        case 2: {
            // Five single-byte counters
            byte cmd[] = {0xD6, 0x5F, 0x05, 0x07};
            success = cmdAndReadCC(cmd, 4, rsp, 8) && rsp[7] == 0x06;
            if (success) {
                sum = (uint32_t)rsp[0] + rsp[2] + rsp[3] + rsp[5] + rsp[6];
            }
            break;
        }
        case 3: {
            // Three single-byte counters
            byte cmd[] = {0xD6, 0x5B, 0x03, 0x04};
            success = cmdAndReadCC(cmd, 4, rsp, 6) && rsp[5] == 0x06;
            if (success) {
                sum = (uint32_t)rsp[0] + rsp[2] + rsp[3];
            }
            break;
        }
        default:
            setEnable(false);
            return false;
    }

    if (success) {
        if (sum > 0 && batteryData.chargeCount > 0) {
            batteryData.overloadPct = 4.0f + 100.0f * (float)sum / batteryData.chargeCount;
        } else {
            batteryData.overloadPct = 0.0f;
        }
    }

    setEnable(false);
    return success;
}

// ------------------------------------------------------------------
// OTA Updates
// ------------------------------------------------------------------

void setupMDNS() {
    if (MDNS.begin(MDNS_HOSTNAME)) {
        MDNS.addService("http", "tcp", 80);
        debugLog("mDNS: http://%s.local", MDNS_HOSTNAME);
    } else {
        debugLog("mDNS failed");
    }
}

void setupOTA() {
    ArduinoOTA.setHostname("obi-esp32");

    ArduinoOTA.onStart([]() {
        String type = (ArduinoOTA.getCommand() == U_FLASH) ? "sketch" : "filesystem";
        debugLog("OTA Start: %s", type.c_str());
    });

    ArduinoOTA.onEnd([]() {
        debugLog("OTA End");
    });

    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
        debugLog("OTA Progress: %u%%", progress / (total / 100));
    });

    ArduinoOTA.onError([](ota_error_t error) {
        const char *msg = "Unknown";
        if (error == OTA_AUTH_ERROR)    msg = "Auth Failed";
        else if (error == OTA_BEGIN_ERROR)   msg = "Begin Failed";
        else if (error == OTA_CONNECT_ERROR) msg = "Connect Failed";
        else if (error == OTA_RECEIVE_ERROR) msg = "Receive Failed";
        else if (error == OTA_END_ERROR)     msg = "End Failed";
        debugLog("OTA Error: %s", msg);
    });

    ArduinoOTA.begin();
    debugLog("OTA Ready");
}

// ------------------------------------------------------------------
// Web Server
// ------------------------------------------------------------------

void handleRoot() {
    server.send_P(200, "text/html", INDEX_HTML);
}

void handleApiRead() {
    readBatteryInfo();
    readBatteryModel();
    readBatteryVoltages();
    readBatteryHealth();
    readBatteryStateOfCharge();
    readBatteryOverdischarge();
    readBatteryOverload();

    JsonDocument doc;
    doc["success"] = batteryData.valid;
    doc["model"] = batteryData.model;
    doc["locked"] = batteryData.locked;
    doc["chargeCount"] = batteryData.chargeCount;
    doc["mfgDate"] = batteryData.mfgDate;
    doc["capacity"] = batteryData.capacity;
    doc["errorCode"] = batteryData.errorCode;
    doc["packVoltage"] = batteryData.packVoltage;
    doc["cell1"] = batteryData.cellVoltage[0];
    doc["cell2"] = batteryData.cellVoltage[1];
    doc["cell3"] = batteryData.cellVoltage[2];
    doc["cell4"] = batteryData.cellVoltage[3];
    doc["cell5"] = batteryData.cellVoltage[4];
    doc["cellDiff"] = batteryData.cellDiff;
    doc["tempCell"] = batteryData.tempCell;
    doc["tempMosfet"] = batteryData.tempMosfet;
    doc["batteryType"] = batteryData.batteryType;
    doc["healthRating"] = batteryData.healthRating;
    doc["stateOfCharge"] = batteryData.stateOfCharge;
    doc["overdischargePct"] = batteryData.overdischargePct;
    doc["overloadPct"] = batteryData.overloadPct;

    String response;
    serializeJson(doc, response);
    server.send(200, "application/json", response);
}

void handleApiVoltages() {
    bool success = readBatteryVoltages();

    JsonDocument doc;
    doc["success"] = success;
    doc["packVoltage"] = batteryData.packVoltage;
    doc["cell1"] = batteryData.cellVoltage[0];
    doc["cell2"] = batteryData.cellVoltage[1];
    doc["cell3"] = batteryData.cellVoltage[2];
    doc["cell4"] = batteryData.cellVoltage[3];
    doc["cell5"] = batteryData.cellVoltage[4];
    doc["cellDiff"] = batteryData.cellDiff;
    doc["tempCell"] = batteryData.tempCell;
    doc["tempMosfet"] = batteryData.tempMosfet;

    String response;
    serializeJson(doc, response);
    server.send(200, "application/json", response);
}

void handleApiLeds() {
    bool state = server.hasArg("state") && server.arg("state") == "1";

    setEnable(true);
    delay(400);

    // Enter test mode: cc d9 96 a5 -> 1 byte response (0x06)
    byte cmd1[] = {0xD9, 0x96, 0xA5};
    byte rsp[4];
    cmdAndReadCC(cmd1, 3, rsp, 1);

    // LED command: cc da <state> -> 1 byte response (0x06)
    byte cmd2[] = {0xDA, (byte)(state ? 0x31 : 0x34)};
    cmdAndReadCC(cmd2, 2, rsp, 1);

    // Exit test mode before releasing enable
    exitTestMode();

    setEnable(false);

    server.send(200, "application/json", "{\"success\":true}");
}

void handleApiReset() {
    setEnable(true);
    delay(400);

    // Enter test mode: cc d9 96 a5 -> 1 byte response (0x06)
    byte cmd1[] = {0xD9, 0x96, 0xA5};
    byte rsp[4];
    cmdAndReadCC(cmd1, 3, rsp, 1);

    // Reset error: cc da 04 -> 1 byte response (0x06)
    byte cmd2[] = {0xDA, 0x04};
    cmdAndReadCC(cmd2, 2, rsp, 1);

    // Exit test mode before releasing enable
    exitTestMode();

    setEnable(false);

    server.send(200, "application/json", "{\"success\":true}");
}

void handleApiLog() {
    // Return log entries as a JSON array, oldest first
    String json = "[";
    for (uint8_t i = 0; i < logCount; i++) {
        uint8_t idx = (logHead + i) % LOG_LINES;
        if (i > 0) json += ",";
        json += "\"";
        for (const char *p = logBuffer[idx]; *p; p++) {
            if (*p == '"') json += "\\\"";
            else if (*p == '\\') json += "\\\\";
            else json += *p;
        }
        json += "\"";
    }
    json += "]";
    server.send(200, "application/json", json);
}

void setupWebServer() {
    // Captive portal detection endpoints used by Android, iOS, Windows, macOS
    server.on("/generate_204",        HTTP_GET, handleRoot); // Android
    server.on("/gen_204",             HTTP_GET, handleRoot); // Android old
    server.on("/hotspot-detect.html", HTTP_GET, handleRoot); // Apple
    server.on("/library/test/success.html", HTTP_GET, handleRoot); // Apple old
    server.on("/connecttest.txt",     HTTP_GET, handleRoot); // Windows
    server.on("/redirect",            HTTP_GET, handleRoot); // Windows
    server.on("/ncsi.txt",            HTTP_GET, handleRoot); // Windows NCSI
    server.onNotFound(handleRoot); // catch-all: redirect everything else

    server.on("/",              HTTP_GET, handleRoot);
    server.on("/api/read",     HTTP_GET, handleApiRead);
    server.on("/api/voltages", HTTP_GET, handleApiVoltages);
    server.on("/api/leds",     HTTP_GET, handleApiLeds);
    server.on("/api/reset",    HTTP_GET, handleApiReset);
    server.on("/api/log",      HTTP_GET, handleApiLog);

    server.begin();
    Serial.println("Web server started on port 80");
}
