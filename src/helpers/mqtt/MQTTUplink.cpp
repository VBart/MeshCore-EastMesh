#include "MQTTUplink.h"
#include "MQTTCaCerts.h"

#ifdef WITH_MQTT_UPLINK

#if defined(ESP_PLATFORM)

#include <helpers/ESP32Board.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_idf_version.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <esp_sntp.h>
#include <helpers/TxtDataHelpers.h>
#include <ctype.h>
#include <string.h>
#include <time.h>

#ifndef FIRMWARE_VERSION
  #define FIRMWARE_VERSION "v1.14.1"
#endif

#ifndef FIRMWARE_BUILD_DATE
  #define FIRMWARE_BUILD_DATE "20 Mar 2026"
#endif

#ifndef CLIENT_VERSION
  #define CLIENT_VERSION "eastmesh-repeater-mqtt"
#endif

#ifndef MQTT_DEBUG
  #define MQTT_DEBUG 0
#endif

#if MQTT_DEBUG
  #define LOG_CAT(tag, fmt, ...) Serial.printf("[" tag "] " fmt "\n", ##__VA_ARGS__)
  #define WIFI_LOG(fmt, ...) LOG_CAT("WIFI", fmt, ##__VA_ARGS__)
  #define WEB_LOG(fmt, ...) LOG_CAT("WEB", fmt, ##__VA_ARGS__)
  #define TIME_LOG(fmt, ...) LOG_CAT("TIME", fmt, ##__VA_ARGS__)
  #define MQTT_LOG(fmt, ...) LOG_CAT("MQTT", fmt, ##__VA_ARGS__)
#else
  #define LOG_CAT(...) do { } while (0)
  #define WIFI_LOG(...) do { } while (0)
  #define WEB_LOG(...) do { } while (0)
  #define TIME_LOG(...) do { } while (0)
  #define MQTT_LOG(...) do { } while (0)
#endif

namespace {
constexpr unsigned long kWifiRetryMillis = 15000;
constexpr unsigned long kWifiConnectTimeoutMillis = 45000;
constexpr unsigned long kBrokerRetryMillis = 10000;
constexpr time_t kTokenLifetimeSecs = 3600;
constexpr time_t kTokenRefreshSlackSecs = 300;
constexpr time_t kMinSaneEpoch = 1735689600;  // 2025-01-01T00:00:00Z

int getWifiQualityPercent(int rssi_dbm) {
  if (rssi_dbm <= -100) {
    return 0;
  }
  if (rssi_dbm >= -50) {
    return 100;
  }
  return 2 * (rssi_dbm + 100);
}

const char* getWifiQualityLabel(int rssi_dbm) {
  if (rssi_dbm >= -60) {
    return "excellent";
  }
  if (rssi_dbm >= -67) {
    return "good";
  }
  if (rssi_dbm >= -75) {
    return "fair";
  }
  return "poor";
}

char* allocScratchBuffer(size_t size) {
  void* ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (ptr == nullptr) {
    ptr = heap_caps_malloc(size, MALLOC_CAP_8BIT);
  }
  return static_cast<char*>(ptr);
}

void freeScratchBuffer(void* ptr) {
  if (ptr != nullptr) {
    heap_caps_free(ptr);
  }
}

const char* getWifiStateLabel(const MQTTPrefs& prefs, bool wifi_started) {
  if (prefs.wifi_ssid[0] == 0) {
    return "off";
  }
  wl_status_t status = WiFi.status();
  if (status == WL_CONNECTED) {
    return "up";
  }
  if (wifi_started) {
    return "conn";
  }
  return "down";
}

}

const MQTTUplink::BrokerSpec MQTTUplink::kBrokerSpecs[3] = {
    {"eastmesh-au", "eastmesh-au", "mqtt2.eastmesh.au", "wss://mqtt2.eastmesh.au:443/mqtt", kEastmeshBit},
    {"letsmesh-eu", "letsmesh-eu", "mqtt-eu-v1.letsmesh.net", "wss://mqtt-eu-v1.letsmesh.net:443/mqtt",
     kLetsmeshEuBit},
    {"letsmesh-us", "letsmesh-us", "mqtt-us-v1.letsmesh.net", "wss://mqtt-us-v1.letsmesh.net:443/mqtt",
     kLetsmeshUsBit},
};

MQTTUplink::MQTTUplink(mesh::RTCClock& rtc, mesh::LocalIdentity& identity)
    : _fs(nullptr), _rtc(&rtc), _identity(&identity), _running(false), _wifi_started(false), _sntp_started(false),
      _have_time_sync(false), _last_wifi_attempt(0), _last_status_publish(0), _last_status{},
      _node_name(nullptr),
      _web_runner(nullptr)
       {
  memset(_device_id, 0, sizeof(_device_id));
  MQTTPrefsStore::setDefaults(_prefs);
  for (size_t i = 0; i < 3; ++i) {
    memset(&_brokers[i], 0, sizeof(_brokers[i]));
    _brokers[i].spec = &kBrokerSpecs[i];
  }
  MQTT_LOG("uplink init");
}

void MQTTUplink::setWebCommandRunner(MQTTWebCommandRunner* runner) {
  _web_runner = runner;
  _web_panel.setCommandRunner(runner);
}

bool MQTTUplink::savePrefs() {
  return MQTTPrefsStore::save(_fs, _prefs);
}

bool MQTTUplink::hasEnabledBroker() const {
  return (_prefs.enabled_mask & 0x07) != 0;
}

