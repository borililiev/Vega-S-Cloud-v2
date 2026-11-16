/*
  Infrared Heater ESP-12F / ESP12F_Relay_X1_V1.2
  Arduino IDE: ESP8266 core, board "Generic ESP8266 Module" or "NodeMCU 1.0"

  !!! DEVELOPMENT / BENCH-TEST FIRMWARE. DO NOT USE ON 230 V BEFORE
  BOARD REVISION, RELAY POLARITY, WIRING AND INDEPENDENT SAFETY PROTECTION
  HAVE BEEN VERIFIED BY A QUALIFIED ELECTRICIAN.

  GPIO5  -> on-board relay (HIGH = energised on documented X1 V1.2)
  GPIO14 -> AM2301/DHT21 data; sensor VCC=3V3, GND=GND
  GPIO12 -> EXTERNAL momentary pushbutton to GND (INPUT_PULLUP)
  The on-board pushbutton is RESET, not a usable mode button.

  Libraries: DHTNew (Rob Tillaart), ArduinoJson v7.
*/

#include <Arduino.h>
#include <stdarg.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <DNSServer.h>
#include <LittleFS.h>
#include <Updater.h>  // Firmware-only HTTP OTA (ESP8266 core)
#include <ArduinoJson.h>
#include <dhtnew.h>
#include <math.h>
#include <time.h>
#include <TZ.h>
#include <coredecls.h> // settimeofday_cb: real NTP success notification
#include "HeaterStats.h"
#include "HeaterIcons.h"
#include "HeaterOnboarding.h"

constexpr uint8_t RELAY_GPIO = 5;
constexpr uint8_t DEFAULT_SENSOR_GPIO = 14;
constexpr uint8_t BUTTON_GPIO = 12;
constexpr uint8_t RELAY_ON_LEVEL = HIGH;  // Confirm on the UNPOWERED / BENCH board!
constexpr uint8_t RELAY_OFF_LEVEL = LOW;
constexpr const char *AP_PASSWORD = "12345678";
constexpr unsigned long AP_FALLBACK_DELAY = 12000UL;
constexpr unsigned long AP_RETRY_INTERVAL = 10000UL;
constexpr unsigned long ONBOARDING_AP_GRACE_MS = 180000UL; // 3 minutes
constexpr unsigned long SENSOR_INTERVAL = 4000UL;
constexpr unsigned long SENSOR_STALE = 16000UL;
constexpr unsigned long BOOT_DELAY = 20000UL;
constexpr unsigned long OFF_TIMER_TIME = 2UL * 60UL * 60UL * 1000UL;
constexpr unsigned long NTP_REFRESH_INTERVAL = 6UL * 60UL * 60UL * 1000UL;
constexpr unsigned long NTP_RETRY_INTERVAL = 60UL * 1000UL;
constexpr unsigned long NTP_RECONNECT_MIN_INTERVAL = 30UL * 1000UL;
constexpr float HARD_ROOM_LIMIT = 29.0f; // Non-latching safety stop while valid sensor >= 29 C
// User-adjustable asymmetric switching thresholds and dwell times.
// Defaults tuned as a STARTING POINT for an inertial 400 W quartz panel.
constexpr float DEFAULT_HEAT_ON_DELTA = 0.6f;
constexpr float DEFAULT_HEAT_OFF_DELTA = 0.0f;
constexpr uint16_t DEFAULT_MIN_ON_MINUTES = 10;
constexpr uint16_t DEFAULT_MIN_OFF_MINUTES = 10;

struct Settings {
  bool enabled = false;               // Safe default: heater disabled
  bool manualMode = false;            // Explicit 2-hour manual override
  uint8_t sensorPin = DEFAULT_SENSOR_GPIO;
  uint8_t sensorType = 70;            // Sonoff AM2301 V1.0 (2019): often Si7021 protocol
  uint16_t powerW = 400;               // Nominal heater power for kWh/cost estimates
  float target = 21.0f;
  // On when roomTemp <= target - onDelta; off when roomTemp >= target + offDelta.
  float heatOnDelta = DEFAULT_HEAT_ON_DELTA;
  float heatOffDelta = DEFAULT_HEAT_OFF_DELTA;
  uint16_t minOnMinutes = DEFAULT_MIN_ON_MINUTES;
  uint16_t minOffMinutes = DEFAULT_MIN_OFF_MINUTES;
  // EUR/kWh; zero means not configured and prices are not displayed.
  float dayRate = 0.0f;
  float nightRate = 0.0f;
  uint8_t dayStartHour = 7;
  uint8_t nightStartHour = 23;
} cfg;

ESP8266WebServer httpServer(80);
DNSServer dnsServer;
DHTNEW *dht = nullptr;
HeaterStats heaterStats;

String deviceId, apName, csrf;
// OTA firmware upload: local STA only. Password is stored separately in LittleFS.
// OTA functionality is DISABLED until the user sets a >=12 character password.
String otaPassword;
String otaUploadRoute;
bool otaUploadAuthorized = false;
bool otaUploadStarted = false;
bool otaUploadSucceeded = false;
bool otaUploading = false;
String otaUploadError;
unsigned long otaRebootAt = 0;

float roomTemp = NAN, roomHumidity = NAN;
bool relayOn = false;
bool apRunning = false;
// Local Wi-Fi onboarding state. The thermostat is always independent of onboarding.
bool netStatic = false, netRecoveryTried = false;
IPAddress netIP, netGateway, netMask, netDNS;
String netNetworkName;
bool wifiChangePending = false, onboardingActive = false;
bool onboardingSuccess = false, onboardingWantStatic = true;
unsigned long wifiChangeQueuedAt = 0, onboardingStartedAt = 0, onboardingConnectedAt = 0;
String onboardingSSID, onboardingPassword, onboardingError;
IPAddress onboardingDHCPIP;
unsigned long bootNetworkAt = 0;
unsigned long wifiDisconnectedAt = 0, lastApStartAttempt = 0;
bool offTimerActive = false;
bool buttonWasDown = false, buttonLongDone = false;
unsigned long sensorErrorCount = 0;
unsigned long lastSensorDiagnostic = 0;
unsigned long lastButtonEdge = 0, buttonDownAt = 0;
unsigned long lastSensor = 0, lastGoodSensor = 0, lastRelayChange = 0;
unsigned long relayStartedAt = 0;
unsigned long offTimerStartedAt = 0;
unsigned long lastNtpRequestAt = 0;
volatile unsigned long lastNtpSuccessAt = 0;
volatile uint32_t ntpSuccessCount = 0;
uint32_t ntpCountAtLastRequest = 0;
bool ntpRequestStarted = false;
bool ntpWasConnected = false;
bool settingsWritePending = false;
unsigned long lastSettingsChange = 0;
unsigned long lastHistoryRequest = 0;
unsigned long lastWebActivity = 0;
uint32_t lastHistoryBuildMs = 0;
uint32_t lastHistoryResponseBytes = 0;
// Keep only one short lived history JSON response cached in RAM.

void touchWeb() { lastWebActivity = millis(); }
void invalidateHistoryCache() { /* History computed on demand, no RAM cache. */ }

constexpr unsigned long TARGET_SAVE_DEBOUNCE = 8000UL;

