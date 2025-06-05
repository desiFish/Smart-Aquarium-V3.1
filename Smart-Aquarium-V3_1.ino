/*
 * Smart-Aquarium-V3.1 - ESP8266-based Interactive Networked Aquarium
 * Copyright (C) 2025
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include <ESP8266WiFi.h>
#include <ESPAsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <ElegantOTA.h>
#include "global.h" //remove this 

// NTP and RTC
#include <NTPClient.h>
#include <WiFiUdp.h>
#include "RTClib.h"

RTC_DS3231 rtc;
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "asia.pool.ntp.org", 19800); // 19800 = UTC+5:30 for India

// Software version
#define SWVersion "v1.0.0"

// WiFi credentials
const char *ssid = pssid;     // replace "pssid" and with your Wifi Name a.k.a SSID (STRING type)
const char *password = ppass; // replace "ppass" with WIFI Password (STRING type)

AsyncWebServer server(80);

// Set your Static IP address
IPAddress local_IP(192, 168, 29, 123);
// Set your Gateway IP address
IPAddress gateway(192, 168, 29, 1);

IPAddress subnet(255, 255, 255, 0);

IPAddress primaryDNS(1, 1, 1, 1);   // optional
IPAddress secondaryDNS(1, 0, 0, 1); // optional

unsigned long ota_progress_millis = 0;

void onOTAStart()
{
    Serial.println("OTA update started!");
}

void onOTAProgress(size_t current, size_t final)
{
    if (millis() - ota_progress_millis > 1000)
    {
        ota_progress_millis = millis();
        Serial.printf("OTA Progress Current: %u bytes, Final: %u bytes\n", current, final);
    }
}

void onOTAEnd(bool success)
{
    if (success)
    {
        Serial.println("OTA update finished successfully!");
    }
    else
    {
        Serial.println("There was an error during OTA update!");
    }
}

// ESP8266 compatible pins for relays
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
        File configFile = LittleFS.open("/config/relay" + String(relayNum) + ".json", "r");
        if (!configFile)
        {
            // Use defaults if no config file exists
            name = "Relay " + String(relayNum);
            mode = "manual";
            isOn = false;
            isDisabled = false;
            onTime = 0;
            offTime = 0;
        }
        else
        {
            String jsonStr = configFile.readString();
            configFile.close();

            JsonDocument doc;
            DeserializationError error = deserializeJson(doc, jsonStr);

            if (!error)
            {
                isOn = doc["isOn"] | false;
                isDisabled = doc["isDisabled"] | false;
                onTime = doc["onTime"] | 0;
                offTime = doc["offTime"] | 0;
                name = doc["name"] | ("Relay " + String(relayNum));
                mode = doc["mode"] | "manual";
            }
        }

        // Runtime states always start fresh
        timerActive = false;
        timerStart = 0;
        timerDuration = 0;
        toggleActive = false;
        toggleStart = 0;
        toggleOnMinutes = 0;
        toggleOffMinutes = 0;
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

void setup()
{
    Serial.begin(115200);
    Wire.begin();
    if (!LittleFS.begin())
    {
        Serial.println("LittleFS Mount Failed");
        return;
    }

    // Ensure config directory exists
    if (!LittleFS.exists("/config"))
    {
        LittleFS.mkdir("/config");
    }

    // Configures static IP address
    if (!WiFi.config(local_IP, gateway, subnet, primaryDNS, secondaryDNS))
    {
        Serial.println("STA Failed to configure");
    }

    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED)
    {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\nConnected to WiFi");
    Serial.println(WiFi.localIP());

    if (!rtc.begin())
    {
        Serial.println("Couldn't find RTC");
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
}

void loop()
{
    ElegantOTA.loop();
    unsigned long currentMillis = millis(); // Update time if requested
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
    }

    // Check toggles
    for (byte i = 0; i < NUM_RELAYS; i++)
    {
        if (relays[i]->getMode() == "toggle" && relays[i]->isToggleActive())
        {
            relays[i]->updateToggleState();
        }
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
        }
    }

    delay(50); // Small delay to prevent watchdog reset
}

bool rtcTimeUpdater()
{
    if (WiFi.status() == WL_CONNECTED)
    {
        timeClient.begin();
        if (timeClient.update() && timeClient.isTimeSet())
        {
            time_t rawtime = timeClient.getEpochTime();
            struct tm *ti;
            ti = localtime(&rawtime);

            uint16_t year = ti->tm_year + 1900;
            uint8_t month = ti->tm_mon + 1;
            uint8_t day = ti->tm_mday;

            rtc.adjust(DateTime(year, month, day,
                                timeClient.getHours(),
                                timeClient.getMinutes(),
                                timeClient.getSeconds()));

            Serial.println("RTC updated");
            return true;
        }
    }
    return false;
}

void setupServer()
{
    // Serve static files directly
    server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");

    // Status endpoint
    server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *request)
              { request->send(200, "text/plain", "true"); });

    // Version endpoint
    server.on("/api/version", HTTP_GET, [](AsyncWebServerRequest *request)
              { request->send(200, "text/plain", SWVersion); });

    // Add RTC time endpoint
    server.on("/api/rtctime", HTTP_GET, [](AsyncWebServerRequest *request)
              {
        if (!rtc.begin()) {
            request->send(200, "text/plain", "RTC_FAILED");
            return;
        }
        DateTime now = rtc.now();
        char timeStr[6];
        sprintf(timeStr, "%02d:%02d", now.hour(), now.minute());
        request->send(200, "text/plain", timeStr); });

    // Set up endpoints for each relay
    for (byte i = 1; i <= NUM_RELAYS; i++)
    {
        setupRelayEndpoints(i);
    }

    // Time update endpoint
    server.on("/api/time/update", HTTP_POST, [](AsyncWebServerRequest *request)
              {
        updateTime = true;
        request->send(200, "text/plain", "Time update scheduled"); });
}

void setupRelayEndpoints(byte relayIndex)
{
    String baseEndpoint = "/api/led" + String(relayIndex);
    byte idx = relayIndex - 1;

    // Status endpoint (toggle) - Update to return consistent format
    server.on((baseEndpoint + "/status").c_str(), HTTP_GET, [idx](AsyncWebServerRequest *request)
              {
                JsonDocument doc;
                doc["success"] = true;
                doc["state"] = relays[idx]->getState() ? "ON" : "OFF";
                String response;
                serializeJson(doc, response);
                request->send(200, "application/json", response); });

    // System state endpoints - Update to use consistent error handling
    server.on((baseEndpoint + "/system/state").c_str(), HTTP_GET, [idx](AsyncWebServerRequest *request)
              {
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
              [idx](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t /*index*/, size_t /*total*/)
              {
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
    server.on((baseEndpoint + "/name").c_str(), HTTP_GET, [idx](AsyncWebServerRequest *request)
              { request->send(200, "text/plain", relays[idx]->getName()); });

    // POST handler for setting name
    server.on((baseEndpoint + "/name").c_str(), HTTP_POST,
              [](AsyncWebServerRequest *request) {},
              NULL,
              [idx](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t /*index*/, size_t /*total*/)
              {
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

    // Toggle endpoint - Update to return consistent format
    server.on((baseEndpoint + "/toggle").c_str(), HTTP_POST, [idx](AsyncWebServerRequest *request)
              {
                JsonDocument doc;
                relays[idx]->toggle();
                doc["success"] = true;
                doc["state"] = relays[idx]->getState() ? "ON" : "OFF";
                String response;
                serializeJson(doc, response);
                request->send(200, "application/json", response); });

    // Mode endpoint
    server.on((baseEndpoint + "/mode").c_str(), HTTP_GET, [idx](AsyncWebServerRequest *request)
              { request->send(200, "text/plain", relays[idx]->getMode()); });

    server.on((baseEndpoint + "/mode").c_str(), HTTP_POST,
              [](AsyncWebServerRequest *request) {},
              NULL,
              [idx](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t /*index*/, size_t /*total*/)
              {
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
    server.on((baseEndpoint + "/schedule").c_str(), HTTP_GET, [idx](AsyncWebServerRequest *request)
              {
        JsonDocument doc;
        doc["onTime"] = relays[idx]->getOnTime();
        doc["offTime"] = relays[idx]->getOffTime();
        
        String response;
        serializeJson(doc, response);
        request->send(200, "application/json", response); });

    server.on((baseEndpoint + "/schedule").c_str(), HTTP_POST,
              [](AsyncWebServerRequest *request) {},
              NULL,
              [idx](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t /*index*/, size_t /*total*/)
              {
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
              [idx](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t /*index*/, size_t /*total*/)
              {
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

    server.on((baseEndpoint + "/timer/state").c_str(), HTTP_GET, [idx](AsyncWebServerRequest *request)
              {
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
              [idx](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t /*index*/, size_t /*total*/)
              {
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

    server.on((baseEndpoint + "/toggle-mode/state").c_str(), HTTP_GET, [idx](AsyncWebServerRequest *request)
              {
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
}