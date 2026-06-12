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

    bool wifiConnected;
    bool autoMode;
    bool windowOpened;
    bool storageReady;
    bool sensorsReady;
    bool deepSleepCycleDone;

    unsigned long lastTelegramCheck;
    unsigned long lastSensorRead;

    long lastTelegramUpdateId;
    long lastBotMessageId;

private:
    // Настройка
    void setupPins();
    void setupRelaysOff();

    // Память
    void initStorage();
    void loadState();
    void saveState();

    // Wi-Fi
    void connectWiFi();
    bool isWiFiConnected();

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
    void telegramGetUpdatesDirect();
    void processTelegramCommand(const String& chatId, const String& text, long userMessageId);

    void sendStatus(const String& chatId);
    void sendHelp(const String& chatId);

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

    long extractMessageIdFromTelegramResponse(const String& responseBody);

    String jsonEscape(const String& value);

    // HTTP
    String extractHttpBody(const String& rawResponse);
    String decodeChunkedBody(const String& body);

    // Энергосбережение
    void goToSleepIfEnabled();
};