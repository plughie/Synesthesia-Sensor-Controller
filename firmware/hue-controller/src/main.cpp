#include <Arduino.h>
#include <Wire.h>
#include <math.h>
#include <VL53L1X.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <WebServer.h>
#include <ESPmDNS.h>

#ifndef DEVICE_HOSTNAME
#define DEVICE_HOSTNAME "huecontroller"
#endif

static constexpr int STATUS_LED_GPIO = 48;
static constexpr int ENCODER_A_GPIO = 4;
static constexpr int ENCODER_B_GPIO = 5;
static constexpr int ENCODER_SW_GPIO = 6;
static constexpr int SENSOR_SDA_GPIO = 10;
static constexpr int SENSOR_SCL_GPIO = 11;
static constexpr int SENSOR_MIN_RANGE_MM = 500;
static constexpr int SENSOR_MAX_RANGE_MM = 4000;
static constexpr int SENSOR_RANGE_STEP_MM = 50;
static constexpr unsigned long TEST_PERIOD_MS = 1000;
static char line_buffer[256];
static size_t line_length = 0;
static float hue_raw = 0.0f;
static bool test_enabled = false;
static unsigned long last_test_ms = 0;
static int hold_timeout_ms = 400;
static int pullaway_speed_mm_s = 800;
static Preferences preferences;
static String wifi_ssid;
static String wifi_secret;
static String osc_host = "192.168.1.100";
static int osc_port = 6000;
static String osc_address = "/controls/meta/hue";
static WiFiUDP osc_udp;
static IPAddress osc_ip;
static WebServer web_server(80);
static bool web_started = false;
static unsigned long last_wifi_status_ms = 0;
static int encoder_state = 0;
static int encoder_accum = 0;
static bool locked = false;
static bool last_button = true;
static unsigned long last_button_ms = 0;
static VL53L1X distance_sensor;
static bool sensor_ready = false;
static uint8_t sensor_i2c_status = 0xFF;
static int sensor_range_mm = 500;
static uint16_t sensor_distance_mm = 0;
static unsigned long last_sensor_ms = 0;

static bool has(const char *line, const char *needle) { return strstr(line, needle) != nullptr; }

static bool number_after(const char *line, const char *key, float *out) {
  const char *at = strstr(line, key);
  if (!at) return false;
  at = strchr(at, ':');
  if (!at) return false;
  *out = strtof(at + 1, nullptr);
  return true;
}

static bool integer_after(const char *line, const char *key, int *out) {
  float value = 0;
  if (!number_after(line, key, &value)) return false;
  *out = (int)value;
  return true;
}

static bool string_after(const char *line, const char *key, String *out) {
  const char *at = strstr(line, key);
  if (!at) return false;
  at = strchr(at, ':');
  if (!at) return false;
  at = strchr(at, '\"');
  if (!at) return false;
  const char *end = strchr(at + 1, '\"');
  if (!end) return false;
  *out = String(at + 1).substring(0, end - at - 1);
  return true;
}

static void save_osc() {
  preferences.putString("osc_host", osc_host);
  preferences.putInt("osc_port", osc_port);
  preferences.putString("osc_addr", osc_address);
  WiFi.hostByName(osc_host.c_str(), osc_ip);
}

static void send_osc_value(const String &address, float value) {
  if (WiFi.status() != WL_CONNECTED || osc_host.isEmpty()) return;
  uint8_t packet[128] = {0};
  size_t offset = 0;
  auto append_string = [&](const String &value) {
    size_t n = value.length() + 1;
    size_t padded = (n + 3) & ~((size_t)3);
    if (offset + padded > sizeof(packet)) return false;
    memcpy(packet + offset, value.c_str(), value.length());
    offset += padded;
    return true;
  };
  if (!append_string(address) || !append_string(",f")) return;
  if (offset + 4 > sizeof(packet)) return;
  memcpy(packet + offset, &value, 4);
  uint8_t *b = packet + offset;
  uint8_t t = b[0]; b[0] = b[3]; b[3] = t;
  t = b[1]; b[1] = b[2]; b[2] = t;
  offset += 4;
  osc_udp.beginPacket(osc_ip, osc_port);
  osc_udp.write(packet, offset);
  osc_udp.endPacket();
}

static void send_osc(float normalized) { send_osc_value(osc_address, normalized); }

static void send_lock() {
  send_osc_value(osc_address + "/lock", locked ? 1.0f : 0.0f);
  Serial.printf("{\"type\":\"lock\",\"locked\":%s}\n", locked ? "true" : "false");
}

