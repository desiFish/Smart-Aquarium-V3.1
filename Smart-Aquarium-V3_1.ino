/*
 * Smart-Aquarium-V3.1 - ESP8266-based Interactive Networked Aquarium
 * Copyright (C) 2025 desiFish (https://github.com/desiFish)
 *
 * This program is free software under GPL v3: you can:
 * - Use it for any purpose
 * - Study and modify the code
 * - Share any modifications under the same license
 *
 * Key requirements:
 * - Keep all copyright notices
 * - Include original source code when distributing
 * - Share modifications under GPL v3
 *
 * Full license at: https://www.gnu.org/licenses/gpl-3.0.txt
 * Project repo: https://github.com/desiFish/Smart-Aquarium-V3.1
 */

#include <ESP8266WiFi.h>
#include <ESPAsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <ElegantOTA.h>

// NTP and RTC
#include <NTPClient.h>
#include <WiFiUdp.h>
#include "RTClib.h"
// RGB LED control
#include <Adafruit_NeoPixel.h>

// Software version
#define SWVersion "v1.1.5"

// Number of leds
#define NUMPIXELS 1
// LED Pin
#define PIN 15
// Buzzer pin
#define BUZZER_PIN 2
// Define the leds
Adafruit_NeoPixel pixels(NUMPIXELS, PIN, NEO_GRB + NEO_KHZ800);

RTC_DS3231 rtc;
// RTC_DS1307 rtc; // Uncomment if using DS1307 RTC
WiFiUDP ntpUDP;

// --- NTP/Timezone config variables ---
String ntpServer;
int timezoneOffset;
String timezoneString;

// NTP client instance (will be re-created if settings change)
NTPClient timeClient(ntpUDP, ntpServer.c_str(), timezoneOffset); // check Readme for more details

// --- NTP config persistence ---
void saveNtpConfigToFS()
{
    JsonDocument doc;
    doc["ntpServer"] = ntpServer;
    doc["timezoneOffset"] = timezoneOffset;
    doc["timezoneString"] = timezoneString;
    File f = LittleFS.open("/ntp.json", "w");
    if (f)
    {
        serializeJson(doc, f);
        f.close();
    }
}

void loadNtpConfigFromFS()
{
    Serial.println("[DEBUG] loadNtpConfigFromFS called");
    if (!LittleFS.exists("/ntp.json"))
    {
        Serial.println("[DEBUG] /ntp.json not found, using defaults");
        ntpServer = "in.pool.ntp.org";
        timezoneOffset = 19800;
        timezoneString = "+5:30";
        return;
    }
    File f = LittleFS.open("/ntp.json", "r");
    if (!f)
    {
        Serial.println("[DEBUG] Failed to open /ntp.json, using defaults");
        return;
    }
    Serial.println("[DEBUG] /ntp.json opened successfully");
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (!err)
    {
        Serial.println("[DEBUG] NTP config loaded from JSON");
        ntpServer = doc["ntpServer"] | "in.pool.ntp.org";
        timezoneOffset = doc["timezoneOffset"] | 19800;
        timezoneString = doc["timezoneString"] | "+5:30";
    }
    else
    {
        Serial.print("[DEBUG] NTP config JSON error: ");
        Serial.println(err.c_str());
    }
}

// WiFi config persistence
void saveWifiConfigToFS(const String &ssidVal, const String &passwordVal,
                        const String &ip, const String &gateway, const String &subnet,
                        const String &dns1, const String &dns2, bool useStaticIp)
{
    Serial.println("[DEBUG] saveWifiConfigToFS called");
    JsonDocument doc;
    doc["ssid"] = ssidVal;
    doc["password"] = passwordVal;
    if (ip.length())
        doc["ip"] = ip;
    if (gateway.length())
        doc["gateway"] = gateway;
    if (subnet.length())
        doc["subnet"] = subnet;
    if (dns1.length())
        doc["dns1"] = dns1;
    if (dns2.length())
        doc["dns2"] = dns2;
    doc["useStaticIp"] = useStaticIp;
    File f = LittleFS.open("/wifi.json", "w");
    if (f)
    {
        Serial.println("[DEBUG] /wifi.json opened for writing");
        serializeJson(doc, f);
        f.close();
        Serial.println("[DEBUG] /wifi.json written and closed");
    }
    else
    {
        Serial.println("[DEBUG] Failed to open /wifi.json for writing");
    }
}

void loadWifiConfigFromFS(String &ssidVal, String &passwordVal,
                          String *ip, String *gateway, String *subnet,
                          String *dns1, String *dns2, bool *useStaticIp)
{
    Serial.println("[DEBUG] loadWifiConfigFromFS called");
    if (!LittleFS.exists("/wifi.json"))
    {
        Serial.println("[DEBUG] /wifi.json not found");
        return;
    }
    File f = LittleFS.open("/wifi.json", "r");
    if (!f)
    {
        Serial.println("[DEBUG] Failed to open /wifi.json");
        return;
    }
    Serial.println("[DEBUG] /wifi.json opened successfully");
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (!err)
    {
        Serial.println("[DEBUG] WiFi config loaded from JSON");
        ssidVal = doc["ssid"] | ssidVal;
        passwordVal = doc["password"] | passwordVal;
        if (ip)
            *ip = doc["ip"] | "";
        if (gateway)
            *gateway = doc["gateway"] | "";
        if (subnet)
            *subnet = doc["subnet"] | "";
        if (dns1)
            *dns1 = doc["dns1"] | "";
        if (dns2)
            *dns2 = doc["dns2"] | "";
        if (useStaticIp)
            *useStaticIp = doc["useStaticIp"] | false;
    }
    else
    {
        Serial.print("[DEBUG] WiFi config JSON error: ");
        Serial.println(err.c_str());
    }
}

// WiFi credentials
String wifiSsid;
String wifiPassword;
String wifiIp, wifiGateway, wifiSubnet, wifiDns1, wifiDns2;
bool wifiUseStaticIp = false;

AsyncWebServer server(80);

unsigned long ota_progress_millis = 0;
byte otaActive = 0; // 0 = not in OTA, 1 = OTA in progress
// --- Beep function ---
void beep(unsigned int durationMs = 100, unsigned int count = 1, unsigned int pauseMs = 100)
{
    pinMode(BUZZER_PIN, OUTPUT);
    for (unsigned int i = 0; i < count; i++)
    {
        digitalWrite(BUZZER_PIN, HIGH);
        delay(durationMs);
        digitalWrite(BUZZER_PIN, LOW);
        if (i < count - 1)
        {
            delay(pauseMs);
        }
    }
}