bool MQTTUplink::isActive() const {
  return _running && hasEnabledBroker();
}

void MQTTUplink::reconnectWifi() {
  WIFI_LOG("reset");
  stopWebServer();
  for (BrokerState& broker : _brokers) {
    destroyBroker(broker);
  }
  if (_wifi_started) {
    WiFi.disconnect(true, true);
    WiFi.mode(WIFI_OFF);
  }
  _wifi_started = false;
  _sntp_started = false;
  _have_time_sync = false;
  _last_wifi_attempt = 0;
}

bool MQTTUplink::sendStatusNow() {
  if (!_running || !_have_time_sync || WiFi.status() != WL_CONNECTED) {
    return false;
  }

  bool any_connected = false;
  for (const BrokerState& broker : _brokers) {
    if ((_prefs.enabled_mask & broker.spec->bit) != 0 && broker.connected && broker.client != nullptr) {
      any_connected = true;
      break;
    }
  }
  if (!any_connected) {
    return false;
  }

  publishStatus(true);
  _last_status_publish = millis();
  return true;
}

void MQTTUplink::makeSafeToken(const char* input, char* output, size_t output_size) {
  if (output_size == 0) {
    return;
  }
  size_t oi = 0;
  for (size_t i = 0; input != nullptr && input[i] != 0 && oi + 1 < output_size; ++i) {
    char c = input[i];
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_') {
      output[oi++] = c;
    } else {
      output[oi++] = '_';
    }
  }
  output[oi] = 0;
}

void MQTTUplink::bytesToHexUpper(const uint8_t* src, size_t len, char* dst, size_t dst_size) {
  if (dst_size == 0) {
    return;
  }
  size_t di = 0;
  for (size_t i = 0; i < len && di + 2 < dst_size; ++i) {
    snprintf(&dst[di], dst_size - di, "%02X", src[i]);
    di += 2;
  }
  dst[min(di, dst_size - 1)] = 0;
}

void MQTTUplink::formatIsoTimestamp(time_t ts, char* dst, size_t dst_size) {
  if (dst_size == 0) {
    return;
  }
  struct tm tm_local;
  localtime_r(&ts, &tm_local);
  strftime(dst, dst_size, "%Y-%m-%dT%H:%M:%S", &tm_local);
  size_t len = strlen(dst);
  if (len + 8 < dst_size) {
    memcpy(&dst[len], ".000000", 8);
  }
}

void MQTTUplink::escapeJsonString(const char* input, char* output, size_t output_size) {
  if (output_size == 0) {
    return;
  }

  size_t oi = 0;
  for (size_t i = 0; input != nullptr && input[i] != 0 && oi + 1 < output_size; ++i) {
    char c = input[i];
    const char* escape = nullptr;
    switch (c) {
      case '\"':
        escape = "\\\"";
        break;
      case '\\':
        escape = "\\\\";
        break;
      case '\n':
        escape = "\\n";
        break;
      case '\r':
        escape = "\\r";
        break;
      case '\t':
        escape = "\\t";
        break;
      default:
        break;
    }

    if (escape != nullptr) {
      for (size_t j = 0; escape[j] != 0 && oi + 1 < output_size; ++j) {
        output[oi++] = escape[j];
      }
      continue;
    }

    unsigned char uc = static_cast<unsigned char>(c);
    if (uc < 0x20) {
      output[oi++] = '_';
      continue;
    }
    output[oi++] = c;
  }
  output[oi] = 0;
}

wifi_ps_type_t MQTTUplink::toEspPowerSave(uint8_t mode) {
  switch (mode) {
    case 1:
      return WIFI_PS_MIN_MODEM;
    case 2:
      return WIFI_PS_MAX_MODEM;
    default:
      return WIFI_PS_NONE;
  }
}

const char* MQTTUplink::getPowerSaveLabel(uint8_t mode) {
  switch (mode) {
    case 1:
      return "min";
    case 2:
      return "max";
    default:
      return "none";
  }
}

void MQTTUplink::refreshIdentityStrings() {
  bytesToHexUpper(_identity->pub_key, PUB_KEY_SIZE, _device_id, sizeof(_device_id));
  for (size_t i = 0; i < 3; ++i) {
    BrokerState& broker = _brokers[i];
    snprintf(broker.username, sizeof(broker.username), "v1_%s", _device_id);
    snprintf(broker.client_id, sizeof(broker.client_id), "mqtt_%s-%.6s", broker.spec->key, _device_id);
    snprintf(broker.status_topic, sizeof(broker.status_topic), "meshcore/%s/%s/status", _prefs.iata, _device_id);
    snprintf(broker.packets_topic, sizeof(broker.packets_topic), "meshcore/%s/%s/packets", _prefs.iata, _device_id);
    snprintf(broker.raw_topic, sizeof(broker.raw_topic), "meshcore/%s/%s/raw", _prefs.iata, _device_id);
  }
}

