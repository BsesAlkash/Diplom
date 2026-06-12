#include "Greenhouse.h"

#include <time.h>

#ifndef SERIAL_DEBUG
#define SERIAL_DEBUG false
#endif

#define DEBUG_PRINT(value)     do { if (SERIAL_DEBUG) { Serial.print(value); } } while (0)
#define DEBUG_PRINTLN(value)   do { if (SERIAL_DEBUG) { Serial.println(value); } } while (0)

// ============================================================
// Конструктор и основной цикл
// ============================================================

Greenhouse::Greenhouse()
    : dht(DHT_PIN, DHT_TYPE),
      wifiConnected(false),
      autoMode(true),
      windowOpened(false),
      storageReady(false),
      sensorsReady(false),
      deepSleepCycleDone(false),
      timeKnown(false),
      lastTelegramCheck(0),
      lastSensorRead(0),
      lastKnownTimeMillis(0),
      lastTelegramUpdateId(0),
      lastBotMessageId(0),
      lastKnownUnixTime(0) {
}

void Greenhouse::begin() {
    setupPins();
    setupRelaysOff();

    loadDefaultSettings();
    initStorage();
    loadState();

    resetSensorData();
    dht.begin();

    connectWiFi();

    readSensorsAndControl();

    if (wifiConnected) {
        handleTelegram(true);
    } else {
        enableAutoModeBecauseOffline();
    }

    if (ENABLE_DEEP_SLEEP) {
        deepSleepCycleDone = true;
    }
}

void Greenhouse::update() {
    if (ENABLE_DEEP_SLEEP) {
        goToSleepIfEnabled();
        return;
    }

    if (millis() - lastSensorRead >= SENSOR_READ_INTERVAL_MS) {
        readSensorsAndControl();
    }

    if (isWiFiConnected()) {
        handleTelegram(false);
    } else {
        enableAutoModeBecauseOffline();
    }

    delay(50);
}

// ============================================================
// Настройка пинов
// ============================================================

void Greenhouse::setupPins() {
    pinMode(PUMP_RELAY_PIN, OUTPUT);

    pinMode(WINDOW_OPEN_RELAY_PIN, OUTPUT);
    pinMode(WINDOW_CLOSE_RELAY_PIN, OUTPUT);

    pinMode(SOIL_SENSOR_1_POWER_PIN, OUTPUT);
    pinMode(SOIL_SENSOR_2_POWER_PIN, OUTPUT);

    digitalWrite(SOIL_SENSOR_1_POWER_PIN, LOW);
    digitalWrite(SOIL_SENSOR_2_POWER_PIN, LOW);
}

void Greenhouse::setupRelaysOff() {
    digitalWrite(PUMP_RELAY_PIN, RELAY_OFF);
    digitalWrite(WINDOW_OPEN_RELAY_PIN, RELAY_OFF);
    digitalWrite(WINDOW_CLOSE_RELAY_PIN, RELAY_OFF);
}

void Greenhouse::loadDefaultSettings() {
    settings.dayOpenTemperature = TEMP_OPEN_WINDOW_DAY;
    settings.nightOpenTemperature = TEMP_OPEN_WINDOW_NIGHT;
    settings.soilMinPercent = SOIL_MIN_PERCENT;

    settings.windowMoveTimeMs = WINDOW_MOVE_TIME_MS;
    settings.manualWateringTimeMs = MANUAL_WATERING_TIME_MS;
    settings.autoWateringTimeMs = AUTO_WATERING_TIME_MS;

    normalizeSettings();
}

void Greenhouse::normalizeSettings() {
    settings.dayOpenTemperature = constrain(settings.dayOpenTemperature, 5.0f, 60.0f);
    settings.nightOpenTemperature = constrain(settings.nightOpenTemperature, 5.0f, 60.0f);
    settings.soilMinPercent = constrain(settings.soilMinPercent, 0, 95);

    settings.windowMoveTimeMs = constrain(settings.windowMoveTimeMs, 1000UL, 60000UL);
    settings.manualWateringTimeMs = constrain(settings.manualWateringTimeMs, 1000UL, 120000UL);
    settings.autoWateringTimeMs = constrain(settings.autoWateringTimeMs, 1000UL, 120000UL);
}

// ============================================================
// Встроенная память LittleFS
// ============================================================

void Greenhouse::initStorage() {
    if (LittleFS.begin()) {
        storageReady = true;
        return;
    }

    if (LittleFS.format() && LittleFS.begin()) {
        storageReady = true;
        return;
    }

    storageReady = false;
}

