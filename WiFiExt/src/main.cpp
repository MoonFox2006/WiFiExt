#include <Arduino.h>
#include <EEPROM.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <DNSServer.h>
#include <dhcpserver.h>
#include <lwip/napt.h>
#include <lwip/dns.h>

#if !LWIP_FEATURES || LWIP_IPV6
#error "NAPT not supported!"
#endif

#define LED_PIN 2 // LED_BUILTIN
#ifdef LED_PIN
#define LED_LEVEL LOW
const uint32_t BLINK_DURATION = 25; // 25 ms.

uint32_t blinkPeriod;
#endif

#define EXTENDER_IP IPAddress(172, 217, 28, 254)
#define EXTENDER_MASK IPAddress(255, 255, 255, 0)

static const char DEF_AUTH_NAME[] PROGMEM = "ESP8266";
static const char DEF_AUTH_PSWD[] PROGMEM = "6628PSE";
const bool DEF_USE_AUTH = true;

struct config_t {
  char wifi_ssid[32 + 1];
  char wifi_pswd[64 + 1];
  char ap_ssid[32 + 1];
  char auth_name[16 + 1];
  char auth_pswd[16 + 1];
  bool use_auth;
} config;

ESP8266WebServer *http = NULL;
bool useCaptivePortal;
bool rebooting = false;

static void halt(const __FlashStringHelper *msg) {
  Serial.println();
  Serial.println(msg);
  Serial.flush();
#ifdef LED_PIN
  digitalWrite(LED_PIN, ! LED_LEVEL);
#endif

  ESP.deepSleep(0);
}

static void reboot(const __FlashStringHelper *msg) {
  rebooting = true;
  Serial.println();
  Serial.println(msg);
  Serial.flush();
#ifdef LED_PIN
  digitalWrite(LED_PIN, ! LED_LEVEL);
#endif

  ESP.restart();
}

static uint8_t crc8(uint8_t data, uint8_t crc = 0xFF) {
  crc ^= data;
  for (uint8_t bit = 8; bit > 0; --bit) {
    if (crc & 0x80)
      crc = (crc << 1) ^ 0x31;
    else
      crc <<= 1;
  }
  return crc;
}

static uint8_t crc8(const uint8_t *data, uint8_t len, uint8_t crc = 0xFF) {
  while (len--) {
    crc = crc8(*data++, crc);
  }
  return crc;
}

static const uint8_t EEPROM_SIGN = 0xA5;

static bool readConfig() {
  uint16_t offset = 0;

  if (EEPROM.read(offset) == EEPROM_SIGN) {
    uint8_t crc = crc8(EEPROM_SIGN);

    EEPROM.get(++offset, config);
    if (EEPROM.read(offset + sizeof(config_t)) == crc8((uint8_t*)&config, sizeof(config_t), crc))
      return true;
  }
  memset(&config, 0, sizeof(config_t));
  strcpy_P(config.auth_name, DEF_AUTH_NAME);
  strcpy_P(config.auth_pswd, DEF_AUTH_PSWD);
  config.use_auth = DEF_USE_AUTH;
  return false;
}

static bool writeConfig() {
  uint16_t offset = 0;
  uint8_t crc = crc8(EEPROM_SIGN);

  EEPROM.write(offset++, EEPROM_SIGN);
  EEPROM.put(offset, config);
  EEPROM.write(offset + sizeof(config_t), crc8((uint8_t*)&config, sizeof(config_t), crc));
  return EEPROM.commit();
}

static String timeToStr(uint32_t seconds) {
  String result;
  uint8_t part;

  result.reserve(16);
  result = String(seconds / 3600 * 24);
  result += '.';
  part = seconds / 3600 % 24;
  if (part < 10)
    result += '0';
  result += String(part);
  result += ':';
  part = seconds / 60 % 60;
  if (part < 10)
    result += '0';
  result += String(part);
  result += ':';
  part = seconds % 60;
  if (part < 10)
    result += '0';
  result += String(part);
  return result;
}

static String macToString(const uint8_t *mac) {
  String result;

  result.reserve(18);
  for (uint8_t i = 0; i < 6; ++i) {
    uint8_t d;

    if (i)
      result += ':';
    d = mac[i] >> 4;
    if (d <= 9)
      result += (char)('0' + d);
    else
      result += (char)('A' + d - 10);
    d = mac[i] & 0x0F;
    if (d <= 9)
      result += (char)('0' + d);
    else
      result += (char)('A' + d - 10);
  }
  return result;
}

static const char TEXTHTML[] PROGMEM = "text/html";
static const char TEXTPLAIN[] PROGMEM = "text/plain";