void onOTAStart()
{
    Serial.println("OTA update started!");
    pixels.setBrightness(100);
    pixels.setPixelColor(0, pixels.Color(200, 0, 0));
    pixels.show();
    otaActive = 1; // Mark OTA as active
}

void onOTAProgress(size_t current, size_t final)
{
    static byte flag = 1;
    if (millis() - ota_progress_millis > 970)
    {
        ota_progress_millis = millis();
        Serial.printf("OTA Progress Current: %u bytes, Final: %u bytes\n", current, final);
        if (flag)
        {
            pixels.setPixelColor(0, pixels.Color(0, 0, 100));
            pixels.show();
        }
        else
        {
            pixels.setPixelColor(0, pixels.Color(0, 0, 0));
            pixels.show();
        }
        flag = !flag; // Toggle flag for alternating colors
    }
}

void onOTAEnd(bool success)
{
    if (success)
    {
        Serial.println("OTA update finished successfully!");
        pixels.setPixelColor(0, pixels.Color(100, 100, 100));
    }
    else
    {
        Serial.println("There was an error during OTA update!");
        pixels.setPixelColor(0, pixels.Color(200, 0, 0));
    }
    pixels.show();
}

// ESP8266 gpio pins for relays
const byte RELAY_PINS[] = {0, 12, 13, 14};
const byte NUM_RELAYS = 4;

class Relay
{
private:
    bool isOn;
    int onTime;
    int offTime;
    String mode; // "manual", "auto", or "timer"
    bool isDisabled;
    byte pin;
    String name;
    int relayNum;
    unsigned long timerStart;
    int timerDuration;
    bool timerActive;

    unsigned long toggleStart;
    int toggleOnMinutes;
    int toggleOffMinutes;
    bool toggleActive;
    void loadFromFS()
    {
        Serial.printf("[DEBUG] Relay %d: loadFromFS called\n", relayNum);
        File configFile = LittleFS.open("/config/relay" + String(relayNum) + ".json", "r");
        if (!configFile)
        {
            Serial.printf("[DEBUG] Relay %d: Config file missing, using defaults\n", relayNum);
            // Use defaults if no config file exists
            name = "Relay " + String(relayNum);
            mode = "manual";
            isOn = false;
            isDisabled = true; // Default to disabled
            onTime = 0;
            offTime = 0;
            // Toggle defaults
            toggleOnMinutes = 0;
            toggleOffMinutes = 0;
            toggleActive = false;
            toggleStart = 0;
        }
        else
        {
            Serial.printf("[DEBUG] Relay %d: Config file opened\n", relayNum);
            String jsonStr = configFile.readString();
            configFile.close();

            JsonDocument doc;
            DeserializationError error = deserializeJson(doc, jsonStr);

            if (!error)
            {
                Serial.printf("[DEBUG] Relay %d: Config loaded from JSON\n", relayNum);
                isOn = doc["isOn"] | false;
                isDisabled = doc["isDisabled"] | false;
                onTime = doc["onTime"] | 0;
                offTime = doc["offTime"] | 0;
                name = doc["name"] | ("Relay " + String(relayNum));
                mode = doc["mode"] | "manual";
                // Restore toggle mode settings
                toggleOnMinutes = doc["toggleOnMinutes"] | 0;
                toggleOffMinutes = doc["toggleOffMinutes"] | 0;
                toggleActive = doc["toggleActive"] | false;
                toggleStart = doc["toggleStart"] | 0;
                // Restore timer mode settings
                timerDuration = doc["timerDuration"] | 0;
                timerActive = doc["timerActive"] | false;
                timerStart = doc["timerStart"] | 0;

                // Adjust toggleStart to resume correct ON/OFF phase after reboot
                if (toggleActive && (toggleOnMinutes > 0 || toggleOffMinutes > 0))
                {
                    unsigned long totalCycleSeconds = (toggleOnMinutes + toggleOffMinutes) * 60UL;
                    if (totalCycleSeconds > 0)
                    {
                        unsigned long now = millis() / 1000UL;
                        unsigned long elapsed = now - toggleStart;
                        unsigned long cycleSeconds = elapsed % totalCycleSeconds;
                        bool shouldBeOn = cycleSeconds < (toggleOnMinutes * 60UL);
                        if (shouldBeOn != isOn)
                        {
                            // Adjust toggleStart so that the relay resumes in the correct phase
                            if (isOn)
                            {
                                // Should be ON: set toggleStart so cycleSeconds = something < toggleOnMinutes*60
                                unsigned long offset = cycleSeconds - (toggleOnMinutes * 60UL) + totalCycleSeconds;
                                toggleStart = now - ((elapsed - offset) % totalCycleSeconds);
                            }
                            else
                            {
                                // Should be OFF: set toggleStart so cycleSeconds = something >= toggleOnMinutes*60
                                unsigned long offset = cycleSeconds - (toggleOnMinutes * 60UL);
                                toggleStart = now - ((elapsed - offset) % totalCycleSeconds);
                            }
                        }
                    }
                }
            }
            else
            {
                Serial.printf("[DEBUG] Relay %d: Config file corrupt, using defaults. Error: %s\n", relayNum, error.c_str());
                // Use defaults if config is corrupt
                name = "Relay " + String(relayNum);
                mode = "manual";
                isOn = false;
                isDisabled = false;
                onTime = 0;
                offTime = 0;
                // Toggle defaults
                toggleOnMinutes = 0;
                toggleOffMinutes = 0;
                toggleActive = false;
                toggleStart = 0;
            }
        }

        // Runtime states always start fresh except toggle
        timerActive = false;
        timerStart = 0;
        timerDuration = 0;
    }

    void saveToFS()
    {
        // Ensure the config directory exists
        if (!LittleFS.exists("/config"))
        {
            LittleFS.mkdir("/config");
        }

        JsonDocument doc;
        doc["isOn"] = isOn;
        doc["isDisabled"] = isDisabled;
        doc["onTime"] = onTime;
        doc["offTime"] = offTime;
        doc["name"] = name;
        doc["mode"] = mode;
        // Save toggle mode settings
        doc["toggleOnMinutes"] = toggleOnMinutes;
        doc["toggleOffMinutes"] = toggleOffMinutes;
        doc["toggleActive"] = toggleActive;
        doc["toggleStart"] = toggleStart;
        // Save timer mode settings
        doc["timerDuration"] = timerDuration;
        doc["timerActive"] = timerActive;
        doc["timerStart"] = timerStart;

        File configFile = LittleFS.open("/config/relay" + String(relayNum) + ".json", "w");
        if (configFile)
        {
            serializeJson(doc, configFile);
            configFile.close();
        }
    }

public:
    Relay(byte pinNumber, int num) : pin(pinNumber),
                                     relayNum(num),
                                     timerStart(0),
                                     timerDuration(0),
                                     timerActive(false)
    {
        pinMode(pin, OUTPUT);
        loadFromFS();
        digitalWrite(pin, isOn ? HIGH : LOW);
    }

