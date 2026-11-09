#include <Arduino.h>
#include <HardwareSerial.h>
#include <math.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <WebServer.h>
#include <ESPmDNS.h>

#ifndef DEVICE_HOSTNAME
#define DEVICE_HOSTNAME "crowdcontroller"
#endif

static constexpr int STATUS_LED_GPIO = 48;
static constexpr int ENCODER_A_GPIO = 4;
static constexpr int ENCODER_B_GPIO = 5;
static constexpr int ENCODER_SW_GPIO = 6;
static constexpr int RADAR_OUT_GPIO = 7;
static constexpr int PRESENCE_LED_GPIO = 12;
// The board headers labeled RX/TX are GPIO44/GPIO43 (UART0 RX/TX).
static constexpr int RADAR_RX_GPIO = 44;
static constexpr int RADAR_TX_GPIO = 43;
static constexpr uint32_t RADAR_BAUD = 256000;
static constexpr unsigned long TEST_PERIOD_MS = 1000;
static char line_buffer[256];
static size_t line_length = 0;
static float crowd_distance_mm = 500.0f;
static bool test_enabled = false;
static unsigned long last_test_ms = 0;
static int hold_timeout_ms = 400;
static int pullaway_speed_mm_s = 800;
static Preferences preferences;
static String wifi_ssid;
static String wifi_secret;
static String osc_host = "192.168.1.100";
static int osc_port = 6000;
static String osc_address = "/controls/global/slider/1";
static WiFiUDP osc_udp;
static HardwareSerial radar_serial(2);
static IPAddress osc_ip;
static WebServer web_server(80);
static bool web_started = false;
static unsigned long last_wifi_status_ms = 0;
static int encoder_state = 0;
static int encoder_accum = 0;
static bool locked = false;
static bool last_button = true;
static unsigned long last_button_ms = 0;
static bool last_presence = false;
static unsigned long last_indicator_ms = 0;
static bool indicator_output_on = true;

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
  if (WiFi.status() == WL_CONNECTED) {
    WiFi.hostByName(osc_host.c_str(), osc_ip);
  }
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

static void radar_command(const uint8_t *data, size_t length) {
  radar_serial.write(data, length);
  radar_serial.flush();
  delay(30);
  while (radar_serial.available()) radar_serial.read();
}

static uint8_t radar_gate_for_distance(float distance_mm) {
  const int gate = (int)((distance_mm + 375.0f) / 750.0f);
  return (uint8_t)constrain(gate, 2, 8);
}

static void apply_radar_range() {
  const uint8_t gate = radar_gate_for_distance(crowd_distance_mm);
  const uint8_t enter_config[] = {
    0xFD, 0xFC, 0xFB, 0xFA, 0x04, 0x00,
    0xFF, 0x00, 0x01, 0x00, 0x04, 0x03, 0x02, 0x01
  };
  const uint8_t set_range[] = {
    0xFD, 0xFC, 0xFB, 0xFA, 0x14, 0x00,
    0x60, 0x00,
    0x00, 0x00, gate, 0x00, 0x00, 0x00,
    0x01, 0x00, gate, 0x00, 0x00, 0x00,
    0x02, 0x00, 0x05, 0x00, 0x00, 0x00,
    0x04, 0x03, 0x02, 0x01
  };
  const uint8_t exit_config[] = {
    0xFD, 0xFC, 0xFB, 0xFA, 0x02, 0x00,
    0xFE, 0x00, 0x04, 0x03, 0x02, 0x01
  };
  radar_command(enter_config, sizeof(enter_config));
  radar_command(set_range, sizeof(set_range));
  radar_command(exit_config, sizeof(exit_config));
  Serial.printf("{\"type\":\"radar_range\",\"distance_mm\":%.1f,\"gate\":%u}\n", crowd_distance_mm, gate);
}

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
  String body = String("{\"ip\":\"") + WiFi.localIP().toString() + "\",\"ssid\":\"" + wifi_ssid + "\",\"osc_address\":\"" + osc_address + "\",\"distance_mm\":" + String(crowd_distance_mm, 1) + ",\"presence\":" + (last_presence ? "true" : "false") + "}";
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

static float crowd_normalized() {
  return (crowd_distance_mm - 500.0f) / 3500.0f;
}

static void set_indicator_rgb(uint8_t brightness, uint8_t red, uint8_t green, uint8_t blue, bool enabled) {
  if (!enabled) {
    red = green = blue = 0;
  } else if (brightness != 255) {
    red = (uint8_t)((red * brightness) / 255);
    green = (uint8_t)((green * brightness) / 255);
    blue = (uint8_t)((blue * brightness) / 255);
  }
  neopixelWrite(STATUS_LED_GPIO, red, green, blue);
}