void MQTTUplink::refreshBrokerState(BrokerState& broker) {
  char safe_name[40];
  makeSafeToken(board.getManufacturerName(), safe_name, sizeof(safe_name));
  char origin[80];
  const char* node_name = (_node_name != nullptr && _node_name[0] != 0) ? _node_name : _device_id;
  escapeJsonString(node_name, origin, sizeof(origin));
  char client_version[96];
  snprintf(client_version, sizeof(client_version), "%s", CLIENT_VERSION);
  char radio_info[48];
  snprintf(radio_info, sizeof(radio_info), "%.6f,%.1f,%u,%u", static_cast<double>(_last_status.radio_freq),
           static_cast<double>(_last_status.radio_bw), _last_status.radio_sf, _last_status.radio_cr);

  char ts[32];
  formatIsoTimestamp(time(nullptr), ts, sizeof(ts));
  snprintf(broker.offline_payload, sizeof(broker.offline_payload),
           "{\"status\":\"offline\",\"timestamp\":\"%s\",\"origin\":\"%s\",\"origin_id\":\"%s\",\"model\":\"%s\","
           "\"firmware_version\":\"%s\",\"radio\":\"%s\",\"client_version\":\"%s\"}",
           ts, origin, _device_id, safe_name, FIRMWARE_VERSION, radio_info, client_version);
}

bool MQTTUplink::refreshToken(BrokerState& broker) {
  time_t now = time(nullptr);
  if (now < kMinSaneEpoch) {
    TIME_LOG("%s token skipped: clock not ready (%lu)", broker.spec->label, static_cast<unsigned long>(now));
    return false;
  }

  time_t expires_at = now + kTokenLifetimeSecs;
  const char* owner = _prefs.owner_public_key[0] ? _prefs.owner_public_key : nullptr;
  const char* email = _prefs.owner_email[0] ? _prefs.owner_email : nullptr;
  if (!JWTHelper::createAuthToken(*_identity, broker.spec->host, now, expires_at, broker.token, sizeof(broker.token),
                                  owner, email)) {
    MQTT_LOG("%s token creation failed", broker.spec->label);
    return false;
  }
  broker.token_expires_at = expires_at;
  MQTT_LOG("%s token ready exp=%lu owner=%s email=%s", broker.spec->label,
           static_cast<unsigned long>(expires_at), owner != nullptr ? "yes" : "no", email != nullptr ? "yes" : "no");
  return true;
}

void MQTTUplink::destroyBroker(BrokerState& broker) {
  if (broker.client != nullptr) {
    MQTT_LOG("%s destroy broker client", broker.spec->label);
    esp_mqtt_client_stop(broker.client);
    esp_mqtt_client_destroy(broker.client);
    broker.client = nullptr;
  }
  broker.connected = false;
  broker.connect_announced = false;
}

void MQTTUplink::queuePublish(BrokerState& broker, const char* topic, const char* payload, bool retain) {
  if (broker.client == nullptr || !broker.connected) {
    return;
  }
  MQTT_LOG("%s publish topic=%s retain=%d bytes=%u", broker.spec->label, topic, retain ? 1 : 0,
           static_cast<unsigned>(strlen(payload)));
  esp_mqtt_client_enqueue(broker.client, topic, payload, 0, 1, retain, true);
}

int MQTTUplink::buildStatusJson(char* buffer, size_t buffer_size, bool online) const {
  char ts[32];
  formatIsoTimestamp(time(nullptr), ts, sizeof(ts));
  char model[48];
  makeSafeToken(board.getManufacturerName(), model, sizeof(model));
  char origin[80];
  const char* node_name = (_node_name != nullptr && _node_name[0] != 0) ? _node_name : _device_id;
  escapeJsonString(node_name, origin, sizeof(origin));
  char client_version[96];
  snprintf(client_version, sizeof(client_version), "%s", CLIENT_VERSION);
  char radio_info[48];
  snprintf(radio_info, sizeof(radio_info), "%.6f,%.1f,%u,%u", static_cast<double>(_last_status.radio_freq),
           static_cast<double>(_last_status.radio_bw), _last_status.radio_sf, _last_status.radio_cr);
  return snprintf(buffer, buffer_size,
                  "{\"status\":\"%s\",\"timestamp\":\"%s\",\"origin\":\"%s\",\"origin_id\":\"%s\","
                  "\"model\":\"%s\",\"firmware_version\":\"%s\",\"radio\":\"%s\",\"client_version\":\"%s\","
                  "\"stats\":{\"battery_mv\":%d,\"uptime_secs\":%lu,\"errors\":%u,\"queue_len\":%u,"
                  "\"noise_floor\":%d,\"tx_air_secs\":%lu,\"rx_air_secs\":%lu,\"recv_errors\":%lu}}",
                  online ? "online" : "offline", ts, origin, _device_id, model, FIRMWARE_VERSION, radio_info, client_version,
                  _last_status.battery_mv, static_cast<unsigned long>(_last_status.uptime_secs), _last_status.error_flags,
                  _last_status.queue_len, _last_status.noise_floor, static_cast<unsigned long>(_last_status.tx_air_secs),
                  static_cast<unsigned long>(_last_status.rx_air_secs),
                  static_cast<unsigned long>(_last_status.recv_errors));
}

