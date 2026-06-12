#pragma once

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <WiFiClientSecureBearSSL.h>
#include <LittleFS.h>
#include <DHT.h>
#include <ArduinoJson.h>

#include "../../include/Config.h"

// =====================
// Значения по умолчанию,
// если они не указаны в Config.h
// =====================

#ifndef TELEGRAM_API_IP
#define TELEGRAM_API_IP IPAddress(149, 154, 166, 110)
#endif

#ifndef TELEGRAM_CHECK_INTERVAL_MS
#define TELEGRAM_CHECK_INTERVAL_MS 1000UL
#endif

#ifndef TELEGRAM_HTTP_TIMEOUT_MS
#define TELEGRAM_HTTP_TIMEOUT_MS 15000UL
#endif

#ifndef TELEGRAM_JSON_BUFFER_SIZE
#define TELEGRAM_JSON_BUFFER_SIZE 7000
#endif

#ifndef SENSOR_READ_INTERVAL_MS
#define SENSOR_READ_INTERVAL_MS 1200000UL
#endif

#ifndef ENABLE_DEEP_SLEEP
#define ENABLE_DEEP_SLEEP false
#endif

#ifndef DEEP_SLEEP_INTERVAL_SECONDS
#define DEEP_SLEEP_INTERVAL_SECONDS 1200
#endif

#ifndef TEMP_OPEN_WINDOW_DAY
#define TEMP_OPEN_WINDOW_DAY 30.0
#endif

#ifndef TEMP_OPEN_WINDOW_NIGHT
#define TEMP_OPEN_WINDOW_NIGHT 26.0
#endif

#ifndef TEMP_HYSTERESIS
#define TEMP_HYSTERESIS 4.0
#endif

#ifndef DAY_START_HOUR
#define DAY_START_HOUR 6
#endif

#ifndef NIGHT_START_HOUR
#define NIGHT_START_HOUR 21
#endif

#ifndef SOIL_TARGET_GAP_PERCENT
#define SOIL_TARGET_GAP_PERCENT 20
#endif

#ifndef SOIL_RAW_DISCONNECTED_LOW
#define SOIL_RAW_DISCONNECTED_LOW 20
#endif

#ifndef SOIL_RAW_DISCONNECTED_HIGH
#define SOIL_RAW_DISCONNECTED_HIGH 1000
#endif

#ifndef STATE_FILE_PATH
#define STATE_FILE_PATH "/state.json"
#endif

// =====================
// Данные датчиков
// =====================

struct SensorData {
    float temperature;
    float airHumidity;

    int soilRaw1;
    int soilRaw2;

    int soilPercent1;
    int soilPercent2;
    int soilAveragePercent;

    bool dhtValid;
    bool soilValid;
};

// =====================
// Настройки, которые можно менять через Telegram
// =====================

struct RuntimeSettings {
    float dayOpenTemperature;
    float nightOpenTemperature;

    int soilMinPercent;

    unsigned long windowMoveTimeMs;
    unsigned long manualWateringTimeMs;
    unsigned long autoWateringTimeMs;
};

// =====================
// Основной класс системы
// =====================

class Greenhouse {
public:
    Greenhouse();

    void begin();
    void update();

private:
    DHT dht;

    SensorData sensors;
    RuntimeSettings settings;

    bool wifiConnected;
    bool autoMode;
    bool windowOpened;
    bool storageReady;
    bool sensorsReady;
    bool deepSleepCycleDone;
    bool timeKnown;

    unsigned long lastTelegramCheck;
    unsigned long lastSensorRead;
    unsigned long lastKnownTimeMillis;

    long lastTelegramUpdateId;
    long lastBotMessageId;
    long lastKnownUnixTime;

private:
    // Настройка
    void setupPins();
    void setupRelaysOff();
    void loadDefaultSettings();
    void normalizeSettings();

    // Память
    void initStorage();
    void loadState();
    void saveState();

    // Wi-Fi
    void connectWiFi();
    bool isWiFiConnected();
    void enableAutoModeBecauseOffline();

    // Время
    void updateTimeFromTelegram(long telegramUnixTime);
    long getCurrentUnixTime();
    int getCurrentHour();
    bool isDayTime();

    // Датчики
    void resetSensorData();
    void readSensorsAndControl();

    SensorData readSensors();
    int readSoilRaw(uint8_t powerPin);
    int convertSoilRawToPercent(int rawValue);

    // Автоматическое управление
    void automaticControl();
    void automaticWatering();
    void automaticVentilation();

    bool isMorningWateringTime();
    float getCurrentOpenTemperature();
    float getCurrentCloseTemperature();
    int getSoilTargetPercent();

    // Насос
    void pumpOn();
    void pumpOff();
    void waterFor(unsigned long durationMs);

    // Окно
    void openWindow();
    void closeWindow();
    void stopWindowMotor();

    // Telegram
    void handleTelegram(bool forceCheck = false);
    bool telegramGetUpdatesDirect();
    void processTelegramCommand(const String& chatId, const String& text, long userMessageId, long telegramUnixTime);

    void sendStatus(const String& chatId);
    void sendHelp(const String& chatId);
    void sendSettings(const String& chatId);
    void processSetCommand(const String& chatId, const String& fullText);

    bool telegramPostJson(const String& method, const String& jsonBody, String& responseBody);

    bool telegramSendMessage(const String& chatId, const String& text, long* sentMessageId = nullptr);
    bool telegramEditMessage(const String& chatId, long messageId, const String& text);
    bool telegramDeleteMessage(const String& chatId, long messageId);

    void telegramRespondOrEdit(const String& chatId, const String& text);
    bool telegramCreateNewControlMessage(const String& chatId, long startMessageId);

    String buildTelegramSendMessageJson(const String& chatId, const String& text);
    String buildTelegramEditMessageJson(const String& chatId, long messageId, const String& text);
    String buildTelegramDeleteMessageJson(const String& chatId, long messageId);

    String buildHelpMessage();
    String buildStatusMessage();
    String buildSettingsMessage();
    String buildSettingsHelpMessage();

    long extractMessageIdFromTelegramResponse(const String& responseBody);

    String jsonEscape(const String& value);
    bool isNumericValue(const String& value);

    // HTTP
    String extractHttpBody(const String& rawResponse);
    String decodeChunkedBody(const String& body);

    // Энергосбережение
    void goToSleepIfEnabled();
};