static void configure_osc(const char *line) {
  String host, address;
  float port = 0;
  if (!string_after(line, "\"host\"", &host) || !number_after(line, "\"port\"", &port) || !string_after(line, "\"address\"", &address)) return;
  osc_host = host;
  osc_port = (int)port;
  osc_address = address;
  save_osc();
  Serial.printf("{\"type\":\"osc_ack\",\"host\":\"%s\",\"port\":%d,\"address\":\"%s\"}\n", osc_host.c_str(), osc_port, osc_address.c_str());
}

static void handle_osc_http() {
  String body = web_server.arg("plain");
  configure_osc(body.c_str());
  web_server.send(200, "application/json", "{\"ok\":true}");
}

static void handle_status_http() {
  String body = String("{\"ip\":\"") + WiFi.localIP().toString() + "\",\"ssid\":\"" + wifi_ssid + "\",\"osc_address\":\"" + osc_address + "\",\"hue_raw\":" + String(hue_raw, 4) + ",\"hue_locked\":" + (locked ? "true" : "false") + ",\"sensor_ready\":" + (sensor_ready ? "true" : "false") + ",\"sensor_i2c_status\":" + String(sensor_i2c_status) + ",\"sensor_distance_mm\":" + String(sensor_distance_mm) + ",\"sensor_range_mm\":" + String(sensor_range_mm) + "}";
  web_server.send(200, "application/json", body);
}

static void emit_wifi_status(const char *state) {
  Serial.printf("{\"type\":\"wifi_status\",\"state\":\"%s\",\"ssid\":\"%s\",\"ip\":\"%s\"}\n", state, wifi_ssid.c_str(), WiFi.localIP().toString().c_str());
}

static void connect_wifi() {
  if (wifi_ssid.isEmpty()) return;
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifi_ssid.c_str(), wifi_secret.c_str());
  emit_wifi_status("connecting");
  const unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) delay(100);
  emit_wifi_status(WiFi.status() == WL_CONNECTED ? "connected" : "failed");
  if (WiFi.status() == WL_CONNECTED) {
    MDNS.end();
    MDNS.begin(DEVICE_HOSTNAME);
    MDNS.addService("http", "tcp", 80);
  }
}

static void configure_wifi(const char *line) {
  String ssid, secret;
  if (!string_after(line, "\"ssid\"", &ssid) || !string_after(line, "\"secret\"", &secret) || ssid.isEmpty()) {
    emit_wifi_status("invalid");
    return;
  }
  wifi_ssid = ssid;
  wifi_secret = secret;
  preferences.putString("ssid", wifi_ssid);
  preferences.putString("secret", wifi_secret);
  WiFi.disconnect(true, true);
  connect_wifi();
  if (WiFi.status() == WL_CONNECTED && !web_started) {
    web_server.begin();
    web_started = true;
  }
}

static void set_indicator(float raw) {
  const uint8_t red = (uint8_t)(255.0f * (1.0f - raw / 2.0f));
  const uint8_t green = (uint8_t)(255.0f * (raw / 2.0f));
  neopixelWrite(STATUS_LED_GPIO, red, green, 0);
}

static void emit_hue(bool announce = true) {
  if (hue_raw < 0) hue_raw = 0;
  if (hue_raw > 2) hue_raw = 2;
  if (announce) Serial.printf("{\"type\":\"hue\",\"raw\":%.6f,\"normalized\":%.6f}\n", hue_raw, hue_raw / 2.0f);
  set_indicator(hue_raw);
  send_osc(hue_raw / 2.0f);
}

static void poll_encoder() {
  const int current = (digitalRead(ENCODER_A_GPIO) << 1) | digitalRead(ENCODER_B_GPIO);
  if (current == encoder_state) return;
  const int transition = (encoder_state << 2) | current;
  static const int8_t table[16] = {0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0};
  encoder_accum += table[transition & 0x0f];
  encoder_state = current;
  if (encoder_accum >= 4 || encoder_accum <= -4) {
    sensor_range_mm += encoder_accum > 0 ? SENSOR_RANGE_STEP_MM : -SENSOR_RANGE_STEP_MM;
    sensor_range_mm = constrain(sensor_range_mm, SENSOR_MIN_RANGE_MM, SENSOR_MAX_RANGE_MM);
    encoder_accum = 0;
    Serial.printf("{\"type\":\"sensor_range\",\"range_mm\":%d}\n", sensor_range_mm);
  }
}

static void poll_button() {
  const bool pressed = digitalRead(ENCODER_SW_GPIO) == LOW;
  if (pressed != last_button && millis() - last_button_ms > 30) {
    last_button_ms = millis();
    last_button = pressed;
    if (pressed) {
      locked = !locked;
      send_lock();
    }
  }
}