void Greenhouse::loadState() {
    if (!storageReady || !LittleFS.exists(STATE_FILE_PATH)) {
        return;
    }

    File file = LittleFS.open(STATE_FILE_PATH, "r");

    if (!file) {
        return;
    }

    DynamicJsonDocument doc(2048);
    DeserializationError error = deserializeJson(doc, file);

    file.close();

    if (error) {
        return;
    }

    lastBotMessageId = doc["last_bot_message_id"] | 0;
    lastTelegramUpdateId = doc["last_update_id"] | 0;
    autoMode = doc["auto_mode"] | true;
    windowOpened = doc["window_opened"] | false;

    lastKnownUnixTime = doc["last_known_unix_time"] | 0;

    if (lastKnownUnixTime > 100000) {
        if (ENABLE_DEEP_SLEEP) {
            lastKnownUnixTime += DEEP_SLEEP_INTERVAL_SECONDS;
        }

        timeKnown = true;
        lastKnownTimeMillis = millis();
    }

    JsonObject savedSettings = doc["settings"].as<JsonObject>();

    if (!savedSettings.isNull()) {
        settings.dayOpenTemperature = savedSettings["day_open_temp"] | settings.dayOpenTemperature;
        settings.nightOpenTemperature = savedSettings["night_open_temp"] | settings.nightOpenTemperature;
        settings.soilMinPercent = savedSettings["soil_min_percent"] | settings.soilMinPercent;

        settings.windowMoveTimeMs = savedSettings["window_move_ms"] | settings.windowMoveTimeMs;
        settings.manualWateringTimeMs = savedSettings["manual_water_ms"] | settings.manualWateringTimeMs;
        settings.autoWateringTimeMs = savedSettings["auto_water_ms"] | settings.autoWateringTimeMs;
    }

    normalizeSettings();
}

void Greenhouse::saveState() {
    if (!storageReady) {
        return;
    }

    DynamicJsonDocument doc(2048);

    doc["last_bot_message_id"] = lastBotMessageId;
    doc["last_update_id"] = lastTelegramUpdateId;
    doc["auto_mode"] = autoMode;
    doc["window_opened"] = windowOpened;
    doc["last_known_unix_time"] = getCurrentUnixTime();

    JsonObject savedSettings = doc.createNestedObject("settings");
    savedSettings["day_open_temp"] = settings.dayOpenTemperature;
    savedSettings["night_open_temp"] = settings.nightOpenTemperature;
    savedSettings["soil_min_percent"] = settings.soilMinPercent;
    savedSettings["window_move_ms"] = settings.windowMoveTimeMs;
    savedSettings["manual_water_ms"] = settings.manualWateringTimeMs;
    savedSettings["auto_water_ms"] = settings.autoWateringTimeMs;

    File file = LittleFS.open(STATE_FILE_PATH, "w");

    if (!file) {
        return;
    }

    serializeJson(doc, file);
    file.close();
}

// ============================================================
// Wi-Fi
// ============================================================

void Greenhouse::connectWiFi() {
    WiFi.persistent(false);
    WiFi.disconnect(true);
    delay(500);

    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    unsigned long startAttempt = millis();

    while (WiFi.status() != WL_CONNECTED &&
           millis() - startAttempt < WIFI_CONNECT_TIMEOUT_MS) {
        delay(250);
    }

    wifiConnected = (WiFi.status() == WL_CONNECTED);

    if (!wifiConnected) {
        enableAutoModeBecauseOffline();
    }
}

bool Greenhouse::isWiFiConnected() {
    if (WiFi.status() == WL_CONNECTED) {
        wifiConnected = true;
        return true;
    }

    wifiConnected = false;
    enableAutoModeBecauseOffline();

    static unsigned long lastReconnectAttempt = 0;

    if (millis() - lastReconnectAttempt > 30000UL) {
        lastReconnectAttempt = millis();
        connectWiFi();
    }

    return wifiConnected;
}

void Greenhouse::enableAutoModeBecauseOffline() {
    if (autoMode) {
        return;
    }

    autoMode = true;
    saveState();
}

// ============================================================
// Время
// ============================================================

void Greenhouse::updateTimeFromTelegram(long telegramUnixTime) {
    if (telegramUnixTime <= 100000) {
        return;
    }

    lastKnownUnixTime = telegramUnixTime;
    lastKnownTimeMillis = millis();
    timeKnown = true;
}

long Greenhouse::getCurrentUnixTime() {
    time_t systemNow = time(nullptr);

    if (systemNow > 100000) {
        return (long) systemNow;
    }

    if (!timeKnown || lastKnownUnixTime <= 100000) {
        return 0;
    }

    return lastKnownUnixTime + (long) ((millis() - lastKnownTimeMillis) / 1000UL);
}

int Greenhouse::getCurrentHour() {
    long unixTime = getCurrentUnixTime();

    if (unixTime <= 100000) {
        return -1;
    }

    long localSeconds = (unixTime + TIMEZONE_OFFSET_SECONDS) % 86400L;

    if (localSeconds < 0) {
        localSeconds += 86400L;
    }

    return (int) (localSeconds / 3600L);
}