static bool captivePortal() {
  if (useCaptivePortal && (! http->hostHeader().equals(WiFi.softAPIP().toString()))) {
    http->sendHeader(F("Location"), String(F("http://")) + WiFi.softAPIP().toString(), true);
    http->send_P(302, TEXTPLAIN, NULL, 0);
    return true;
  }
  return false;
}

static bool checkAuthorization() {
  if ((! useCaptivePortal) && config.use_auth && (! http->authenticate(config.auth_name, config.auth_pswd))) {
    http->requestAuthentication();
    return false;
  }
  return true;
}

static const char WIFI_SSID_PARAM[] PROGMEM = "wifi_ssid";
static const char WIFI_PSWD_PARAM[] PROGMEM = "wifi_pswd";
static const char AP_SSID_PARAM[] PROGMEM = "ap_ssid";
static const char AUTH_NAME_PARAM[] PROGMEM = "auth_name";
static const char AUTH_PSWD_PARAM[] PROGMEM = "auth_pswd";
static const char USE_AUTH_PARAM[] PROGMEM = "use_auth";

static void handleRoot() {
  if (captivePortal() || (! checkAuthorization()))
    return;

  String page;

  page.reserve(2048);
  page = F("<!DOCTYPE html>\n"
    "<html>\n"
    "<head><title>WiFi Extender</title></head>\n"
    "<body bgcolor=#EEE>\n");
  if (WiFi.isConnected()) {
    page += F("<h4>Source WiFi connected with RSSI ");
    page += String(WiFi.RSSI());
    page += F(" dB</h4>\n");
  }
  page += F("<h4>Uptime ");
  page += timeToStr(millis() / 1000);
  page += F("</h4>\n"
    "<hr>\n"
    "<form method=\"POST\">\n"
    "<table cols=2>\n"
    "<tr><td align=\"right\">Source SSID:</td>\n"
    "<td><input type=\"text\" name=\"");
  page += FPSTR(WIFI_SSID_PARAM);
  page += F("\" size=16 maxlen=");
  page += String((uint8_t)(sizeof(config.wifi_ssid) - 1));
  page += F(" value=\"");
  page += config.wifi_ssid;
  page += F("\"></td></tr>\n"
    "<tr><td align=\"right\">Password:</td>\n"
    "<td><input type=\"password\" name=\"");
  page += FPSTR(WIFI_PSWD_PARAM);
  page += F("\" size=16 maxlen=");
  page += String((uint8_t)(sizeof(config.wifi_pswd) - 1));
  page += F(" value=\"");
  page += config.wifi_pswd;
  page += F("\"></td></tr>\n"
    "<tr><td align=\"right\">Extender SSID:</td>\n"
    "<td><input type=\"text\" name=\"");
  page += FPSTR(AP_SSID_PARAM);
  page += F("\" size=16 maxlen=");
  page += String((uint8_t)(sizeof(config.ap_ssid) - 1));
  page += F(" value=\"");
  page += config.ap_ssid;
  page += F("\"></td></tr>\n"
    "<tr><td align=\"right\">Use authorization:</td>\n"
    "<td><input type=\"checkbox\" name=\"");
  page += FPSTR(USE_AUTH_PARAM);
  page += F("\" value=\"1\"");
  if (config.use_auth)
    page += F(" checked");
  page += F("></td></tr>\n"
    "<tr><td align=\"right\">Authorization name:</td>\n"
    "<td><input type=\"text\" name=\"");
  page += FPSTR(AUTH_NAME_PARAM);
  page += F("\" size=16 maxlen=");
  page += String((uint8_t)(sizeof(config.auth_name) - 1));
  page += F(" value=\"");
  page += config.auth_name;
  page += F("\"></td></tr>\n"
    "<tr><td align=\"right\">Authorization password:</td>\n"
    "<td><input type=\"password\" name=\"");
  page += FPSTR(AUTH_PSWD_PARAM);
  page += F("\" size=16 maxlen=");
  page += String((uint8_t)(sizeof(config.auth_pswd) - 1));
  page += F(" value=\"");
  page += config.auth_pswd;
  page += F("\"></td></tr>\n"
    "</table>\n"
    "<input type=\"submit\" value=\"Update\">\n"
    "<input type=\"reset\" value=\"Cancel\">\n"
    "<input type=\"button\" value=\"Restart!\" onclick='location.href=\"/restart\"'>\n"
    "</form>\n");
  if (WiFi.softAPgetStationNum()) {
    station_info *info = wifi_softap_get_station_info();

    page += F("<hr>\n"
      "Client(s) MAC:<br/>\n");
    while (info) {
      page += macToString(info->bssid);
      page += F("<br/>\n");
      info = STAILQ_NEXT(info, next);
    }
    wifi_softap_free_station_info();
  }
  page += F("<h4>Free heap size: ");
  page += String(ESP.getFreeHeap());
  page += F(" byte(s)</h4>\n"
    "</body>\n"
    "</html>");
  http->send(200, FPSTR(TEXTHTML), page);
}

