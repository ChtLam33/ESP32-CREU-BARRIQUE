/*
 * Firmware ESP32-C3 — Capteur barrique — v2.0.0
 * Refonte (assistee par Claude Code) suite a l'incident de vidage de batterie :
 * - Suppression du mode maintenance : en cas d'echec reseau/config, le capteur
 *   repart dormir avec l'intervalle par defaut plutot que de rester eveille
 *   indefiniment (c'etait la cause racine de l'incident).
 * - Suppression du mode test : l'intervalle de mesure est entierement pilote
 *   par la config serveur (jours + minutes), plancher de securite a 1 minute.
 * - Verification OTA systematique a chaque reveil, avant toute decision de
 *   sommeil (plus besoin de rester eveille pour recevoir une mise a jour).
 * - Ordre au reveil : Wi-Fi -> OTA -> config serveur -> mesure + envoi -> sleep.
 * - Le message envoye au serveur inclut desormais des champs de diagnostic
 *   (wifi_ok, ota_ok, config_ok) pour faciliter le diagnostic a distance.
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
#include <Preferences.h>

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
const char* FIRMWARE_VERSION = "2.0.0";

// =============================
// HARDWARE & ADC
// =============================
const int   PIN_CAPTEUR = 1;   // niveau
const int   PIN_BAT     = 0;   // batterie (point milieu diviseur)
const float VREF        = 3.3f;
const int   ADC_MAX     = 4095;

// Diviseur batterie : BAT+ -> R_TOP -> (GPIO0) -> R_BOT -> GND
// Ici : R_TOP = 100k, R_BOT = 100k => VBAT = VGPIO * 2
const float R_TOP_OHMS = 100000.0f;
const float R_BOT_OHMS = 100000.0f;

// =============================
// CONFIG MESURE
// =============================
const unsigned long DEFAULT_MEASURE_INTERVAL_S = 7UL * 24UL * 3600UL; // 7 jours, utilise si la config serveur est injoignable
const unsigned long MIN_INTERVAL_MS            = 60000UL;             // 1 minute : plancher de securite (temps mini pour se reveiller/rendormir)

unsigned long measureIntervalMs = DEFAULT_MEASURE_INTERVAL_S * 1000UL;

// =============================
// ID materiel (9 chiffres)
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
// ADC MOYENNE
// =============================
uint16_t readAdcAveraged(int pin, int samples = 40) {
  uint32_t sum = 0;
  for (int i = 0; i < samples; i++) {
    sum += analogRead(pin);
    delay(3);
  }
  return (uint16_t)(sum / samples);
}

// =============================
// BATTERIE (mV)
// =============================
uint16_t readBatteryMv(int samples = 40) {
  uint16_t raw = readAdcAveraged(PIN_BAT, samples);

  float v_mid = (raw * VREF) / (float)ADC_MAX; // V au GPIO
  float ratio = (R_TOP_OHMS + R_BOT_OHMS) / R_BOT_OHMS;
  float v_bat = v_mid * ratio; // VBAT estimee

  if (v_bat < 0.0f) v_bat = 0.0f;
  if (v_bat > 6.0f) v_bat = 6.0f;

  return (uint16_t)roundf(v_bat * 1000.0f);
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
// Provisionnement (memoire permanente, survit aux redemarrages et coupures)
// =============================
bool isProvisioned() {
  Preferences prefs;
  prefs.begin("barrique", true); // lecture seule
  bool done = prefs.getBool("provisioned", false);
  prefs.end();
  return done;
}

void markProvisioned() {
  Preferences prefs;
  prefs.begin("barrique", false);
  if (!prefs.getBool("provisioned", false)) {
    prefs.putBool("provisioned", true);
  }
  prefs.end();
}

// =============================
// Wi-Fi
// =============================
static bool tryQuickReconnect(unsigned long timeoutMs) {
  Serial.println(F("[WiFi] Tentative reconnexion rapide..."));
  WiFi.mode(WIFI_STA);
  WiFi.persistent(true);

  // si les identifiants sont deja en NVS, WiFi.begin() sans args suffit
  WiFi.begin();

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < timeoutMs) {
    delay(250);
    Serial.print('.');
  }
  Serial.println();

  return (WiFi.status() == WL_CONNECTED);
}

void setupWiFi() {
  Serial.println(F("\n[WiFi] Initialisation..."));

  // 1) Reco rapide (ne bloque pas 30 secondes inutilement)
  if (tryQuickReconnect(8000UL)) {
    Serial.println(F("[WiFi] Reconnexion OK (sans portail)."));
  } else {
    Serial.println(F("[WiFi] Reconnexion echouee -> WiFiManager (portail)."));

    WiFiManager wm;
    wm.setDebugOutput(false);
    wm.setConnectTimeout(20);         // tentative de connexion AP->STA max 20 s
    wm.setWiFiAutoReconnect(true);

    if (isProvisioned()) {
      // Capteur deja configure avec succes au moins une fois : ne pas rester
      // eveille indefiniment pour un simple accroc reseau passager.
      wm.setConfigPortalTimeout(180); // 3 minutes
      Serial.println(F("[WiFi] Deja provisionne -> portail limite a 3 min."));
    } else {
      // Premiere configuration : on laisse tout le temps necessaire a
      // l'humain pour renseigner le Wi-Fi (pas de setConfigPortalTimeout()
      // = pas de limite, WiFiManager attend indefiniment).
      Serial.println(F("[WiFi] Premiere configuration -> portail sans limite de temps."));
    }

    String apName = "Barrique-" + deviceId;
    Serial.print(F("[WiFi] AP config = "));
    Serial.println(apName);

    // autoConnect : demarre AP+portail, essaie de se connecter, puis rend la main
    bool ok = wm.autoConnect(apName.c_str());

    if (!ok) {
      Serial.println(F("[WiFi] WiFiManager timeout/echec -> pas de Wi-Fi (continue)."));
      // On laisse WiFi en STA pour permettre retries plus tard
      WiFi.mode(WIFI_STA);
    }
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print(F("[WiFi] CONNECTE a "));
    Serial.println(WiFi.SSID());
    Serial.print(F("       IP = "));
    Serial.println(WiFi.localIP());
    Serial.print(F("       RSSI = "));
    Serial.println(WiFi.RSSI());
  } else {
    Serial.println(F("[WiFi] Toujours pas connecte."));
  }
}

// =============================
// HTTP POST mesures (+ diagnostic)
// =============================
bool postMeasurement(uint16_t raw, int rssi, uint16_t batteryMv, time_t ts,
                      bool wifiOk, bool otaOk, bool configOk) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(F("[HTTP] Wi-Fi non connecte, envoi annule."));
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
  payload += "\"ts\":" + String((unsigned long)ts) + ",";
  payload += "\"wifi_ok\":" + String(wifiOk ? "true" : "false") + ",";
  payload += "\"ota_ok\":" + String(otaOk ? "true" : "false") + ",";
  payload += "\"config_ok\":" + String(configOk ? "true" : "false");
  payload += "}";

  Serial.print(F("[HTTP] Payload = "));
  Serial.println(payload);

  int code = https.POST(payload);
  Serial.print(F("[HTTP] Code = "));
  Serial.println(code);

  if (code > 0) {
    Serial.print(F("[HTTP] Reponse = "));
    Serial.println(https.getString());
  }

  https.end();
  return (code == 200 || code == 201);
}

// =============================
// SEMVER & OTA
// =============================
void parseSemver(const String& v, int &maj, int &min, int &pat) {
  maj = min = pat = 0;
  int p1 = v.indexOf('.');
  int p2 = (p1 >= 0) ? v.indexOf('.', p1 + 1) : -1;

  if (p1 < 0) { maj = v.toInt(); return; }
  maj = v.substring(0, p1).toInt();

  if (p2 < 0) { min = v.substring(p1 + 1).toInt(); return; }
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

// Retourne true si la verification OTA a pu etre menee a bien (mise a jour
// trouvee et appliquee, ou pas de mise a jour necessaire). Retourne false en
// cas d'echec reseau/serveur (le firmware ne bloque jamais le sommeil pour
// autant : on continue le cycle normalement).
bool checkForOTAUpdate() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(F("[OTA] Wi-Fi non connecte, skip."));
    return false;
  }

  Serial.println(F("\n[OTA] Verification de mise a jour..."));

  WiFiClientSecure client;
  client.setInsecure();

  if (!client.connect(SERVER_HOST, SERVER_PORT)) {
    Serial.println(F("[OTA] Connexion HTTPS echouee (firmware.json)"));
    return false;
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
  while (client.available()) payload += client.readString();
  client.stop();

  int start = payload.indexOf('{');
  int end   = payload.lastIndexOf('}');
  if (start < 0 || end <= start) {
    Serial.println(F("[OTA] JSON introuvable dans la reponse :"));
    Serial.println(payload);
    return false;
  }

  String jsonStr = payload.substring(start, end + 1);
  Serial.println(F("[OTA] JSON firmware.json ="));
  Serial.println(jsonStr);

  StaticJsonDocument<512> doc;
  DeserializationError err = deserializeJson(doc, jsonStr);
  if (err) {
    Serial.println(F("[OTA] Erreur parse JSON firmware.json"));
    Serial.println(err.c_str());
    return false;
  }

  String remoteVersion = doc["version"] | "";
  String fwUrl         = doc["url"]     | "";

  if (remoteVersion.length() == 0 || fwUrl.length() == 0) {
    Serial.println(F("[OTA] Champs 'version' ou 'url' manquants"));
    return false;
  }

  Serial.print(F("[OTA] Version distante = "));
  Serial.println(remoteVersion);
  Serial.print(F("[OTA] Version locale   = "));
  Serial.println(FIRMWARE_VERSION);

  int cmp = compareSemver(String(FIRMWARE_VERSION), remoteVersion);
  if (cmp >= 0) {
    Serial.println(F("[OTA] Firmware deja a jour ou plus recent, pas d'update."));
    return true;
  }

  Serial.println(F("[OTA] Nouvelle version detectee, telechargement..."));
  Serial.print(F("[OTA] URL firmware = "));
  Serial.println(fwUrl);

  HTTPClient https;
  WiFiClientSecure fwClient;
  fwClient.setInsecure();

  if (!https.begin(fwClient, fwUrl)) {
    Serial.println(F("[OTA] https.begin() echoue"));
    return false;
  }

  int httpCode = https.GET();
  if (httpCode != HTTP_CODE_OK) {
    Serial.print(F("[OTA] Code HTTP inattendu: "));
    Serial.println(httpCode);
    https.end();
    return false;
  }

  int contentLength = https.getSize();
  if (contentLength <= 0) {
    Serial.println(F("[OTA] Taille firmware invalide"));
    https.end();
    return false;
  }

  WiFiClient *stream = https.getStreamPtr();
  Serial.printf("[OTA] Taille firmware = %d octets\n", contentLength);

  if (!Update.begin(contentLength)) {
    Serial.println(F("[OTA] Update.begin() echoue"));
    https.end();
    return false;
  }

  size_t written = Update.writeStream(*stream);
  if (written != (size_t)contentLength) {
    Serial.printf("[OTA] Ecrit %u / %d octets\n", (unsigned)written, contentLength);
    Update.end();
    https.end();
    return false;
  }

  if (!Update.end()) {
    Serial.println(F("[OTA] Update.end() a echoue"));
    https.end();
    return false;
  }

  if (!Update.isFinished()) {
    Serial.println(F("[OTA] Mise a jour incomplete"));
    https.end();
    return false;
  }

  Serial.println(F("[OTA] Mise a jour reussie, redemarrage..."));
  https.end();
  delay(500);
  ESP.restart();
  return true; // jamais atteint (ESP.restart() ne rend pas la main)
}

// =============================
// CONFIG SERVEUR
// =============================
void applyDefaultConfig() {
  measureIntervalMs = DEFAULT_MEASURE_INTERVAL_S * 1000UL;
}

// Retourne true si la config serveur a ete recuperee et appliquee avec succes.
// En cas d'echec (Wi-Fi, connexion, JSON invalide), on revient a l'intervalle
// par defaut et le capteur repart dormir normalement, il retentera au
// prochain reveil.
bool checkConfigUpdate() {
  applyDefaultConfig();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(F("[CFG] Wi-Fi non connecte, config par defaut."));
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();

  String url = String(CONFIG_PATH);

  Serial.print(F("[CFG] GET "));
  Serial.println(url);

  if (!client.connect(SERVER_HOST, SERVER_PORT)) {
    Serial.println(F("[CFG] Connexion HTTPS echouee"));
    return false;
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
  while (client.available()) payload += client.readString();
  client.stop();

  int start = payload.indexOf('{');
  int end   = payload.lastIndexOf('}');
  if (start < 0 || end <= start) {
    Serial.println(F("[CFG] JSON introuvable dans la reponse"));
    Serial.println(payload);
    return false;
  }

  String jsonStr = payload.substring(start, end + 1);
  Serial.println(F("[CFG] JSON recu ="));
  Serial.println(jsonStr);

  StaticJsonDocument<512> doc;
  DeserializationError err = deserializeJson(doc, jsonStr);
  if (err) {
    Serial.println(F("[CFG] Erreur parse JSON config"));
    Serial.println(err.c_str());
    return false;
  }

  // Seul measure_interval_s est encore utilise (calcule cote serveur a partir
  // des champs jours + minutes). D'anciens champs "maintenance"/"test_mode"
  // eventuellement encore renvoyes par le serveur sont ignores sans erreur.
  unsigned long intervalS = doc["measure_interval_s"] | DEFAULT_MEASURE_INTERVAL_S;
  if (intervalS == 0) intervalS = DEFAULT_MEASURE_INTERVAL_S;

  unsigned long intervalMs = intervalS * 1000UL;
  if (intervalMs < MIN_INTERVAL_MS) {
    Serial.print(F("[CFG] Intervalle recu trop court, plancher applique (ms) = "));
    Serial.println(MIN_INTERVAL_MS);
    intervalMs = MIN_INTERVAL_MS;
  }
  measureIntervalMs = intervalMs;

  Serial.print(F("[CFG] measure_interval_s (effectif) = "));
  Serial.println(measureIntervalMs / 1000UL);

  return true;
}

// =============================
// DEEP SLEEP
// =============================
void goToDeepSleep(unsigned long intervalMs) {
  if (intervalMs < MIN_INTERVAL_MS) intervalMs = MIN_INTERVAL_MS;

  Serial.print(F("[SLEEP] Prochain reveil dans (ms) = "));
  Serial.println(intervalMs);
  Serial.println(F("[SLEEP] Bonne nuit..."));

  uint64_t sleepUs = (uint64_t)intervalMs * 1000ULL;
  esp_sleep_enable_timer_wakeup(sleepUs);
  delay(200);
  esp_deep_sleep_start();
}

// =============================
// UNE MESURE COMPLETE (ADC + POST)
// =============================
void doOneMeasurement(bool wifiOk, bool otaOk, bool configOk) {
  uint16_t raw = readAdcAveraged(PIN_CAPTEUR);
  float v = (raw * VREF) / (float)ADC_MAX;

  Serial.print(F("[CAPTEUR] RAW = "));
  Serial.print(raw);
  Serial.print(F("   V ~= "));
  Serial.println(v, 3);

  uint16_t batteryMv = readBatteryMv();
  Serial.print(F("[BAT] battery_mv = "));
  Serial.println(batteryMv);

  int rssi = (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : -127;
  time_t ts = getTimestamp();

  postMeasurement(raw, rssi, batteryMv, ts, wifiOk, otaOk, configOk);
}

// =============================
// SETUP - tout se joue ici, un seul cycle par reveil :
// Wi-Fi -> OTA -> config -> mesure/envoi -> deep sleep
// =============================
void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println();
  Serial.print(F("=== Barrique ESP32-C3 v"));
  Serial.print(FIRMWARE_VERSION);
  Serial.println(F(" -- Wi-Fi + OTA + Config + DeepSleep + BAT ==="));

  deviceId = makeDeviceId9Digits();
  Serial.print(F("[ID] Device ID = "));
  Serial.println(deviceId);

  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);

  setupWiFi();
  bool wifiOk = (WiFi.status() == WL_CONNECTED);

  bool otaOk    = checkForOTAUpdate();   // redemarre seul si une mise a jour est appliquee
  bool configOk = checkConfigUpdate();   // ajuste measureIntervalMs, sinon garde le defaut

  if (configOk) {
    markProvisioned(); // le capteur a reussi a parler au serveur au moins une fois
  }

  doOneMeasurement(wifiOk, otaOk, configOk);

  Serial.print(F("[CYCLE] wifi_ok="));
  Serial.print(wifiOk);
  Serial.print(F(" ota_ok="));
  Serial.print(otaOk);
  Serial.print(F(" config_ok="));
  Serial.println(configOk);

  goToDeepSleep(measureIntervalMs);
  // jamais atteint : esp_deep_sleep_start() ne rend pas la main
}

// =============================
// LOOP - jamais executee : chaque cycle se termine par un deep sleep dans
// setup(), qui redemarre l'ESP32 depuis le debut au reveil suivant.
// =============================
void loop() {
}