bool Greenhouse::isDayTime() {
    int hour = getCurrentHour();

    if (hour < 0) {
        return true;
    }

    if (DAY_START_HOUR < NIGHT_START_HOUR) {
        return hour >= DAY_START_HOUR && hour < NIGHT_START_HOUR;
    }

    return hour >= DAY_START_HOUR || hour < NIGHT_START_HOUR;
}

// ============================================================
// Датчики
// ============================================================

void Greenhouse::resetSensorData() {
    sensors.temperature = -100.0;
    sensors.airHumidity = -1.0;

    sensors.soilRaw1 = 0;
    sensors.soilRaw2 = 0;

    sensors.soilPercent1 = -1;
    sensors.soilPercent2 = -1;
    sensors.soilAveragePercent = -1;

    sensors.dhtValid = false;
    sensors.soilValid = false;

    sensorsReady = false;
}

void Greenhouse::readSensorsAndControl() {
    sensors = readSensors();
    sensorsReady = true;
    lastSensorRead = millis();

    if (autoMode) {
        automaticControl();
    }
}

SensorData Greenhouse::readSensors() {
    SensorData data;

    data.temperature = dht.readTemperature();
    data.airHumidity = dht.readHumidity();

    data.soilRaw1 = 0;
    data.soilRaw2 = 0;

    data.soilPercent1 = -1;
    data.soilPercent2 = -1;
    data.soilAveragePercent = -1;

    data.dhtValid = !(isnan(data.temperature) || isnan(data.airHumidity));

    if (!data.dhtValid) {
        data.temperature = -100.0;
        data.airHumidity = -1.0;
    }

    data.soilRaw1 = readSoilRaw(SOIL_SENSOR_1_POWER_PIN);
    data.soilRaw2 = readSoilRaw(SOIL_SENSOR_2_POWER_PIN);

    bool soil1Connected =
        data.soilRaw1 > SOIL_RAW_DISCONNECTED_LOW &&
        data.soilRaw1 < SOIL_RAW_DISCONNECTED_HIGH;

    bool soil2Connected =
        data.soilRaw2 > SOIL_RAW_DISCONNECTED_LOW &&
        data.soilRaw2 < SOIL_RAW_DISCONNECTED_HIGH;

    if (soil1Connected) {
        data.soilPercent1 = convertSoilRawToPercent(data.soilRaw1);
    }

    if (soil2Connected) {
        data.soilPercent2 = convertSoilRawToPercent(data.soilRaw2);
    }

    if (soil1Connected && soil2Connected) {
        data.soilAveragePercent = (data.soilPercent1 + data.soilPercent2) / 2;
        data.soilValid = true;
    } else if (soil1Connected) {
        data.soilAveragePercent = data.soilPercent1;
        data.soilValid = true;
    } else if (soil2Connected) {
        data.soilAveragePercent = data.soilPercent2;
        data.soilValid = true;
    } else {
        data.soilAveragePercent = -1;
        data.soilValid = false;
    }

    return data;
}

int Greenhouse::readSoilRaw(uint8_t powerPin) {
    digitalWrite(powerPin, HIGH);
    delay(SOIL_SENSOR_STABILIZATION_MS);

    int rawValue = analogRead(SOIL_ANALOG_PIN);

    digitalWrite(powerPin, LOW);
    delay(50);

    return rawValue;
}

int Greenhouse::convertSoilRawToPercent(int rawValue) {
    int percent = map(rawValue, SOIL_RAW_DRY, SOIL_RAW_WET, 0, 100);
    return constrain(percent, 0, 100);
}

// ============================================================
// Автоматическое управление
// ============================================================

void Greenhouse::automaticControl() {
    automaticWatering();
    automaticVentilation();
}

void Greenhouse::automaticWatering() {
    if (!sensors.soilValid) {
        return;
    }

    if (sensors.soilAveragePercent < settings.soilMinPercent && isMorningWateringTime()) {
        waterFor(settings.autoWateringTimeMs);
    }
}

void Greenhouse::automaticVentilation() {
    if (!sensors.dhtValid) {
        return;
    }

    float openTemperature = getCurrentOpenTemperature();
    float closeTemperature = getCurrentCloseTemperature();

    if (sensors.temperature >= openTemperature && !windowOpened) {
        openWindow();
        return;
    }

    if (sensors.temperature <= closeTemperature && windowOpened) {
        closeWindow();
    }
}

bool Greenhouse::isMorningWateringTime() {
    int hour = getCurrentHour();

    if (hour < 0) {
        return true;
    }

    return hour >= WATERING_START_HOUR && hour < WATERING_END_HOUR;
}