int MQTTUplink::buildPacketJson(char* buffer, size_t buffer_size, const mesh::Packet& packet, bool is_tx, int rssi,
                                float snr) const {
  uint8_t raw[256];
  int raw_len = packet.writeTo(raw);
  char* raw_hex = allocScratchBuffer(520);
  if (raw_hex == nullptr) {
    return -1;
  }
  bytesToHexUpper(raw, raw_len, raw_hex, 520);

  uint8_t packet_hash[MAX_HASH_SIZE];
  packet.calculatePacketHash(packet_hash);
  char hash_hex[(MAX_HASH_SIZE * 2) + 1];
  bytesToHexUpper(packet_hash, MAX_HASH_SIZE, hash_hex, sizeof(hash_hex));

  time_t now = time(nullptr);
  char ts[32];
  formatIsoTimestamp(now, ts, sizeof(ts));
  struct tm tm_utc;
  gmtime_r(&now, &tm_utc);
  char time_only[16];
  char date_only[16];
  strftime(time_only, sizeof(time_only), "%H:%M:%S", &tm_utc);
  strftime(date_only, sizeof(date_only), "%d/%m/%Y", &tm_utc);
  char origin[80];
  const char* node_name = (_node_name != nullptr && _node_name[0] != 0) ? _node_name : _device_id;
  escapeJsonString(node_name, origin, sizeof(origin));
  if (packet.isRouteDirect() && packet.path_len > 0) {
    char path_info[128];
    snprintf(path_info, sizeof(path_info), "path_%dx%d_%db", (int)packet.getPathHashCount(),
             (int)packet.getPathHashSize(), (int)packet.getPathByteLen());
    int len = snprintf(buffer, buffer_size,
                       "{\"origin\":\"%s\",\"origin_id\":\"%s\",\"timestamp\":\"%s\",\"type\":\"PACKET\","
                       "\"direction\":\"%s\",\"time\":\"%s\",\"date\":\"%s\",\"len\":\"%d\",\"packet_type\":\"%u\","
                       "\"route\":\"D\",\"payload_len\":\"%u\",\"raw\":\"%s\",\"SNR\":\"%.1f\",\"RSSI\":\"%d\","
                       "\"hash\":\"%s\",\"path\":\"%s\"}",
                       origin, _device_id, ts, is_tx ? "tx" : "rx", time_only, date_only, raw_len, packet.getPayloadType(),
                       packet.payload_len, raw_hex, snr, rssi, hash_hex, path_info);
    freeScratchBuffer(raw_hex);
    return len;
  }

  int len = snprintf(buffer, buffer_size,
                     "{\"origin\":\"%s\",\"origin_id\":\"%s\",\"timestamp\":\"%s\",\"type\":\"PACKET\","
                     "\"direction\":\"%s\",\"time\":\"%s\",\"date\":\"%s\",\"len\":\"%d\",\"packet_type\":\"%u\","
                     "\"route\":\"F\",\"payload_len\":\"%u\",\"raw\":\"%s\",\"SNR\":\"%.1f\",\"RSSI\":\"%d\","
                     "\"hash\":\"%s\"}",
                     origin, _device_id, ts, is_tx ? "tx" : "rx", time_only, date_only, raw_len, packet.getPayloadType(),
                     packet.payload_len, raw_hex, snr, rssi, hash_hex);
  freeScratchBuffer(raw_hex);
  return len;
}

int MQTTUplink::buildRawJson(char* buffer, size_t buffer_size, const mesh::Packet& packet, bool is_tx, int rssi,
                             float snr) const {
  (void)is_tx;
  (void)rssi;
  (void)snr;
  uint8_t raw[256];
  int raw_len = packet.writeTo(raw);
  char* raw_hex = allocScratchBuffer(520);
  if (raw_hex == nullptr) {
    return -1;
  }
  bytesToHexUpper(raw, raw_len, raw_hex, 520);
  char ts[32];
  formatIsoTimestamp(time(nullptr), ts, sizeof(ts));
  char origin[80];
  const char* node_name = (_node_name != nullptr && _node_name[0] != 0) ? _node_name : _device_id;
  escapeJsonString(node_name, origin, sizeof(origin));

  int len = snprintf(buffer, buffer_size,
                     "{\"origin\":\"%s\",\"origin_id\":\"%s\",\"timestamp\":\"%s\",\"type\":\"RAW\",\"data\":\"%s\"}",
                     origin, _device_id, ts, raw_hex);
  freeScratchBuffer(raw_hex);
  return len;
}

void MQTTUplink::publishOnlineStatus(BrokerState& broker) {
  char* payload = allocScratchBuffer(768);
  if (payload == nullptr) {
    return;
  }
  int len = buildStatusJson(payload, 768, true);
  if (len > 0 && static_cast<size_t>(len) < 768) {
    queuePublish(broker, broker.status_topic, payload, true);
  }
  freeScratchBuffer(payload);
}

void MQTTUplink::publishStatus(bool online) {
  char* payload = allocScratchBuffer(768);
  if (payload == nullptr) {
    return;
  }
  int len = buildStatusJson(payload, 768, online);
  if (len <= 0 || static_cast<size_t>(len) >= 768) {
    freeScratchBuffer(payload);
    return;
  }
  for (BrokerState& broker : _brokers) {
    if ((_prefs.enabled_mask & broker.spec->bit) != 0) {
      queuePublish(broker, broker.status_topic, payload, true);
    }
  }
  freeScratchBuffer(payload);
}

