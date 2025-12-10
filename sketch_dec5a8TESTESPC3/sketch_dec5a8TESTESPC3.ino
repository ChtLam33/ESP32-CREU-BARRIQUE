/*
 * Firmware ESP32-C3 — Capteur barrique — v1.1.0
 *
 * - WiFiManager (pas de SSID/MdP en dur)
 * - Lecture ADC (capteur niveau barrique)
 * - Envoi JSON HTTPS vers api_post.php
 * - Récupération config serveur :
 *      - measure_interval_s (deep-sleep normal)
 *      - maintenance (true = mode maintenance 10 s, sans deep sleep)
 *      - test_20s (true = mode test deep-sleep 20 s)
 * - OTA HTTP via firmware.json (uniquement en mode maintenance)
 * - Deep sleep :
 *      - activé en mode normal & test
 *      - désactivé en maintenance
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WiFiManager.h>   // tzapu / tablatronix
#include <ArduinoJson.h>
#include <Update.h>
#include <time.h>
#include <esp_sleep.h>

// =============================
// CONFIG SERVEUR
// =============================
const char* SERVER_HOST   = "prod.lamothe-despujols.com";
const int   SERVER_PORT   = 443;

// API mesure
const char* API_URL       = "https://prod.lamothe-despujols.com/barriques/api_post.php";

// Config capteur
const char* CONFIG_PATH   = "/barriques/get_config.php"; // JSON global

// OTA JSON (firmware.json avec { "version": "...", "url": "https://.../firmware.bin" })
const char* OTA_JSON_PATH = "/barriques/firmware/firmware.json";

// =============================
// VERSION FIRMWARE
// =============================
const char* FIRMWARE_VERSION = "1.1.0";

// =============================
// HARDWARE & ADC
// =============================
const int   PIN_CAPTEUR = 1;
const float VREF        = 3.3;
const int   ADC_MAX     = 4095;

// =============================
// CONFIG MESURE / DEEP SLEEP
// =============================

// Par défaut : 7 jours (en secondes) pour le mode deep-sleep normal
const unsigned long DEFAULT_MEASURE_INTERVAL_S = 7UL * 24UL * 3600UL;

// Modes fixes
const unsigned long MAINTENANCE_INTERVAL_MS = 10000UL;  // 10 s
const unsigned long TEST_INTERVAL_MS        = 20000UL;  // 20 s

// Intervalle en millisecondes (dérivé de la config / du mode)
unsigned long measureIntervalMs = DEFAULT_MEASURE_INTERVAL_S * 1000UL;

// Mode maintenance : si true → PAS de deep sleep (boucle classique toutes les 10 s)
bool maintenanceMode = true;

// Mode test deep-sleep 20 s
bool testMode20s = false;

// Timers
unsigned long lastMeasureMs   = 0;
unsigned long lastWifiRetryMs = 0;
const unsigned long WIFI_RETRY_INTERVAL_MS = 60000UL;   // toutes les 60s

// =============================
// ID matériel (9 chiffres)
// =============================
String deviceId;   // ex: "330989340"

String makeDeviceId9Digits() {
  uint64_t mac = ESP.getEfuseMac();
  uint32_t low = (uint32_t)(mac & 0xFFFFFFFFULL);
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
  return 0; // le serveur mettra l’heure si besoin
}

// =============================
// WiFi via WiFiManager (ancienne logique + retry)
// =============================
void setupWiFi() {
  Serial.println(F("\n[WiFi] Initialisation..."));
  WiFi.mode(WIFI_STA);
  WiFi.persistent(true);

  // 1) Tentative de reconnexion avec les identifiants déjà connus
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

// Retry auto si on perd le Wi-Fi
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
// SEMVER (major.minor.patch) pour OTA
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

// renvoie -1 si a<b, 0 si a==b, 1 si a>b
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

// =============================
// OTA via firmware.json
// =============================
void checkForOTAUpdate() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(F("[OTA] Wi-Fi non connecté, skip."));
    return;
  }

  Serial.println(F("\n[OTA] Vérification de mise à jour..."));

  // 1) Récupérer firmware.json
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

  // Comparaison sémantique : on n'update que si remote > locale
  int cmp = compareSemver(String(FIRMWARE_VERSION), remoteVersion);
  if (cmp >= 0) {
    Serial.println(F("[OTA] Firmware déjà à jour ou plus récent, pas d'update."));
    return;
  }

  Serial.println(F("[OTA] Nouvelle version détectée, téléchargement..."));
  Serial.print(F("[OTA] URL firmware = "));
  Serial.println(fwUrl);

  // 2) Téléchargement & flash
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
// RÉCUPÉRATION CONFIG SERVEUR
// =============================
void applyDefaultConfig() {
  // Par défaut : maintenance ON, 10 s
  maintenanceMode   = true;
  testMode20s       = false;
  measureIntervalMs = MAINTENANCE_INTERVAL_MS;
}

void checkConfigUpdate() {
  applyDefaultConfig(); // valeurs de base, au cas où

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(F("[CFG] Wi-Fi non connecté, config par défaut (maintenance 10 s)."));
    return;
  }

  WiFiClientSecure client;
  client.setInsecure();

  String url = String(CONFIG_PATH); // config globale

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

  // Champs attendus : measure_interval_s, maintenance, test_20s
  unsigned long intervalS = doc["measure_interval_s"] | DEFAULT_MEASURE_INTERVAL_S;
  bool maintJson          = doc["maintenance"] | true;
  bool testJson           = doc["test_20s"]    | false;

  // Conflit éventuel : les deux à true
  if (maintJson && testJson) {
    Serial.println(F("[CFG] ATTENTION: maintenance=true & test_20s=true -> priorité à maintenance."));
    testJson = false;
  }

  if (maintJson) {
    // Mode maintenance : 10 s, pas de deep-sleep, OTA actif
    maintenanceMode   = true;
    testMode20s       = false;
    measureIntervalMs = MAINTENANCE_INTERVAL_MS;
    Serial.println(F("[CFG] Mode = MAINTENANCE (mesure toutes les 10 s, sans deep-sleep)."));
  } else if (testJson) {
    // Mode test deep-sleep 20 s
    maintenanceMode   = false;
    testMode20s       = true;
    measureIntervalMs = TEST_INTERVAL_MS;
    Serial.println(F("[CFG] Mode = TEST deep-sleep (réveil toutes les 20 s)."));
  } else {
    // Mode deep-sleep normal, basé sur measure_interval_s
    maintenanceMode = false;
    testMode20s     = false;

    if (intervalS == 0) intervalS = DEFAULT_MEASURE_INTERVAL_S;
    measureIntervalMs = intervalS * 1000UL;

    Serial.print(F("[CFG] Mode = DEEP-SLEEP normal, measure_interval_s = "));
    Serial.println(intervalS);
  }

  Serial.print(F("[CFG] maintenance       = "));
  Serial.println(maintenanceMode ? F("true") : F("false"));
  Serial.print(F("[CFG] test_20s          = "));
  Serial.println(testMode20s ? F("true") : F("false"));
  Serial.print(F("[CFG] measureIntervalMs = "));
  Serial.println(measureIntervalMs);
}

// =============================
// DEEP SLEEP
// =============================
void maybeDeepSleep() {
  if (maintenanceMode) {
    Serial.println(F("[SLEEP] Maintenance active → pas de deep sleep."));
    return;
  }

  // Mode normal ou test : on dort pour la durée de l’intervalle de mesure
  uint64_t sleepUs = (uint64_t)measureIntervalMs * 1000ULL;
  Serial.print(F("[SLEEP] Deep sleep pendant (ms) = "));
  Serial.println(measureIntervalMs);

  esp_sleep_enable_timer_wakeup(sleepUs);
  Serial.println(F("[SLEEP] Bonne nuit..."));
  delay(200);
  esp_deep_sleep_start();
}

// =============================
// SETUP
// =============================
void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println();
  Serial.println(F("=== Barrique ESP32-C3 v1.1.0 — WiFiManager + OTA (maintenance) + Config + DeepSleep ==="));

  deviceId = makeDeviceId9Digits();
  Serial.print(F("[ID] Device ID = "));
  Serial.println(deviceId);

  analogReadResolution(12);

  // Wi-Fi
  setupWiFi();

  // Config serveur (measure_interval_s, maintenance, test_20s)
  checkConfigUpdate();

  // OTA au démarrage UNIQUEMENT en mode maintenance
  if (maintenanceMode) {
    checkForOTAUpdate();
  } else {
    Serial.println(F("[OTA] Mode deep-sleep ou test -> OTA ignoré au démarrage."));
  }

  // Démarre les timers à maintenant
  unsigned long now = millis();
  lastMeasureMs   = now;
  lastWifiRetryMs = now;
}

// =============================
// LOOP
// =============================
void loop() {
  // Retente Wi-Fi si perdu (quand on est réveillé)
  retryWiFiIfNeeded();

  unsigned long now = millis();

  if (now - lastMeasureMs >= measureIntervalMs) {
    lastMeasureMs = now;

    // 1) Mesure ADC
    uint16_t raw = readAdcAveraged(PIN_CAPTEUR);
    float v = (raw * VREF) / ADC_MAX;

    Serial.print(F("[CAPTEUR] RAW = "));
    Serial.print(raw);
    Serial.print(F("   V ≈ "));
    Serial.println(v, 3);

    // 2) RSSI & batterie (placeholder)
    int rssi = (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : -127;
    uint16_t batteryMv = 0; // à câbler plus tard

    // 3) Timestamp
    time_t ts = getTimestamp();

    // 4) Envoi HTTP
    postMeasurement(raw, rssi, batteryMv, ts);

    // 5) Récupérer éventuellement une nouvelle config
    checkConfigUpdate();

    // 6) OTA UNIQUEMENT en mode maintenance
    if (maintenanceMode) {
      checkForOTAUpdate();
    } else {
      Serial.println(F("[OTA] Mode deep-sleep/test -> pas d'OTA."));
    }

    // 7) Deep sleep éventuel (normal ou test)
    maybeDeepSleep();
  }

  delay(50);
}