float Greenhouse::getCurrentOpenTemperature() {
    return isDayTime() ? settings.dayOpenTemperature : settings.nightOpenTemperature;
}

float Greenhouse::getCurrentCloseTemperature() {
    return getCurrentOpenTemperature() - TEMP_HYSTERESIS;
}

int Greenhouse::getSoilTargetPercent() {
    return constrain(settings.soilMinPercent + SOIL_TARGET_GAP_PERCENT, 0, 100);
}

// ============================================================
// Насос
// ============================================================

void Greenhouse::pumpOn() {
    digitalWrite(PUMP_RELAY_PIN, RELAY_ON);
}

void Greenhouse::pumpOff() {
    digitalWrite(PUMP_RELAY_PIN, RELAY_OFF);
}

void Greenhouse::waterFor(unsigned long durationMs) {
    pumpOn();
    delay(durationMs);
    pumpOff();
}

// ============================================================
// Окно
// ============================================================

void Greenhouse::openWindow() {
    if (windowOpened) {
        stopWindowMotor();
        return;
    }

    digitalWrite(WINDOW_CLOSE_RELAY_PIN, RELAY_OFF);
    delay(100);

    digitalWrite(WINDOW_OPEN_RELAY_PIN, RELAY_ON);
    delay(settings.windowMoveTimeMs);

    stopWindowMotor();

    windowOpened = true;
    saveState();
}

void Greenhouse::closeWindow() {
    if (!windowOpened) {
        stopWindowMotor();
        return;
    }

    digitalWrite(WINDOW_OPEN_RELAY_PIN, RELAY_OFF);
    delay(100);

    digitalWrite(WINDOW_CLOSE_RELAY_PIN, RELAY_ON);
    delay(settings.windowMoveTimeMs);

    stopWindowMotor();

    windowOpened = false;
    saveState();
}

void Greenhouse::stopWindowMotor() {
    digitalWrite(WINDOW_OPEN_RELAY_PIN, RELAY_OFF);
    digitalWrite(WINDOW_CLOSE_RELAY_PIN, RELAY_OFF);
}

// ============================================================
// Telegram
// ============================================================

void Greenhouse::handleTelegram(bool forceCheck) {
    if (!forceCheck && millis() - lastTelegramCheck < TELEGRAM_CHECK_INTERVAL_MS) {
        return;
    }

    lastTelegramCheck = millis();

    if (!telegramGetUpdatesDirect()) {
        enableAutoModeBecauseOffline();
    }
}

bool Greenhouse::telegramGetUpdatesDirect() {
    String response;

    String jsonBody;
    jsonBody += "{";
    jsonBody += "\"offset\":";
    jsonBody += String(lastTelegramUpdateId + 1);
    jsonBody += ",";
    jsonBody += "\"limit\":10,";
    jsonBody += "\"timeout\":0";
    jsonBody += "}";

    if (!telegramPostJson("getUpdates", jsonBody, response)) {
        return false;
    }

    DynamicJsonDocument doc(TELEGRAM_JSON_BUFFER_SIZE);
    DeserializationError error = deserializeJson(doc, response);

    if (error) {
        return false;
    }

    JsonArray results = doc["result"].as<JsonArray>();
    bool updateIdChanged = false;

    for (JsonObject update : results) {
        long updateId = update["update_id"] | 0;

        if (updateId > lastTelegramUpdateId) {
            lastTelegramUpdateId = updateId;
            updateIdChanged = true;
        }

        if (!update.containsKey("message")) {
            continue;
        }

        JsonObject message = update["message"];

        String chatId = message["chat"]["id"].as<String>();
        String text = message["text"].as<String>();
        long userMessageId = message["message_id"] | 0;
        long telegramUnixTime = message["date"] | 0;

        if (chatId != TELEGRAM_CHAT_ID) {
            telegramDeleteMessage(chatId, userMessageId);
            continue;
        }

        updateTimeFromTelegram(telegramUnixTime);
        processTelegramCommand(chatId, text, userMessageId, telegramUnixTime);
    }

    if (updateIdChanged) {
        saveState();
    }

    return true;
}