    bool shouldBeOnNow()
    {
        if (!rtc.begin())
            return false;

        DateTime now = rtc.now();
        int timeNow = now.hour() * 100 + now.minute();

        if (offTime > onTime)
        {
            return timeNow >= onTime && timeNow < offTime;
        }
        return timeNow >= onTime || timeNow < offTime;
    }

    void toggle()
    {
        if (!isDisabled)
        {
            isOn = !isOn;
            digitalWrite(pin, isOn ? HIGH : LOW);
            Serial.printf("Relay %d: %s - Toggled to %s\n", relayNum, mode.c_str(), isOn ? "ON" : "OFF");
            saveToFS();
        }
        else
        {
            Serial.printf("Relay %d: Toggle blocked - relay disabled\n", relayNum);
        }
    }

    void setToggleMode(int onMins, int offMins, bool start)
    {
        toggleOnMinutes = onMins;
        toggleOffMinutes = offMins;
        toggleActive = start;
        if (start)
        {
            toggleStart = millis() / 1000; // Store in seconds
            isOn = true;
            digitalWrite(pin, HIGH);
            Serial.printf("Relay %d: Toggle mode started (ON: %d mins, OFF: %d mins)\n", relayNum, onMins, offMins);
        }
        else
        {
            Serial.printf("Relay %d: Toggle mode stopped\n", relayNum);
            stopToggle();
        }
        saveToFS(); // Save toggle mode settings
    }
    void turnOn()
    {
        if (!isDisabled)
        {
            isOn = true;
            digitalWrite(pin, HIGH);
            Serial.printf("Relay %d: Turned ON\n", relayNum);
            saveToFS();
        }
    }

    void stopToggle()
    {
        toggleActive = false;
        toggleStart = 0;
        if (mode == "toggle")
        {
            mode = "manual";
            isOn = false;
            digitalWrite(pin, LOW);
        }
        saveToFS(); // Save toggle mode settings
    }

    // Getters and setters
    bool getState() { return isOn; }
    bool isEnabled() { return !isDisabled; }
    String getMode() { return mode; }
    int getOnTime() { return onTime; }
    int getOffTime() { return offTime; }
    String getName() { return name; }
    void setName(String newName)
    {
        name = newName;
        saveToFS();
    }
    void setTimes(int on, int off)
    {
        onTime = on;
        offTime = off;
        // Format times as HH:MM for logging
        char onTimeStr[6];
        char offTimeStr[6];
        sprintf(onTimeStr, "%02d:%02d", on / 100, on % 100);
        sprintf(offTimeStr, "%02d:%02d", off / 100, off % 100);
        Serial.printf("Relay %d: Schedule set - ON at %s, OFF at %s\n", relayNum, onTimeStr, offTimeStr);
        saveToFS();
    }

    void setMode(String newMode)
    {
        Serial.printf("Relay %d: Mode changing from %s to %s\n", relayNum, mode.c_str(), newMode.c_str());
        if (mode == "timer" && newMode != "timer")
        {
            stopTimer();
        }
        if (mode == "toggle" && newMode != "toggle")
        {
            stopToggle();
        }
        mode = newMode;
        Serial.printf("Relay %d: Mode changed successfully\n", relayNum);
    }
    void setDisabled(bool disabled)
    {
        isDisabled = disabled;
        Serial.printf("Relay %d: %s\n", relayNum, disabled ? "Disabled" : "Enabled");
        if (disabled)
        {
            isOn = false;
            digitalWrite(pin, LOW);
            if (timerActive)
            {
                stopTimer();
            }
        }
        saveToFS();
    }
    void setTimer(int duration, bool start, bool maintainState = false)
    {
        timerDuration = duration;
        timerActive = start;
        if (start)
        {
            timerStart = millis();
            Serial.printf("Relay %d: Timer started for %d seconds (State: %s)\n", relayNum, duration, isOn ? "ON" : "OFF");
        }
        else
        {
            Serial.printf("Relay %d: Timer stopped\n", relayNum);
            endTimer(maintainState);
        }
        saveToFS(); // Save timer mode settings
    }
    bool isTimerActive() { return timerActive; }
    int getTimerDuration() { return timerDuration; }
    unsigned long getTimerStart() { return timerStart; }

    void endTimer(bool keepState)
    {
        stopTimer(keepState);
    }

    unsigned long getRemainingTime()
    {
        if (!timerActive)
            return 0;
        unsigned long elapsed = (millis() - timerStart) / 1000;
        if (elapsed >= timerDuration)
            return 0;
        return timerDuration - elapsed;
    }

    bool isToggleActive() { return toggleActive; }
    int getToggleOnMinutes() { return toggleOnMinutes; }
    int getToggleOffMinutes() { return toggleOffMinutes; }

    unsigned long getToggleRemainingTime()
    {
        if (!toggleActive)
            return 0;

        unsigned long currentSeconds = millis() / 1000;
        unsigned long elapsedSeconds = currentSeconds - toggleStart;
        unsigned long totalCycleSeconds = (toggleOnMinutes + toggleOffMinutes) * 60;

        if (totalCycleSeconds == 0)
            return 0;

        unsigned long cycleSeconds = elapsedSeconds % totalCycleSeconds;
        if (isOn)
        {
            // Time until OFF
            return (toggleOnMinutes * 60) - cycleSeconds;
        }
        else
        {
            // Time until ON
            return totalCycleSeconds - cycleSeconds;
        }
    }

    void updateToggleState()
    {
        if (!toggleActive)
            return;

        unsigned long currentSeconds = millis() / 1000;
        unsigned long elapsedSeconds = currentSeconds - toggleStart;
        unsigned long totalCycleSeconds = (toggleOnMinutes + toggleOffMinutes) * 60;

        if (totalCycleSeconds == 0)
            return;

        unsigned long cycleSeconds = elapsedSeconds % totalCycleSeconds;
        bool shouldBeOn = cycleSeconds < (toggleOnMinutes * 60);

        if (shouldBeOn != isOn)
        {
            isOn = shouldBeOn;
            digitalWrite(pin, isOn ? HIGH : LOW);
            Serial.printf("Relay %d: Toggle cycle - switched to %s\n", relayNum, isOn ? "ON" : "OFF");
        }
    }

