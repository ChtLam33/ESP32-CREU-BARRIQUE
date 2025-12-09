/*
 * Firmware ESP32-C3 — Capteur barrique — v1.0.0
 * VERSION STABLE SANS WiFiManager — WiFi en DUR
 *
 * Objectif : tester le capteur, envoyer le JSON, stabiliser WiFi.
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <time.h>

// =============================
// CONFIG UTILISATEUR
// =============================

// ⭐ TON WiFi (STA DIRECTE)
const char* WIFI_SSID = "Pixel_4396";
const char* WIFI_PASS = "4wrtfax7kfivf5s";

// URL API SERVEUR (à créer côté PHP)
const char* API_URL = "https://prod.lamothe-despujols.com/barriques/api_post.php";

#define FW_VERSION "1.0.0"

// Mode test : pas de deep sleep, envoi toutes les 5 secondes
#define TEST_INTERVAL_MS 5000

// Pin capteur niveau
const int PIN_CAPTEUR = 1;
const float VREF = 3.3;
const int ADC_MAX = 4095;

// =============================
// ID matériel sur 9 chiffres
// =============================
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
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");

  for (int i = 0; i < 10; i++) {
    time_t now = time(nullptr);
    if (now > 1700000000) return now;
    delay(300);
  }
  return 0; // Le serveur mettra l’heure
}

// =============================
// WiFi direct
// =============================
void connectWiFiDirect() {
  Serial.print(F("[WIFI] Connexion a "));
  Serial.println(WIFI_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 40) {
    Serial.print(".");
    delay(250);
    tries++;
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println(F("[WIFI] CONNECTE"));
    Serial.print(F("IP = "));
    Serial.println(WiFi.localIP());
    Serial.print(F("RSSI = "));
    Serial.println(WiFi.RSSI());
  } else {
    Serial.println(F("[WIFI] ECHEC — Vérifie SSID et mot de passe"));
  }
}

// =============================
// HTTP POST
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
    Serial.print(F("[HTTP] Reponse = "));
    Serial.println(https.getString());
  }

  https.end();
  return (code == 200 || code == 201);
}

// =============================
// SETUP
// =============================
String deviceId;

void setup() {
  Serial.begin(115200);
  delay(800);

  Serial.println();
  Serial.println(F("=== Barrique ESP32-C3 v1.0.0 — Mode Test ==="));

  deviceId = makeDeviceId9Digits();
  Serial.print(F("[ID] Device ID = "));
  Serial.println(deviceId);

  analogReadResolution(12);

  // Connexion WiFi DIRECTE (pas de WiFiManager)
  connectWiFiDirect();
}

// =============================
// LOOP
// =============================
void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(F("[WIFI] Perdu, tentative reconnection..."));
    connectWiFiDirect();
  }

  // Lecture ADC
  uint16_t raw = readAdcAveraged(PIN_CAPTEUR);
  float v = (raw * VREF) / ADC_MAX;

  Serial.print(F("[CAPTEUR] RAW = "));
  Serial.print(raw);
  Serial.print(F("   V ≈ "));
  Serial.println(v, 3);

  // Lecture RSSI
  int rssi = WiFi.RSSI();

  // Batterie (pas encore branchée)
  uint16_t batteryMv = 0;

  // Timestamp
  time_t ts = getTimestamp();

  // POST
  postMeasurement(deviceId, raw, rssi, batteryMv, ts);

  // Attente (TEST MODE)
  delay(TEST_INTERVAL_MS);
}