void Greenhouse::processTelegramCommand(const String& chatId, const String& text, long userMessageId, long telegramUnixTime) {
    updateTimeFromTelegram(telegramUnixTime);

    String command = text;
    command.trim();

    int spaceIndex = command.indexOf(' ');

    if (spaceIndex >= 0) {
        command = command.substring(0, spaceIndex);
    }

    int botNameIndex = command.indexOf('@');

    if (botNameIndex >= 0) {
        command = command.substring(0, botNameIndex);
    }

    command.toLowerCase();

    telegramDeleteMessage(chatId, userMessageId);

    if (command == "/start") {
        telegramCreateNewControlMessage(chatId, 0);
        return;
    }

    if (command == "/help") {
        sendHelp(chatId);
        return;
    }

    if (command == "/status") {
        sensors = readSensors();
        sensorsReady = true;
        lastSensorRead = millis();

        sendStatus(chatId);
        return;
    }

    if (command == "/settings") {
        sendSettings(chatId);
        return;
    }

    if (command == "/set") {
        processSetCommand(chatId, text);
        return;
    }

    if (command == "/water") {
        telegramRespondOrEdit(chatId, "Полив запущен.");
        waterFor(settings.manualWateringTimeMs);
        telegramRespondOrEdit(chatId, "Полив завершён.");
        return;
    }

    if (command == "/open") {
        autoMode = false;
        saveState();
        telegramRespondOrEdit(chatId, "Открываю окно...");
        openWindow();
        telegramRespondOrEdit(chatId, "Окно открыто.\nАвтоматический режим временно отключён.");
        return;
    }

    if (command == "/close") {
        autoMode = false;
        saveState();
        telegramRespondOrEdit(chatId, "Закрываю окно...");
        closeWindow();
        telegramRespondOrEdit(chatId, "Окно закрыто.\nАвтоматический режим временно отключён.");
        return;
    }

    if (command == "/auto") {
        autoMode = true;
        saveState();
        telegramRespondOrEdit(chatId, "Автоматический режим включён.");
        return;
    }

    telegramRespondOrEdit(chatId, "Неизвестная команда.\nИспользуйте /help.");
}

void Greenhouse::sendStatus(const String& chatId) {
    telegramRespondOrEdit(chatId, buildStatusMessage());
}

void Greenhouse::sendHelp(const String& chatId) {
    telegramRespondOrEdit(chatId, buildHelpMessage());
}

void Greenhouse::sendSettings(const String& chatId) {
    telegramRespondOrEdit(chatId, buildSettingsMessage());
}

void Greenhouse::processSetCommand(const String& chatId, const String& fullText) {
    String text = fullText;
    text.trim();

    int firstSpace = text.indexOf(' ');

    if (firstSpace < 0) {
        telegramRespondOrEdit(chatId, buildSettingsHelpMessage());
        return;
    }

    String rest = text.substring(firstSpace + 1);
    rest.trim();
    rest.toLowerCase();

    if (rest == "help") {
        telegramRespondOrEdit(chatId, buildSettingsHelpMessage());
        return;
    }

    int secondSpace = rest.indexOf(' ');

    if (secondSpace < 0) {
        telegramRespondOrEdit(chatId, "Не хватает значения.\nИспользуйте /set.");
        return;
    }

    String parameter = rest.substring(0, secondSpace);
    String valueString = rest.substring(secondSpace + 1);
    valueString.trim();
    valueString.replace(',', '.');

    if (!isNumericValue(valueString)) {
        telegramRespondOrEdit(chatId, "Значение должно быть числом.\nИспользуйте /set.");
        return;
    }

    float value = valueString.toFloat();
    bool changed = true;
    String answer;

    if (parameter == "daytemp" || parameter == "day") {
        if (value < 5.0f || value > 60.0f) {
            telegramRespondOrEdit(chatId, "Температура дня должна быть от 5 до 60 °C.");
            return;
        }

        settings.dayOpenTemperature = value;
        answer = "Температура открытия окна днём обновлена.";
    } else if (parameter == "nighttemp" || parameter == "night") {
        if (value < 5.0f || value > 60.0f) {
            telegramRespondOrEdit(chatId, "Температура ночи должна быть от 5 до 60 °C.");
            return;
        }

        settings.nightOpenTemperature = value;
        answer = "Температура открытия окна ночью обновлена.";
    } else if (parameter == "soil") {
        int percent = (int) (value + 0.5f);

        if (percent < 0 || percent > 95) {
            telegramRespondOrEdit(chatId, "Влажность почвы должна быть от 0 до 95 %." );
            return;
        }

        settings.soilMinPercent = percent;
        answer = "Порог влажности почвы обновлён.";
    } else if (parameter == "window") {
        int seconds = (int) (value + 0.5f);

        if (seconds < 1 || seconds > 60) {
            telegramRespondOrEdit(chatId, "Время работы окна должно быть от 1 до 60 секунд.");
            return;
        }

        settings.windowMoveTimeMs = (unsigned long) seconds * 1000UL;
        answer = "Длительность открытия/закрытия окна обновлена.";
    } else if (parameter == "water") {
        int seconds = (int) (value + 0.5f);

        if (seconds < 1 || seconds > 120) {
            telegramRespondOrEdit(chatId, "Время ручного полива должно быть от 1 до 120 секунд.");
            return;
        }

        settings.manualWateringTimeMs = (unsigned long) seconds * 1000UL;
        answer = "Длительность ручного полива обновлена.";
    } else if (parameter == "autowater") {
        int seconds = (int) (value + 0.5f);

        if (seconds < 1 || seconds > 120) {
            telegramRespondOrEdit(chatId, "Время автоматического полива должно быть от 1 до 120 секунд.");
            return;
        }

        settings.autoWateringTimeMs = (unsigned long) seconds * 1000UL;
        answer = "Длительность автоматического полива обновлена.";
    } else {
        changed = false;
    }

    if (!changed) {
        telegramRespondOrEdit(chatId, "Неизвестный параметр.\nИспользуйте /set.");
        return;
    }

    normalizeSettings();
    saveState();

    answer += "\n\n";
    answer += buildSettingsMessage();

    telegramRespondOrEdit(chatId, answer);
}