    unsigned long getToggleStart() { return toggleStart; }

private:
    void stopTimer(bool keepState = false)
    {
        if (timerActive)
        {
            Serial.printf("Relay %d: Timer stopped %s\n", relayNum, keepState ? "(keeping state)" : "and turned OFF");
        }
        timerActive = false;
        timerStart = 0;
        timerDuration = 0;
        if (!keepState)
        {
            // Only turn OFF if not keeping state
            isOn = false;
            digitalWrite(pin, LOW);
        }
        saveToFS(); // Save timer mode settings
    }
};

Relay *relays[NUM_RELAYS];
unsigned long previousMillis = 0;
const long interval = 1000;
bool updateTime = false;

// Forward declarations
void setupServer();
bool rtcTimeUpdater();

// Config management function
bool ensureConfigDirectory()
{
    if (!LittleFS.exists("/config"))
    {
        return LittleFS.mkdir("/config");
    }
    return true;
}

volatile bool shouldReboot = false;

// --- Error message buffer for API ---
String errorBuffer = "";

// Helper functions for RTC update flag (optimized: short binary file, 1 byte)
void writeRtcUpdateFlag(bool flag)
{
    File f = LittleFS.open("/r", "w");
    if (f)
    {
        uint8_t val = flag ? 0x01 : 0x00;
        f.write(&val, 1);
        f.close();
    }
}

bool readRtcUpdateFlag()
{
    if (!LittleFS.exists("/r"))
        return false;
    File f = LittleFS.open("/r", "r");
    if (!f)
        return false;
    uint8_t val = 0;
    f.read(&val, 1);
    f.close();
    return val == 0x01;
}

void setup()
{
    beep();
    Wire.begin();
    pixels.begin();
    pixels.clear();

    pixels.setPixelColor(0, pixels.Color(0, 150, 0));
    pixels.show();

    Serial.println("Smart Aquarium V3.1 - ESP8266 Setup");
    Serial.begin(115200);

    if (!LittleFS.begin())
    {
        Serial.println("LittleFS Mount Failed");
        errorBuffer = "Filesystem mount failed. Device cannot operate. Reboot or reflash required.";
        // Error: red

        pixels.setPixelColor(0, pixels.Color(150, 0, 0));
        pixels.show();
        return;
    }

    // Ensure config directory exists
    if (!LittleFS.exists("/config"))
    {
        if (!LittleFS.mkdir("/config"))
        {
            Serial.println("Failed to create /config directory");
            errorBuffer = "Failed to create /config directory. Filesystem error.";
            // Error: red

            pixels.setPixelColor(0, pixels.Color(150, 0, 0));
            pixels.show();
        }
    }

    // Load WiFi config from FS (before WiFi.begin)
    loadWifiConfigFromFS(wifiSsid, wifiPassword, &wifiIp, &wifiGateway, &wifiSubnet, &wifiDns1, &wifiDns2, &wifiUseStaticIp);

    // Load NTP config from FS (before NTPClient is used)
    loadNtpConfigFromFS();
    timeClient = NTPClient(ntpUDP, ntpServer.c_str(), timezoneOffset);

    bool wifiConfigured = wifiSsid.length() > 0 /*&& wifiPassword.length() > 0*/;

    if (wifiConfigured)
    {
        bool staticIpOk = true;
        if (wifiUseStaticIp && wifiIp.length() && wifiGateway.length() && wifiSubnet.length())
        {
            Serial.println("Using static IP configuration");
            IPAddress ip, gateway, subnet, dns1, dns2;
            staticIpOk = ip.fromString(wifiIp) && gateway.fromString(wifiGateway) && subnet.fromString(wifiSubnet);
            if (wifiDns1.length())
                dns1.fromString(wifiDns1);
            else
                dns1 = IPAddress(1, 1, 1, 1); // cloudflare DNS
            if (wifiDns2.length())
                dns2.fromString(wifiDns2);
            else
                dns2 = IPAddress(1, 0, 0, 1); // cloudflare secondary DNS

            if (staticIpOk)
            {
                if (!WiFi.config(ip, gateway, subnet, dns1, dns2))
                {
                    Serial.println("STA Failed to configure static IP");
                    errorBuffer = "Failed to configure static IP. Check IP/gateway/subnet values.";
                    staticIpOk = false;

                    pixels.setPixelColor(0, pixels.Color(0, 0, 150));
                    pixels.show();
                }
            }
            else
            {
                Serial.println("Invalid static IP configuration");
                errorBuffer = "Invalid static IP configuration. Check IP/gateway/subnet values.";

                pixels.setPixelColor(0, pixels.Color(0, 0, 150));
                pixels.show();
            }
        }
        if (!staticIpOk)
        {
            // Mark as not configured so AP mode is started
            wifiConfigured = false;

            pixels.setPixelColor(0, pixels.Color(0, 0, 150));
            pixels.show();
        }
        else
        {
            WiFi.mode(WIFI_STA);
            WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());
            Serial.printf("Connecting to WiFi SSID: %s\n", wifiSsid.c_str());
            unsigned long startAttemptTime = millis();
            // Show progress on the LED
            while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < 20000)
            {
                pixels.setPixelColor(0, pixels.Color(150, 150, 150));
                pixels.show();
                delay(250);
                Serial.print(".");
                pixels.setPixelColor(0, pixels.Color(0, 0, 0));
                pixels.show();
                delay(250);
            }
            if (WiFi.status() == WL_CONNECTED)
            {
                Serial.println("\nConnected to WiFi");
                Serial.println(WiFi.localIP());
            }
            else
            {
                Serial.println("\nWiFi connection failed, switching to AP mode");
                wifiConfigured = false;
            }
        }
    }

    // ensure AP mode if WiFi is not connected
    if (!wifiConfigured || WiFi.status() != WL_CONNECTED)
    {

        pixels.setPixelColor(0, pixels.Color(0, 0, 150));
        pixels.show();
        // Start in AP mode for configuration
        WiFi.mode(WIFI_AP);
        String apSsid = "Aquarium-Setup";
        String apPassword = "12345678";
        WiFi.softAP(apSsid.c_str(), apPassword.c_str());
        IPAddress apIP = WiFi.softAPIP();
        Serial.print("Started AP mode. Connect to WiFi SSID: ");
        Serial.println(apSsid);
        Serial.print("AP IP address: ");
        Serial.println(apIP);
        Serial.println("Use the web interface to configure WiFi.");

        // Optionally, try to connect to any pre-configured WiFi (if present)
        if (wifiSsid.length() > 0)
        {
            WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());
            Serial.printf("Attempting STA connection to SSID: %s\n", wifiSsid.c_str());
            unsigned long startAttemptTime = millis();
            while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < 10000)
            {
                delay(500);
                Serial.print(".");
            }
            if (WiFi.status() == WL_CONNECTED)
            {
                Serial.println("\nSTA connected in AP mode.");
                Serial.print("STA IP address: ");
                Serial.println(WiFi.localIP());
            }
            else
            {
                Serial.println("\nSTA not connected in AP mode.");
            }
        }
    }

    if (!rtc.begin())
    {
        Serial.println("Couldn't find RTC");
        errorBuffer = "RTC not found. Please check hardware connection.";
        // Error: red

        pixels.setPixelColor(0, pixels.Color(150, 0, 0));
        pixels.show();
    }

    // Initialize relay objects
    for (byte i = 0; i < NUM_RELAYS; i++)
    {
        relays[i] = new Relay(RELAY_PINS[i], i + 1);
    }

    ElegantOTA.begin(&server); // Start ElegantOTA
    // ElegantOTA callbacks
    ElegantOTA.onStart(onOTAStart);
    ElegantOTA.onProgress(onOTAProgress);
    ElegantOTA.onEnd(onOTAEnd);

    setupServer();
    server.begin();
    Serial.println("HTTP server started");

    // After all config loads, check RTC update flag
    if (readRtcUpdateFlag())
    {
        rtcTimeUpdater();
        writeRtcUpdateFlag(false);
    }

    pixels.setPixelColor(0, pixels.Color(0, 0, 0));
    pixels.show();

    Serial.println("Setup complete");
}