static void handleConfig() {
  if (captivePortal() || (! checkAuthorization()))
    return;

  String page;
  int16_t code;

  config.use_auth = false; // Checkbox
  for (uint8_t i = 0; i < http->args(); ++i) {
    if (http->argName(i).equals(FPSTR(WIFI_SSID_PARAM))) {
      strcpy(config.wifi_ssid, http->arg(i).c_str());
    } else if (http->argName(i).equals(FPSTR(WIFI_PSWD_PARAM))) {
      strcpy(config.wifi_pswd, http->arg(i).c_str());
    } else if (http->argName(i).equals(FPSTR(AP_SSID_PARAM))) {
      strcpy(config.ap_ssid, http->arg(i).c_str());
    } else if (http->argName(i).equals(FPSTR(AUTH_NAME_PARAM))) {
      strcpy(config.auth_name, http->arg(i).c_str());
    } else if (http->argName(i).equals(FPSTR(AUTH_PSWD_PARAM))) {
      strcpy(config.auth_pswd, http->arg(i).c_str());
    } else if (http->argName(i).equals(FPSTR(USE_AUTH_PARAM))) {
      config.use_auth = *http->arg(i).c_str() == '1';
    }
  }
  page.reserve(512);
  page = F("<!DOCTYPE html>\n"
    "<html>\n"
    "<head><title>Store configuration</title>\n"
    "<meta http-equiv=\"refresh\" content=\"2;URL=/\">\n"
    "</head>\n"
    "<body bgcolor=#EEE>\n");
  if (writeConfig()) {
    page += F("Success<br/>\n"
      "Don't forget to restart module!\n");
    code = 200;
  } else {
    page += F("Error storing configuration!\n");
    code = 500;
  }
  page += F("</body>\n"
    "</html>");
  http->send(code, FPSTR(TEXTHTML), page);
}

static void handleRestart() {
  if (captivePortal() || (! checkAuthorization()))
    return;

  http->send_P(200, TEXTHTML, PSTR("<!DOCTYPE html>\n"
    "<html>\n"
    "<head><title>Restarting</title>\n"
    "<meta http-equiv=\"refresh\" content=\"30;URL=/\">\n"
    "</head>\n"
    "<body bgcolor=#EEE>\n"
    "Restarting...\n"
    "</body>\n"
    "</html>"));
  http->stop();
  delay(10);

  reboot(F("Rebooting by user..."));
}

static void handleNotFound() {
  if (captivePortal() || (! checkAuthorization()))
    return;

  http->send_P(404, TEXTPLAIN, PSTR("Page not found!"));
}

static bool createWebServer() {
  http = new ESP8266WebServer(80);
  if (! http)
    return false;
  http->on(F("/"), HTTP_GET, handleRoot);
  http->on(F("/"), HTTP_POST, handleConfig);
  http->on(F("/restart"), HTTP_GET, handleRestart);
  http->onNotFound(handleNotFound);
  return true;
}

static void destroyWebServer() {
  if (http) {
    http->stop();
    delete http;
    http = NULL;
  }
}

static void startConfigAP() {
  const uint32_t CONFIG_AP_TIMEOUT = 60000; // 1 min.

  static const char AP_PREFIX[] PROGMEM = "ESP8266_EXT";

  char ap_ssid[sizeof(AP_PREFIX) + 8];
  char ap_pswd[8 + 1];

  strcpy_P(ap_ssid, AP_PREFIX);
  sprintf_P(ap_pswd, PSTR("%08X"), ESP.getChipId());
  strcat(ap_ssid, ap_pswd);
  Serial.print(F("Creating config AP \""));
  Serial.print(ap_ssid);
  Serial.print(F("\" with password \""));
  Serial.print(ap_pswd);
  Serial.println('"');
  if (! WiFi.softAP(ap_ssid, ap_pswd))
    reboot(F("Error creating config AP!"));

  DNSServer *dns = new DNSServer();

  if (! dns)
    reboot(F("Error creating DNS server!"));
  dns->setErrorReplyCode(DNSReplyCode::NoError);
  dns->start(53, F("*"), WiFi.softAPIP());

  if (! createWebServer())
    reboot(F("Error creating HTTP server!"));
  useCaptivePortal = true;
  http->begin();

  uint32_t start = millis();

  while ((! *config.wifi_ssid) || (! *config.ap_ssid) || WiFi.softAPgetStationNum() ||
    ((ESP.getResetInfoPtr()->reason != REASON_SOFT_RESTART) && (millis() - start < CONFIG_AP_TIMEOUT))) {
    dns->processNextRequest();
    http->handleClient();
#ifdef LED_PIN
    digitalWrite(LED_PIN, LED_LEVEL == (millis() % 250 < BLINK_DURATION));
#endif
  }
#ifdef LED_PIN
  digitalWrite(LED_PIN, ! LED_LEVEL);
#endif
  useCaptivePortal = false;
//  destroyWebServer();
  dns->stop();
  delete dns;
//  WiFi.softAPdisconnect();
//  Serial.println(F("Config AP closed"));
}