bool Greenhouse::telegramPostJson(const String& method, const String& jsonBody, String& responseBody) {
    BearSSL::WiFiClientSecure client;

    client.setInsecure();
    client.setTimeout(TELEGRAM_HTTP_TIMEOUT_MS / 1000);
    client.setBufferSizes(512, 512);
    client.setSSLVersion(BR_TLS10, BR_TLS12);

    if (!client.connect(TELEGRAM_API_IP, 443)) {
        return false;
    }

    String path;
    path += "/bot";
    path += TELEGRAM_BOT_TOKEN;
    path += "/";
    path += method;

    String request;
    request += "POST ";
    request += path;
    request += " HTTP/1.1\r\n";
    request += "Host: api.telegram.org\r\n";
    request += "User-Agent: ESP8266-Greenhouse\r\n";
    request += "Content-Type: application/json; charset=utf-8\r\n";
    request += "Content-Length: ";
    request += String(jsonBody.length());
    request += "\r\n";
    request += "Connection: close\r\n";
    request += "\r\n";
    request += jsonBody;

    client.print(request);

    unsigned long start = millis();

    while (client.connected() && !client.available()) {
        if (millis() - start > TELEGRAM_HTTP_TIMEOUT_MS) {
            client.stop();
            return false;
        }

        delay(10);
    }

    String rawResponse;

    while (client.connected() || client.available()) {
        while (client.available()) {
            rawResponse += char(client.read());
        }

        if (millis() - start > TELEGRAM_HTTP_TIMEOUT_MS) {
            break;
        }

        delay(10);
    }

    client.stop();

    if (rawResponse.length() == 0) {
        return false;
    }

    responseBody = extractHttpBody(rawResponse);

    if (rawResponse.indexOf("Transfer-Encoding: chunked") >= 0 ||
        rawResponse.indexOf("transfer-encoding: chunked") >= 0) {
        responseBody = decodeChunkedBody(responseBody);
    }

    return responseBody.indexOf("\"ok\":true") >= 0;
}

bool Greenhouse::telegramSendMessage(const String& chatId, const String& text, long* sentMessageId) {
    String response;
    String jsonBody = buildTelegramSendMessageJson(chatId, text);

    bool ok = telegramPostJson("sendMessage", jsonBody, response);

    if (ok && sentMessageId != nullptr) {
        *sentMessageId = extractMessageIdFromTelegramResponse(response);
    }

    return ok;
}

bool Greenhouse::telegramEditMessage(const String& chatId, long messageId, const String& text) {
    if (messageId <= 0) {
        return false;
    }

    String response;
    String jsonBody = buildTelegramEditMessageJson(chatId, messageId, text);

    bool ok = telegramPostJson("editMessageText", jsonBody, response);

    if (!ok && response.indexOf("message is not modified") >= 0) {
        return true;
    }

    return ok;
}

bool Greenhouse::telegramDeleteMessage(const String& chatId, long messageId) {
    if (messageId <= 0) {
        return false;
    }

    String response;
    String jsonBody = buildTelegramDeleteMessageJson(chatId, messageId);

    return telegramPostJson("deleteMessage", jsonBody, response);
}

void Greenhouse::telegramRespondOrEdit(const String& chatId, const String& text) {
    if (lastBotMessageId > 0) {
        if (telegramEditMessage(chatId, lastBotMessageId, text)) {
            return;
        }

        lastBotMessageId = 0;
        saveState();
    }

    long newMessageId = 0;

    if (telegramSendMessage(chatId, text, &newMessageId) && newMessageId > 0) {
        lastBotMessageId = newMessageId;
        saveState();
    }
}