void loop()
{
    ElegantOTA.loop();
    unsigned long currentMillis = millis(); // Update time if requested

    static unsigned long lastGreenFlash = 0;
    if (!otaActive)
    {
        if (currentMillis - lastGreenFlash >= 2000)
        {
            pixels.setBrightness(10);
            pixels.setPixelColor(0, pixels.Color(0, 250, 0));
            pixels.show();
            delay(10);
            pixels.setPixelColor(0, pixels.Color(0, 0, 0));
            pixels.show();
            delay(10);
            lastGreenFlash = currentMillis;
        }
    }

    if (updateTime)
    {
        if (rtcTimeUpdater())
        {
            Serial.println("Time updated successfully");
        }
        else
        {
            Serial.println("Time update failed");
        }
        updateTime = false;
    }

    // Check timers
    for (byte i = 0; i < NUM_RELAYS; i++)
    {
        if (relays[i]->getMode() == "timer" && relays[i]->isTimerActive())
        {
            if (relays[i]->getRemainingTime() == 0)
            { // When timer completes, toggle the relay state
                relays[i]->toggle();
                relays[i]->endTimer(true); // Stop timer and keep the toggled state
            }
        }
        yield(); // Prevent WDT reset during heavy operations
    }

    // Check toggles
    for (byte i = 0; i < NUM_RELAYS; i++)
    {
        if (relays[i]->getMode() == "toggle" && relays[i]->isToggleActive())
        {
            relays[i]->updateToggleState();
        }
        yield(); // Prevent WDT reset during heavy operations
    }

    // Check schedules every second
    if (currentMillis - previousMillis >= interval)
    {
        previousMillis = currentMillis;

        for (byte i = 0; i < NUM_RELAYS; i++)
        {
            if (relays[i]->getMode() == "auto" && relays[i]->isEnabled())
            {
                bool shouldBeOn = relays[i]->shouldBeOnNow();
                if (shouldBeOn != relays[i]->getState())
                {
                    relays[i]->toggle();
                }
            }
            yield(); // Prevent WDT reset during heavy operations
        }
    }

    delay(50); // Small delay to prevent watchdog reset

    // Handle reboot outside async callbacks
    if (shouldReboot)
    {
        Serial.println("Graceful reboot: stopping server...");
        server.end();
        delay(200); // let response finish sending
        ESP.restart();
    }
}

bool rtcTimeUpdater()
{
    if (!rtc.begin())
    {
        Serial.println("[DEBUG] RTC not found in rtcTimeUpdater");
        errorBuffer = "RTC not found. Please check hardware connection.";
        // return false;
    }
    if (WiFi.status() == WL_CONNECTED)
    {
        if (ntpServer.length() == 0)
        {
            ntpServer = "pool.ntp.org";
            Serial.println("[WARN] NTP server string was empty, set to default");
        }
        Serial.print("[DEBUG] NTP server: ");
        Serial.println(ntpServer);

        if (!timeClient.isTimeSet())
        {
            Serial.println("[DEBUG] NTP time not set, attempting update");
        }
        bool updated = false;

        if (WiFi.status() == WL_CONNECTED && ntpServer.length() > 0)
        {
            updated = timeClient.update();
        }
        if (updated && timeClient.isTimeSet())
        {
            time_t rawtime = timeClient.getEpochTime();
            if (rawtime < 1000000000UL) // sanity check for valid epoch (after 2001)
            {
                Serial.println("[ERROR] Invalid epoch time from NTP");
                errorBuffer = "Invalid epoch time from NTP. Time sync failed.";
                return false;
            }
            DateTime dt(rawtime);
            rtc.adjust(dt);
            return true;
        }
        else
        {
            Serial.println("[ERROR] NTP update failed or time not set");
            errorBuffer = "NTP update failed or time not set. Check internet connection and NTP server.";
        }
    }
    else
    {
        Serial.println("[ERROR] WiFi not connected in rtcTimeUpdater");
        errorBuffer = "WiFi not connected. Cannot update time from NTP.";
    }
    return false;
}