static void poll_distance_sensor() {
  if (!sensor_ready || millis() - last_sensor_ms < 60) return;
  last_sensor_ms = millis();
  const uint16_t distance = distance_sensor.read();
  if (distance_sensor.timeoutOccurred()) return;
  sensor_distance_mm = distance;
  if (distance == 0 || distance > (uint16_t)sensor_range_mm || locked) return;
  const float next_hue = constrain((2.0f * (float)distance) / (float)sensor_range_mm, 0.0f, 2.0f);
  if (fabsf(next_hue - hue_raw) >= 0.005f) {
    hue_raw = next_hue;
    emit_hue(false);
  }
}

static void handle_line(const char *line) {
  float value = 0;
  if (has(line, "\"type\":\"hue\"")) {
    if (number_after(line, "\"raw\"", &value)) hue_raw = value;
    else if (number_after(line, "\"normalized\"", &value)) hue_raw = value * 2.0f;
    emit_hue();
    return;
  }
  if (has(line, "\"type\":\"test\"")) {
    test_enabled = has(line, "\"enabled\":true");
    Serial.printf("{\"type\":\"ack\",\"test\":%s}\n", test_enabled ? "true" : "false");
    return;
  }
  if (has(line, "\"type\":\"config\"")) {
    integer_after(line, "\"hold_timeout_ms\"", &hold_timeout_ms);
    integer_after(line, "\"pullaway_speed_mm_s\"", &pullaway_speed_mm_s);
    Serial.printf("{\"type\":\"config_ack\",\"hold_timeout_ms\":%d,\"pullaway_speed_mm_s\":%d}\n", hold_timeout_ms, pullaway_speed_mm_s);
    return;
  }
  if (has(line, "\"type\":\"wifi\"")) {
    configure_wifi(line);
    return;
  }
  if (has(line, "\"type\":\"osc\"")) {
    configure_osc(line);
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(STATUS_LED_GPIO, OUTPUT);
  pinMode(ENCODER_A_GPIO, INPUT_PULLUP);
  pinMode(ENCODER_B_GPIO, INPUT_PULLUP);
  pinMode(ENCODER_SW_GPIO, INPUT_PULLUP);
  Wire.begin(SENSOR_SDA_GPIO, SENSOR_SCL_GPIO, 100000);
  delay(10);
  Wire.beginTransmission(0x29);
  sensor_i2c_status = Wire.endTransmission();
  if (sensor_i2c_status == 0) {
    distance_sensor.setTimeout(500);
    sensor_ready = distance_sensor.init();
    if (sensor_ready) {
      distance_sensor.setDistanceMode(VL53L1X::Long);
      distance_sensor.setMeasurementTimingBudget(50000);
      distance_sensor.startContinuous(60);
    }
  }
  encoder_state = (digitalRead(ENCODER_A_GPIO) << 1) | digitalRead(ENCODER_B_GPIO);
  neopixelWrite(STATUS_LED_GPIO, 0, 0, 8);
  preferences.begin("hue", false);
  wifi_ssid = preferences.getString("ssid", "");
  wifi_secret = preferences.getString("secret", "");
  osc_host = preferences.getString("osc_host", osc_host);
  osc_port = preferences.getInt("osc_port", osc_port);
  osc_address = preferences.getString("osc_addr", osc_address);
  save_osc();
  delay(500);
  Serial.printf("{\"type\":\"hello\",\"device\":\"esp32-s3-hue-vl53l1x\",\"sensor_ready\":%s,\"sda\":%d,\"scl\":%d}\n", sensor_ready ? "true" : "false", SENSOR_SDA_GPIO, SENSOR_SCL_GPIO);
  connect_wifi();
  web_server.on("/api/osc", HTTP_POST, handle_osc_http);
  web_server.on("/api/status", HTTP_GET, handle_status_http);
  if (WiFi.status() == WL_CONNECTED) {
    web_server.begin();
    web_started = true;
  }
}

void loop() {
  if (web_started) web_server.handleClient();
  if (WiFi.status() == WL_CONNECTED && millis() - last_wifi_status_ms > 10000) {
    last_wifi_status_ms = millis();
    emit_wifi_status("connected");
  }
  poll_encoder();
  poll_button();
  poll_distance_sensor();
  while (Serial.available()) {
    const char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (line_length) {
        line_buffer[line_length] = '\0';
        handle_line(line_buffer);
        line_length = 0;
      }
    } else if (line_length < sizeof(line_buffer) - 1) line_buffer[line_length++] = c;
    else line_length = 0;
  }
  if (test_enabled && millis() - last_test_ms >= TEST_PERIOD_MS) {
    last_test_ms = millis();
    hue_raw += 0.25f;
    if (hue_raw > 2.0f) hue_raw = 0.0f;
    emit_hue();
  }
}