bool Greenhouse::telegramCreateNewControlMessage(const String& chatId, long startMessageId) {
    if (startMessageId > 0) {
        telegramDeleteMessage(chatId, startMessageId);
    }

    lastBotMessageId = 0;
    saveState();

    long newMessageId = 0;

    if (telegramSendMessage(chatId, buildHelpMessage(), &newMessageId) && newMessageId > 0) {
        lastBotMessageId = newMessageId;
        saveState();
        return true;
    }

    return false;
}

// ============================================================
// Telegram JSON
// ============================================================

String Greenhouse::buildTelegramSendMessageJson(const String& chatId, const String& text) {
    String json;

    json += "{";
    json += "\"chat_id\":\"";
    json += jsonEscape(chatId);
    json += "\",";
    json += "\"text\":\"";
    json += jsonEscape(text);
    json += "\"";
    json += "}";

    return json;
}

String Greenhouse::buildTelegramEditMessageJson(const String& chatId, long messageId, const String& text) {
    String json;

    json += "{";
    json += "\"chat_id\":\"";
    json += jsonEscape(chatId);
    json += "\",";
    json += "\"message_id\":";
    json += String(messageId);
    json += ",";
    json += "\"text\":\"";
    json += jsonEscape(text);
    json += "\"";
    json += "}";

    return json;
}

String Greenhouse::buildTelegramDeleteMessageJson(const String& chatId, long messageId) {
    String json;

    json += "{";
    json += "\"chat_id\":\"";
    json += jsonEscape(chatId);
    json += "\",";
    json += "\"message_id\":";
    json += String(messageId);
    json += "}";

    return json;
}

String Greenhouse::buildHelpMessage() {
    String message;

    message += "Команды управления теплицей:\n\n";
    message += "/status — текущее состояние системы\n";
    message += "/water — ручной запуск полива\n";
    message += "/open — открыть окно\n";
    message += "/close — закрыть окно\n";
    message += "/auto — включить автоматический режим\n";
    message += "/settings — текущие настройки\n";
    message += "/set — настройка параметров\n";
    message += "/help — список команд\n";
    message += "/start — создать новое сообщение управления\n";

    return message;
}

String Greenhouse::buildStatusMessage() {
    String message;

    message += "Состояние теплицы:\n\n";

    if (!sensorsReady) {
        message += "Данные датчиков ещё не получены.\n\n";
    } else if (sensors.dhtValid) {
        message += "Температура воздуха: ";
        message += String(sensors.temperature, 1);
        message += " °C\n";

        message += "Влажность воздуха: ";
        message += String(sensors.airHumidity, 1);
        message += " %\n";
    } else {
        message += "Температура и влажность воздуха: ошибка датчика DHT22\n";
    }

    message += "Влажность почвы 1: ";

    if (sensors.soilPercent1 >= 0) {
        message += String(sensors.soilPercent1);
        message += " %\n";
    } else {
        message += "датчик не подключён\n";
    }

    message += "Влажность почвы 2: ";

    if (sensors.soilPercent2 >= 0) {
        message += String(sensors.soilPercent2);
        message += " %\n";
    } else {
        message += "датчик не подключён\n";
    }

    message += "Средняя влажность почвы: ";

    if (sensors.soilValid) {
        message += String(sensors.soilAveragePercent);
        message += " %\n\n";
    } else {
        message += "недоступна\n\n";
    }

    message += "Автоматический режим: ";
    message += autoMode ? "включён" : "выключен";
    message += "\n";

    message += "Окно: ";
    message += windowOpened ? "открыто" : "закрыто";
    message += "\n";

    message += "Wi-Fi: ";
    message += wifiConnected ? "подключён" : "отключён";
    message += "\n";

    message += "Deep Sleep: ";
    message += ENABLE_DEEP_SLEEP ? "включён" : "выключен";
    message += "\n";

    int hour = getCurrentHour();

    message += "Режим температуры: ";

    if (hour < 0) {
        message += "день по умолчанию, время не определено";
    } else {
        message += isDayTime() ? "день" : "ночь";
        message += ", текущий час: ";
        message += String(hour);
    }

    message += "\n";

    message += "Окно открывается при: ";
    message += String(getCurrentOpenTemperature(), 1);
    message += " °C\n";

    message += "Окно закрывается при: ";
    message += String(getCurrentCloseTemperature(), 1);
    message += " °C\n";

    message += "Полив включается ниже: ";
    message += String(settings.soilMinPercent);
    message += " %\n";

    message += "Целевая влажность почвы: ";
    message += String(getSoilTargetPercent());
    message += " %\n";

    return message;
}