void setupServer()
{
    // Serve static files directly
    server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");

    // Status endpoint
    server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *request)
              { Serial.println("[DEBUG] /api/status endpoint called"); request->send(200, "text/plain", "true"); });

    // Version endpoint
    server.on("/api/version", HTTP_GET, [](AsyncWebServerRequest *request)
              { Serial.println("[DEBUG] /api/version endpoint called"); request->send(200, "text/plain", SWVersion); });

    // WiFi info endpoint (GET)
    server.on("/api/wifi", HTTP_GET, [](AsyncWebServerRequest *request)
              { Serial.println("[DEBUG] /api/wifi (GET) endpoint called");
        JsonDocument doc;
        doc["ssid"] = wifiSsid;
        doc["password"] = wifiPassword;

        // Use current network info if connected, else fallback to config
        if (WiFi.status() == WL_CONNECTED) {
            doc["ip"] = WiFi.localIP().toString();
            doc["gateway"] = WiFi.gatewayIP().toString();
            doc["subnet"] = WiFi.subnetMask().toString();
            doc["dns1"] = WiFi.dnsIP(0).toString();
            doc["dns2"] = WiFi.dnsIP(1).toString();
        } else {
            doc["ip"] = wifiIp;
            doc["gateway"] = wifiGateway;
            doc["subnet"] = wifiSubnet;
            doc["dns1"] = wifiDns1;
            doc["dns2"] = wifiDns2;
        }
        doc["useStaticIp"] = wifiUseStaticIp;
        String response;
        serializeJson(doc, response);
        request->send(200, "application/json", response); });

    // WiFi update endpoint (POST)
    server.on("/api/wifi", HTTP_POST, [](AsyncWebServerRequest *request)
              { Serial.println("[DEBUG] /api/wifi (POST) endpoint called");
        String ssid = "";
        String password = "";
        String ipStr = "", gatewayStr = "", subnetStr = "", dns1Str = "", dns2Str = "";
        bool useStaticIp = false;

        int params = request->params();
        for (int i = 0; i < params; i++) {
            const AsyncWebParameter *p = request->getParam(i);
            if (p->isPost()) {
                if (p->name() == "ssid") ssid = p->value();
                else if (p->name() == "password") password = p->value();
                else if (p->name() == "ip") ipStr = p->value();
                else if (p->name() == "gateway") gatewayStr = p->value();
                else if (p->name() == "subnet") subnetStr = p->value();
                else if (p->name() == "dns1") dns1Str = p->value();
                else if (p->name() == "dns2") dns2Str = p->value();
                else if (p->name() == "useStaticIp") useStaticIp = (p->value() == "true" || p->value() == "1" || p->value() == "on");
            }
        }

        ssid.trim();
        password.trim();
        ipStr.trim();
        gatewayStr.trim();
        subnetStr.trim();
        dns1Str.trim();
        dns2Str.trim();

        if (ssid.length() == 0) {
            request->send(400, "application/json", "{\"success\":false,\"error\":\"Missing SSID\"}");
            return;
        }

        saveWifiConfigToFS(ssid, password, ipStr, gatewayStr, subnetStr, dns1Str, dns2Str, useStaticIp);

        // Send JSON success
        JsonDocument resp;
        resp["success"] = true;
        resp["info"] = "WiFi settings saved. Device will reboot shortly.";
        String response;
        serializeJson(resp, response);
        Serial.println("WiFi settings saved, device will reboot shortly.");
        request->send(200, "application/json", response);
        return; });

    // reboot endpoint for JS-triggered reboot after message is shown
    server.on("/api/reboot", HTTP_POST, [](AsyncWebServerRequest *request)
              {
                  Serial.println("[DEBUG] /api/reboot endpoint called");
                  request->send(200, "application/json", "{\"success\":true,\"info\":\"Rebooting...\"}");
                  Serial.println("Rebooting ESP8266...");
                  shouldReboot = true; // Set flag for reboot in loop()
              });

    // RTC time endpoint
    server.on("/api/rtctime", HTTP_GET, [](AsyncWebServerRequest *request)
              { Serial.println("[DEBUG] /api/rtctime endpoint called");
        if (!rtc.begin()) {
            request->send(200, "text/plain", "RTC_FAILED");
            errorBuffer = "RTC not found. Please check hardware connection.";
            return;
        }
        DateTime now = rtc.now();
        char timeStr[6];
        sprintf(timeStr, "%02d:%02d", now.hour(), now.minute());
        request->send(200, "text/plain", timeStr); });

    // NTP/Timezone settings endpoint
    server.on("/api/ntp", HTTP_POST, [](AsyncWebServerRequest *request) {}, NULL, [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t, size_t)
              {
            Serial.println("[DEBUG] /api/ntp (POST) endpoint called");
            String json = String((char*)data);
            JsonDocument doc;
            DeserializationError error = deserializeJson(doc, json);
            if (!error && doc["ntpServer"] && doc["offset"].is<int>()) {
                String newNtpServer = doc["ntpServer"].as<String>();
                int newOffset = doc["offset"].as<int>();
                String newTimezone = doc["timezone"] | "";
                // Save to variables
                ntpServer = newNtpServer;
                timezoneOffset = newOffset;
                timezoneString = newTimezone;
                // Save to FS
                saveNtpConfigToFS();
                writeRtcUpdateFlag(true); // Set flag for RTC update after reboot
                Serial.println("[DEBUG] NTP config updated, scheduling reboot");
                request->send(200, "application/json", "{\"success\":true,\"info\":\"Rebooting to apply NTP config...\"}");
            } else {
                request->send(400, "application/json", "{\"success\":false,\"error\":\"Invalid data\"}");
            } });

    // NTP/Timezone GET endpoint for config download
    server.on("/api/ntp", HTTP_GET, [](AsyncWebServerRequest *request)
              { Serial.println("[DEBUG] /api/ntp (GET) endpoint called");
        JsonDocument doc;
        doc["ntpServer"] = ntpServer;
        doc["timezoneOffset"] = timezoneOffset;
        doc["timezoneString"] = timezoneString;
        String response;
        serializeJson(doc, response);
        request->send(200, "application/json", response); });

    // Set up endpoints for each relay
    for (byte i = 1; i <= NUM_RELAYS; i++)
    {
        setupRelayEndpoints(i);
    }

    // Error API endpoint
    server.on("/api/error", HTTP_GET, [](AsyncWebServerRequest *request)
              {
                  Serial.println("[API] /api/error endpoint called");
                  JsonDocument doc;
                  doc["error"] = errorBuffer;
                  String response;
                  serializeJson(doc, response);
                  request->send(200, "application/json", response);
                  errorBuffer = ""; });

    // Time update endpoint
    server.on("/api/time/update", HTTP_POST, [](AsyncWebServerRequest *request)
              { Serial.println("[DEBUG] /api/time/update endpoint called");
        updateTime = true;
        request->send(200, "text/plain", "Time update scheduled"); });

    // Clock endpoint: returns time (HH:MM), date (YYYY-MM-DD), and day of week
    server.on("/api/clock", HTTP_GET, [](AsyncWebServerRequest *request)
              { Serial.println("[DEBUG] /api/clock endpoint called");
        if (!rtc.begin()) {
            request->send(200, "application/json", "{\"error\":\"RTC_FAILED\"}");
            return;
        }
        DateTime now = rtc.now();
        char timeStr[6];
        sprintf(timeStr, "%02d:%02d", now.hour(), now.minute());
        char dateStr[11];
        sprintf(dateStr, "%04d-%02d-%02d", now.year(), now.month(), now.day());
        const char* daysOfWeek[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
        const char* dow = daysOfWeek[now.dayOfTheWeek() % 7];

        JsonDocument doc;
        doc["time"] = timeStr;
        doc["date"] = dateStr;
        doc["dow"] = dow;
        String response;
        serializeJson(doc, response);
        request->send(200, "application/json", response); });
}