static void connectWiFi() {
  Serial.print(F("Connecting to \""));
  Serial.print(config.wifi_ssid);
  Serial.println(F("\"..."));
  WiFi.begin(config.wifi_ssid, config.wifi_pswd);
#ifdef LED_PIN
  blinkPeriod = 500; // 0.5 sec.
#endif
}

static void onWiFiConnected(const WiFiEventStationModeGotIP &event) {
  Serial.print(F("Connected to WiFi \""));
  Serial.print(WiFi.SSID());
  Serial.print(F("\" (IP: "));
  Serial.print(event.ip);
  Serial.print(F(", DNS: "));
  Serial.print(WiFi.dnsIP(0));
  Serial.print('/');
  Serial.print(WiFi.dnsIP(1));
  Serial.println(')');
  destroyWebServer();
  WiFi.softAPdisconnect();
  // give DNS servers to AP side
  dhcps_set_dns(0, WiFi.dnsIP(0));
  dhcps_set_dns(1, WiFi.dnsIP(1));
  // enable AP, with android-compatible google domain
  WiFi.softAPConfig(EXTENDER_IP, EXTENDER_IP, EXTENDER_MASK);
  if (! WiFi.softAP(config.ap_ssid, config.wifi_pswd, WiFi.channel(), false, 8))
    reboot(F("Error creating extender AP!"));
  if (ip_napt_enable_no(SOFTAP_IF, 1) != ERR_OK)
    reboot(F("Error initialization NAPT!"));
  Serial.print(F("WiFi \""));
  Serial.print(WiFi.softAPSSID());
  Serial.print(F("\" with same password is now NATed behind \""));
  Serial.print(WiFi.SSID());
  Serial.println('"');
  if (createWebServer()) {
    http->begin();
  }
#ifdef LED_PIN
  blinkPeriod = 2000; // 2 sec.
#endif
}

static void onWiFiDisconnected(const WiFiEventStationModeDisconnected &event) {
  Serial.println(F("Disconnected from WiFi"));
  ip_napt_enable_no(SOFTAP_IF, 0);
//  destroyWebServer();
//  WiFi.softAPdisconnect();
  if (! rebooting)
    connectWiFi();
}

static void onClientConnected(const WiFiEventSoftAPModeStationConnected &event) {
  Serial.println(F("New NAPT client connected"));
#ifdef LED_PIN
  blinkPeriod = 1000; // 1 sec.
#endif
}

static void onClientDisconnected(const WiFiEventSoftAPModeStationDisconnected &event) {
  Serial.println(F("NAPT client disconnected"));
#ifdef LED_PIN
  if (! WiFi.softAPgetStationNum())
    blinkPeriod = 2000; // 2 sec.
#endif
}

void setup() {
  Serial.begin(115200, SERIAL_8N1, SERIAL_TX_ONLY);
  Serial.println();

#ifdef LED_PIN
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, ! LED_LEVEL);
#endif

  EEPROM.begin(4096);
  if (! readConfig()) {
    Serial.println(F("EEPROM config not found!"));
  }

  WiFi.persistent(false);
  WiFi.mode(WIFI_AP_STA);
  WiFi.disconnect();
  WiFi.softAPdisconnect();

  startConfigAP();

  if (ip_napt_init(1024, 32) != ERR_OK)
    reboot(F("Error initialization NAPT!"));

  static WiFiEventHandler onConnectedHandler = WiFi.onStationModeGotIP(onWiFiConnected);
  static WiFiEventHandler onDisconnectedHandler = WiFi.onStationModeDisconnected(onWiFiDisconnected);
  static WiFiEventHandler onClientConnectedHandler = WiFi.onSoftAPModeStationConnected(onClientConnected);
  static WiFiEventHandler onClientDisconnectedHandler = WiFi.onSoftAPModeStationDisconnected(onClientDisconnected);

  connectWiFi();
}

void loop() {
  if (http) {
    http->handleClient();
  }
#ifdef LED_PIN
  digitalWrite(LED_PIN, LED_LEVEL == (millis() % blinkPeriod < BLINK_DURATION));
#endif
}