void MQTTUplink::handleMqttEvent(void* handler_args, esp_event_base_t, int32_t event_id, void* event_data) {
  auto* broker = static_cast<BrokerState*>(handler_args);
  if (broker == nullptr) {
    return;
  }
  auto* event = static_cast<esp_mqtt_event_handle_t>(event_data);

  switch (event_id) {
    case MQTT_EVENT_CONNECTED:
      broker->connected = true;
      MQTT_LOG("%s connected", broker->spec->label);
      break;
    case MQTT_EVENT_DISCONNECTED:
      MQTT_LOG("%s disconnected", broker->spec->label);
    case MQTT_EVENT_ERROR:
      broker->connected = false;
      if (event_id == MQTT_EVENT_ERROR) {
        if (event != nullptr && event->error_handle != nullptr) {
          MQTT_LOG("%s error type=%d tls_esp=0x%x tls_stack=0x%x cert_flags=0x%x sock_errno=%d conn_refused=%d",
                   broker->spec->label, event->error_handle->error_type, event->error_handle->esp_tls_last_esp_err,
                   event->error_handle->esp_tls_stack_err, event->error_handle->esp_tls_cert_verify_flags,
                   event->error_handle->esp_transport_sock_errno, event->error_handle->connect_return_code);
        } else {
          MQTT_LOG("%s error event", broker->spec->label);
        }
      }
      break;
    case MQTT_EVENT_BEFORE_CONNECT:
      MQTT_LOG("%s before connect", broker->spec->label);
      break;
    default:
      break;
  }
}

void MQTTUplink::stopWebServer() {
  _web_panel.stop();
}

void MQTTUplink::ensureWebServer() {
  if (_web_runner == nullptr || _prefs.web_enabled == 0 || !_wifi_started || WiFi.status() != WL_CONNECTED) {
    stopWebServer();
    return;
  }
  _web_panel.start();
}

void MQTTUplink::ensureWifi() {
  if (_prefs.wifi_ssid[0] == 0) {
    WIFI_LOG("disabled: no ssid");
    stopWebServer();
    reconnectWifi();
    return;
  }

  if (!hasEnabledBroker() && _web_runner == nullptr) {
    WIFI_LOG("disabled: no mqtt endpoints and no web runner");
    stopWebServer();
    if (_wifi_started) {
      WiFi.disconnect(true, true);
      WiFi.mode(WIFI_OFF);
      _wifi_started = false;
      _sntp_started = false;
      _have_time_sync = false;
    }
    return;
  }

  if (WiFi.status() == WL_CONNECTED) {
    return;
  }

  unsigned long now_ms = millis();
  wl_status_t status = WiFi.status();
  if (_wifi_started) {
    if (_last_wifi_attempt != 0 && status == WL_IDLE_STATUS && now_ms - _last_wifi_attempt < kWifiConnectTimeoutMillis) {
      return;
    }
    if (now_ms - _last_wifi_attempt < kWifiRetryMillis) {
      return;
    }
  }

  if (!_wifi_started) {
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(toEspPowerSave(_prefs.wifi_powersave));
    _wifi_started = true;
    WIFI_LOG("sta start powersaving=%s", getPowerSaveLabel(_prefs.wifi_powersave));
  } else {
    WIFI_LOG("retry status=%d", static_cast<int>(status));
  }
  _last_wifi_attempt = now_ms;
  WIFI_LOG("begin ssid=%s", _prefs.wifi_ssid);
  WiFi.begin(_prefs.wifi_ssid, _prefs.wifi_pwd);
}

void MQTTUplink::updateTimeSync() {
  bool prev_have_time_sync = _have_time_sync;
  if (!_wifi_started || WiFi.status() != WL_CONNECTED) {
    _have_time_sync = false;
    if (prev_have_time_sync != _have_time_sync) {
      TIME_LOG("sntp lost");
    }
    return;
  }

  if (!_sntp_started) {
    configTzTime("UTC0", "au.pool.ntp.org", "time.google.com", "time.cloudflare.com");
    _sntp_started = true;
    TIME_LOG("sntp start servers=au.pool.ntp.org,time.google.com,time.cloudflare.com");
  }

  sntp_sync_status_t sync_status = sntp_get_sync_status();
  time_t now = time(nullptr);
  bool sane_time = now >= kMinSaneEpoch;
  bool sync_ready = sync_status == SNTP_SYNC_STATUS_COMPLETED || sync_status == SNTP_SYNC_STATUS_IN_PROGRESS;
  _have_time_sync = sane_time && (sync_ready || prev_have_time_sync);
  if (prev_have_time_sync != _have_time_sync) {
    TIME_LOG("sntp %s epoch=%lu status=%ld", _have_time_sync ? "ready" : "waiting",
             static_cast<unsigned long>(now), static_cast<long>(sync_status));
  }
}

