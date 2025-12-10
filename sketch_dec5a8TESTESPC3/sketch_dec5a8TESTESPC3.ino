/*
 * Firmware ESP32-C3 — Capteur barrique — v1.0.3
 * WiFiManager + OTA HTTP (version.json) + config serveur (interval + maintenance)
 *
 * - WiFi configurable via portail AP "Barrique-XXXXXXXXX"
 * - Reconnexion auto au Wi-Fi connu à chaque démarrage
 * - Retry périodique si le Wi-Fi tombe
 * - OTA HTTP en lisant:
 *     https://prod.lamothe-despujols.com/barriques/firmware/version.json
 *   qui contient:
 *     { "version": "1.0.3", "url": "https://.../firmware.bin" }
 * - Récupération de la config:
 *     https://prod.lamothe-despujols.com/barriques/get_config.php?id=DEVICE_ID
 *   qui renvoie par ex.:
 *     { "measure_interval_sec": 604800, "maintenance": false }
 * - Envoi périodique des mesures brutes ADC vers:
 *     https://prod.lamothe-despujols.com/barriques/api_post.php
 *
 * Pour l’instant : PAS de deep-sleep.
 * Plus tard : on utilisera measureIntervalMs + maintenanceMode pour gérer le sommeil.
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <time.h>
#include <WiFiManager.h>   // tzapu / tablatronix
#include <ArduinoJson.h>
#include <Update.h>

// =============================
// CONFIG UTILISATEUR
// =============================

// API barriques
const char* API_URL = "https://prod.lamothe-despujols.com/barriques/api_post.php";

// OTA : JSON de description firmware
const char* OTA_JSON_URL =
  "https://prod.lamothe-despujols.com/barriques/firmware/version.json";

// Config par capteur
const char* CONFIG_URL =
  "https://prod.lamothe-despujols.com/barriques/get_config.php";

#define FW_VERSION "1.0.3"

// Mode test si pas de config : 5 s entre mesures
#define TEST_INTERVAL_MS 5000

// Pin capteur niveau (ADC)
const int   PIN_CAPTEUR = 1;
const float VREF        = 3.3;
const int   ADC_MAX     = 4095;

// =============================
// ID matériel sur 9 chiffres
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
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");

  for (int i = 0; i < 10; i++) {
    time_t now = time(nullptr);
    if (now > 1700000000) return now; // ~2023+
    delay(300);
  }
  return 0; // Le serveur mettra l’heure si besoin
}

// =============================
// WiFi — logique "comme les cuves"
// =============================

// Timers / paramètres dynamiques
unsigned long lastMeasureMs    = 0;
unsigned long lastWifiRetryMs  = 0;

unsigned long measureIntervalMs = TEST_INTERVAL_MS;  // par défaut
const unsigned long wifiRetryInterval = 60000UL;     // retry Wi-Fi toutes les 60 s

bool maintenanceMode = false; // lu depuis le serveur (config barrique)

// Initialisation Wi-Fi
void setupWiFiBarrique() {
  Serial.println("\n[WIFI] Initialisation...");

  WiFi.persistent(true);
  WiFi.mode(WIFI_STA);
  WiFi.begin();   // utilise les identifiants mémorisés par WiFiManager

  int tries = 0;
  // ~30 s pour tenter de se reconnecter tout seul
  while (WiFi.status() != WL_CONNECTED && tries < 60) {
    delay(500);
    Serial.print(".");
    tries++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[WIFI] Reconnexion Wi-Fi réussie sans portail.");
    Serial.print("[WIFI] SSID = ");
    Serial.println(WiFi.SSID());
    Serial.print("[WIFI] IP   = ");
    Serial.println(WiFi.localIP());
    Serial.print("[WIFI] RSSI = ");
    Serial.println(WiFi.RSSI());
    return;
  }

  Serial.println("\n[WIFI] Impossible de se reconnecter automatiquement.");
  Serial.println("[WIFI] Démarrage du portail WiFiManager...");

  WiFiManager wm;
  wm.setDebugOutput(false);
  wm.setConfigPortalTimeout(180); // portail actif max 3 minutes

  String apName = "Barrique-" + deviceId;
  bool res = wm.autoConnect(apName.c_str());

  if (!res) {
    Serial.println("[WIFI] WiFiManager ECHEC ou timeout → pas de Wi-Fi.");
  } else {
    Serial.println("[WIFI] CONNECTE via WiFiManager");
    Serial.print("[WIFI] SSID = ");
    Serial.println(WiFi.SSID());
    Serial.print("[WIFI] IP   = ");
    Serial.println(WiFi.localIP());
    Serial.print("[WIFI] RSSI = ");
    Serial.println(WiFi.RSSI());
  }
}

// Retry automatique du Wi-Fi si perdu
void retryWifiIfNeeded() {
  if (WiFi.status() == WL_CONNECTED) return;

  unsigned long now = millis();
  if (now - lastWifiRetryMs < wifiRetryInterval) return;

  lastWifiRetryMs = now;
  Serial.println("[WIFI] Non connecté → tentative auto...");

  WiFi.disconnect(true, true);
  delay(200);
  WiFi.mode(WIFI_STA);
  WiFi.begin();  // derniers identifiants connus (WiFiManager)
}

// =============================
// HTTP POST vers API barriques
// =============================
bool postMeasurement(String deviceId, uint16_t raw, int rssi, uint16_t batteryMv, time_t ts) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(F("[HTTP] WiFi non connecté"));
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure(); // Pas de vérification SSL

  HTTPClient https;

  Serial.print(F("[HTTP] POST -> "));
  Serial.println(API_URL);

  if (!https.begin(client, API_URL)) {
    Serial.println(F("[HTTP] begin() ECHEC"));
    return false;
  }

  https.addHeader("Content-Type", "application/json");

  // JSON minimal
  String payload = "{";
  payload += "\"id\":\"" + deviceId + "\",";
  payload += "\"fw\":\"" + String(FW_VERSION) + "\",";
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
// CONFIG SERVEUR (interval + maintenance)
// =============================
void checkConfigUpdate() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[CONFIG] Wi-Fi non connecté, skip.");
    return;
  }

  HTTPClient https;
  WiFiClientSecure client;
  client.setInsecure();

  String url = String(CONFIG_URL) + "?id=" + deviceId;
  Serial.print("[CONFIG] GET ");
  Serial.println(url);

  if (!https.begin(client, url)) {
    Serial.println("[CONFIG] begin() ECHEC");
    return;
  }

  int code = https.GET();
  if (code != HTTP_CODE_OK) {
    Serial.print("[CONFIG] HTTP code = ");
    Serial.println(code);
    https.end();
    return;
  }

  String body = https.getString();
  https.end();

  int start = body.indexOf('{');
  int end   = body.lastIndexOf('}');
  if (start < 0 || end <= start) {
    Serial.println("[CONFIG] JSON introuvable dans la réponse");
    Serial.println(body);
    return;
  }

  String jsonStr = body.substring(start, end + 1);
  Serial.print("[CONFIG] JSON reçu: ");
  Serial.println(jsonStr);

  StaticJsonDocument<512> doc;
  DeserializationError err = deserializeJson(doc, jsonStr);
  if (err) {
    Serial.print("[CONFIG] Erreur parse JSON: ");
    Serial.println(err.c_str());
    return;
  }

  long intervalSec = doc["measure_interval_sec"] | 0;
  bool maint       = doc["maintenance"] | false;

  // Si le serveur n’envoie pas d’intervalle, on garde la valeur actuelle
  if (intervalSec > 0) {
    // On borne : min 5 s, max 30 jours
    if (intervalSec < 5) intervalSec = 5;
    long maxSec = 30L * 24L * 3600L;
    if (intervalSec > maxSec) intervalSec = maxSec;

    measureIntervalMs = (unsigned long)intervalSec * 1000UL;
  }

  maintenanceMode = maint;

  Serial.print("[CONFIG] Interval = ");
  Serial.print(measureIntervalMs / 1000UL);
  Serial.print(" s  (");
  Serial.print(measureIntervalMs / 1000UL / 3600.0);
  Serial.println(" h)");

  Serial.print("[CONFIG] Maintenance = ");
  Serial.println(maintenanceMode ? "true" : "false");
}

// =============================
// OTA HTTP via version.json
// =============================
void checkForOTAUpdate() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[OTA] Wi-Fi non connecté, skip.");
    return;
  }

  Serial.println("\n[OTA] Vérification de mise à jour...");

  // 1) Récupérer .json
  HTTPClient https;
  WiFiClientSecure client;
  client.setInsecure();

  if (!https.begin(client, OTA_JSON_URL)) {
    Serial.println("[OTA] Impossible d'initialiser la requête sur firmware.json");
    return;
  }

  int code = https.GET();
  if (code != HTTP_CODE_OK) {
    Serial.print("[OTA] HTTP code inattendu pour firmware.json: ");
    Serial.println(code);
    https.end();
    return;
  }

  String body = https.getString();
  https.end();

  int start = body.indexOf('{');
  int end   = body.lastIndexOf('}');
  if (start < 0 || end <= start) {
    Serial.println("[OTA] JSON introuvable dans firmware.json");
    Serial.println(body);
    return;
  }

  String jsonStr = body.substring(start, end + 1);
  Serial.print("[OTA] JSON reçu: ");
  Serial.println(jsonStr);

  StaticJsonDocument<512> doc;
  DeserializationError err = deserializeJson(doc, jsonStr);
  if (err) {
    Serial.print("[OTA] Erreur parse JSON: ");
    Serial.println(err.c_str());
    return;
  }

  const char* newVersion = doc["version"] | "";
  String fwUrl           = doc["url"]     | "";

  if (strlen(newVersion) == 0 || fwUrl.length() == 0) {
    Serial.println("[OTA] Champs 'version' ou 'url' manquants.");
    return;
  }

  Serial.print("[OTA] Version distante = ");
  Serial.println(newVersion);
  Serial.print("[OTA] Version locale   = ");
  Serial.println(FW_VERSION);

  if (String(newVersion) == String(FW_VERSION)) {
    Serial.println("[OTA] Firmware déjà à jour.");
    return;
  }

  Serial.println("[OTA] Nouvelle version détectée, lancement mise à jour...");
  Serial.print("[OTA] URL firmware: ");
  Serial.println(fwUrl);

  // 2) Téléchargement et flash
  HTTPClient fwHttp;
  WiFiClientSecure fwClient;
  fwClient.setInsecure();

  if (!fwHttp.begin(fwClient, fwUrl)) {
    Serial.println("[OTA] Impossible d'initialiser la requête sur firmware.bin");
    return;
  }

  int httpCode = fwHttp.GET();
  if (httpCode != HTTP_CODE_OK) {
    Serial.print("[OTA] Code HTTP inattendu pour firmware.bin: ");
    Serial.println(httpCode);
    fwHttp.end();
    return;
  }

  int contentLength = fwHttp.getSize();
  if (contentLength <= 0) {
    Serial.println("[OTA] Taille firmware inconnue ou nulle");
    fwHttp.end();
    return;
  }

  WiFiClient* stream = fwHttp.getStreamPtr();
  Serial.printf("[OTA] Taille firmware = %d octets\n", contentLength);

  if (!Update.begin(contentLength)) {
    Serial.println("[OTA] Échec Update.begin()");
    fwHttp.end();
    return;
  }

  size_t written = Update.writeStream(*stream);
  if (written != (size_t)contentLength) {
    Serial.printf("[OTA] Écrit %u / %d octets\n", (unsigned)written, contentLength);
    Update.end();
    fwHttp.end();
    return;
  }

  if (!Update.end()) {
    Serial.println("[OTA] Update.end() a échoué");
    fwHttp.end();
    return;
  }

  if (!Update.isFinished()) {
    Serial.println("[OTA] Mise à jour incomplète");
    fwHttp.end();
    return;
  }

  Serial.println("[OTA] Mise à jour réussie, redémarrage...");
  fwHttp.end();
  delay(500);
  ESP.restart();
}

// =============================
// SETUP
// =============================
void setup() {
  Serial.begin(115200);
  delay(800);

  Serial.println();
  Serial.println(F("=== Barrique ESP32-C3 v1.0.3 — WiFiManager + OTA + Config ==="));

  deviceId = makeDeviceId9Digits();
  Serial.print(F("[ID] Device ID = "));
  Serial.println(deviceId);

  analogReadResolution(12);

  // Wi-Fi
  setupWiFiBarrique();

  // NTP (première sync)
  getTimestamp();

  // Première récupération de config
  checkConfigUpdate();

  // Première vérification OTA au démarrage
  checkForOTAUpdate();

  // Init timers
  lastMeasureMs   = millis();
  lastWifiRetryMs = millis();
}

// =============================
// LOOP
// =============================
void loop() {
  retryWifiIfNeeded();

  unsigned long now = millis();

  // Mesures périodiques + config + OTA
  if (now - lastMeasureMs >= measureIntervalMs) {
    lastMeasureMs = now;

    // 1) Récupérer la config (interval + maintenance) depuis le dashboard
    checkConfigUpdate();

    // 2) Vérifier OTA (nouvelle version ?)
    checkForOTAUpdate();

    // 3) Lecture ADC
    uint16_t raw = readAdcAveraged(PIN_CAPTEUR);
    float v = (raw * VREF) / ADC_MAX;

    Serial.print(F("[CAPTEUR] RAW = "));
    Serial.print(raw);
    Serial.print(F("   V ≈ "));
    Serial.println(v, 3);

    // RSSI
    int rssi = (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : -127;

    // Batterie (à câbler plus tard)
    uint16_t batteryMv = 0;

    // Timestamp
    time_t ts = getTimestamp();

    // 4) POST vers API
    postMeasurement(deviceId, raw, rssi, batteryMv, ts);

    // 🔜 Plus tard : si !maintenanceMode → deep sleep pour measureIntervalMs
  }

  // petite respiration, tout le reste utilise déjà des delays
}
