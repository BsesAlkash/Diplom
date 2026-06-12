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
      lastTelegramCheck(0),
      lastSensorRead(0),
      lastTelegramUpdateId(0),
      lastBotMessageId(0) {
}

void Greenhouse::begin() {
    setupPins();
    setupRelaysOff();

    initStorage();
    loadState();

    resetSensorData();
    dht.begin();

    connectWiFi();

    readSensorsAndControl();

    if (wifiConnected) {
        handleTelegram(true);
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

    DynamicJsonDocument doc(512);
    DeserializationError error = deserializeJson(doc, file);

    file.close();

    if (error) {
        return;
    }

    lastBotMessageId = doc["last_bot_message_id"] | 0;
    lastTelegramUpdateId = doc["last_update_id"] | 0;
    autoMode = doc["auto_mode"] | true;
    windowOpened = doc["window_opened"] | false;
}

void Greenhouse::saveState() {
    if (!storageReady) {
        return;
    }

    DynamicJsonDocument doc(512);

    doc["last_bot_message_id"] = lastBotMessageId;
    doc["last_update_id"] = lastTelegramUpdateId;
    doc["auto_mode"] = autoMode;
    doc["window_opened"] = windowOpened;

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
}

bool Greenhouse::isWiFiConnected() {
    if (WiFi.status() == WL_CONNECTED) {
        wifiConnected = true;
        return true;
    }

    wifiConnected = false;

    static unsigned long lastReconnectAttempt = 0;

    if (millis() - lastReconnectAttempt > 30000UL) {
        lastReconnectAttempt = millis();
        connectWiFi();
    }

    return wifiConnected;
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

    if (sensors.soilAveragePercent < SOIL_MIN_PERCENT && isMorningWateringTime()) {
        waterFor(AUTO_WATERING_TIME_MS);
    }
}

void Greenhouse::automaticVentilation() {
    if (!sensors.dhtValid) {
        return;
    }

    if (sensors.temperature >= TEMP_OPEN_WINDOW && !windowOpened) {
        openWindow();
        return;
    }

    if (sensors.temperature <= TEMP_CLOSE_WINDOW && windowOpened) {
        closeWindow();
    }
}

bool Greenhouse::isMorningWateringTime() {
    time_t now = time(nullptr);

    if (now < 100000) {
        return true;
    }

    struct tm* timeInfo = localtime(&now);

    if (timeInfo == nullptr) {
        return true;
    }

    int hour = timeInfo->tm_hour;

    return hour >= WATERING_START_HOUR && hour < WATERING_END_HOUR;
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
    digitalWrite(WINDOW_CLOSE_RELAY_PIN, RELAY_OFF);
    delay(100);

    digitalWrite(WINDOW_OPEN_RELAY_PIN, RELAY_ON);
    delay(WINDOW_MOVE_TIME_MS);

    stopWindowMotor();

    windowOpened = true;
    saveState();
}

void Greenhouse::closeWindow() {
    digitalWrite(WINDOW_OPEN_RELAY_PIN, RELAY_OFF);
    delay(100);

    digitalWrite(WINDOW_CLOSE_RELAY_PIN, RELAY_ON);
    delay(WINDOW_MOVE_TIME_MS);

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
    telegramGetUpdatesDirect();
}

void Greenhouse::telegramGetUpdatesDirect() {
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
        return;
    }

    DynamicJsonDocument doc(TELEGRAM_JSON_BUFFER_SIZE);
    DeserializationError error = deserializeJson(doc, response);

    if (error) {
        return;
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

        if (chatId != TELEGRAM_CHAT_ID) {
            telegramDeleteMessage(chatId, userMessageId);
            continue;
        }

        processTelegramCommand(chatId, text, userMessageId);
    }

    if (updateIdChanged) {
        saveState();
    }
}

void Greenhouse::processTelegramCommand(const String& chatId, const String& text, long userMessageId) {
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

    if (command == "/water") {
        telegramRespondOrEdit(chatId, "Полив запущен.");
        waterFor(MANUAL_WATERING_TIME_MS);
        telegramRespondOrEdit(chatId, "Полив завершён.");
        return;
    }

    if (command == "/open") {
        autoMode = false;
        telegramRespondOrEdit(chatId, "Открываю окно...");
        openWindow();
        telegramRespondOrEdit(chatId, "Окно открыто.\nАвтоматический режим временно отключён.");
        return;
    }

    if (command == "/close") {
        autoMode = false;
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

    saveState();
    delay(200);

    ESP.deepSleep((uint64_t)DEEP_SLEEP_INTERVAL_SECONDS * 1000000ULL);
}