void requestNtpSync() {
  // Non-blocking: SNTP contacts the servers in the background.
  ntpCountAtLastRequest = ntpSuccessCount;
  configTime(TZ_Europe_Sofia, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
  lastNtpRequestAt = millis();
  ntpRequestStarted = true;
  Serial.println("[NTP] Synchronization requested.");
}

void clockTask() {
  const bool connected = WiFi.status() == WL_CONNECTED;
  if (!connected) {
    ntpWasConnected = false;
    return;
  }
  const unsigned long now = millis();
  const unsigned long sinceRequest = now - lastNtpRequestAt;
  const bool firstRequest = !ntpRequestStarted;
  const bool retry = ntpRequestStarted && ntpCountAtLastRequest == ntpSuccessCount &&
                     sinceRequest >= NTP_RETRY_INTERVAL;
  const bool periodic = ntpRequestStarted && sinceRequest >= NTP_REFRESH_INTERVAL;
  const bool reconnected = !ntpWasConnected && ntpRequestStarted &&
                           sinceRequest >= NTP_RECONNECT_MIN_INTERVAL;
  if (firstRequest || retry || periodic || reconnected) requestNtpSync();
  ntpWasConnected = true;
}

String safeHtml(String s) {
  s.replace("&", "&amp;"); s.replace("<", "&lt;");
  s.replace(">", "&gt;"); s.replace("\"", "&quot;"); s.replace("'", "&#39;");
  return s;
}

bool validNumber(float value, float low, float high) {
  return isfinite(value) && value >= low && value <= high;
}

bool isSupportedPower(uint16_t watts) {
  return watts == 250 || watts == 400 || watts == 600 ||
         watts == 750 || watts == 1250 || watts == 2000;
}

String deviceName() { return "Vega-S " + String(cfg.powerW) + " W"; }

String makeSecret(size_t length) {
  static const char characters[] = "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789";
  String out; out.reserve(length);
  for (size_t i = 0; i < length; ++i)
    out += characters[ESP.random() % (sizeof(characters) - 1)];
  return out;
}

void saveSettings() {
  settingsWritePending = false;
  JsonDocument d;
  d["firmware"] = "Performance-v6.6-Cloud";
  d["powerW"] = cfg.powerW;
  d["enabled"] = cfg.enabled;
  d["manualMode"] = cfg.manualMode;
  d["sensorPin"] = cfg.sensorPin;
  d["sensorType"] = cfg.sensorType;
  d["offTimerPending"] = offTimerActive;
  d["target"] = cfg.target;
  d["heatOnDelta"] = cfg.heatOnDelta;
  d["heatOffDelta"] = cfg.heatOffDelta;
  d["minOnMinutes"] = cfg.minOnMinutes;
  d["minOffMinutes"] = cfg.minOffMinutes;
  d["dayRate"] = cfg.dayRate;
  d["nightRate"] = cfg.nightRate;
  d["dayStartHour"] = cfg.dayStartHour;
  d["nightStartHour"] = cfg.nightStartHour;
  File f = LittleFS.open("/settings.json", "w");
  if (f) { serializeJson(d, f); f.close(); }
}


// Keep the Wi-Fi lease separately from thermostat settings and statistics.
// No Wi-Fi password is stored in LittleFS: the station SDK manages credentials.
void saveNetworkConfig() {
  JsonDocument doc;
  doc["static"] = netStatic;
  doc["ssid"] = netNetworkName;
  doc["ip"] = netIP.toString();
  doc["gateway"] = netGateway.toString();
  doc["subnet"] = netMask.toString();
  doc["dns"] = netDNS.toString();
  File f = LittleFS.open("/network.json", "w");
  if (f) { serializeJson(doc, f); f.close(); }
}

void loadNetworkConfig() {
  File f = LittleFS.open("/network.json", "r");
  if (!f) return;
  JsonDocument doc;
  if (!deserializeJson(doc, f)) {
    netStatic = doc["static"] | false;
    netNetworkName = String(doc["ssid"] | "");
    IPAddress ip, gw, mask, dns;
    const bool valid = ip.fromString(String(doc["ip"] | "")) &&
      gw.fromString(String(doc["gateway"] | "")) &&
      mask.fromString(String(doc["subnet"] | "")) &&
      dns.fromString(String(doc["dns"] | ""));
    if (valid && ip != IPAddress(0,0,0,0) &&
        gw != IPAddress(0,0,0,0) && mask != IPAddress(0,0,0,0)) {
      netIP=ip; netGateway=gw; netMask=mask; netDNS=dns;
    } else netStatic=false;
  }
  f.close();
}

// Call only after Wi-Fi association has been established through DHCP.
// This freezes the same lease on the station without changing IP digits.
void setStaticFromLease() {
  const IPAddress ip=WiFi.localIP(), gw=WiFi.gatewayIP(), mask=WiFi.subnetMask();
  IPAddress dns=WiFi.dnsIP(0);
  if (dns == IPAddress(0,0,0,0)) dns=gw;
  onboardingDHCPIP=ip;
  if (ip == IPAddress(0,0,0,0) || gw == IPAddress(0,0,0,0) ||
      mask == IPAddress(0,0,0,0)) {
    onboardingError="Неуспешно получаване на IP, gateway или маска.";
    return;
  }
  if (!WiFi.config(ip, gw, mask, dns)) {
    onboardingError="WiFi.config отказа статична настройка. Оставаме с DHCP.";
    return;
  }
  netStatic=true;
  netIP=ip; netGateway=gw; netMask=mask; netDNS=dns;
  netNetworkName=onboardingSSID;
  saveNetworkConfig();
  Serial.println("[SETUP] Static IP from DHCP lease: "+ip.toString());
}

// Browser cannot reach the STA IP while it is still connected to the device AP.
// Send the final address and keep the AP alive for a short transition window.
void setupWizardPage() {
  touchWeb();
  httpServer.setContentLength(CONTENT_LENGTH_UNKNOWN);
  httpServer.sendHeader("Cache-Control", "no-store");
  httpServer.send(200, "text/html; charset=utf-8", "");
  httpServer.sendContent_P(SETUP_WIZARD_TOP);
  httpServer.sendContent(csrf);
  httpServer.sendContent_P(SETUP_WIZARD_END);
  httpServer.sendContent("");
}

void setupInfoApi() {
  touchWeb();
  JsonDocument d;
  const bool connected=WiFi.status()==WL_CONNECTED;
  d["ap"] = apName;
  d["ap_active"] = apRunning;
  d["connected"] = connected;
  d["connecting"] = wifiChangePending || (onboardingActive && !onboardingSuccess);
  d["success"] = onboardingSuccess;
  d["ssid"] = connected ? WiFi.SSID() : onboardingSSID;
  d["static_ip"] = netStatic;
  d["ip"] = connected ? WiFi.localIP().toString() :
      (onboardingSuccess ? onboardingDHCPIP.toString() : "");
  d["url"] = String("http://") + (connected ? WiFi.localIP().toString() : onboardingDHCPIP.toString()) + "/";
  d["gateway"] = connected ? WiFi.gatewayIP().toString() : netGateway.toString();
  d["subnet"] = connected ? WiFi.subnetMask().toString() : netMask.toString();
  d["dns"] = connected ? WiFi.dnsIP(0).toString() : netDNS.toString();
  d["error"] = onboardingError;
  d["seconds_to_ap_off"] = (apRunning && onboardingConnectedAt != 0 &&
      millis()-onboardingConnectedAt < ONBOARDING_AP_GRACE_MS) ?
      ((ONBOARDING_AP_GRACE_MS-(millis()-onboardingConnectedAt))/1000UL) : 0;
  String json; serializeJson(d,json);
  httpServer.sendHeader("Cache-Control", "no-store");
  httpServer.send(200,"application/json; charset=utf-8",json);
}

void setupNetworksApi() {
  touchWeb();
  int count=WiFi.scanComplete();
  if (count == -2) { // scan not started
    WiFi.scanNetworks(true); // asynchronous scan; do not block the thermostat
    httpServer.send(202,"application/json","{\"scanning\":true,\"networks\":[]}");
    return;
  }
  if (count == -1) { // scan running
    httpServer.send(202,"application/json","{\"scanning\":true,\"networks\":[]}");
    return;
  }
  JsonDocument doc;
  doc["scanning"]=false;
  JsonArray list=doc["networks"].to<JsonArray>();
  // Limit JSON response size and avoid duplicates.
  for (int i=0;i<count && list.size()<14;i++) {
    String name=WiFi.SSID(i);
    if (!name.length()) continue;
    bool exists=false;
    for (JsonVariant v:list) if (v["ssid"].as<String>()==name) { exists=true; break; }
    if (exists) continue;
    JsonObject row=list.add<JsonObject>();
    row["ssid"]=name;
    row["rssi"]=WiFi.RSSI(i);
    row["open"]=WiFi.encryptionType(i)==ENC_TYPE_NONE;
  }
  WiFi.scanDelete();
  String json;serializeJson(doc,json);
  httpServer.sendHeader("Cache-Control","no-store");
  httpServer.send(200,"application/json; charset=utf-8",json);
}

void startSetupConnection() {
  if (millis()-wifiChangeQueuedAt < 900UL) return; // Let HTTP response finish.
  wifiChangePending=false;
  onboardingStartedAt=millis();
  onboardingConnectedAt=0;
  onboardingSuccess=false;
  onboardingDHCPIP=IPAddress(0,0,0,0);
  onboardingError="";
  // Also expose AP during network changes initiated from the home interface.
  // Its three-minute lifetime lets the phone discover the new URL.
  if (!apRunning) {
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAPConfig(IPAddress(192,168,4,1),IPAddress(192,168,4,1),IPAddress(255,255,255,0));
    apRunning=WiFi.softAP(apName.c_str(), AP_PASSWORD);
    if (apRunning) {
      dnsServer.start(53,"*",IPAddress(192,168,4,1));
      httpServer.stop();httpServer.begin();
    }
  }
  netStatic=false;
  saveNetworkConfig();
  // Reset previous lease before requesting a new DHCP address.
  WiFi.config(0U,0U,0U);
  WiFi.persistent(true);
  WiFi.begin(onboardingSSID.c_str(),onboardingPassword.c_str());
  WiFi.persistent(false);
  onboardingPassword="";
  Serial.println("[SETUP] Connecting to home Wi-Fi: "+onboardingSSID);
}

void deferSettingsSave() {
  settingsWritePending = true;
  lastSettingsChange = millis();
}
void pendingSettingsTask() {
  if (settingsWritePending && millis() - lastSettingsChange >= TARGET_SAVE_DEBOUNCE)
    saveSettings();
}

void loadSettings() {
  bool interruptedOffTimer = false;
  File f = LittleFS.open("/settings.json", "r");
  if (f) {
    JsonDocument d;
    if (!deserializeJson(d, f)) {
      cfg.enabled = d["enabled"] | false;
      cfg.manualMode = d["manualMode"] | false;
      cfg.sensorPin = d["sensorPin"] | DEFAULT_SENSOR_GPIO;
      cfg.sensorType = d["sensorType"] | 70;
      cfg.powerW = d["powerW"] | 400;
      interruptedOffTimer = d["offTimerPending"] | false;
      cfg.target = d["target"] | 21.0f;
      cfg.heatOnDelta = d["heatOnDelta"] | DEFAULT_HEAT_ON_DELTA;
      cfg.heatOffDelta = d["heatOffDelta"] | DEFAULT_HEAT_OFF_DELTA;
      cfg.minOnMinutes = d["minOnMinutes"] | DEFAULT_MIN_ON_MINUTES;
      cfg.minOffMinutes = d["minOffMinutes"] | DEFAULT_MIN_OFF_MINUTES;
      cfg.dayRate = d["dayRate"] | 0.0f;
      cfg.nightRate = d["nightRate"] | 0.0f;
      cfg.dayStartHour = d["dayStartHour"] | 7;
      cfg.nightStartHour = d["nightStartHour"] | 23;
    }
    f.close();
  }
  if (!validNumber(cfg.target, 12, 28)) cfg.target = 21;
  if (!validNumber(cfg.heatOnDelta, 0.1f, 3.0f)) cfg.heatOnDelta = DEFAULT_HEAT_ON_DELTA;
  if (!validNumber(cfg.heatOffDelta, -1.0f, 2.0f) ||
      cfg.heatOnDelta + cfg.heatOffDelta < 0.1f) cfg.heatOffDelta = DEFAULT_HEAT_OFF_DELTA;
  if (cfg.minOnMinutes < 1 || cfg.minOnMinutes > 30) cfg.minOnMinutes = DEFAULT_MIN_ON_MINUTES;
  if (cfg.minOffMinutes < 1 || cfg.minOffMinutes > 30) cfg.minOffMinutes = DEFAULT_MIN_OFF_MINUTES;
  if (cfg.sensorPin != 4 && cfg.sensorPin != 13 && cfg.sensorPin != 14)
    cfg.sensorPin = DEFAULT_SENSOR_GPIO;
  if (cfg.sensorType != 22 && cfg.sensorType != 70) cfg.sensorType = 70;
  if (!isSupportedPower(cfg.powerW)) cfg.powerW = 400;
  if (!validNumber(cfg.dayRate, 0.0f, 5.0f)) cfg.dayRate = 0;
  if (!validNumber(cfg.nightRate, 0.0f, 5.0f)) cfg.nightRate = 0;
  if (cfg.dayStartHour > 23) cfg.dayStartHour = 7;
  if (cfg.nightStartHour > 23 || cfg.nightStartHour == cfg.dayStartHour)
    cfg.nightStartHour = 23;
  // millis() cannot recover the expiry after a power loss. Restore OFF safely.
  if (interruptedOffTimer || cfg.manualMode) {
    offTimerActive = false;
    cfg.enabled = false;
    cfg.manualMode = false;
    saveSettings();
  }
}

bool sensorHealthy() {
  return lastGoodSensor != 0 && (millis() - lastGoodSensor) <= SENSOR_STALE;
}

void applySensorConfiguration() {
  // Reconstruct DHTNew because its GPIO is fixed in the constructor.
  // Called only at setup or explicit settings changes, never every reading.
  delete dht;
  dht = new DHTNEW(cfg.sensorPin);
  dht->setType(cfg.sensorType);
  roomTemp = NAN;
  roomHumidity = NAN;
  lastGoodSensor = 0;
  lastSensor = millis();
  sensorErrorCount = 0;
  lastSensorDiagnostic = 0;
  Serial.printf("[SENSOR] pin=GPIO%u, protocol=%s\n", cfg.sensorPin,
                cfg.sensorType == 70 ? "Sonoff Si7021" : "AM2301/DHT21");
}

float effectiveTarget() { return cfg.target; }

void setRelay(bool on) {
  if (on == relayOn) return;
  // Flush the previous relay state up to the switching instant.
  heaterStats.tick(relayOn, roomTemp, sensorHealthy(),
                   cfg.dayStartHour, cfg.nightStartHour);
  if (relayOn && !on) heaterStats.completedCycle(millis() - relayStartedAt);
  digitalWrite(RELAY_GPIO, on ? RELAY_ON_LEVEL : RELAY_OFF_LEVEL);
  relayOn = on;
  lastRelayChange = millis();
  if (on) relayStartedAt = lastRelayChange;
}

void thermostat() {
  if (otaUploading || otaRebootAt) { setRelay(false); return; }
  unsigned long now = millis();
  bool healthy = sensorHealthy() && validNumber(roomTemp, -15, 50);
  // No latched lockout or maximum continuous-ON time.
  // A temporary overtemperature condition always de-energizes the relay.
  if (!cfg.enabled || (healthy && roomTemp >= HARD_ROOM_LIMIT)) {
    setRelay(false); // Safety shutdown ignores minimum ON duration
    return;
  }
  if (cfg.manualMode) {
    // User explicitly requested manual heating without a sensor.
    // The forced two-hour OFF timer is never optional in this mode.
    if (now >= BOOT_DELAY) setRelay(true);
    return;
  }
  if (!healthy) { setRelay(false); return; } // Automatic thermostat needs sensor
  if (relayOn) {
    if (roomTemp >= effectiveTarget() + cfg.heatOffDelta &&
        now - lastRelayChange >= (uint32_t)cfg.minOnMinutes * 60000UL)
      setRelay(false);
  } else {
    bool settled = (lastRelayChange == 0 && now >= BOOT_DELAY) ||
                   (lastRelayChange != 0 && now - lastRelayChange >= (uint32_t)cfg.minOffMinutes * 60000UL);
    if (settled && roomTemp <= effectiveTarget() - cfg.heatOnDelta)
      setRelay(true);
  }
}

void readSensor() {
  unsigned long now = millis();
  if (now - lastSensor < SENSOR_INTERVAL) return;
  lastSensor = now;
  if (!dht) return;
  const int result = dht->read();
  const float t = dht->getTemperature();
  const float h = dht->getHumidity();
  if (result == DHTLIB_OK && validNumber(t, -15, 50) && validNumber(h, 0, 100)) {
    roomTemp = t;
    roomHumidity = h;
    lastGoodSensor = now;
    if (sensorErrorCount) Serial.printf("[AM2301] recovered: %.1f C, %.1f %%RH\n", t, h);
    sensorErrorCount = 0;
  } else {
    ++sensorErrorCount;
    if (!lastSensorDiagnostic || now - lastSensorDiagnostic >= 20000UL) {
      lastSensorDiagnostic = now;
      Serial.printf("[SENSOR] error=%d, GPIO%u=%d, type=%u, failures=%lu. Check 3V3/GND/DATA.\n",
                    result, cfg.sensorPin, digitalRead(cfg.sensorPin),
                    cfg.sensorType, sensorErrorCount);
    }
  }
}

void toggleHeating() {
  if (otaUploading || otaRebootAt) return;
  cfg.enabled = !cfg.enabled;
  if (!cfg.enabled) {
    cfg.manualMode = false;
    offTimerActive = false;  // Manual OFF cancels pending automatic OFF.
    setRelay(false);
  } else {
    // Automatic mode never silently bypasses a missing temperature sensor.
    // Relay stays OFF until valid readings resume, then the thermostat works.
    cfg.manualMode = false;
  }
  saveSettings();
}

void enableManual() {
  if (otaUploading || otaRebootAt) return;
  cfg.enabled = true;
  cfg.manualMode = true;
  offTimerActive = true;
  offTimerStartedAt = millis();
  saveSettings();
}

void readButton() {
  if (otaUploading || otaRebootAt) return;
  // Short press toggles AUTO / OFF; missing sensor temporarily forces relay OFF.
  // Hold 8s for a safe OFF; the setup AP is always available.
  bool down = digitalRead(BUTTON_GPIO) == LOW;
  unsigned long now = millis();
  if (down != buttonWasDown && now - lastButtonEdge >= 50) {
    buttonWasDown = down;
    lastButtonEdge = now;
    if (down) { buttonDownAt = now; buttonLongDone = false; }
    else if (!buttonLongDone && now - buttonDownAt >= 50) toggleHeating();
  }
  if (buttonWasDown && !buttonLongDone && now - buttonDownAt >= 8000UL) {
    buttonLongDone = true;
    cfg.enabled = false;
    cfg.manualMode = false;
    offTimerActive = false;
    setRelay(false);
    saveSettings();
  }
}

unsigned long offTimerRemainingSeconds() {
  if (!offTimerActive) return 0;
  unsigned long elapsed = millis() - offTimerStartedAt;
  if (elapsed >= OFF_TIMER_TIME) return 0;
  return (OFF_TIMER_TIME - elapsed + 999UL) / 1000UL;
}

void offTimerTask() {
  if (!offTimerActive) return;
  if (millis() - offTimerStartedAt < OFF_TIMER_TIME) return;
  offTimerActive = false;
  cfg.enabled = false;
  cfg.manualMode = false;
  setRelay(false);  // Timer expiry overrides thermostat demand immediately.
  saveSettings();
}

bool checkPost() {
  touchWeb();
  if (httpServer.method() != HTTP_POST || !httpServer.hasArg("csrf") ||
      httpServer.arg("csrf") != csrf) {
    httpServer.send(403, "text/plain; charset=utf-8", "Invalid request token");
    return false;
  }
  return true;
}

String pageTop(const String &title) {
  String s = F("<!doctype html><html lang='bg'><head><meta charset='utf-8'>"
               "<meta name='viewport' content='width=device-width,initial-scale=1'>"
               "<meta name='theme-color' content='#10233b'>"
               "<meta name='apple-mobile-web-app-capable' content='yes'>"
               "<meta name='apple-mobile-web-app-status-bar-style' content='black-translucent'>");
  s += "<meta name='apple-mobile-web-app-title' content='" + safeHtml(deviceName()) + "'>";
  s += F("<link rel='apple-touch-icon' sizes='180x180' href='/apple-touch-icon.png'>"
               "<link rel='icon' href='/favicon.ico' sizes='any'>"
               "<link rel='manifest' href='/manifest.webmanifest'>"
               "<style>body{font:16px system-ui;background:#edf3f8;color:#102339;margin:0}"
               "main{max-width:720px;margin:auto;padding:18px}h1{font-size:1.5rem}"
               "section{background:white;padding:20px;border-radius:16px;margin:14px 0;"
               "box-shadow:0 3px 16px #0001}label{display:block;margin:12px 0 5px}"
               "input{font:inherit;padding:10px;box-sizing:border-box;width:100%;border:1px solid #ccd6df;"
               "border-radius:8px}button,.link{background:#1565c0;color:white;border:0;border-radius:9px;"
               "padding:12px 18px;font:inherit;cursor:pointer;text-decoration:none;display:inline-block}"
               "button.off{background:#bb2638}.muted{color:#667789}"
               "form{margin:10px 0}.pill{padding:5px 10px;background:#e8f4ee;border-radius:50px}"
               "nav a{margin-right:16px}small{color:#666}"
               ".temp-controls{display:flex;align-items:center;justify-content:center;gap:14px;margin:16px 0}"
               ".temp-controls button{font-size:30px;font-weight:bold;width:70px;height:64px;padding:0}"
               ".temp-reading{min-width:150px;text-align:center}"
               ".temp-reading strong{display:block;font-size:38px;font-variant-numeric:tabular-nums}"
               ".temp-controls button:disabled{opacity:.55;cursor:wait}"
               ".readings{display:grid;grid-template-columns:1fr 1fr;gap:12px;margin:18px 0}"
               ".readings>div{background:#eff5fc;border-radius:12px;padding:18px 10px;text-align:center}"
               ".readings strong{display:block;font-size:clamp(24px,6vw,36px);font-variant-numeric:tabular-nums}"
               ".readings small{display:block;margin-bottom:8px}"
               ".actions{display:flex;flex-wrap:wrap;gap:12px;margin-top:12px}"
               ".actions form{flex:1;min-width:180px;margin:0}.actions button{width:100%;height:100%}"
               ".actions button.timer{background:#455a64}"
               ".actions button:disabled{background:#aab5bd;cursor:not-allowed}"
               ".feedback{min-height:22px;text-align:center;color:#176a35}"
               "</style><title>");
  s += safeHtml(deviceName());
  s += F(" – ");
  s += safeHtml(title);
  s += F("</title></head><body><main><nav><a href='/'>Термостат</a>"
         "<a href='/settings'>Настройки</a><a href='/history'>Данни</a><a href='/setup'>Wi-Fi</a><a href='/help'>Помощ</a><a href='/update'>Обновяване</a><a href='/cloud'>Облак</a></nav>");
  return s;
}

String hiddenToken() { return "<input type='hidden' name='csrf' value='" + csrf + "'>"; }

String htmlForm(const String &action, const String &button, bool danger = false) {
  return "<form method='post' action='" + action + "'>" + hiddenToken() +
         "<button" + String(danger ? " class='off'" : "") + ">" + button +
         "</button></form>";
}

void homePage() {
  touchWeb();
  // Chunked page response avoids constructing ~12 KB of HTML in ESP8266 RAM.
  httpServer.setContentLength(CONTENT_LENGTH_UNKNOWN);
  httpServer.sendHeader("Cache-Control", "no-store");
  httpServer.send(200, "text/html; charset=utf-8", "");
  httpServer.sendContent(pageTop("Инфрачервен термостат"));
  httpServer.sendContent_P(PSTR(R"CSS(<style>
body{background:#0c1523;color:#f4f8ff;font-family:-apple-system,BlinkMacSystemFont,system-ui,sans-serif}
main{max-width:430px;margin:auto;padding:18px 16px 34px}
nav{display:flex;gap:18px;justify-content:flex-end;margin:0 0 24px}
nav a{color:#a9bfd8;text-decoration:none;font-size:15px}
h1{font-size:23px;margin:0 0 18px;letter-spacing:-.4px}
.thermo-card{background:linear-gradient(155deg,#1b2d43,#111f32);border:1px solid #31435c;
 border-radius:28px;box-shadow:0 16px 40px #0004;padding:30px 20px 24px;margin:0}
.room-label,.target-label{font-size:15px;color:#b9c8db;text-align:center}
.room-value{font-size:clamp(56px,17vw,78px);font-weight:650;line-height:1.22;
 text-align:center;font-variant-numeric:tabular-nums;letter-spacing:-3px;margin:10px 0 28px}
.divider{height:1px;background:#34445b;margin:0 0 24px}
.target-control{display:flex;align-items:center;justify-content:space-between;
 gap:8px;margin:16px 0 30px}
.target-number{text-align:center;font-size:clamp(34px,10vw,48px);font-weight:650;
 font-variant-numeric:tabular-nums;letter-spacing:-1px;white-space:nowrap}
.adjust{flex:0 0 66px;width:66px;height:66px;border:0;border-radius:19px;
 background:#31557f;color:white;font-size:39px;font-weight:400;padding:0;
 touch-action:manipulation;-webkit-tap-highlight-color:transparent}
.adjust:active{background:#4779b5;transform:scale(.97)}
.adjust:disabled{opacity:.4}
.relay-status{display:flex;align-items:center;justify-content:center;gap:11px;
 background:#0d1a2a;border-radius:14px;padding:14px 12px;margin:0 0 19px;
 color:#d7e2f0;font-size:15px}
.relay-led{height:15px;width:15px;min-width:15px;border-radius:50%;
 background:#566071;border:1px solid #8993a1;display:inline-block}
.relay-led.active{background:#f83a47;border-color:#ff7580;
 box-shadow:0 0 0 4px #f83a4723,0 0 19px #ff394b9c}
.power-form{margin:0}
.power-button{display:block;width:100%;min-height:68px;border:0;border-radius:19px;
 background:#1971d4;color:#fff;font-size:21px;font-weight:650;cursor:pointer;
 touch-action:manipulation;-webkit-tap-highlight-color:transparent}
.power-button.on{background:#b72f3d}
.power-button:active{filter:brightness(1.13)}
.feedback{color:#b9c8db;text-align:center;min-height:18px;font-size:13px;margin:12px 0 0}
.notice{font-size:13px;color:#f6c58a;text-align:center;min-height:18px;margin:5px 0 0}
.stats-block{margin-top:18px;background:#17283d;color:#f4f8ff;border:1px solid #304862;
 border-radius:23px;padding:18px 16px}
.stats-block h2{font-size:19px;margin:4px 0 10px}.stats-block h3{font-size:15px;margin:16px 0 8px;color:#cad7e8}
.stat-grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:10px}
.stat-tile{border-radius:13px;padding:12px 10px;background:#213851;min-width:0}
.stat-tile small{display:block;color:#bbccdf;font-size:12px;margin-bottom:7px}
.stat-tile strong{display:block;font-size:clamp(18px,5.5vw,24px);font-weight:650}
.stat-sub{font-size:13px;color:#b9c8dc;line-height:1.8;margin:10px 0 0}
.stats-block a{color:#a9d4ff}.stats-help{font-size:12px;color:#a7bed6;line-height:1.5;margin:13px 0 2px}
@media(max-width:360px){.thermo-card{padding:24px 14px}.adjust{width:60px;height:60px;flex-basis:60px}}
</style>)CSS"));
  httpServer.sendContent_P(PSTR("<h1>"));
  httpServer.sendContent(deviceName());
  httpServer.sendContent_P(PSTR("</h1><section class='thermo-card'>"
         "<div class='room-label'>Температура в стаята</div>"
         "<div id='currentValue' class='room-value' aria-live='polite'>—</div>"
         "<div class='divider'></div>"
         "<div class='target-label'>Зададена температура</div>"
         "<div class='target-control'>"
         "<button type='button' class='adjust' aria-label='Намали с 0,5 градуса' "
         "onclick='changeTarget(-0.5)'>−</button>"
         "<div id='targetValue' class='target-number'>"));
  httpServer.sendContent(String(cfg.target, 1));
  httpServer.sendContent_P(PSTR(" °C</div>"
         "<button type='button' class='adjust' aria-label='Увеличи с 0,5 градуса' "
         "onclick='changeTarget(0.5)'>+</button></div>"
         "<div class='relay-status'><span id='relayLed' class='relay-led' "
         "aria-hidden='true'></span><span id='relayText'>Реле: проверка…</span></div>"
         "<form class='power-form' method='post' action='/toggle'>"));
  httpServer.sendContent(hiddenToken());
  httpServer.sendContent_P(PSTR("<button id='toggleButton' class='power-button"));
  if (cfg.enabled) httpServer.sendContent_P(PSTR(" on"));
  httpServer.sendContent_P(PSTR("'>"));
  httpServer.sendContent(cfg.enabled ? String(F("Изключи")) : String(F("Включи")));
  httpServer.sendContent_P(PSTR(R"STATS(</button></form><p id='feedback' class='feedback' role='status'></p>
<p id='notice' class='notice' aria-live='polite'></p></section>
<section class='stats-block'><h2>Разход и време на работа</h2>
<div id='statsClock' class='stats-help'></div>
<h3>Днес</h3><div class='stat-grid'>
<div class='stat-tile'><small>Общо енергия</small><strong id='todayEnergy'>—</strong></div>
<div class='stat-tile'><small>Обща цена</small><strong id='todayCost'>—</strong></div>
<div class='stat-tile'><small>Време ВКЛ.</small><strong id='todayRuntime'>—</strong></div>
<div class='stat-tile'><small>Среден отоплителен цикъл</small><strong id='todayAvg'>—</strong></div>
</div><p id='todaySplit' class='stat-sub'>Зареждане…</p>
<h3>Този месец</h3><div class='stat-grid'>
<div class='stat-tile'><small>Общо енергия</small><strong id='monthEnergy'>—</strong></div>
<div class='stat-tile'><small>Обща цена</small><strong id='monthCost'>—</strong></div>
<div class='stat-tile'><small>Време ВКЛ.</small><strong id='monthRuntime'>—</strong></div>
<div class='stat-tile'><small>Среден цикъл</small><strong id='monthAvg'>—</strong></div>
</div><p id='monthSplit' class='stat-sub'>Зареждане…</p>
<p id='monthAverageDay' class='stat-sub'></p>
<p class='stats-help'>Оценка по избраната мощност при включено реле, не измерване с електромер.
Средният цикъл е средната продължителност на завършените включвания.
Цените и часовете на тарифите се задават от „Настройки“.</p>
<a href='/history'>Графики: 24 часа / месец / година</a>
</section><script>const token=')STATS"));
  httpServer.sendContent(csrf);
  httpServer.sendContent_P(PSTR(R"JS(';
const powerForm=document.querySelector('.power-form');
powerForm.addEventListener('submit',async ev=>{
  ev.preventDefault();
  const button=document.getElementById('toggleButton');
  if(button.disabled)return;
  button.disabled=true;
  try{
    const r=await fetch('/api/toggle',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams({csrf:token}),cache:'no-store'});
    if(!r.ok)throw Error('HTTP '+r.status);
    await refresh();
  }catch(e){document.getElementById('feedback').textContent='Неуспешно превключване';}
  finally{button.disabled=false;}
});
function showTarget(value) {
  document.getElementById('targetValue').textContent = value.toFixed(1) + ' °C';
}
let statusInFlight = false;
async function refresh() {
  if (statusInFlight || document.hidden) return;
  statusInFlight = true;
  try {
    const response = await fetch('/api/status', {cache:'no-store'});
    if (!response.ok) throw new Error('HTTP');
    const d = await response.json();
    document.getElementById('currentValue').textContent =
      d.sensor_ok ? d.temperature.toFixed(1) + ' °C' : '—';
    if (nextTarget === null && !targetSending) showTarget(d.target);
    document.getElementById('relayLed').classList.toggle('active', !!d.relay);
    document.getElementById('relayText').textContent = d.relay ? 'Реле: ВКЛЮЧЕНО' : 'Реле: ИЗКЛЮЧЕНО';
    const toggle = document.getElementById('toggleButton');
    toggle.textContent = d.enabled ? 'Изключи' : 'Включи';
    toggle.className = d.enabled ? 'power-button on' : 'power-button';
    const note = d.safety_reason === 'temperature_limit' ?
      'Температурата е над допустимата граница. Релето е изключено.' :
      (d.safety_reason === 'sensor_unavailable' ?
      'Датчикът не отчита. Релето е изключено до възстановяване на измерването.' :
      (d.manual ? 'Ръчен режим: автоматично изключване след 2 часа' :
      (d.off_timer ? 'Активен таймер за изключване' : '')));
    document.getElementById('notice').textContent = note;
  } catch (error) {
    document.getElementById('notice').textContent = 'Няма връзка с устройството';
  } finally {statusInFlight=false;}
}
let nextTarget = null, targetTimer = null, targetSending = false;
function changeTarget(delta) {
  const current = nextTarget === null ? Number.parseFloat(document.getElementById('targetValue').textContent) : nextTarget;
  const next=Math.round((current+delta)*2)/2;
  if(!Number.isFinite(next)||next<12||next>28)return;
  nextTarget=next;
  showTarget(next);
  clearTimeout(targetTimer);
  targetTimer=setTimeout(sendTarget,300);
}
async function sendTarget(){
  if(targetSending||nextTarget===null)return;
  targetSending=true;
  const value=nextTarget;
  const feedback=document.getElementById('feedback');
  try{
    const r=await fetch('/api/target',{method:'POST',
      headers:{'Content-Type':'application/x-www-form-urlencoded'},
      body:new URLSearchParams({csrf:token,target:value.toFixed(1)}),cache:'no-store'});
    if(!r.ok)throw Error('HTTP');
    const result=await r.json();
    if(nextTarget===value){nextTarget=null;showTarget(result.target);feedback.textContent='';}
  }catch(e){feedback.textContent='Грешка при настройване';}
  finally{
    targetSending=false;
    if(nextTarget!==null&&nextTarget!==value)setTimeout(sendTarget,150);
  }
}
let statsInFlight = false;
async function refreshStats(){
  if(statsInFlight || document.hidden)return;
  statsInFlight=true;
  try{
    const r=await fetch('/api/stats',{cache:'no-store'});
    if(!r.ok)throw Error('status');
    const s=await r.json();
    const clock=document.getElementById('statsClock');
    if(!s.clock_ok){clock.textContent='Изчакване на интернет и синхронизация на часовника (NTP).';return;}
    clock.textContent=s.tariff_ok?'':'Задай дневна и нощна цена в „Настройки“, за да виждаш разходите.';
    const fmt=(n,dec=2)=>Number(n).toFixed(dec);
    for(const period of ['today','month']){
      const b=s[period];
      const label=period==='today'?'today':'month';
      document.getElementById(label+'Energy').textContent=fmt(b.energy_total,3)+' kWh';
      document.getElementById(label+'Cost').textContent=s.tariff_ok?fmt(b.cost_total)+' €':'—';
      document.getElementById(label+'Runtime').textContent=fmt(b.runtime_h)+' ч';
      document.getElementById(label+'Avg').textContent=
        b.completed_cycles?fmt(b.avg_cycle_h)+' ч':'—';
      document.getElementById(label+'Split').textContent=
        'Дневна: '+fmt(b.energy_day,3)+' kWh'+(s.tariff_ok?' / '+fmt(b.cost_day)+' €':'')+
        ' · Нощна: '+fmt(b.energy_night,3)+' kWh'+(s.tariff_ok?' / '+fmt(b.cost_night)+' €':'');
    }
    document.getElementById('monthAverageDay').textContent=
      'Средно време на работа за ден през месеца: '+fmt(s.month.avg_per_day_h)+' ч';
  }catch(e){document.getElementById('statsClock').textContent='Няма връзка със статистиката.';}
  finally{statsInFlight=false;}
}
refresh(); refreshStats();
setInterval(()=>{if(!document.hidden)refresh();},3500);
setInterval(()=>{if(!document.hidden)refreshStats();},30000);
</script></main></body></html>)JS"));
  httpServer.sendContent("");
}

void historyPage() {
  touchWeb();
  httpServer.setContentLength(CONTENT_LENGTH_UNKNOWN);
  httpServer.sendHeader("Cache-Control", "private, max-age=120");
  httpServer.send(200, "text/html; charset=utf-8", "");
  httpServer.sendContent(pageTop("История на отоплението"));
  httpServer.sendContent_P(PSTR(R"HIST(<style>
body{background:#0c1523;color:#f4f8ff;font-family:-apple-system,BlinkMacSystemFont,system-ui,sans-serif}
main{max-width:720px;margin:auto;padding:18px 14px 40px}nav a{color:#a9d4ff}
section{background:#17283d;border:1px solid #344b67;color:#f4f8ff;border-radius:20px;padding:16px 12px}
h1{font-size:24px}.ranges{display:flex;gap:8px;margin:12px 0 24px}
.ranges button{flex:1;padding:12px 4px;background:#294561;font-size:15px;border-radius:12px}
.ranges button.selected{background:#2475c9}
.graph{background:#0d1c2d;border-radius:13px;padding:12px 4px;margin:10px 0 22px}
.graph svg{width:100%;max-width:680px;height:auto;display:block;margin:auto}
.legend{color:#bed0e2;text-align:center;font-size:12px}.tip{font-size:13px;color:#b9cadb}
</style><h1>История и графики</h1><section>
<div class='ranges'>
<button id='btn24h' type='button' onclick="loadHistory('24h')">24 часа</button>
<button id='btnmonth' type='button' onclick="loadHistory('month')">Месец</button>
<button id='btnyear' type='button' onclick="loadHistory('year')">Година</button>
</div>
<p id='graphStatus' class='tip'>Зареждане…</p>
<h3>Изразходвана енергия (kWh)</h3>
<div class='graph'><svg id='energyGraph' viewBox='0 0 360 220' role='img' aria-label='Енергия по тарифи'></svg></div>
<p class='legend'>🟠 Дневна тарифа · 🟣 Нощна тарифа</p>
<h3>Средна температура (°C)</h3>
<div class='graph'><svg id='temperatureGraph' viewBox='0 0 360 220' role='img' aria-label='Средна температура'></svg></div>
<p class='tip'>Часовете и датите са по време за България. Статистиката започва от
инсталирането на този фърмуер. За периоди без измерване няма линия за температура.
Годишната графика показва месеците на текущата година.</p>
</section><script>
const NS='http://www.w3.org/2000/svg';
function svgEl(tag,attr,text){
  const el=document.createElementNS(NS,tag);
  Object.keys(attr||{}).forEach(k=>el.setAttribute(k,attr[k]));
  if(text!==undefined)el.textContent=text;
  return el;
}
function drawAxes(svg,maxY,labels){
  svg.replaceChildren();
  const L=42,R=349,T=14,B=181,H=B-T;
  for(let i=0;i<=4;i++){
    const y=B-i*H/4;
    svg.appendChild(svgEl('line',{x1:L,y1:y,x2:R,y2:y,stroke:'#36516c','stroke-width':1}));
    svg.appendChild(svgEl('text',{x:36,y:y+4,'text-anchor':'end',fill:'#aac3dd','font-size':10},
      (maxY*i/4).toFixed(maxY<5?1:0)));
  }
  const n=labels.length, step=(R-L)/Math.max(1,n);
  const every=n>25?5:n>18?4:n>10?2:1;
  for(let i=0;i<n;i++)if(i%every===0 || i===n-1){
    svg.appendChild(svgEl('text',{x:L+(i+.5)*step,y:201,
      'text-anchor':'middle',fill:'#aac3dd','font-size':10},labels[i]));
  }
  return {L,R,T,B,H,n,step};
}
function drawEnergy(d){
  const svg=document.getElementById('energyGraph');
  const totals=d.day_kwh.map((v,i)=>v+d.night_kwh[i]);
  const max=Math.max(.05,...totals)*1.15;
  const a=drawAxes(svg,max,d.labels);
  for(let i=0;i<a.n;i++){
    const x=a.L+i*a.step+a.step*.14,w=a.step*.72;
    const dayH=d.day_kwh[i]/max*a.H,nightH=d.night_kwh[i]/max*a.H;
    if(dayH>.1)svg.appendChild(svgEl('rect',{x,y:a.B-dayH,width:w,height:dayH,
      fill:'#f2a545',rx:Math.min(2,w/4)}));
    if(nightH>.1)svg.appendChild(svgEl('rect',{x,y:a.B-dayH-nightH,width:w,
      height:nightH,fill:'#8e8cf7',rx:Math.min(2,w/4)}));
  }
}
function drawTemp(d){
  const svg=document.getElementById('temperatureGraph');
  const values=d.temperature.filter(v=>typeof v==='number');
  const min=values.length?Math.floor(Math.min(...values)-1):10;
  const max=values.length?Math.ceil(Math.max(...values)+1):30;
  const range=Math.max(1,max-min);
  const a=drawAxes(svg,range,d.labels);
  // Replace 0-based axis labels with real temperatures.
  svg.querySelectorAll('text').forEach(el=>{
    if(el.getAttribute('x')==='36')el.textContent=(min+Number(el.textContent)).toFixed(0);
  });
  let path='';
  d.temperature.forEach((v,i)=>{
    if(typeof v!=='number'){path+=' ';return;}
    const x=a.L+(i+.5)*a.step;
    const y=a.B-(v-min)/range*a.H;
    const prev=i>0?d.temperature[i-1]:null;
    path+=(typeof prev==='number'?' L ':' M ')+x.toFixed(1)+' '+y.toFixed(1);
  });
  if(path.trim())svg.appendChild(svgEl('path',{d:path,stroke:'#70d9ed',fill:'none',
    'stroke-width':2.5,'stroke-linejoin':'round','stroke-linecap':'round'}));
  else svg.appendChild(svgEl('text',{x:185,y:105,'text-anchor':'middle',fill:'#aac3dd'},
    'Няма измервания'));
}
// V5: no global loading lock. Only the newest user selection may draw.
// If another period is requested, abort the obsolete HTTP request immediately.
let historyController=null;
let historySequence=0;
const historyMemory=new Map();
function nextFrame(){return new Promise(resolve=>requestAnimationFrame(resolve));}
async function loadHistory(range){
  const sequence=++historySequence;
  if(historyController)historyController.abort();
  historyController=new AbortController();
  const signal=historyController.signal;
  ['24h','month','year'].forEach(x=>document.getElementById('btn'+x)
    .classList.toggle('selected',x===range));
  const status=document.getElementById('graphStatus');
  const started=performance.now();
  status.textContent='Зареждане на '+(range==='24h'?'24 часа':range==='month'?'месец':'година')+'…';
  try{
    let data;
    const cached=historyMemory.get(range);
    const usingCache=!!(cached && Date.now()-cached.time<20000);
    if(usingCache){data=cached.data;}
    else{
      const response=await fetch('/api/history?range='+encodeURIComponent(range),{
        cache:'no-store',signal,headers:{'Accept':'application/json'}
      });
      if(!response.ok)throw Error('HTTP '+response.status);
      data=await response.json();
      historyMemory.set(range,{data,time:Date.now()});
    }
    if(sequence!==historySequence)return;
    const networkMs=performance.now()-started;
    await nextFrame();
    if(sequence!==historySequence)return;
    const paintStarted=performance.now();
    drawEnergy(data);
    drawTemp(data);
    const drawMs=performance.now()-paintStarted;
    status.textContent=d.power_w+' W × време на реле · Данни '+Math.round(networkMs)+' ms'+
      ' · Графики '+Math.round(drawMs)+' ms'+(usingCache?' · кеш':'');
  }catch(error){
    if(error.name==='AbortError'||sequence!==historySequence)return;
    status.textContent='Неуспешно зареждане: '+error.message;
  }finally{
    if(sequence===historySequence)historyController=null;
  }
}
loadHistory('24h');
</script></main></body></html>)HIST"));
  httpServer.sendContent("");
}

void statusApi() {
  touchWeb();
  JsonDocument d;
  d["enabled"] = cfg.enabled;
  d["manual"] = cfg.manualMode;
  d["relay"] = relayOn;
  d["locked"] = false; // Compatibility field for existing Home Assistant integration
  d["device_name"] = deviceName();
  d["power_w"] = cfg.powerW;
  const bool safeReading = sensorHealthy() && validNumber(roomTemp, -15, 50);
  d["safety_reason"] = (safeReading && roomTemp >= HARD_ROOM_LIMIT) ? "temperature_limit" :
                       (!safeReading && !cfg.manualMode ? "sensor_unavailable" : "none");
  d["sensor_ok"] = sensorHealthy();
  d["sensor_errors"] = sensorErrorCount;
  d["sensor_pin_level"] = digitalRead(cfg.sensorPin);
  d["sensor_pin"] = cfg.sensorPin;
  d["sensor_type"] = cfg.sensorType;
  d["temperature"] = sensorHealthy() ? roomTemp : 0;
  d["humidity"] = sensorHealthy() ? roomHumidity : 0;
  d["target"] = cfg.target;
  d["effective"] = effectiveTarget();
  d["heat_on_delta"] = cfg.heatOnDelta;
  d["heat_off_delta"] = cfg.heatOffDelta;
  d["heat_on_threshold"] = effectiveTarget() - cfg.heatOnDelta;
  d["heat_off_threshold"] = effectiveTarget() + cfg.heatOffDelta;
  d["min_on_minutes"] = cfg.minOnMinutes;
  d["min_off_minutes"] = cfg.minOffMinutes;
  d["off_timer"] = offTimerActive;
  d["off_timer_seconds"] = offTimerRemainingSeconds();
  d["free_heap"] = ESP.getFreeHeap();
  d["max_free_block"] = ESP.getMaxFreeBlockSize();
  d["heap_fragmentation"] = ESP.getHeapFragmentation();
  d["history_build_ms"] = lastHistoryBuildMs;
  d["history_bytes"] = lastHistoryResponseBytes;
  d["ip"] = WiFi.localIP().toString();
  d["clock_ok"] = heaterStats.clockReady();
  d["ntp_synced"] = ntpSuccessCount > 0;
  d["ntp_syncs_since_boot"] = ntpSuccessCount;
  d["ntp_refresh_hours"] = NTP_REFRESH_INTERVAL / 3600000UL;
  d["last_ntp_sync_seconds"] = ntpSuccessCount > 0
    ? (long)((millis() - lastNtpSuccessAt) / 1000UL) : -1;
  if (heaterStats.clockReady()) {
    time_t now = time(nullptr);
    tm local; localtime_r(&now, &local);
    char localTime[25];
    strftime(localTime, sizeof(localTime), "%d.%m.%Y %H:%M:%S", &local);
    d["local_time"] = localTime;
  } else d["local_time"] = "не е синхронизиран";
  String body; serializeJson(d, body);
  httpServer.sendHeader("Cache-Control", "no-store");
  httpServer.send(200, "application/json; charset=utf-8", body);
}

void addSummary(JsonObject object, const HeaterBucket &b) {
  const float kd = HeaterStats::kwh(b.dayMs, cfg.powerW);
  const float kn = HeaterStats::kwh(b.nightMs, cfg.powerW);
  object["energy_day"] = kd;
  object["energy_night"] = kn;
  object["energy_total"] = kd + kn;
  object["cost_day"] = kd * cfg.dayRate;
  object["cost_night"] = kn * cfg.nightRate;
  object["cost_total"] = kd * cfg.dayRate + kn * cfg.nightRate;
  object["runtime_h"] = HeaterStats::hours(b.dayMs) + HeaterStats::hours(b.nightMs);
  object["avg_cycle_h"] = b.completedCycles
    ? HeaterStats::hours(b.completedCycleMs) / b.completedCycles : 0;
  object["completed_cycles"] = b.completedCycles;
  object["avg_temp"] = b.tempSamples ? b.tempSum / b.tempSamples : 0;
  object["has_temp"] = b.tempSamples > 0;
}

void statsApi() {
  touchWeb();
  JsonDocument doc;
  doc["clock_ok"] = heaterStats.clockReady();
  doc["tariff_ok"] = cfg.dayRate > 0 && cfg.nightRate > 0;
  doc["currency"] = "EUR";
  doc["power_w"] = cfg.powerW;
  doc["device_name"] = deviceName();
  doc["day_rate"] = cfg.dayRate;
  doc["night_rate"] = cfg.nightRate;
  if (heaterStats.clockReady()) {
    time_t now = time(nullptr);
    tm local; localtime_r(&now, &local);
    addSummary(doc["today"].to<JsonObject>(), heaterStats.dayAt(now));
    JsonObject month = doc["month"].to<JsonObject>();
    addSummary(month, heaterStats.monthAt(now));
    // Average daily ON time so far in the current calendar month.
    month["avg_per_day_h"] = month["runtime_h"].as<float>() / local.tm_mday;
  }
  String out; serializeJson(doc, out);
  httpServer.sendHeader("Cache-Control", "no-store");
  httpServer.send(200, "application/json; charset=utf-8", out);
}

// Performance v4: one bounded buffer, no dynamic ArduinoJson arrays or large
// temporary String copies while returning the history chart data.
static char historyJson[4096];
static size_t histLength = 0;
static bool histOverflow = false;
static void histAppend(const char *fmt, ...) {
  if (histOverflow) return;
  va_list args;
  va_start(args, fmt);
  const int n = vsnprintf(historyJson + histLength, sizeof(historyJson) - histLength, fmt, args);
  va_end(args);
  if (n < 0 || (size_t)n >= sizeof(historyJson) - histLength) {
    histOverflow = true;
    return;
  }
  histLength += (size_t)n;
}

// Prepare each data point once, rather than repeatedly looking up buckets
// for each JSON series. The response is small enough to fit in a fixed buffer.
void historyApi() {
  touchWeb();
  if (!heaterStats.clockReady()) {
    httpServer.send(503, "application/json", "{\"error\":\"clock_not_synced\"}");
    return;
  }
  const String period = httpServer.arg("range");
  const bool hourly = period == "24h";
  const bool daily = period == "month";
  const bool monthly = period == "year";
  if (!hourly && !daily && !monthly) {
    httpServer.send(400, "text/plain", "Invalid range"); return;
  }
  const uint32_t started = millis();
  time_t now = time(nullptr);
  tm current;
  localtime_r(&now, &current);
  const int count = hourly ? 24 : (daily ? current.tm_mday : 12);
  HeaterBucket buckets[31];
  char labels[31][8];
  for (int i = 0; i < count; ++i) {
    if (hourly) {
      // Use the same chronological hour slots as the legacy API.
      const time_t at = now - (now % 3600) - (time_t)(23 - i) * 3600;
      tm t; localtime_r(&at, &t);
      snprintf(labels[i], sizeof(labels[i]), "%02d:00", t.tm_hour);
      buckets[i] = heaterStats.hourAt(at);
    } else if (daily) {
      tm date = current;
      date.tm_mday = i + 1; date.tm_hour = 12; date.tm_min = 0;
      date.tm_sec = 0; date.tm_isdst = -1;
      const time_t at = mktime(&date);
      snprintf(labels[i], sizeof(labels[i]), "%d", i + 1);
      buckets[i] = heaterStats.dayAt(at);
    } else {
      tm date = current;
      date.tm_mon = i; date.tm_mday = 1; date.tm_hour = 12;
      date.tm_min = 0; date.tm_sec = 0; date.tm_isdst = -1;
      const time_t at = mktime(&date);
      snprintf(labels[i], sizeof(labels[i]), "%d", i + 1);
      buckets[i] = heaterStats.monthAt(at);
    }
  }
  histLength = 0; histOverflow = false;
  histAppend("{\"period\":\"%s\",\"power_w\":%u,\"labels\":[", period.c_str(), (unsigned int)cfg.powerW);
  for (int i = 0; i < count; ++i)
    histAppend("%s\"%s\"", i ? "," : "", labels[i]);
  histAppend("],\"day_kwh\":[");
  for (int i = 0; i < count; ++i)
    histAppend("%s%.5f", i ? "," : "", HeaterStats::kwh(buckets[i].dayMs, cfg.powerW));
  histAppend("],\"night_kwh\":[");
  for (int i = 0; i < count; ++i)
    histAppend("%s%.5f", i ? "," : "", HeaterStats::kwh(buckets[i].nightMs, cfg.powerW));
  histAppend("],\"temperature\":[");
  for (int i = 0; i < count; ++i) {
    if (buckets[i].tempSamples)
      histAppend("%s%.2f", i ? "," : "", buckets[i].tempSum / buckets[i].tempSamples);
    else
      histAppend("%snull", i ? "," : "");
  }
  histAppend("]}");
  if (histOverflow) {
    httpServer.send(500, "application/json", "{\"error\":\"history_buffer_overflow\"}");
    return;
  }
  lastHistoryBuildMs = millis() - started;
  lastHistoryResponseBytes = histLength;
  // Avoid a cached second copy of this response in RAM. Client caches for 20 s.
  httpServer.sendHeader("Cache-Control", "no-store");
  httpServer.send(200, "application/json; charset=utf-8", historyJson);
}

String field(const String &name, const String &label, const String &value,
             const String &type = "text") {
  return "<label>" + safeHtml(label) + "</label><input type='" + type + "' name='" +
         name + "' value='" + safeHtml(value) + "'>";
}

void settingsPage() {
  touchWeb();
  String s = pageTop("Настройки");
  s.reserve(10500);
  s += F("<h1>Настройки</h1><p><a href='/api/perf'>Диагностика на паметта (/api/perf)</a></p>"
         "<section><form method='post' action='/settings'>");
  s += hiddenToken();
  s += F("<h3>Температурен датчик</h3><label for='sensorPin'>GPIO за DATA</label>"
         "<select id='sensorPin' name='sensorPin' style='width:100%;padding:10px'>");
  for (uint8_t pin : {uint8_t(4), uint8_t(13), uint8_t(14)}) {
    s += "<option value='" + String(pin) + "'";
    if (cfg.sensorPin == pin) s += " selected";
    s += ">GPIO" + String(pin) + "</option>";
  }
  s += F("</select><label for='sensorType'>Протокол на датчика</label>"
         "<select id='sensorType' name='sensorType' style='width:100%;padding:10px'>"
         "<option value='70'");
  if (cfg.sensorType == 70) s += F(" selected");
  s += F(">Sonoff Si7021 (препоръчано за версия 2019)</option><option value='22'");
  if (cfg.sensorType == 22) s += F(" selected");
  s += F(">AM2301 / DHT21 / DHT22</option></select>"
         "<p class='muted'>Променете физическото DATA свързване само при изключено захранване. "
         "GPIO16 не е подходящ за този датчик. Запазването изключва отоплението, ако смените датчика.</p>");
  s += F("<h3>Режим на отопление — ");
  s += safeHtml(deviceName());
  s += F("</h3>"
         "<p class='muted'>Асиметричен хистерезис. Включва при зададена температура минус праг; "
         "изключва при зададена температура плюс праг. "
         "Отрицателен праг за изключване изключва предварително.</p>");
  s += field("heatOnDelta", "Праг на включване под зададената (°C, 0.1–3.0)",
             String(cfg.heatOnDelta, 1), "number' min='0.1' max='3' step='0.1");
  s += field("heatOffDelta", "Праг на изключване спрямо зададената (°C, -1.0 до +2.0)",
             String(cfg.heatOffDelta, 1), "number' min='-1' max='2' step='0.1");
  s += field("minOnMinutes", "Минимално време на включване (минути, 1–30)",
             String(cfg.minOnMinutes), "number' min='1' max='30' step='1");
  s += field("minOffMinutes", "Минимално време на изключване (минути, 1–30)",
             String(cfg.minOffMinutes), "number' min='1' max='30' step='1");
  s += F("<p class='muted'>При зададени 22 °C и стойности 0.6 / 0.0 релето се включва "
         "при ≤21.4 °C и изключва при ≥22.0 °C, ако са изпълнени времената. "
         "Защитите изключват незабавно. След промяна на праговете отоплението "
         "се изключва до ново включване от контролния панел.</p>");
  s += F("<h3>Електроенергия и тарифи</h3>"
         "<label for='powerW'><b>Мощност на отоплителя</b></label>"
         "<select id='powerW' name='powerW' style='width:100%;padding:12px;font-size:17px'>");
  for (uint16_t watts : {uint16_t(250),uint16_t(400),uint16_t(600),
                         uint16_t(750),uint16_t(1250),uint16_t(2000)}) {
    s += "<option value='" + String(watts) + "'";
    if (cfg.powerW == watts) s += " selected";
    s += ">Vega-S " + String(watts) + " W</option>";
  }
  s += F("</select><p class='muted'>Избирай действителната номинална мощност от табелката на отоплителя. "
         "Този избор променя само името и прогнозните kWh/разход, а не реалната електрическа мощност. "
         "Вече натрупаните часове ще бъдат оценявани по новоизбраната мощност и в старите графики.</p>"
         "<p>Тарифите се въвеждат в евро за kWh. Въведи своите актуални цени с включен ДДС.</p>");
  s += field("dayRate", "Дневна цена (€/kWh)", String(cfg.dayRate, 4),
             "number' min='0' max='5' step='0.0001");
  s += field("nightRate", "Нощна цена (€/kWh)", String(cfg.nightRate, 4),
             "number' min='0' max='5' step='0.0001");
  s += field("dayStartHour", "Начален час на дневната тарифа (0–23)",
             String(cfg.dayStartHour), "number' min='0' max='23' step='1");
  s += field("nightStartHour", "Начален час на нощната тарифа (0–23)",
             String(cfg.nightStartHour), "number' min='0' max='23' step='1");
  s += F("<p class='muted'>Настрой часовете според твоя доставчик и сезон. "
         "Промяна на цените преизчислява и показаната стойност на натрупаната енергия "
         "за минали периоди; това не е фактура.</p>");
  s += F("<p class='muted'>Уеб панелът е без потребителско име и парола. "
         "Използвай само доверена локална мрежа.</p>");
  s += F("<button>Запази настройките</button></form>");
  s += F("</section><section><h3>Изчистване на статистиката</h3>"
         "<p class='muted'>Изтрива натрупаните часове, енергия и температурна история. "
         "Настройките на термостата остават без промяна.</p>"
         "<button type='button' id='clearStats' class='off'>Изчисти статистиката</button>"
         "<p id='clearResult' class='muted'></p></section><script>");
  s += F("const resetToken='"); s += csrf;
  s += F(R"RESET(';
  document.getElementById('clearStats').addEventListener('click',async()=>{
    if(!confirm('Да се изтрият ли всички събрани статистики и графики?'))return;
    const b=document.getElementById('clearStats');b.disabled=true;
    try{
      const r=await fetch('/api/stats/reset',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams({csrf:resetToken})});
      if(!r.ok)throw Error('HTTP');
      document.getElementById('clearResult').textContent='Статистиката е изчистена.';
    }catch(e){document.getElementById('clearResult').textContent='Грешка при изчистване.';}
    finally{b.disabled=false;}
  });</script>)RESET");
  s += F("<section><h3>Безжична мрежа</h3>"
         "<p>Помощник за свързване, избор на домашната мрежа и постоянен IP адрес.</p>"
         "<p><a class='link' href='/setup'>Отвори Wi-Fi помощника</a></p>"
         "<p class='muted'>Резервен AP: Heater-xxxxxx, парола 12345678; "
         "активен само при липса на домашен Wi-Fi.</p></section>");
  s += F(R"NTP(</section><section><h3>Часовник и синхронизация</h3>
<p id='ntpStatus'>Проверка на NTP…</p>
<p class='muted'>Часова зона: България (Europe/Sofia), автоматично лятно/зимно време.
При първо свързване към Wi-Fi се заявява синхронизация.
След това NTP се опреснява през 6 часа; при неуспех се прави нов опит през 60 секунди.
За синхронизацията е необходим достъп до NTP сървър през домашната мрежа.</p>
</section><script>
async function refreshNtpStatus(){
  try{
    const response=await fetch('/api/status',{cache:'no-store'});
    if(!response.ok)throw Error('status');
    const s=await response.json();
    const synced=s.ntp_synced;
    const age=synced?Math.floor(s.last_ntp_sync_seconds/60):null;
    document.getElementById('ntpStatus').textContent=
      'Местно време: '+s.local_time+' · NTP: '+
      (synced?'успешен, преди '+age+' мин':'изчакване на сървър');
  }catch(e){document.getElementById('ntpStatus').textContent='Няма връзка с устройството.';}
}
refreshNtpStatus();setInterval(refreshNtpStatus,10000);
</script></main></body></html>)NTP");
  httpServer.send(200, "text/html; charset=utf-8", s);
}

void redirectHome() {
  touchWeb();
  httpServer.sendHeader("Location", "/", true);
  httpServer.send(303, "text/plain", "");
}

bool applyRequestedTarget() {
  String value = httpServer.arg("target");
  value.trim();
  char *end = nullptr;
  float target = strtof(value.c_str(), &end);
  if (value.length() == 0 || end == value.c_str() || *end != 0 ||
      !validNumber(target, 12, 28)) {
    httpServer.send(400, "text/plain; charset=utf-8", "Temperature must be 12..28");
    return false;
  }
  cfg.target = roundf(target * 2.0f) / 2.0f;
  deferSettingsSave();
  thermostat();
  return true;
}

// Simple, lightweight on-device help page. Thermostat operation remains unchanged.
void helpPage() {
  touchWeb();
  String s = pageTop("Помощ – Отопление");
  s += F(R"HELP(
<style>
.guide{font-size:16px;line-height:1.65}
.guide h2{font-size:18px;color:#123c62;margin:0 0 10px}
.guide p{margin:9px 0}
.guide .step{background:#edf5fe;padding:12px;border-radius:11px;margin:10px 0}
.guide .hint{background:#fff8e8;padding:12px;border-radius:10px}
.guide .buttons a{margin-right:7px;margin-bottom:7px}
</style>
<h1>Помощ за термостата</h1>
<section class='guide'>
<h2>Как се използва</h2>
<div class='step'><b>1. Включване:</b> натисни бутона <b>„Включи“</b> на началния екран. За спиране натисни <b>„Изключи“</b>.</div>
<div class='step'><b>2. Температура:</b> „Температура в стаята“ е измерената стойност. „Зададена температура“ е желаната стойност. Използвай <b>+ / −</b> за промяна с 0,5 °C.</div>
<div class='step'><b>3. Червена точка:</b> показва, че релето реално е включено. Сива точка означава изключено реле. Включен термостат не означава постоянно работещо реле.</div>
<div class='step'><b>4. Данни:</b> показват потребление, разход и графики за 24 часа, месец и година. Това са оценки по зададена мощност, а не измерване с електромер.</div>
<div class='step'><b>5. Настройки:</b> температурата на превключване, минималните времена, цената на тока и датчикът се настройват от „Настройки“.</div>
<div class='step'><b>6. Wi-Fi:</b> от раздел „Wi-Fi“ можеш да смениш домашната мрежа и да видиш IP адреса.</div>
<div class='hint'><b>Ако страницата не се отваря:</b> провери дали телефонът е в домашната Wi-Fi мрежа. Управлението е локално и не трябва да е публикувано в интернет.</div>
<p><b>Добавяне на началния екран:</b></p>
<p>iPhone (Safari): Споделяне → Добави към Начален екран → Добави.</p>
<p>Android (Chrome): меню ⋮ → Добави към началния екран → Добави.</p>
<p class='buttons'><a class='link' href='/'>Към управлението</a><a class='link' href='/history'>Към данните</a></p>
</section></main></body></html>
)HELP");
  httpServer.sendHeader("Cache-Control", "no-store");
  httpServer.send(200, "text/html; charset=utf-8", s);
}


// ---------------- OTA firmware update (STA only, not the provisioning AP) ----------------
// Password creation is limited to the trusted home LAN. The heater UI stays login-free.
// The secret is stored in its own LittleFS file so old settings and history stay intact.
bool otaFromHomeLan() {
  return WiFi.status() == WL_CONNECTED &&
         httpServer.client().localIP() == WiFi.localIP();
}

void loadOtaPassword() {
  File f = LittleFS.open("/ota_auth.json", "r");
  if (!f) return;
  JsonDocument d;
  if (!deserializeJson(d, f)) {
    String p = String(d["password"] | "");
    if (p.length() >= 12 && p.length() <= 64) otaPassword = p;
  }
  f.close();
}

bool saveOtaPassword(const String &password) {
  JsonDocument d;
  d["password"] = password;
  File f = LittleFS.open("/ota_auth.json", "w");
  if (!f) return false;
  size_t count = serializeJson(d, f);
  f.close();
  if (count == 0) return false;
  otaPassword = password;
  return true;
}

bool otaLocalOrReject() {
  if (otaFromHomeLan()) return true;
  httpServer.send(403, "text/plain; charset=utf-8",
                  "Обновяването е достъпно само през домашната Wi-Fi мрежа, не през AP.");
  return false;
}

bool otaAuthenticate() {
  if (otaPassword.length() < 12) {
    httpServer.send(403, "text/plain; charset=utf-8", "Първо създай парола за обновяване.");
    return false;
  }
  if (httpServer.authenticate("vega-update", otaPassword.c_str())) return true;
  httpServer.requestAuthentication(DIGEST_AUTH, "Vega Firmware Update");
  return false;
}

void otaUpdatePage() {
  touchWeb();
  if (!otaLocalOrReject()) return;
  if (otaPassword.length() && !otaAuthenticate()) return;
  String s = pageTop("Обновяване на фърмуера");
  s += F("<h1>Обновяване на фърмуера</h1><section>"
         "<p>Само за ESP8266 от серия Vega-S v6.x. Използвай <b>*.bin</b> файл,"
         " компилиран за същата платка и Flash конфигурация.</p>"
         "<p>При започване отоплението се <b>изключва</b> и няма да се"
         " включи отново автоматично след рестарта.</p>"
         "<p><b>Важно:</b> не изключвай захранването по време на обновяване."
         " Ако обновяването се провали, управлението ще остане изключено.</p>");
  if (otaPassword.length() == 0) {
    s += F("<h2>Първа стъпка: създай парола</h2>"
           "<p>Тази парола е само за обновяване, не за управление на термостата."
           " Въведи поне 12 символа и я запиши на сигурно място.</p>"
           "<form method='post' action='/update/password'>");
    s += hiddenToken();
    s += F("<label>Нова парола за обновяване</label>"
           "<input type='password' minlength='12' maxlength='64' name='password' required>"
           "<button type='submit'>Създай парола</button></form>");
  } else {
    s += F("<h2>Качи нова версия</h2>"
           "<p>Първо в Arduino IDE избери <b>Sketch → Export Compiled Binary</b>,"
           " след това посочи получения <code>.bin</code> файл.</p>"
           "<form id='fwUpload' method='post' enctype='multipart/form-data' action='");
    s += otaUploadRoute;
    s += F("'><label>Фърмуер (.bin)</label>"
           "<input name='firmware' type='file' accept='.bin' required>"
           "<button type='submit' id='upBtn'>Качи и обнови</button></form>"
           "<p id='uploadProgress' class='muted' role='status'></p>"
           "<script>document.getElementById('fwUpload').addEventListener('submit',function(ev){"
           "ev.preventDefault();const f=this,btn=document.getElementById('upBtn'),p=document.getElementById('uploadProgress');"
           "if(!f.elements.firmware.files.length)return;btn.disabled=true;p.textContent='Подготовка…';"
           "const x=new XMLHttpRequest();x.open('POST',f.action);"
           "x.upload.onprogress=e=>{if(e.lengthComputable)p.textContent='Качване: '+Math.round(100*e.loaded/e.total)+'%';};"
           "x.onload=()=>{p.textContent=x.responseText;if(x.status!==200)btn.disabled=false;};"
           "x.onerror=()=>{p.textContent='Връзката прекъсна. Проверете дали устройството се рестартира.';btn.disabled=false;};"
           "x.send(new FormData(f));});</script>");
    s += F("<hr><h3>Смяна на паролата</h3><form method='post' action='/update/password'>");
    s += hiddenToken();
    s += F("<label>Нова парола (12–64 символа)</label>"
           "<input type='password' name='password' minlength='12' maxlength='64' required>"
           "<button type='submit'>Смени паролата</button></form>");
  }
  s += F("<p class='muted'>Обновяват се само програмните файлове."
         " Настройките и историята в LittleFS се запазват. Паролата не трябва"
         " да се използва на публична Wi-Fi мрежа. Ако нямаш достатъчно свободно"
         " място във Flash, обновяването ще бъде отказано.</p></section></main></body></html>");
  httpServer.sendHeader("Cache-Control", "no-store");
  httpServer.send(200, "text/html; charset=utf-8", s);
}

void otaSetPassword() {
  touchWeb();
  if (!otaLocalOrReject()) return;
  if (otaPassword.length() && !otaAuthenticate()) return;
  if (!checkPost()) return;
  String password = httpServer.arg("password");
  if (password.length() < 12 || password.length() > 64) {
    httpServer.send(400, "text/plain; charset=utf-8", "Паролата трябва да бъде 12–64 символа.");
    return;
  }
  if (!saveOtaPassword(password)) {
    httpServer.send(500, "text/plain; charset=utf-8", "Неуспешен запис в LittleFS.");
    return;
  }
  httpServer.sendHeader("Location", "/update", true);
  httpServer.send(303, "text/plain; charset=utf-8", "Запазено.");
}

// The upload path contains a fresh random token each boot, reducing cross-site
// request risk. Firmware-only U_FLASH: no LittleFS image is accepted.
void otaUploadHandler() {
  HTTPUpload &upload = httpServer.upload();
  if (upload.status == UPLOAD_FILE_START) {
    otaUploadAuthorized = false;
    otaUploadSucceeded = false;
    otaUploadStarted = false;
    otaUploadError = "";
    if (!otaFromHomeLan() || otaPassword.length() < 12 ||
        !httpServer.authenticate("vega-update", otaPassword.c_str())) {
      otaUploadError = "Няма достъп за обновяване.";
      return;
    }
    if (upload.name != "firmware" || !upload.filename.endsWith(".bin")) {
      otaUploadError = "Изберете само .bin фърмуер за ESP8266.";
      return;
    }
    otaUploadAuthorized = true;
    // Fail-safe BEFORE writing to flash and while main loop may be delayed.
    cfg.enabled = false;
    cfg.manualMode = false;
    offTimerActive = false;
    setRelay(false);
    saveSettings();
    otaUploading = true;
    // OTA requires room for another complete sketch. Do not underflow.
    const uint32_t freeSketch = ESP.getFreeSketchSpace();
    if (freeSketch <= 4096) {
      otaUploadError = "Няма достатъчно Flash за OTA.";
      otaUploading = false;
      return;
    }
    const uint32_t maxSketchSpace = (freeSketch - 4096U) & 0xFFFFF000U;
    if (!maxSketchSpace || !Update.begin(maxSketchSpace, U_FLASH)) {
      otaUploadError = "Update.begin отказа записа. Код=" + String(Update.getError());
      otaUploading = false;
      return;
    }
    otaUploadStarted = true;
    Serial.printf("[OTA] Firmware update started: %s, max=%lu bytes\n",
                  upload.filename.c_str(), (unsigned long)maxSketchSpace);
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (otaUploadAuthorized && otaUploadStarted && !otaUploadError.length()) {
      if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
        otaUploadError = "Грешка при запис. Код=" + String(Update.getError());
        Update.end();
        otaUploading = false;
      }
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (otaUploadAuthorized && otaUploadStarted && !otaUploadError.length()) {
      if (upload.totalSize == 0 || !Update.end(true)) {
        otaUploadError = "Невалиден или непълен фърмуер. Код=" + String(Update.getError());
      } else {
        otaUploadSucceeded = true;
        Serial.printf("[OTA] Firmware written: %lu bytes. Reboot pending.\n",
                      (unsigned long)upload.totalSize);
      }
    }
    otaUploading = false;
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    if (otaUploadStarted) Update.end();
    otaUploadError = "Качването е прекъснато.";
    otaUploading = false;
  }
  yield();
}

void otaUploadFinished() {
  touchWeb();
  if (!otaLocalOrReject()) return;
  if (!otaAuthenticate()) return;
  if (!otaUploadAuthorized || !otaUploadSucceeded) {
    String msg = otaUploadError.length() ? otaUploadError : "Неуспешно обновяване.";
    httpServer.send(400, "text/plain; charset=utf-8", msg);
    return;
  }
  httpServer.sendHeader("Cache-Control", "no-store");
  httpServer.send(200, "text/plain; charset=utf-8",
                  "Фърмуерът е приет. Устройството се рестартира."
                  " Изчакай и отвори началната страница. Отоплението остава изключено.");
  otaRebootAt = millis() + 1800UL;
}

#include "CloudConnector.h"

void setupRoutes() {
  // iOS Safari requests these automatically when saving to the Home Screen.
  // Serve all images from PROGMEM to avoid filesystem and RAM overhead.
  httpServer.on("/apple-touch-icon.png", HTTP_GET, []() {
    httpServer.sendHeader("Cache-Control", "public, max-age=604800");
    httpServer.send_P(200, PSTR("image/png"), (PGM_P)APPLE_ICON_PNG, sizeof(APPLE_ICON_PNG));
  });
  httpServer.on("/apple-touch-icon-precomposed.png", HTTP_GET, []() {
    httpServer.sendHeader("Cache-Control", "public, max-age=604800");
    httpServer.send_P(200, PSTR("image/png"), (PGM_P)APPLE_ICON_PNG, sizeof(APPLE_ICON_PNG));
  });
  httpServer.on("/favicon.ico", HTTP_GET, []() {
    httpServer.sendHeader("Cache-Control", "public, max-age=604800");
    httpServer.send_P(200, PSTR("image/x-icon"), (PGM_P)FAVICON_ICO, sizeof(FAVICON_ICO));
  });
  httpServer.on("/icon-192.png", HTTP_GET, []() {
    httpServer.sendHeader("Cache-Control", "public, max-age=604800");
    httpServer.send_P(200, PSTR("image/png"), (PGM_P)ICON_192_PNG, sizeof(ICON_192_PNG));
  });
  httpServer.on("/manifest.webmanifest", HTTP_GET, []() {
    httpServer.sendHeader("Cache-Control", "no-store");
    JsonDocument manifest;
    manifest["name"] = deviceName();
    manifest["short_name"] = deviceName();
    manifest["start_url"] = "/";
    manifest["scope"] = "/";
    manifest["display"] = "standalone";
    manifest["background_color"] = "#10233b";
    manifest["theme_color"] = "#10233b";
    JsonObject icon = manifest["icons"].to<JsonArray>().add<JsonObject>();
    icon["src"] = "/icon-192.png";
    icon["sizes"] = "192x192";
    icon["type"] = "image/png";
    String manifestJson;
    serializeJson(manifest, manifestJson);
    httpServer.send(200, "application/manifest+json", manifestJson);
  });
  httpServer.on("/", HTTP_GET, [](){
    if (apRunning && httpServer.client().localIP()==WiFi.softAPIP()) setupWizardPage();
    else homePage();
  });
  httpServer.on("/help", HTTP_GET, helpPage);
  httpServer.on("/cloud", HTTP_GET, cloudOnboardingPage);
  httpServer.on("/cloud", HTTP_POST, cloudSavePage);
  httpServer.on("/cloud/rotate", HTTP_POST, cloudRotatePairing);
  httpServer.on("/update", HTTP_GET, otaUpdatePage);
  httpServer.on("/update/password", HTTP_POST, otaSetPassword);
  httpServer.on(otaUploadRoute, HTTP_POST, otaUploadFinished, otaUploadHandler);
  httpServer.on("/setup", HTTP_GET, setupWizardPage);
  httpServer.on("/setup/state", HTTP_GET, setupInfoApi);
  httpServer.on("/setup/networks", HTTP_GET, setupNetworksApi);
  httpServer.on("/setup/connect", HTTP_POST, [](){
    if (!checkPost()) return;
    String ssid=httpServer.arg("ssid"), password=httpServer.arg("password");
    ssid.trim();
    if (!ssid.length() || ssid.length()>32 || password.length()>63 ||
        (password.length()>0 && password.length()<8)) {
      httpServer.send(400,"application/json","{\"error\":\"Invalid Wi-Fi SSID or password\"}"); return;
    }
    cfg.enabled=false; cfg.manualMode=false; offTimerActive=false; setRelay(false);
    saveSettings(); // Safer during network commissioning.
    onboardingSSID=ssid; onboardingPassword=password;
    onboardingWantStatic=httpServer.arg("freeze")=="1";
    onboardingActive=true; onboardingSuccess=false; onboardingError="";
    wifiChangeQueuedAt=millis(); wifiChangePending=true;
    httpServer.sendHeader("Cache-Control","no-store");
    httpServer.send(202,"application/json","{\"ok\":true}");
  });
  httpServer.on("/setup/dhcp", HTTP_POST, [](){
    if (!checkPost()) return;
    cfg.enabled=false;cfg.manualMode=false;offTimerActive=false;setRelay(false);saveSettings();
    netStatic=false;saveNetworkConfig();
    WiFi.config(0U,0U,0U);
    WiFi.reconnect();
    httpServer.send(200,"application/json","{\"ok\":true,\"dhcp\":true}");
  });
  httpServer.on("/api/status", HTTP_GET, statusApi);
  // Lightweight LAN API for Home Assistant (no MQTT or cloud).
  // The thermostat continues to operate locally with its existing safety limits.
  // This device has no web authentication: keep it on a trusted local network.
  httpServer.on("/api/ha/state", HTTP_GET, statusApi);
  httpServer.on("/api/ha/mode", HTTP_POST, []() {
    touchWeb();
    String mode = httpServer.arg("mode");
    if ((otaUploading || otaRebootAt) && mode != "off") {
      httpServer.send(423, "application/json", "{\"error\":\"firmware_update_in_progress\"}");
      return;
    }
    if (mode == "off") {
      // Idempotent OFF and unconditional relay de-energize.
      bool changed = cfg.enabled || cfg.manualMode || offTimerActive || relayOn;
      cfg.enabled = false;
      cfg.manualMode = false;
      offTimerActive = false;
      setRelay(false);
      if (changed) saveSettings();
      httpServer.sendHeader("Cache-Control", "no-store");
      httpServer.send(200, "application/json", "{\"ok\":true,\"mode\":\"off\"}");
      return;
    }
    if (mode != "heat") {
      httpServer.send(400, "application/json", "{\"error\":\"invalid_mode\"}");
      return;
    }
    // HA is automatic heating only, never an unsafe manual sensorless bypass.
    if (!sensorHealthy() || !validNumber(roomTemp, -15, 50) ||
        roomTemp >= HARD_ROOM_LIMIT) {
      httpServer.send(409, "application/json", "{\"error\":\"safety_or_sensor_unavailable\"}");
      return;
    }
    bool changed = !cfg.enabled || cfg.manualMode || offTimerActive;
    cfg.enabled = true;
    cfg.manualMode = false;
    offTimerActive = false;
    if (changed) saveSettings();
    thermostat();
    httpServer.sendHeader("Cache-Control", "no-store");
    httpServer.send(200, "application/json", "{\"ok\":true,\"mode\":\"heat\"}");
  });
  httpServer.on("/api/ha/target", HTTP_POST, []() {
    touchWeb();
    // Reuse exact validation, rounding, delayed persistence and control logic.
    if (!applyRequestedTarget()) return;
    char result[80];
    snprintf(result, sizeof(result), "{\"ok\":true,\"target\":%.1f}", cfg.target);
    httpServer.sendHeader("Cache-Control", "no-store");
    httpServer.send(200, "application/json", result);
  });

  httpServer.on("/health", HTTP_GET, []() { httpServer.send(200, "text/plain", "Heater Performance v6.6 Cloud alive"); });
  httpServer.on("/api/stats", HTTP_GET, statsApi);
  httpServer.on("/api/history", HTTP_GET, historyApi);
  httpServer.on("/api/perf", HTTP_GET, []() {
    touchWeb();
    char out[180];
    snprintf(out, sizeof(out), "{\"firmware\":\"Performance-v6.6-Cloud\",\"heap\":%u,\"max_block\":%u,\"frag_pct\":%u,\"history_build_ms\":%lu,\"history_bytes\":%lu}",
             ESP.getFreeHeap(), ESP.getMaxFreeBlockSize(), ESP.getHeapFragmentation(),
             (unsigned long)lastHistoryBuildMs, (unsigned long)lastHistoryResponseBytes);
    httpServer.send(200, "application/json; charset=utf-8", out);
  });
  httpServer.on("/api/stats/reset", HTTP_POST, []() {
    if (!checkPost()) return;
    // Explicit action only. Both redundant snapshots must be erased.
    heaterStats.clear();
    invalidateHistoryCache();
    if (relayOn) relayStartedAt = millis(); // reset current cycle accounting too
    httpServer.send(200,"application/json","{\"ok\":true}");
  });
  httpServer.on("/settings", HTTP_GET, settingsPage);
  httpServer.on("/history", HTTP_GET, historyPage);
  httpServer.on("/api/toggle", HTTP_POST, []() {
    if (!checkPost()) return;
    toggleHeating(); thermostat();
    httpServer.sendHeader("Cache-Control", "no-store");
    httpServer.send(200,"application/json","{\"ok\":true}");
  });
  httpServer.on("/toggle", HTTP_POST, []() {
    if (!checkPost()) return;
    toggleHeating(); thermostat(); redirectHome();
  });
  httpServer.on("/manual", HTTP_POST, []() {
    if (!checkPost()) return;
    if (cfg.enabled && cfg.manualMode) {
      cfg.enabled = false; cfg.manualMode = false;
      offTimerActive = false; setRelay(false); saveSettings();
    } else {
      // Explicit manual mode is time-limited and still honors high temperature.
      enableManual();
    }
    thermostat(); redirectHome();
  });
  httpServer.on("/timer", HTTP_POST, []() {
    if (!checkPost()) return;
    if (cfg.manualMode) {
      httpServer.send(409, "text/plain", "Manual safety timer cannot be cancelled"); return;
    }
    if (offTimerActive) {
      offTimerActive = false;
    } else {
      if (!cfg.enabled) {
        httpServer.send(409, "text/plain; charset=utf-8", "Enable thermostat first");
        return;
      }
      offTimerStartedAt = millis();
      offTimerActive = true;
    }
    saveSettings();
    redirectHome();
  });
  httpServer.on("/target", HTTP_POST, []() {
    if (!checkPost()) return;
    if (applyRequestedTarget()) redirectHome();
  });
  httpServer.on("/api/target", HTTP_POST, []() {
    if (!checkPost()) return;
    if (!applyRequestedTarget()) return;
    JsonDocument response;
    response["target"] = cfg.target;
    response["effective"] = effectiveTarget();
    String body;
    serializeJson(response, body);
    httpServer.sendHeader("Cache-Control", "no-store");
    httpServer.send(200, "application/json; charset=utf-8", body);
  });
  httpServer.on("/settings", HTTP_POST, []() {
    if (!checkPost()) return;
    String pinText = httpServer.arg("sensorPin");
    String typeText = httpServer.arg("sensorType");
    if (!((pinText == "4") || (pinText == "13") || (pinText == "14")) ||
        !((typeText == "22") || (typeText == "70"))) {
      httpServer.send(400, "text/plain", "Unsupported sensor GPIO or protocol"); return;
    }
    const uint8_t newSensorPin = (uint8_t) pinText.toInt();
    const uint8_t newSensorType = (uint8_t) typeText.toInt();
    const String powerText = httpServer.arg("powerW");
    const uint16_t newPowerW = (uint16_t)powerText.toInt();
    if (powerText != String(newPowerW) || !isSupportedPower(newPowerW)) {
      httpServer.send(400, "text/plain", "Unsupported heater power"); return;
    }
    String dayText = httpServer.arg("dayRate"); dayText.trim(); dayText.replace(',', '.');
    String nightText = httpServer.arg("nightRate"); nightText.trim(); nightText.replace(',', '.');
    char *endDay = nullptr, *endNight = nullptr;
    float newDayRate = strtof(dayText.c_str(), &endDay);
    float newNightRate = strtof(nightText.c_str(), &endNight);
    String dayHourText = httpServer.arg("dayStartHour");
    String nightHourText = httpServer.arg("nightStartHour");
    char *endDayHour = nullptr, *endNightHour = nullptr;
    long newDayHour = strtol(dayHourText.c_str(), &endDayHour, 10);
    long newNightHour = strtol(nightHourText.c_str(), &endNightHour, 10);
    if (        dayText.length() == 0 || nightText.length() == 0 ||
        endDay == dayText.c_str() || *endDay != 0 ||
        endNight == nightText.c_str() || *endNight != 0 ||
        !validNumber(newDayRate, 0.0f, 5.0f) || !validNumber(newNightRate, 0.0f, 5.0f) ||
        dayHourText.length() == 0 || nightHourText.length() == 0 ||
        endDayHour == dayHourText.c_str() || *endDayHour != 0 ||
        endNightHour == nightHourText.c_str() || *endNightHour != 0 ||
        newDayHour < 0 || newDayHour > 23 ||
        newNightHour < 0 || newNightHour > 23 || newDayHour == newNightHour) {
      httpServer.send(400, "text/plain", "Invalid input"); return;
    }
    String onDeltaText = httpServer.arg("heatOnDelta");
    String offDeltaText = httpServer.arg("heatOffDelta");
    String minOnText = httpServer.arg("minOnMinutes");
    String minOffText = httpServer.arg("minOffMinutes");
    onDeltaText.trim(); onDeltaText.replace(',', '.');
    offDeltaText.trim(); offDeltaText.replace(',', '.');
    char *endOn = nullptr, *endOff = nullptr, *endMinOn = nullptr, *endMinOff = nullptr;
    float newHeatOn = strtof(onDeltaText.c_str(), &endOn);
    float newHeatOff = strtof(offDeltaText.c_str(), &endOff);
    long newMinOn = strtol(minOnText.c_str(), &endMinOn, 10);
    long newMinOff = strtol(minOffText.c_str(), &endMinOff, 10);
    if (onDeltaText.length() == 0 || offDeltaText.length() == 0 ||
        minOnText.length() == 0 || minOffText.length() == 0 ||
        endOn == onDeltaText.c_str() || *endOn != 0 ||
        endOff == offDeltaText.c_str() || *endOff != 0 ||
        endMinOn == minOnText.c_str() || *endMinOn != 0 ||
        endMinOff == minOffText.c_str() || *endMinOff != 0 ||
        !validNumber(newHeatOn, 0.1f, 3.0f) ||
        !validNumber(newHeatOff, -1.0f, 2.0f) ||
        newHeatOn + newHeatOff < 0.1f ||
        newMinOn < 1 || newMinOn > 30 ||
        newMinOff < 1 || newMinOff > 30) {
      httpServer.send(400, "text/plain", "Invalid heating cycle settings"); return;
    }
    const bool heatingChanged =
      fabsf(cfg.heatOnDelta - newHeatOn) > 0.001f ||
      fabsf(cfg.heatOffDelta - newHeatOff) > 0.001f ||
      cfg.minOnMinutes != (uint16_t)newMinOn ||
      cfg.minOffMinutes != (uint16_t)newMinOff;
    const bool sensorChanged =
      cfg.sensorPin != newSensorPin || cfg.sensorType != newSensorType;
    if (heatingChanged) {
      // Explicit re-arm after changing timing. Never risk an abrupt new duty cycle.
      cfg.enabled = false;
      cfg.manualMode = false;
      offTimerActive = false;
      setRelay(false);
    }
    cfg.heatOnDelta = newHeatOn;
    cfg.heatOffDelta = newHeatOff;
    cfg.minOnMinutes = (uint16_t)newMinOn;
    cfg.minOffMinutes = (uint16_t)newMinOff;
    if (sensorChanged) {
      cfg.enabled = false; cfg.manualMode = false;
      offTimerActive = false;
      setRelay(false);
      cfg.sensorPin = newSensorPin;
      cfg.sensorType = newSensorType;
      applySensorConfiguration();
    }
    cfg.powerW = newPowerW;
    cfg.dayRate = newDayRate; cfg.nightRate = newNightRate;
    cfg.dayStartHour = (uint8_t)newDayHour;
    cfg.nightStartHour = (uint8_t)newNightHour;
    saveSettings();
    httpServer.sendHeader("Location", "/settings", true);
    httpServer.send(303, "text/plain", "Saved");
  });
  httpServer.on("/wifi", HTTP_POST, []() {
    httpServer.sendHeader("Location","/setup",true);
    httpServer.send(303,"text/plain","Use Wi-Fi setup wizard");
  });
  httpServer.onNotFound([]() {
    Serial.printf("[WEB] 404 path=%s method=%d\n", httpServer.uri().c_str(), (int)httpServer.method());
    if (apRunning && httpServer.client().localIP()==WiFi.softAPIP()) {
      httpServer.sendHeader("Location","http://192.168.4.1/setup",true);
      httpServer.send(302,"text/plain","Open setup portal");
    } else {
      httpServer.send(404, "text/plain", "Not found on Vega-S Performance v6.6 Cloud: " + httpServer.uri());
    }
  });
}

void networkTask() {
  static bool previouslyConnected=false;
  const unsigned long now=millis();
  if (wifiChangePending) startSetupConnection();
  const bool connected=WiFi.status()==WL_CONNECTED;
  if (connected) {
    wifiDisconnectedAt=0;
    if (onboardingActive && !onboardingSuccess && !wifiChangePending) {
      onboardingError="";
      onboardingSuccess=true;
      onboardingConnectedAt=now;
      onboardingDHCPIP=WiFi.localIP();
      if (onboardingWantStatic) setStaticFromLease();
      else { netStatic=false; saveNetworkConfig(); }
      Serial.println("[SETUP] Home URL: http://"+WiFi.localIP().toString());
    }
    if (netRecoveryTried && netStatic) {
      netStatic=false; saveNetworkConfig();
      Serial.println("[Wi-Fi] DHCP recovery successful; static configuration cleared.");
    }
    if (apRunning && (!onboardingActive ||
       (onboardingSuccess && now-onboardingConnectedAt>=ONBOARDING_AP_GRACE_MS))) {
      dnsServer.stop();
      WiFi.softAPdisconnect(true);
      WiFi.mode(WIFI_STA);
      apRunning=false;
      httpServer.stop();httpServer.begin();
      onboardingActive=false;
      Serial.println("[Wi-Fi] Setup AP disabled; local home Wi-Fi is active.");
    }
    if (!previouslyConnected)
      Serial.println("Home Wi-Fi web: http://"+WiFi.localIP().toString());
  } else {
    if (!wifiDisconnectedAt) wifiDisconnectedAt=now;
    if (onboardingActive && !onboardingSuccess && !wifiChangePending &&
        now-onboardingStartedAt>30000UL && onboardingError.length()==0)
      onboardingError="Връзката не е установена. Проверете името и паролата на Wi-Fi.";
    // Recover a stale static lease: DHCP fallback before the AP setup mode.
    if (netStatic && !netRecoveryTried && now-bootNetworkAt>30000UL &&
        (!onboardingActive || (onboardingSuccess &&
         now-onboardingConnectedAt>45000UL))) {
      netRecoveryTried=true;
      WiFi.config(0U,0U,0U);
      WiFi.reconnect();
      Serial.println("[Wi-Fi] Static address not reachable; trying DHCP recovery.");
    }
    const unsigned long waitTime=onboardingActive || WiFi.SSID().length()==0 ? 1500UL : AP_FALLBACK_DELAY;
    if (!apRunning && now-wifiDisconnectedAt>=waitTime &&
        (!lastApStartAttempt || now-lastApStartAttempt>=AP_RETRY_INTERVAL)) {
      lastApStartAttempt=now;
      WiFi.mode(WIFI_AP_STA);
      WiFi.softAPConfig(IPAddress(192,168,4,1),IPAddress(192,168,4,1),IPAddress(255,255,255,0));
      apRunning=WiFi.softAP(apName.c_str(),AP_PASSWORD);
      if (apRunning) {
        dnsServer.start(53,"*",IPAddress(192,168,4,1));
        httpServer.stop();httpServer.begin();
        Serial.println("[Wi-Fi] Setup AP: "+apName+" http://192.168.4.1/setup");
      }
    }
  }
  previouslyConnected=connected;
  if (apRunning) dnsServer.processNextRequest();
  httpServer.handleClient();
}

void setup() {
  // FIRST action: put relay output into de-energised state.
  digitalWrite(RELAY_GPIO, RELAY_OFF_LEVEL);
  pinMode(RELAY_GPIO, OUTPUT);
  pinMode(BUTTON_GPIO, INPUT_PULLUP);
  Serial.begin(115200);
  delay(100);
  WiFi.mode(WIFI_STA);
  WiFi.setSleepMode(WIFI_NONE_SLEEP); // lower HTTP latency; slightly higher power
  deviceId = "irheater_" + String(ESP.getChipId(), HEX);
  apName = "Heater-" + String(ESP.getChipId(), HEX);
  csrf = makeSecret(20);
  otaUploadRoute = "/update/fw/" + csrf;

  if (!LittleFS.begin()) Serial.println("WARNING: LittleFS unavailable");
  loadSettings();
  loadOtaPassword();
  cloudLoad();
  loadNetworkConfig();
  heaterStats.begin();
  // Called only after a real time update from an SNTP server.
  settimeofday_cb([](bool fromNtp) {
    if (fromNtp && time(nullptr) >= 1704067200) {
      lastNtpSuccessAt = millis();
      ++ntpSuccessCount;
    }
  });
  applySensorConfiguration();
  setupRoutes();

  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);
  if (netStatic) {
    if (!WiFi.config(netIP, netGateway, netMask, netDNS)) {
      Serial.println("[Wi-Fi] Static configuration rejected; trying DHCP.");
      netStatic=false;
      saveNetworkConfig();
      WiFi.config(0U,0U,0U);
    }
  } else WiFi.config(0U,0U,0U);
  bootNetworkAt=millis();
  WiFi.begin(); // First try saved home Wi-Fi; fallback AP is started by networkTask()
  httpServer.begin();
  Serial.println("Firmware: Heater Performance v6.6 Cloud (2026-11)");
  Serial.println("Device: " + deviceId);
  Serial.println("Wi-Fi: connecting to saved network. AP starts only if offline.");
  Serial.println(String("Fallback AP: ") + apName + " | password: " + AP_PASSWORD);
  Serial.println("On-board button is RESET. External button is GPIO12 -> GND.");
}

void loop() {
  if (otaRebootAt != 0 && (int32_t)(millis() - otaRebootAt) >= 0) {
    setRelay(false);
    ESP.restart();
  }
  readButton();
  readSensor();
  offTimerTask();
  thermostat();
  networkTask();
  clockTask();
  cloudTask();
  static unsigned long lastStatsTick = 0;
  if (millis() - lastStatsTick >= 1000UL) {
    lastStatsTick = millis();
    heaterStats.tick(relayOn, roomTemp, sensorHealthy(),
                     cfg.dayStartHour, cfg.nightStartHour);
  }
  pendingSettingsTask();
  yield();
}