static void refresh_indicator() {
  const unsigned long now = millis();
  if (locked) {
    indicator_output_on = true;
  } else if (now - last_indicator_ms >= 500) {
    last_indicator_ms = now;
    indicator_output_on = !indicator_output_on;
  }
  if (locked) {
    // Locked: bright=25, red=255, green=0, blue=0.
    set_indicator_rgb(25, 255, 0, 0, true);
  } else if (last_presence) {
    // Unlocked + presence: bright=255, red=255, green=0, blue=255.
    set_indicator_rgb(255, 255, 0, 255, indicator_output_on);
  } else {
    // Unlocked + no presence: bright=255, red=0, green=0, blue=255.
    set_indicator_rgb(255, 0, 0, 255, indicator_output_on);
  }
}

static void emit_crowd_config() {
  if (crowd_distance_mm < 500) crowd_distance_mm = 500;
  if (crowd_distance_mm > 4000) crowd_distance_mm = 4000;
  const float normalized = crowd_normalized();
  Serial.printf("{\"type\":\"crowd_config\",\"distance_mm\":%.1f,\"normalized\":%.6f}\n", crowd_distance_mm, normalized);
  apply_radar_range();
  refresh_indicator();
  send_osc(normalized);
}

static void poll_encoder() {
  const int current = (digitalRead(ENCODER_A_GPIO) << 1) | digitalRead(ENCODER_B_GPIO);
  if (current == encoder_state) return;
  const int transition = (encoder_state << 2) | current;
  static const int8_t table[16] = {0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0};
  encoder_accum += table[transition & 0x0f];
  encoder_state = current;
  if (locked) return;
  if (encoder_accum >= 4 || encoder_accum <= -4) {
    crowd_distance_mm += encoder_accum > 0 ? 50.0f : -50.0f;
    encoder_accum = 0;
    emit_crowd_config();
  }
}

static void poll_button() {
  const bool pressed = digitalRead(ENCODER_SW_GPIO) == LOW;
  if (pressed != last_button && millis() - last_button_ms > 30) {
    last_button_ms = millis();
    last_button = pressed;
    if (pressed) {
      locked = !locked;
      indicator_output_on = true;
      last_indicator_ms = millis();
      refresh_indicator();
      send_lock();
    }
  }
}

static void poll_presence() {
  const bool present = digitalRead(RADAR_OUT_GPIO) == HIGH;
  digitalWrite(PRESENCE_LED_GPIO, present ? HIGH : LOW);
  if (present != last_presence) {
    last_presence = present;
    Serial.printf("{\"type\":\"presence\",\"present\":%s}\n", present ? "true" : "false");
  }
  refresh_indicator();
}

static void handle_line(const char *line) {
  float value = 0;
  if (has(line, "\"type\":\"crowd_config\"")) {
    if (number_after(line, "\"distance_mm\"", &value)) crowd_distance_mm = value;
    emit_crowd_config();
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
  pinMode(STATUS_LED_GPIO, OUTPUT);
  pinMode(ENCODER_A_GPIO, INPUT_PULLUP);
  pinMode(ENCODER_B_GPIO, INPUT_PULLUP);
  pinMode(ENCODER_SW_GPIO, INPUT_PULLUP);
  pinMode(RADAR_OUT_GPIO, INPUT_PULLDOWN);
  radar_serial.begin(RADAR_BAUD, SERIAL_8N1, RADAR_RX_GPIO, RADAR_TX_GPIO);
  pinMode(PRESENCE_LED_GPIO, OUTPUT);
  digitalWrite(PRESENCE_LED_GPIO, LOW);
  encoder_state = (digitalRead(ENCODER_A_GPIO) << 1) | digitalRead(ENCODER_B_GPIO);
  neopixelWrite(STATUS_LED_GPIO, 0, 0, 8);
  delay(100);
  apply_radar_range();
  Serial.begin(115200);
  preferences.begin("hue", false);
  wifi_ssid = preferences.getString("ssid", "");
  wifi_secret = preferences.getString("secret", "");
  osc_host = preferences.getString("osc_host", osc_host);
  osc_port = preferences.getInt("osc_port", osc_port);
  osc_address = preferences.getString("osc_addr", osc_address);
  save_osc();
  delay(500);
  Serial.println("{\"type\":\"hello\",\"device\":\"esp32-s3-crowd-prep\",\"sensors\":\"unassigned\",\"gpio48\":\"status\"}");
  connect_wifi();
  web_server.on("/api/osc", HTTP_POST, handle_osc_http);
  web_server.on("/api/status", HTTP_GET, handle_status_http);
  if (WiFi.status() == WL_CONNECTED) {
    web_server.begin();
    web_started = true;
  }
}

void loop() {
  if (WiFi.status() == WL_CONNECTED) web_server.handleClient();
  if (WiFi.status() == WL_CONNECTED && millis() - last_wifi_status_ms > 10000) {
    last_wifi_status_ms = millis();
    emit_wifi_status("connected");
  }
  poll_encoder();
  poll_button();
  poll_presence();
  refresh_indicator();
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
    crowd_distance_mm += 0.25f;
    if (crowd_distance_mm > 4000.0f) crowd_distance_mm = 500.0f;
    emit_crowd_config();
  }
}