void MQTTUplink::ensureBroker(BrokerState& broker) {
  bool enabled = (_prefs.enabled_mask & broker.spec->bit) != 0;
  if (!enabled) {
    destroyBroker(broker);
    return;
  }

  if (!_have_time_sync || WiFi.status() != WL_CONNECTED) {
    return;
  }

  time_t now = time(nullptr);
  if (broker.client != nullptr && broker.token_expires_at > 0 && now + kTokenRefreshSlackSecs >= broker.token_expires_at) {
    destroyBroker(broker);
  }

  if (broker.client != nullptr) {
    return;
  }

  unsigned long now_ms = millis();
  if (now_ms - broker.last_connect_attempt < kBrokerRetryMillis) {
    return;
  }
  broker.last_connect_attempt = now_ms;

  if (!refreshToken(broker)) {
    return;
  }

  refreshBrokerState(broker);
  MQTT_LOG("%s mqtt init host=%s port=%d path=%s client_id=%s heap_free=%u heap_max=%u", broker.spec->label,
           broker.spec->host, 443, "/mqtt", broker.client_id, ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  esp_mqtt_client_config_t cfg = {};
#if ESP_IDF_VERSION_MAJOR >= 5
  cfg.broker.address.hostname = broker.spec->host;
  cfg.broker.address.port = 443;
  cfg.broker.address.transport = MQTT_TRANSPORT_OVER_WSS;
  cfg.broker.address.path = "/mqtt";
  cfg.broker.verification.certificate = mqtt_ca_certs::kCombinedPem;
  cfg.credentials.username = broker.username;
  cfg.credentials.client_id = broker.client_id;
  cfg.credentials.authentication.password = broker.token;
  cfg.session.keepalive = 30;
  cfg.session.last_will.topic = broker.status_topic;
  cfg.session.last_will.msg = broker.offline_payload;
  cfg.session.last_will.qos = 1;
  cfg.session.last_will.retain = 1;
  cfg.network.reconnect_timeout_ms = 10000;
  cfg.network.timeout_ms = 10000;
  cfg.network.disable_auto_reconnect = false;
  cfg.buffer.size = 1024;
  cfg.buffer.out_size = 1024;
#else
  cfg.host = broker.spec->host;
  cfg.port = 443;
  cfg.username = broker.username;
  cfg.password = broker.token;
  cfg.client_id = broker.client_id;
  cfg.keepalive = 30;
  cfg.buffer_size = 1024;
  cfg.out_buffer_size = 1024;
  cfg.reconnect_timeout_ms = 10000;
  cfg.network_timeout_ms = 10000;
  cfg.disable_auto_reconnect = false;
  cfg.transport = MQTT_TRANSPORT_OVER_WSS;
  cfg.cert_pem = mqtt_ca_certs::kCombinedPem;
  cfg.lwt_topic = broker.status_topic;
  cfg.lwt_msg = broker.offline_payload;
  cfg.lwt_qos = 1;
  cfg.lwt_retain = 1;
  cfg.path = "/mqtt";
#endif

  broker.client = esp_mqtt_client_init(&cfg);
  if (broker.client == nullptr) {
    MQTT_LOG("%s mqtt init failed", broker.spec->label);
    return;
  }

  esp_mqtt_client_register_event(broker.client, MQTT_EVENT_ANY, &MQTTUplink::handleMqttEvent, &broker);
  if (esp_mqtt_client_start(broker.client) != ESP_OK) {
    MQTT_LOG("%s mqtt start failed", broker.spec->label);
    destroyBroker(broker);
  } else {
    MQTT_LOG("%s mqtt start requested", broker.spec->label);
  }
}

void MQTTUplink::begin(FILESYSTEM* fs) {
  _fs = fs;
  MQTTPrefsStore::load(_fs, _prefs);
  refreshIdentityStrings();
  _running = true;
  _last_status_publish = millis();
  MQTT_LOG("begin iata=%s enabled_mask=0x%02X wifi_ssid=%s", _prefs.iata, _prefs.enabled_mask, _prefs.wifi_ssid);
}

void MQTTUplink::end() {
  MQTT_LOG("end");
  publishStatus(false);
  stopWebServer();
  for (BrokerState& broker : _brokers) {
    destroyBroker(broker);
  }
  if (_wifi_started) {
    WiFi.disconnect(true, true);
    WiFi.mode(WIFI_OFF);
  }
  _wifi_started = false;
  _sntp_started = false;
  _have_time_sync = false;
  _last_wifi_attempt = 0;
  _running = false;
}

void MQTTUplink::loop(const MQTTStatusSnapshot& snapshot) {
  if (!_running) {
    return;
  }

  _last_status = snapshot;
  ensureWifi();
  updateTimeSync();
  ensureWebServer();

  for (BrokerState& broker : _brokers) {
    ensureBroker(broker);
    if (broker.connected && !broker.connect_announced) {
      publishOnlineStatus(broker);
      broker.connect_announced = true;
      _last_status_publish = millis();
    } else if (!broker.connected) {
      broker.connect_announced = false;
    }
  }

  if (_prefs.status_enabled && hasEnabledBroker() && _have_time_sync &&
      millis() - _last_status_publish >= _prefs.status_interval_ms) {
    publishStatus(true);
    _last_status_publish = millis();
  }
}

void MQTTUplink::publishPacket(const mesh::Packet& packet, bool is_tx, int rssi, float snr) {
  if (!_running || !_have_time_sync || !hasEnabledBroker() || !_prefs.packets_enabled) {
    return;
  }
  if (is_tx && !_prefs.tx_enabled) {
    return;
  }
  MQTT_LOG("packet dir=%s type=%u payload_len=%u rssi=%d snr=%.1f", is_tx ? "tx" : "rx", packet.getPayloadType(),
           packet.payload_len, rssi, snr);

  char* payload = allocScratchBuffer(1280);
  if (payload == nullptr) {
    return;
  }
  int len = buildPacketJson(payload, 1280, packet, is_tx, rssi, snr);
  if (len <= 0 || static_cast<size_t>(len) >= 1280) {
    freeScratchBuffer(payload);
    return;
  }

  for (BrokerState& broker : _brokers) {
    if ((_prefs.enabled_mask & broker.spec->bit) != 0) {
      queuePublish(broker, broker.packets_topic, payload, false);
    }
  }
  freeScratchBuffer(payload);

  if (!_prefs.raw_enabled) {
    return;
  }

  char* raw_payload = allocScratchBuffer(896);
  if (raw_payload == nullptr) {
    return;
  }
  len = buildRawJson(raw_payload, 896, packet, is_tx, rssi, snr);
  if (len <= 0 || static_cast<size_t>(len) >= 896) {
    freeScratchBuffer(raw_payload);
    return;
  }

  for (BrokerState& broker : _brokers) {
    if ((_prefs.enabled_mask & broker.spec->bit) != 0) {
      queuePublish(broker, broker.raw_topic, raw_payload, false);
    }
  }
  freeScratchBuffer(raw_payload);
}

void MQTTUplink::formatStatusReply(char* reply, size_t reply_size) const {
  auto broker_state = [this](const BrokerState& broker) -> const char* {
    if ((_prefs.enabled_mask & broker.spec->bit) == 0) {
      return "off";
    }
    if (broker.connected) {
      return "up";
    }
    if (WiFi.status() != WL_CONNECTED || !_have_time_sync) {
      return "wait";
    }
    if (broker.client != nullptr) {
      return "conn";
    }
    return "retry";
  };

  snprintf(reply, reply_size, "> wifi:%s ntp:%s iata:%s eastmesh-au:%s letsmesh-eu:%s letsmesh-us:%s status:%s tx:%s",
           getWifiStateLabel(_prefs, _wifi_started), _have_time_sync ? "up" : "wait", _prefs.iata,
           broker_state(_brokers[0]), broker_state(_brokers[1]), broker_state(_brokers[2]),
           _prefs.status_enabled ? "on" : "off", _prefs.tx_enabled ? "on" : "off");
}

void MQTTUplink::formatWebStatusReply(char* reply, size_t reply_size) const {
#if WITH_WEB_PANEL
  if (_web_runner == nullptr) {
    snprintf(reply, reply_size, "> web:off");
    return;
  }

  if (_prefs.web_enabled == 0) {
    snprintf(reply, reply_size, "> web:off");
    return;
  }

  if (!_web_panel.isRunning() || !_wifi_started || WiFi.status() != WL_CONNECTED) {
    snprintf(reply, reply_size, "> web:down");
    return;
  }

  snprintf(reply, reply_size, "> web:up url:https://%s/ auth:%s", WiFi.localIP().toString().c_str(),
           _web_panel.hasSessionToken() ? "unlocked" : "locked");
#else
  (void)reply_size;
  snprintf(reply, reply_size, "> web:unsupported");
#endif
}

bool MQTTUplink::setEndpointEnabled(uint8_t bit, bool enabled) {
  if (enabled) {
    _prefs.enabled_mask |= bit;
  } else {
    _prefs.enabled_mask &= ~bit;
  }
  savePrefs();
  return true;
}

bool MQTTUplink::isEndpointEnabled(uint8_t bit) const {
  return (_prefs.enabled_mask & bit) != 0;
}

bool MQTTUplink::setPacketsEnabled(bool enabled) {
  _prefs.packets_enabled = enabled ? 1 : 0;
  return savePrefs();
}

bool MQTTUplink::setRawEnabled(bool enabled) {
  _prefs.raw_enabled = enabled ? 1 : 0;
  return savePrefs();
}

bool MQTTUplink::setStatusEnabled(bool enabled) {
  _prefs.status_enabled = enabled ? 1 : 0;
  return savePrefs();
}

bool MQTTUplink::setTxEnabled(bool enabled) {
  _prefs.tx_enabled = enabled ? 1 : 0;
  return savePrefs();
}

bool MQTTUplink::setWebEnabled(bool enabled) {
#if WITH_WEB_PANEL
  _prefs.web_enabled = enabled ? 1 : 0;
  bool ok = savePrefs();
  if (_prefs.web_enabled != 0) {
    ensureWebServer();
  }
  return ok;
#else
  (void)enabled;
  return false;
#endif
}

bool MQTTUplink::setIata(const char* iata) {
  if (iata == nullptr || *iata == 0) {
    return false;
  }

  char cleaned[sizeof(_prefs.iata)];
  memset(cleaned, 0, sizeof(cleaned));
  makeSafeToken(iata, cleaned, sizeof(cleaned));
  for (size_t i = 0; cleaned[i] != 0; ++i) {
    cleaned[i] = toupper(static_cast<unsigned char>(cleaned[i]));
  }
  StrHelper::strncpy(_prefs.iata, cleaned, sizeof(_prefs.iata));
  refreshIdentityStrings();
  return savePrefs();
}

bool MQTTUplink::setWifiPowerSave(const char* mode) {
  if (mode == nullptr) {
    return false;
  }

  uint8_t next_mode;
  if (strcmp(mode, "none") == 0) {
    next_mode = 0;
  } else if (strcmp(mode, "min") == 0) {
    next_mode = 1;
  } else if (strcmp(mode, "max") == 0) {
    next_mode = 2;
  } else {
    return false;
  }

  _prefs.wifi_powersave = next_mode;
  bool ok = savePrefs();
  if (_wifi_started) {
    ok = WiFi.setSleep(toEspPowerSave(_prefs.wifi_powersave)) && ok;
  }
  return ok;
}

const char* MQTTUplink::getWifiPowerSave() const {
  return getPowerSaveLabel(_prefs.wifi_powersave);
}

bool MQTTUplink::setWifiSSID(const char* ssid) {
  if (ssid == nullptr) {
    return false;
  }
  StrHelper::strncpy(_prefs.wifi_ssid, ssid, sizeof(_prefs.wifi_ssid));
  bool ok = savePrefs();
  reconnectWifi();
  return ok;
}

bool MQTTUplink::setWifiPassword(const char* pwd) {
  if (pwd == nullptr) {
    return false;
  }
  StrHelper::strncpy(_prefs.wifi_pwd, pwd, sizeof(_prefs.wifi_pwd));
  bool ok = savePrefs();
  reconnectWifi();
  return ok;
}

bool MQTTUplink::setOwnerPublicKey(const char* owner_public_key) {
  if (owner_public_key == nullptr) {
    return false;
  }

  if (owner_public_key[0] == 0) {
    _prefs.owner_public_key[0] = 0;
    return savePrefs();
  }

  if (strlen(owner_public_key) != 64) {
    return false;
  }

  for (size_t i = 0; i < 64; ++i) {
    if (!isxdigit(static_cast<unsigned char>(owner_public_key[i]))) {
      return false;
    }
    _prefs.owner_public_key[i] = toupper(static_cast<unsigned char>(owner_public_key[i]));
  }
  _prefs.owner_public_key[64] = 0;
  return savePrefs();
}

bool MQTTUplink::setOwnerEmail(const char* owner_email) {
  if (owner_email == nullptr) {
    return false;
  }
  StrHelper::strncpy(_prefs.owner_email, owner_email, sizeof(_prefs.owner_email));
  return savePrefs();
}

void MQTTUplink::formatWifiStatusReply(char* reply, size_t reply_size) const {
  const char* status = "disconnected";
  const char* state = "disconnected";
  wl_status_t wifi_status = WiFi.status();
  if (_prefs.wifi_ssid[0] == 0) {
    status = "unconfigured";
    state = "unconfigured";
  } else if (wifi_status == WL_CONNECTED) {
    status = "connected";
    state = "connected";
  } else if (_wifi_started) {
    status = "connecting";
  }

  switch (wifi_status) {
    case WL_IDLE_STATUS:
      state = "idle";
      break;
    case WL_NO_SSID_AVAIL:
      state = "no_ssid";
      break;
    case WL_SCAN_COMPLETED:
      state = "scan_completed";
      break;
    case WL_CONNECTED:
      state = "connected";
      break;
    case WL_CONNECT_FAILED:
      state = "connect_failed";
      break;
    case WL_CONNECTION_LOST:
      state = "connection_lost";
      break;
    case WL_DISCONNECTED:
      state = "disconnected";
      break;
    default:
      state = "unknown";
      break;
  }

  if (wifi_status == WL_CONNECTED) {
    const int rssi_dbm = WiFi.RSSI();
    snprintf(reply, reply_size,
             "> ssid:%s status:%s code:%d state:%s ip:%s rssi:%d quality:%d%% signal:%s",
             _prefs.wifi_ssid, status, static_cast<int>(wifi_status), state, WiFi.localIP().toString().c_str(),
             rssi_dbm, getWifiQualityPercent(rssi_dbm), getWifiQualityLabel(rssi_dbm));
  } else {
    snprintf(reply, reply_size, "> ssid:%s status:%s code:%d state:%s", _prefs.wifi_ssid[0] ? _prefs.wifi_ssid : "-",
             status, static_cast<int>(wifi_status), state);
  }
}

#else

MQTTUplink::MQTTUplink(mesh::RTCClock&, mesh::LocalIdentity&)
    : _fs(nullptr), _rtc(nullptr), _identity(nullptr), _running(false), _wifi_started(false), _sntp_started(false),
      _have_time_sync(false), _last_wifi_attempt(0), _last_status_publish(0), _last_status{},
      _node_name(nullptr),
      _web_runner(nullptr) {
}

bool MQTTUplink::savePrefs() { return false; }
void MQTTUplink::begin(FILESYSTEM*) {}
void MQTTUplink::end() {}
void MQTTUplink::loop(const MQTTStatusSnapshot&) {}
void MQTTUplink::publishPacket(const mesh::Packet&, bool, int, float) {}
void MQTTUplink::formatStatusReply(char* reply, size_t reply_size) const { snprintf(reply, reply_size, "> unsupported"); }
void MQTTUplink::formatWebStatusReply(char* reply, size_t reply_size) const { snprintf(reply, reply_size, "> unsupported"); }
bool MQTTUplink::setEndpointEnabled(uint8_t, bool) { return false; }
bool MQTTUplink::isEndpointEnabled(uint8_t) const { return false; }
bool MQTTUplink::setPacketsEnabled(bool) { return false; }
bool MQTTUplink::setRawEnabled(bool) { return false; }
bool MQTTUplink::setStatusEnabled(bool) { return false; }
bool MQTTUplink::setTxEnabled(bool) { return false; }
bool MQTTUplink::setWebEnabled(bool) { return false; }
bool MQTTUplink::setIata(const char*) { return false; }
bool MQTTUplink::setWifiPowerSave(const char*) { return false; }
const char* MQTTUplink::getWifiPowerSave() const { return "unsupported"; }
bool MQTTUplink::isActive() const { return false; }
bool MQTTUplink::setWifiSSID(const char*) { return false; }
bool MQTTUplink::setWifiPassword(const char*) { return false; }
bool MQTTUplink::setOwnerPublicKey(const char*) { return false; }
bool MQTTUplink::setOwnerEmail(const char*) { return false; }
void MQTTUplink::formatWifiStatusReply(char* reply, size_t reply_size) const { snprintf(reply, reply_size, "> unsupported"); }
void MQTTUplink::reconnectWifi() {}
bool MQTTUplink::sendStatusNow() { return false; }

#endif

#endif
