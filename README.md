# Firmware ESP32-C3 – Capteurs de barriques (niveau / creux)

Firmware des capteurs de barriques basés sur **ESP32-C3** pour le Château Lamothe Despujols.

Ils mesurent le **creux** d’une barrique via un capteur analogique, envoient les données en HTTPS au serveur, supportent OTA et WiFiManager, et utilisent un deep-sleep configurable pour économiser l’énergie.

---

## ✨ Fonctionnalités principales

- Lecture du **capteur analogique** (ADC ESP32-C3, moyenne sur 40 échantillons)
- Conversion “creux → litres” (min/max), cohérente avec le dashboard barriques
- Envoi périodique des mesures via **JSON HTTPS** :
  - `https://prod.lamothe-despujols.com/barriques/api_post.php`
- Récupération des paramètres capteurs depuis :
  - `https://prod.lamothe-despujols.com/barriques/get_config.php`
- Configuration Wi-Fi via **WiFiManager**
  - AP automatique si aucun Wi-Fi n’est connu : `Barrique_Config_AP`
- Mise à jour OTA centralisée :
  - `https://prod.lamothe-despujols.com/barriques/ota_check.php`
  - Téléchargement automatique du fichier `firmware.bin` si nouvelle version
- ID matériel sur **9 chiffres** dérivé de l’eFuse MAC
- Mode **maintenance** (désactive deep-sleep pour flasher facilement en USB)
- Deep-sleep configurable depuis le dashboard (période de mesure)

---

## 📡 Architecture

### 👉 Capteur ESP32-C3  
Mesures → JSON → API PHP  

Exemple de payload :

```json
{
  "id": "330123456",
  "fw": "1.0.1",
  "value_raw": 1870,
  "rssi": -62,
  "battery_mv": 3920,
  "ts": 1765000000
}
```

### 👉 Serveur Infomaniak  
Scripts principaux :

| Script | Rôle |
|--------|------|
| `api_post.php` | Reçoit les mesures, écrit dans `logs/barriques.log` |
| `get_config.php` | Envoie la config capteur (lot, fréquence, maintenance…) |
| `ota_check.php` | Indique si un nouveau firmware est disponible |
| `firmware.bin` | Binaire OTA à installer |

Le dashboard complet se trouve dans ce dépôt :  
➡️ **https://github.com/ChtLam33/DashboardWeb**

---

## 🔧 Dépendances Firmware

| Librairie | Version |
|-----------|---------|
| ArduinoJson | 7.x |
| WiFiManager (tzapu) | 2.0.17 |
| TFmini (si capteur distance, optionnel) | 0.1.0 |
| HTTPClient | incluse ESP32 |
| Update.h | incluse ESP32 |

### Core ESP32-C3 à installer

```
esp32:esp32  (version ≥ 3.0)
```

---

## ⚙️ Paramètres importants

| Paramètre | Description |
|----------|-------------|
| `PIN_CAPTEUR` | GPIO ADC connectée au capteur de niveau |
| `FW_VERSION` | Numéro utilisé pour OTA |
| `MAINTENANCE_MODE` | Désactive deep-sleep pour flasher en USB |
| `sleep_interval_sec` | Intervalle de mesure défini par dashboard |

---

## 🔄 OTA – Mise à jour automatique

1. Démarrage de l'ESP32-C3  
2. Connexion Wi-Fi  
3. Appel à `ota_check.php`  
   - si version distante > version locale → téléchargement et installation  
4. Reboot  
5. Récupération configuration  
6. Mesure → POST JSON  
7. Deep-sleep  

OTA fonctionne **sans USB**, via serveur uniquement.

---

## 🔌 Mode maintenance (USB sans deep-sleep)

Activable via le dashboard web.

- Le capteur **ne dort plus**
- Le port USB reste actif en continu
- Permet de flasher via Arduino IDE / Codespaces facilement  
- À désactiver ensuite pour retrouver le deep-sleep normal

---

## 📁 Structure du dépôt

```
/src
  └── barrique_firmware.ino
/lib
  ├── WiFiManager
  ├── ArduinoJson
  └── ...
README.md
```

---

## 🚀 Compilation

### Avec Arduino IDE
- Board : **ESP32C3 Dev Module**
- Core : `esp32:esp32`
- Upload via USB

### Avec arduino-cli (Codespaces)

```sh
arduino-cli compile \
  --fqbn esp32:esp32:esp32c3 \
  /workspaces/GITHUB-ESP32-BARRIQUES/firmware.ino
```

---

## 🧭 Dashboard associé

Dashboard complet :  
➡️ **https://github.com/ChtLam33/DashboardWeb**

---

## 🏁 Roadmap

- Lecture température
- Historique batterie
- Optimisation part des anges
- Mode debug WiFi avancé

---

Château Lamothe Despujols – Capteurs de barriques  
🍷 *Innovation au service du Sauternes*