void setupRelayEndpoints(byte relayIndex)
{
    String baseEndpoint = "/api/led" + String(relayIndex);
    byte idx = relayIndex - 1;

    // Status endpoint (toggle)
    server.on((baseEndpoint + "/status").c_str(), HTTP_GET, [idx, baseEndpointStr = baseEndpoint](AsyncWebServerRequest *request)
              { Serial.printf("[API] %s/status GET called\n", baseEndpointStr.c_str());
                JsonDocument doc;
                doc["success"] = true;
                doc["state"] = relays[idx]->getState() ? "ON" : "OFF";
                String response;
                serializeJson(doc, response);
                request->send(200, "application/json", response); });

    // System state endpoints
    server.on((baseEndpoint + "/system/state").c_str(), HTTP_GET, [idx, baseEndpointStr = baseEndpoint](AsyncWebServerRequest *request)
              { Serial.printf("[API] %s/system/state GET called\n", baseEndpointStr.c_str());
                  JsonDocument doc;
                  doc["success"] = true;
                  doc["enabled"] = relays[idx]->isEnabled();
                  doc["disabled"] = !relays[idx]->isEnabled();
                  doc["state"] = relays[idx]->getState() ? "ON" : "OFF";
                  String response;
                  serializeJson(doc, response);
                  request->send(200, "application/json", response); });

    server.on((baseEndpoint + "/system/state").c_str(), HTTP_POST,
              [](AsyncWebServerRequest *request) {},
              NULL,
              [idx, baseEndpointStr = baseEndpoint](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t /*index*/, size_t /*total*/)
              {
                  Serial.printf("[API] %s/system/state POST called\n", baseEndpointStr.c_str());
                  String json = String((char *)data);
                  JsonDocument inputDoc;
                  JsonDocument responseDoc;
                  DeserializationError error = deserializeJson(inputDoc, json);

                  if (!error && (inputDoc["enabled"].is<bool>() || inputDoc["disabled"].is<bool>()))
                  {
                      bool shouldBeDisabled;
                      if (inputDoc["enabled"].is<bool>())
                      {
                          shouldBeDisabled = !inputDoc["enabled"].as<bool>();
                      }
                      else
                      {
                          shouldBeDisabled = inputDoc["disabled"].as<bool>();
                      }

                      relays[idx]->setDisabled(shouldBeDisabled);

                      responseDoc["success"] = true;
                      responseDoc["enabled"] = relays[idx]->isEnabled();
                      responseDoc["disabled"] = !relays[idx]->isEnabled();
                      responseDoc["state"] = relays[idx]->getState() ? "ON" : "OFF";
                  }
                  else
                  {
                      responseDoc["success"] = false;
                      responseDoc["error"] = "Invalid request format";
                  }

                  String responseStr;
                  serializeJson(responseDoc, responseStr);
                  request->send(200, "application/json", responseStr);
              });

    // Name endpoints
    server.on((baseEndpoint + "/name").c_str(), HTTP_GET, [idx, baseEndpointStr = baseEndpoint](AsyncWebServerRequest *request)
              { Serial.printf("[API] %s/name GET called\n", baseEndpointStr.c_str());
                request->send(200, "text/plain", relays[idx]->getName()); });

    server.on((baseEndpoint + "/name").c_str(), HTTP_POST,
              [](AsyncWebServerRequest *request) {},
              NULL,
              [idx, baseEndpointStr = baseEndpoint](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t /*index*/, size_t /*total*/)
              {
                  Serial.printf("[API] %s/name POST called\n", baseEndpointStr.c_str());
                  String json = String((char *)data);
                  JsonDocument doc;
                  DeserializationError error = deserializeJson(doc, json);

                  if (!error && doc["name"].is<const char *>())
                  {
                      String newName = doc["name"].as<String>();
                      relays[idx]->setName(newName);
                      request->send(200, "text/plain", "Name updated");
                  }
                  else
                  {
                      request->send(400, "text/plain", "Invalid request");
                  }
              });

    // Toggle endpoint
    server.on((baseEndpoint + "/toggle").c_str(), HTTP_POST, [idx, baseEndpointStr = baseEndpoint](AsyncWebServerRequest *request)
              { Serial.printf("[API] %s/toggle POST called\n", baseEndpointStr.c_str());
                JsonDocument doc;
                relays[idx]->toggle();
                doc["success"] = true;
                doc["state"] = relays[idx]->getState() ? "ON" : "OFF";
                String response;
                serializeJson(doc, response);
                request->send(200, "application/json", response); });

    // Mode endpoint
    server.on((baseEndpoint + "/mode").c_str(), HTTP_GET, [idx, baseEndpointStr = baseEndpoint](AsyncWebServerRequest *request)
              { Serial.printf("[API] %s/mode GET called\n", baseEndpointStr.c_str());
                request->send(200, "text/plain", relays[idx]->getMode()); });

    server.on((baseEndpoint + "/mode").c_str(), HTTP_POST,
              [](AsyncWebServerRequest *request) {},
              NULL,
              [idx, baseEndpointStr = baseEndpoint](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t /*index*/, size_t /*total*/)
              {
                  Serial.printf("[API] %s/mode POST called\n", baseEndpointStr.c_str());
                  String json = String((char *)data);
                  JsonDocument doc;
                  DeserializationError error = deserializeJson(doc, json);

                  if (!error && doc["mode"].is<const char *>())
                  {
                      String newMode = doc["mode"].as<String>();
                      relays[idx]->setMode(newMode);
                      request->send(200, "text/plain", "Mode updated");
                  }
                  else
                  {
                      request->send(400, "text/plain", "Invalid request");
                  }
              });

    // Schedule endpoint
    server.on((baseEndpoint + "/schedule").c_str(), HTTP_GET, [idx, baseEndpointStr = baseEndpoint](AsyncWebServerRequest *request)
              { Serial.printf("[API] %s/schedule GET called\n", baseEndpointStr.c_str());
        JsonDocument doc;
        doc["onTime"] = relays[idx]->getOnTime();
        doc["offTime"] = relays[idx]->getOffTime();
        
        String response;
        serializeJson(doc, response);
        request->send(200, "application/json", response); });

    server.on((baseEndpoint + "/schedule").c_str(), HTTP_POST,
              [](AsyncWebServerRequest *request) {},
              NULL,
              [idx, baseEndpointStr = baseEndpoint](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t /*index*/, size_t /*total*/)
              {
                  Serial.printf("[API] %s/schedule POST called\n", baseEndpointStr.c_str());
                  String json = String((char *)data);
                  JsonDocument doc;
                  JsonDocument response;
                  DeserializationError error = deserializeJson(doc, json);

                  if (!error && doc["onTime"].is<const char *>() && doc["offTime"].is<const char *>())
                  {
                      String onTimeStr = doc["onTime"].as<String>();
                      String offTimeStr = doc["offTime"].as<String>();

                      // Convert HH:MM to HHMM format
                      int onTime = (onTimeStr.substring(0, 2).toInt() * 100) + onTimeStr.substring(3, 5).toInt();
                      int offTime = (offTimeStr.substring(0, 2).toInt() * 100) + offTimeStr.substring(3, 5).toInt();

                      relays[idx]->setTimes(onTime, offTime);

                      response["success"] = true;
                      response["onTime"] = onTime;
                      response["offTime"] = offTime;
                  }
                  else
                  {
                      response["success"] = false;
                      response["error"] = "Invalid time format";
                  }

                  String responseStr;
                  serializeJson(response, responseStr);
                  request->send(200, "application/json", responseStr);
              });

    // Timer endpoints
    server.on((baseEndpoint + "/timer").c_str(), HTTP_POST,
              [](AsyncWebServerRequest *request) {},
              NULL,
              [idx, baseEndpointStr = baseEndpoint](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t /*index*/, size_t /*total*/)
              {
                  Serial.printf("[API] %s/timer POST called\n", baseEndpointStr.c_str());
                  String json = String((char *)data);
                  JsonDocument doc;
                  JsonDocument response;
                  DeserializationError error = deserializeJson(doc, json);

                  if (!error && doc["duration"].is<int>() && doc["start"].is<bool>())
                  {
                      int duration = doc["duration"].as<int>();
                      bool start = doc["start"].as<bool>();
                      bool maintainState = doc["maintainState"] | false;
                      relays[idx]->setTimer(duration, start, maintainState);

                      response["success"] = true;
                      response["active"] = start;
                      response["duration"] = duration;
                      response["state"] = start ? "Timer started" : "Timer stopped";
                  }
                  else
                  {
                      response["success"] = false;
                      response["error"] = "Invalid timer parameters";
                  }

                  String responseStr;
                  serializeJson(response, responseStr);
                  request->send(200, "application/json", responseStr);
              });

    server.on((baseEndpoint + "/timer/state").c_str(), HTTP_GET, [idx, baseEndpointStr = baseEndpoint](AsyncWebServerRequest *request)
              { Serial.printf("[API] %s/timer/state GET called\n", baseEndpointStr.c_str());
                  JsonDocument doc;
                  doc["success"] = true;
                  doc["active"] = relays[idx]->isTimerActive();
                  doc["duration"] = relays[idx]->getTimerDuration();
                  doc["remaining"] = relays[idx]->getRemainingTime();
                  
                  String response;
                  serializeJson(doc, response);
                  request->send(200, "application/json", response); });

    // Toggle mode endpoints
    server.on((baseEndpoint + "/toggle-mode").c_str(), HTTP_POST,
              [](AsyncWebServerRequest *request) {},
              NULL,
              [idx, baseEndpointStr = baseEndpoint](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t /*index*/, size_t /*total*/)
              {
                  Serial.printf("[API] %s/toggle-mode POST called\n", baseEndpointStr.c_str());
                  String json = String((char *)data);
                  JsonDocument doc;
                  JsonDocument response;
                  DeserializationError error = deserializeJson(doc, json);

                  if (!error && doc["onMinutes"].is<int>() && doc["offMinutes"].is<int>() && doc["start"].is<bool>())
                  {
                      int onMins = doc["onMinutes"].as<int>();
                      int offMins = doc["offMinutes"].as<int>();
                      bool start = doc["start"].as<bool>();

                      relays[idx]->setMode("toggle");
                      relays[idx]->setToggleMode(onMins, offMins, start);

                      response["success"] = true;
                      response["active"] = start;
                      response["onMinutes"] = onMins;
                      response["offMinutes"] = offMins;
                      response["state"] = relays[idx]->getState() ? "ON" : "OFF";
                  }
                  else
                  {
                      response["success"] = false;
                      response["error"] = "Invalid toggle parameters";
                  }

                  String responseStr;
                  serializeJson(response, responseStr);
                  request->send(200, "application/json", responseStr);
              });

    server.on((baseEndpoint + "/toggle-mode/state").c_str(), HTTP_GET, [idx, baseEndpointStr = baseEndpoint](AsyncWebServerRequest *request)
              { Serial.printf("[API] %s/toggle-mode/state GET called\n", baseEndpointStr.c_str());
                  JsonDocument doc;
                  doc["success"] = true;
                  doc["active"] = relays[idx]->isToggleActive();
                  doc["onMinutes"] = relays[idx]->getToggleOnMinutes();
                  doc["offMinutes"] = relays[idx]->getToggleOffMinutes();
                  doc["remaining"] = relays[idx]->getToggleRemainingTime();
                  doc["state"] = relays[idx]->getState() ? "ON" : "OFF";
                  
                  String response;
                  serializeJson(doc, response);
                  request->send(200, "application/json", response); });
    // System details endpoint
    server.on("/api/system/details", HTTP_GET, [](AsyncWebServerRequest *request)
              { Serial.printf("[API] /api/system/details GET called\n");
        JsonDocument doc;
        doc["chipId"] = ESP.getChipId();
        doc["flashChipId"] = ESP.getFlashChipId();
        doc["flashChipSize"] = ESP.getFlashChipSize();
        doc["flashChipSpeed"] = ESP.getFlashChipSpeed();
        doc["freeHeap"] = ESP.getFreeHeap();
        doc["cpuFreqMHz"] = ESP.getCpuFreqMHz();
        doc["sdkVersion"] = ESP.getSdkVersion();
        doc["coreVersion"] = ESP.getCoreVersion();
        doc["macAddress"] = WiFi.macAddress();
        doc["wifiRssi"] = WiFi.RSSI();
        String response;
        serializeJson(doc, response);
        request->send(200, "application/json", response); });
}