String Greenhouse::buildSettingsMessage() {
    String message;

    message += "Текущие настройки:\n\n";

    message += "Температура днём:\n";
    message += "открытие ";
    message += String(settings.dayOpenTemperature, 1);
    message += " °C, закрытие ";
    message += String(settings.dayOpenTemperature - TEMP_HYSTERESIS, 1);
    message += " °C\n";

    message += "Температура ночью:\n";
    message += "открытие ";
    message += String(settings.nightOpenTemperature, 1);
    message += " °C, закрытие ";
    message += String(settings.nightOpenTemperature - TEMP_HYSTERESIS, 1);
    message += " °C\n";

    message += "Влажность почвы:\n";
    message += "полив ниже ";
    message += String(settings.soilMinPercent);
    message += " %, цель ";
    message += String(getSoilTargetPercent());
    message += " %\n";

    message += "Окно: ";
    message += String(settings.windowMoveTimeMs / 1000UL);
    message += " сек.\n";

    message += "Ручной полив: ";
    message += String(settings.manualWateringTimeMs / 1000UL);
    message += " сек.\n";

    message += "Автополив: ";
    message += String(settings.autoWateringTimeMs / 1000UL);
    message += " сек.\n\n";

    message += "Для изменения используйте /set.";

    return message;
}

String Greenhouse::buildSettingsHelpMessage() {
    String message;

    message += "Настройка параметров:\n\n";
    message += "/set daytemp 30 — температура открытия окна днём\n";
    message += "/set nighttemp 26 — температура открытия окна ночью\n";
    message += "/set soil 45 — влажность запуска полива\n";
    message += "/set window 3 — время открытия/закрытия окна, сек.\n";
    message += "/set water 5 — время ручного полива, сек.\n";
    message += "/set autowater 3 — время автополива, сек.\n\n";
    message += "Температура закрытия считается автоматически:\n";
    message += "закрытие = открытие - ";
    message += String(TEMP_HYSTERESIS, 1);
    message += " °C\n";
    message += "Целевая влажность считается автоматически:\n";
    message += "цель = минимум + ";
    message += String(SOIL_TARGET_GAP_PERCENT);
    message += " %";

    return message;
}

long Greenhouse::extractMessageIdFromTelegramResponse(const String& responseBody) {
    DynamicJsonDocument doc(TELEGRAM_JSON_BUFFER_SIZE);
    DeserializationError error = deserializeJson(doc, responseBody);

    if (error) {
        return 0;
    }

    return doc["result"]["message_id"] | 0;
}

String Greenhouse::jsonEscape(const String& value) {
    String escaped;

    for (uint16_t i = 0; i < value.length(); i++) {
        char c = value[i];

        if (c == '\"') {
            escaped += "\\\"";
        } else if (c == '\\') {
            escaped += "\\\\";
        } else if (c == '\n') {
            escaped += "\\n";
        } else if (c == '\r') {
            escaped += "\\r";
        } else if (c == '\t') {
            escaped += "\\t";
        } else {
            escaped += c;
        }
    }

    return escaped;
}

bool Greenhouse::isNumericValue(const String& value) {
    if (value.length() == 0) {
        return false;
    }

    bool digitSeen = false;
    bool dotSeen = false;

    for (uint16_t i = 0; i < value.length(); i++) {
        char c = value[i];

        if (c >= '0' && c <= '9') {
            digitSeen = true;
            continue;
        }

        if (c == '.' && !dotSeen) {
            dotSeen = true;
            continue;
        }

        if (c == '-' && i == 0) {
            continue;
        }

        return false;
    }

    return digitSeen;
}

// ============================================================
// HTTP
// ============================================================

String Greenhouse::extractHttpBody(const String& rawResponse) {
    int bodyIndex = rawResponse.indexOf("\r\n\r\n");

    if (bodyIndex < 0) {
        return rawResponse;
    }

    return rawResponse.substring(bodyIndex + 4);
}

String Greenhouse::decodeChunkedBody(const String& body) {
    String decoded;
    int position = 0;

    while (position < body.length()) {
        int lineEnd = body.indexOf("\r\n", position);

        if (lineEnd < 0) {
            break;
        }

        String sizeString = body.substring(position, lineEnd);
        sizeString.trim();

        int chunkSize = (int) strtol(sizeString.c_str(), nullptr, 16);

        if (chunkSize <= 0) {
            break;
        }

        int chunkStart = lineEnd + 2;
        int chunkEnd = chunkStart + chunkSize;

        if (chunkEnd > body.length()) {
            break;
        }

        decoded += body.substring(chunkStart, chunkEnd);
        position = chunkEnd + 2;
    }

    return decoded;
}

// ============================================================
// Энергосбережение
// ============================================================

void Greenhouse::goToSleepIfEnabled() {
    if (!ENABLE_DEEP_SLEEP || !deepSleepCycleDone) {
        return;
    }

    lastKnownUnixTime = getCurrentUnixTime();
    saveState();
    delay(200);

    ESP.deepSleep((uint64_t)DEEP_SLEEP_INTERVAL_SECONDS * 1000000ULL);
}