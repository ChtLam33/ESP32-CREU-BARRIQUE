/*
 * Firmware ESP32-C3 — Capteur barrique — v1.1.2
 *
 * - WiFiManager (pas de SSID/MdP en dur)
 * - Lecture ADC (capteur niveau barrique)
 * - Envoi JSON HTTPS vers api_post.php
 * - Récupération config serveur (measure_interval_s, maintenance, test_mode)
 * - OTA HTTP via firmware.json (UNIQUEMENT en mode maintenance)
 * - 3 modes :
 *     • maintenance      : pas de deep-sleep, mesure toutes les 10 s, OTA ON
 *     • test deep-sleep  : deep-sleep 20 s, OTA OFF
 *     • deep-sleep normal: deep-sleep measure_interval_s, OTA OFF
 *
 * Comportement deep-sleep (normal & test) :
 *   - À chaque réveil / démarrage :
 *       1) Wi-Fi
 *       2) 1 mesure + POST
 *       3) Récup config serveur
 *       4) "Bonne nuit..." puis deep-sleep
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WiFiManager.h>
#include <ArduinoJson.h>
#include <Update.h>
#include <time.h>
#include <esp_sleep.h>

// =============================
// CONFIG SERVEUR
// =============================
const char* SERVER_HOST   = "prod.lamothe-despujols.com";
const int   SERVER_PORT   = 443;

const char* API_URL       = "https://prod.lamothe-despujols.com/barriques/api_post.php";
const char* CONFIG_PATH   = "/barriques/get_config.php";
const char* OTA_JSON_PATH = "/barriques/firmware/firmware.json";

// =============================
// VERSION FIRMWARE
// =============================
const char* FIRMWARE_VERSION = "1.1.2";

// =============================
// HARDWARE & ADC
// =============================
const int   PIN_CAPTEUR = 1;
const float VREF        = 3.3;
const int   ADC_MAX     = 4095;

// =============================
// CONFIG MESURE / MODES
// =============================
const unsigned long DEFAULT_MEASURE_INTERVAL_S = 7UL * 24UL * 3600UL; // 7 jours
const unsigned long TEST_INTERVAL_MS          = 20000UL;             // 20 s
const unsigned long MAINT_INTERVAL_MS         = 10000UL;             // 10 s

unsigned long measureIntervalMs = DEFAULT_MEASURE_INTERVAL_S * 1000UL;

bool maintenanceMode = true;   // true = pas de deep-sleep
bool testMode        = false;  // true = deep-sleep 20 s

// Timers pour mode maintenance uniquement
unsigned long lastMeasureMs   = 0;
unsigned long lastWifiRetryMs = 0;
const unsigned long WIFI_RETRY_INTERVAL_MS = 60000UL;

// =============================
// ID matériel (9 chiffres)
// =============================
String deviceId;

String makeDeviceId9Digits() {
  uint64_t mac = ESP.getEfuseMac();
  uint32_t low  = (uint32_t)(mac & 0xFFFFFFFFULL);
  uint32_t high = (uint32_t)((mac >> 32) & 0xFFFFFFFFULL);
  uint64_t mixed = ((uint64_t)high << 32) ^ low;
  uint32_t id9 = (uint32_t)(mixed % 1000000000ULL);

  char buf[16];
  snprintf(buf, sizeof(buf), "%09u", id9);
  return String(buf);
}

// =============================
// ADC MOYENNÉ
// =============================
uint16_t readAdcAveraged(int pin, int samples = 40) {
  uint32_t sum = 0;
  for (int i = 0; i < samples; i++) {
    sum += analogRead(pin);
    delay(3);
  }
  return sum / samples;
}

// =============================
// NTP
// =============================
time_t getTimestamp() {
  static bool configured = false;

  if (!configured) {
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    configured = true;
  }

  for (int i = 0; i < 10; i++) {
    time_t now = time(nullptr);
    if (now > 1700000000) return now;
    delay(300);
  }
  return 0;
}

// =============================
// WiFi via WiFiManager (ancienne version qui marchait)
// =============================
void setupWiFi() {
  Serial.println(F("\n[WiFi] Initialisation..."));
  WiFi.mode(WIFI_STA);
  WiFi.persistent(true);

  // Tentative de reconnexion avec les identifiants déjà connus
  WiFi.begin();
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 60) { // ~30s
    delay(500);
    Serial.print('.');
    tries++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println(F("\n[WiFi] Reconnexion OK sans portail."));
  } else {
    Serial.println(F("\n[WiFi] Échec de reconnexion automatique, WiFiManager..."));

    WiFiManager wm;
    wm.setDebugOutput(false);
    wm.setConfigPortalTimeout(180); // 3 min max

    String apName = "Barrique-" + deviceId;
    Serial.print(F("[WiFi] AP config = "));
    Serial.println(apName);

    if (!wm.autoConnect(apName.c_str())) {
      Serial.println(F("[WiFi] WiFiManager échec ou timeout -> pas de Wi-Fi."));
    }
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print(F("[WiFi] CONNECTÉ à "));
    Serial.println(WiFi.SSID());
    Serial.print(F("       IP = "));
    Serial.println(WiFi.localIP());
    Serial.print(F("       RSSI = "));
    Serial.println(WiFi.RSSI());
  } else {
    Serial.println(F("[WiFi] Toujours pas connecté après WiFiManager."));
  }
}

// Retry auto (utilisé en maintenance uniquement)
void retryWiFiIfNeeded() {
  if (WiFi.status() == WL_CONNECTED) return;

  unsigned long now = millis();
  if (now - lastWifiRetryMs < WIFI_RETRY_INTERVAL_MS) return;
  lastWifiRetryMs = now;

  Serial.println(F("[WiFi] Perdu -> tentative automatique..."));
  WiFi.disconnect(true, true);
  delay(200);
  WiFi.mode(WIFI_STA);
  WiFi.begin();
}

// =============================
// HTTP POST mesures
// =============================
bool postMeasurement(uint16_t raw, int rssi, uint16_t batteryMv, time_t ts) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(F("[HTTP] Wi-Fi non connecté, envoi annulé."));
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;

  Serial.print(F("[HTTP] POST -> "));
  Serial.println(API_URL);

  if (!https.begin(client, API_URL)) {
    Serial.println(F("[HTTP] begin() ECHEC"));
    return false;
  }

  https.addHeader("Content-Type", "application/json");

  String payload = "{";
  payload += "\"id\":\"" + deviceId + "\",";
  payload += "\"fw\":\"" + String(FIRMWARE_VERSION) + "\",";
  payload += "\"value_raw\":" + String(raw) + ",";
  payload += "\"rssi\":" + String(rssi) + ",";
  payload += "\"battery_mv\":" + String(batteryMv) + ",";
  payload += "\"ts\":" + String((unsigned long)ts);
  payload += "}";

  Serial.print(F("[HTTP] Payload = "));
  Serial.println(payload);

  int code = https.POST(payload);
  Serial.print(F("[HTTP] Code = "));
  Serial.println(code);

  if (code > 0) {
    Serial.print(F("[HTTP] Réponse = "));
    Serial.println(https.getString());
  }

  https.end();
  return (code == 200 || code == 201);
}

// =============================
// SEMVER & OTA (uniquement maintenance)
// =============================
void parseSemver(const String& v, int &maj, int &min, int &pat) {
  maj = min = pat = 0;
  int p1 = v.indexOf('.');
  int p2 = (p1 >= 0) ? v.indexOf('.', p1 + 1) : -1;

  if (p1 < 0) {
    maj = v.toInt();
    return;
  }
  maj = v.substring(0, p1).toInt();

  if (p2 < 0) {
    min = v.substring(p1 + 1).toInt();
    return;
  }
  min = v.substring(p1 + 1, p2).toInt();
  pat = v.substring(p2 + 1).toInt();
}

int compareSemver(const String& a, const String& b) {
  int aMaj, aMin, aPat;
  int bMaj, bMin, bPat;
  parseSemver(a, aMaj, aMin, aPat);
  parseSemver(b, bMaj, bMin, bPat);

  if (aMaj != bMaj) return (aMaj < bMaj) ? -1 : 1;
  if (aMin != bMin) return (aMin < bMin) ? -1 : 1;
  if (aPat != bPat) return (aPat < bPat) ? -1 : 1;
  return 0;
}

void checkForOTAUpdate() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(F("[OTA] Wi-Fi non connecté, skip."));
    return;
  }

  Serial.println(F("\n[OTA] Vérification de mise à jour..."));

  WiFiClientSecure client;
  client.setInsecure();

  if (!client.connect(SERVER_HOST, SERVER_PORT)) {
    Serial.println(F("[OTA] Connexion HTTPS échouée (firmware.json)"));
    return;
  }

  String path = String(OTA_JSON_PATH);
  client.println(String("GET ") + path + " HTTP/1.1");
  client.println(String("Host: ") + SERVER_HOST);
  client.println("Connection: close");
  client.println();

  String payload;
  while (client.connected()) {
    String line = client.readStringUntil('\n');
    if (line == "\r") break;
  }
  while (client.available()) {
    payload += client.readString();
  }
  client.stop();

  int start = payload.indexOf('{');
  int end   = payload.lastIndexOf('}');
  if (start < 0 || end <= start) {
    Serial.println(F("[OTA] JSON introuvable dans la réponse :"));
    Serial.println(payload);
    return;
  }

  String jsonStr = payload.substring(start, end + 1);
  Serial.println(F("[OTA] JSON firmware.json ="));
  Serial.println(jsonStr);

  StaticJsonDocument<512> doc;
  DeserializationError err = deserializeJson(doc, jsonStr);
  if (err) {
    Serial.println(F("[OTA] Erreur parse JSON firmware.json"));
    Serial.println(err.c_str());
    return;
  }

  String remoteVersion = doc["version"] | "";
  String fwUrl         = doc["url"]     | "";

  if (remoteVersion.length() == 0 || fwUrl.length() == 0) {
    Serial.println(F("[OTA] Champs 'version' ou 'url' manquants"));
    return;
  }

  Serial.print(F("[OTA] Version distante = "));
  Serial.println(remoteVersion);
  Serial.print(F("[OTA] Version locale   = "));
  Serial.println(FIRMWARE_VERSION);

  int cmp = compareSemver(String(FIRMWARE_VERSION), remoteVersion);
  if (cmp >= 0) {
    Serial.println(F("[OTA] Firmware déjà à jour ou plus récent, pas d'update."));
    return;
  }

  Serial.println(F("[OTA] Nouvelle version détectée, téléchargement..."));
  Serial.print(F("[OTA] URL firmware = "));
  Serial.println(fwUrl);

  HTTPClient https;
  WiFiClientSecure fwClient;
  fwClient.setInsecure();

  if (!https.begin(fwClient, fwUrl)) {
    Serial.println(F("[OTA] https.begin() échoué"));
    return;
  }

  int httpCode = https.GET();
  if (httpCode != HTTP_CODE_OK) {
    Serial.print(F("[OTA] Code HTTP inattendu: "));
    Serial.println(httpCode);
    https.end();
    return;
  }

  int contentLength = https.getSize();
  if (contentLength <= 0) {
    Serial.println(F("[OTA] Taille firmware invalide"));
    https.end();
    return;
  }

  WiFiClient *stream = https.getStreamPtr();
  Serial.printf("[OTA] Taille firmware = %d octets\n", contentLength);

  if (!Update.begin(contentLength)) {
    Serial.println(F("[OTA] Update.begin() échoué"));
    https.end();
    return;
  }

  size_t written = Update.writeStream(*stream);
  if (written != (size_t)contentLength) {
    Serial.printf("[OTA] Écrit %u / %d octets\n", (unsigned)written, contentLength);
    Update.end();
    https.end();
    return;
  }

  if (!Update.end()) {
    Serial.println(F("[OTA] Update.end() a échoué"));
    https.end();
    return;
  }

  if (!Update.isFinished()) {
    Serial.println(F("[OTA] Mise à jour incomplète"));
    https.end();
    return;
  }

  Serial.println(F("[OTA] Mise à jour réussie, redémarrage..."));
  https.end();
  delay(500);
  ESP.restart();
}

// =============================
// CONFIG SERVEUR
// =============================
void applyDefaultConfig() {
  measureIntervalMs = DEFAULT_MEASURE_INTERVAL_S * 1000UL;
  maintenanceMode   = true;
  testMode          = false;
}

void checkConfigUpdate() {
  applyDefaultConfig();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(F("[CFG] Wi-Fi non connecté, config par défaut."));
    return;
  }

  WiFiClientSecure client;
  client.setInsecure();

  String url = String(CONFIG_PATH);

  Serial.print(F("[CFG] GET "));
  Serial.println(url);

  if (!client.connect(SERVER_HOST, SERVER_PORT)) {
    Serial.println(F("[CFG] Connexion HTTPS échouée"));
    return;
  }

  client.println(String("GET ") + url + " HTTP/1.1");
  client.println(String("Host: ") + SERVER_HOST);
  client.println("Connection: close");
  client.println();

  String payload;
  while (client.connected()) {
    String line = client.readStringUntil('\n');
    if (line == "\r") break;
  }
  while (client.available()) {
    payload += client.readString();
  }
  client.stop();

  int start = payload.indexOf('{');
  int end   = payload.lastIndexOf('}');
  if (start < 0 || end <= start) {
    Serial.println(F("[CFG] JSON introuvable dans la réponse"));
    Serial.println(payload);
    return;
  }

  String jsonStr = payload.substring(start, end + 1);
  Serial.println(F("[CFG] JSON reçu ="));
  Serial.println(jsonStr);

  StaticJsonDocument<512> doc;
  DeserializationError err = deserializeJson(doc, jsonStr);
  if (err) {
    Serial.println(F("[CFG] Erreur parse JSON config"));
    Serial.println(err.c_str());
    return;
  }

  unsigned long intervalS =
    doc["measure_interval_s"] | DEFAULT_MEASURE_INTERVAL_S;
  if (intervalS == 0) intervalS = DEFAULT_MEASURE_INTERVAL_S;
  measureIntervalMs = intervalS * 1000UL;

  maintenanceMode = doc["maintenance"] | true;
  testMode        = doc["test_mode"]   | false;

  Serial.print(F("[CFG] measure_interval_s = "));
  Serial.println(intervalS);
  Serial.print(F("[CFG] maintenance       = "));
  Serial.println(maintenanceMode ? F("true") : F("false"));
  Serial.print(F("[CFG] test_mode         = "));
  Serial.println(testMode ? F("true") : F("false"));
}

// =============================
// DEEP SLEEP helper
// =============================
void goToDeepSleep(const char* modeLabel, unsigned long intervalMs) {
  if (intervalMs < 5000UL) intervalMs = 5000UL; // sécurité

  Serial.print(F("[SLEEP] Mode = "));
  Serial.println(modeLabel ? modeLabel : "");
  Serial.print(F("[SLEEP] Prochain réveil dans (ms) = "));
  Serial.println(intervalMs);
  Serial.println(F("[SLEEP] Bonne nuit..."));

  uint64_t sleepUs = (uint64_t)intervalMs * 1000ULL;
  esp_sleep_enable_timer_wakeup(sleepUs);
  delay(200);
  esp_deep_sleep_start();
}

// =============================
// UNE MESURE COMPLETTE (ADC + POST)
// =============================
void doOneMeasurement() {
  uint16_t raw = readAdcAveraged(PIN_CAPTEUR);
  float v = (raw * VREF) / ADC_MAX;

  Serial.print(F("[CAPTEUR] RAW = "));
  Serial.print(raw);
  Serial.print(F("   V ≈ "));
  Serial.println(v, 3);

  int rssi = (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : -127;
  uint16_t batteryMv = 0; // à câbler plus tard
  time_t ts = getTimestamp();

  postMeasurement(raw, rssi, batteryMv, ts);
}

// =============================
// SETUP
// =============================
void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println();
  Serial.println(F("=== Barrique ESP32-C3 v1.1.0 — WiFiManager + OTA + Config + DeepSleep ==="));

  deviceId = makeDeviceId9Digits();
  Serial.print(F("[ID] Device ID = "));
  Serial.println(deviceId);

  analogReadResolution(12);

  // 1) Wi-Fi
  setupWiFi();

  // 2) Première mesure immédiatement (si possible)
  doOneMeasurement();

  // 3) Récup config serveur pour savoir quel mode on utilise
  checkConfigUpdate();

  Serial.print(F("[MODE] maintenance = "));
  Serial.println(maintenanceMode ? F("true") : F("false"));
  Serial.print(F("[MODE] testMode    = "));
  Serial.println(testMode ? F("true") : F("false"));
  Serial.print(F("[MODE] measureIntervalMs = "));
  Serial.println(measureIntervalMs);

  // 4) Décision de mode
  if (!maintenanceMode) {
    // MODE DEEP-SLEEP (normal ou test) → pas d'OTA
    unsigned long intervalMs = testMode ? TEST_INTERVAL_MS : measureIntervalMs;
    const char* label = testMode ? "test 20s" : "normal";
    goToDeepSleep(label, intervalMs);
    // ne revient jamais
  }

  // Si on arrive ici → mode maintenance
  // OTA autorisé uniquement ici
  checkForOTAUpdate();

  unsigned long now = millis();
  lastMeasureMs   = now;
  lastWifiRetryMs = now;
}

// =============================
// LOOP : uniquement pour mode maintenance
// =============================
void loop() {
  if (!maintenanceMode) {
    // Sécurité : si la config a changé à chaud, on repasse en deep-sleep
    unsigned long intervalMs = testMode ? TEST_INTERVAL_MS : measureIntervalMs;
    const char* label = testMode ? "test 20s" : "normal";
    goToDeepSleep(label, intervalMs);
  }

  retryWiFiIfNeeded();

  unsigned long now = millis();
  if (now - lastMeasureMs >= MAINT_INTERVAL_MS) {
    lastMeasureMs = now;

    doOneMeasurement();
    checkConfigUpdate();   // peut changer maintenance/test/interval
    checkForOTAUpdate();   // OTA seulement en maintenance
  }

  delay(50);
}