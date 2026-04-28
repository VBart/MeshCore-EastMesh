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
#if defined(CONFIG_MBEDTLS_CERTIFICATE_BUNDLE) && !(defined(ARDUINO) && ESP_IDF_VERSION_MAJOR < 5)
#include <esp_crt_bundle.h>
#define MQTT_USE_CRT_BUNDLE 1
#define MQTT_CRT_BUNDLE_ATTACH esp_crt_bundle_attach
#endif
#include <helpers/TxtDataHelpers.h>
#include <ctype.h>
#include <string.h>
#include <strings.h>
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
  #define MQTT_LOG(fmt, ...) LOG_CAT("MQTT", fmt, ##__VA_ARGS__)
#else
  #define LOG_CAT(...) do { } while (0)
  #define MQTT_LOG(...) do { } while (0)
#endif

namespace {
constexpr unsigned long kBrokerRetryBaseMillis = 10000;
constexpr unsigned long kBrokerRetryMaxMillis = 300000;
constexpr unsigned long kHeapHeadroomRetryMillis = 30000;
constexpr unsigned long kWsPreflightTimeoutMillis = 5000;
constexpr unsigned long kTokenRefreshEventWindowMs = 2UL * 60UL * 1000UL;
constexpr size_t kBrokerTokenSize = 640;
constexpr size_t kWsTransportBufferSize = 1024;
constexpr size_t kWsPreflightResponseSize = kWsTransportBufferSize;
constexpr size_t kDualBrokerMinFreeHeap = 80U * 1024U;
constexpr size_t kDualBrokerMinLargestHeap = 32U * 1024U;
constexpr time_t kTokenLifetimeSecs = 6UL * 60UL * 60UL;
constexpr time_t kTokenRefreshSlackSecs = 300;
constexpr time_t kMinSaneEpoch = 1735689600;  // 2025-01-01T00:00:00Z

unsigned long getBrokerRetryDelayMillis(uint8_t failures) {
  unsigned long delay_ms = kBrokerRetryBaseMillis;
  if (failures > 0) {
    uint8_t shifts = min<uint8_t>(failures - 1, 5);
    delay_ms <<= shifts;
  }
  if (delay_ms > kBrokerRetryMaxMillis) {
    delay_ms = kBrokerRetryMaxMillis;
  }
  return delay_ms;
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

bool containsIgnoreCase(const char* haystack, const char* needle) {
  if (haystack == nullptr || needle == nullptr || needle[0] == 0) {
    return false;
  }
  const size_t needle_len = strlen(needle);
  for (const char* scan = haystack; *scan != 0; ++scan) {
    if (strncasecmp(scan, needle, needle_len) == 0) {
      return true;
    }
  }
  return false;
}

#if MQTT_DEBUG
void logMqttMemorySnapshot(const char* phase, const char* broker_label = nullptr) {
  MQTT_LOG("mem phase=%s broker=%s uptime_ms=%lu heap_free=%u heap_min=%u heap_max=%u psram_free=%u psram_min=%u "
           "psram_max=%u",
           phase != nullptr ? phase : "-",
           broker_label != nullptr ? broker_label : "-",
           millis(),
           ESP.getFreeHeap(),
           ESP.getMinFreeHeap(),
           ESP.getMaxAllocHeap(),
           ESP.getFreePsram(),
           ESP.getMinFreePsram(),
           ESP.getMaxAllocPsram());
}
#else
void logMqttMemorySnapshot(const char*, const char* = nullptr) {
}
#endif

}

const MQTTUplink::BrokerSpec MQTTUplink::kBrokerSpecs[3] = {
    {"eastmesh-au", "eastmesh-au", "mqtt2.eastmesh.au", "wss://mqtt2.eastmesh.au:443/mqtt", kEastmeshBit},
    {"letsmesh-eu", "letsmesh-eu", "mqtt-eu-v1.letsmesh.net", "wss://mqtt-eu-v1.letsmesh.net:443/mqtt",
     kLetsmeshEuBit},
    {"letsmesh-us", "letsmesh-us", "mqtt-us-v1.letsmesh.net", "wss://mqtt-us-v1.letsmesh.net:443/mqtt",
     kLetsmeshUsBit},
};

MQTTUplink::MQTTUplink(mesh::RTCClock& rtc, mesh::LocalIdentity& identity)
    : _fs(nullptr), _rtc(&rtc), _identity(&identity), _running(false), _last_status_publish(0),
      _token_refresh_count(0), _token_refresh_active_until_ms(0), _last_status{}, _node_name(nullptr), _network(nullptr)
       {
  memset(_device_id, 0, sizeof(_device_id));
  MQTTPrefsStore::setDefaults(_prefs);
  for (size_t i = 0; i < 3; ++i) {
    memset(&_brokers[i], 0, sizeof(_brokers[i]));
    _brokers[i].spec = &kBrokerSpecs[i];
  }
  MQTT_LOG("uplink init");
}

const char* MQTTUplink::getClientVersion() const {
  return CLIENT_VERSION;
}

bool MQTTUplink::savePrefs() {
  return MQTTPrefsStore::save(_fs, _prefs);
}

bool MQTTUplink::isTokenRefreshInProgress() const {
  return _token_refresh_active_until_ms != 0 &&
         static_cast<long>(millis() - _token_refresh_active_until_ms) < 0;
}

bool MQTTUplink::hasEnabledBroker() const {
  return (_prefs.enabled_mask & 0x07) != 0;
}

uint8_t MQTTUplink::countEnabledBrokers() const {
  uint8_t count = 0;
  for (const BrokerState& broker : _brokers) {
    if (broker.spec != nullptr && (_prefs.enabled_mask & broker.spec->bit) != 0) {
      ++count;
    }
  }
  return count;
}

uint8_t MQTTUplink::countConnectedBrokers() const {
  uint8_t count = 0;
  for (const BrokerState& broker : _brokers) {
    if (broker.spec != nullptr && broker.connected) {
      ++count;
    }
  }
  return count;
}

bool MQTTUplink::isUnsetIataValue(const char* iata) {
  return iata == nullptr || iata[0] == 0 || strcmp(iata, MQTT_UNSET_IATA) == 0;
}

const char* MQTTUplink::brokerCaCert(const BrokerSpec& spec) {
  if (spec.bit == kEastmeshBit) {
    return mqtt_ca_certs::kEastmeshIsrgRootX1Pem;
  }
  return mqtt_ca_certs::kLetsmeshWe1Pem;
}

uint8_t MQTTUplink::normalizeEnabledMask(uint8_t mask) {
  uint8_t normalized = 0;
  uint8_t count = 0;
  for (const BrokerSpec& spec : kBrokerSpecs) {
    if ((mask & spec.bit) != 0) {
      if (count >= kMaxEnabledBrokers) {
        break;
      }
      normalized |= spec.bit;
      ++count;
    }
  }
  return normalized;
}

bool MQTTUplink::hasConnectHeadroom(const BrokerState& broker) const {
  const uint8_t enabled_count = countEnabledBrokers();
  const uint8_t connected_count = countConnectedBrokers();
  const bool dual_broker_attempt = enabled_count > 1 && connected_count > 0;
  if (!dual_broker_attempt) {
    return true;
  }

  const size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  const size_t largest_heap = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

  if (free_heap >= kDualBrokerMinFreeHeap && largest_heap >= kDualBrokerMinLargestHeap) {
    return true;
  }

  MQTT_LOG("%s connect deferred: heap headroom low heap_free=%u heap_max=%u required_heap_free=%u "
           "required_heap_max=%u enabled=%u connected=%u",
           broker.spec != nullptr ? broker.spec->label : "-",
           static_cast<unsigned>(free_heap),
           static_cast<unsigned>(largest_heap),
           static_cast<unsigned>(kDualBrokerMinFreeHeap),
           static_cast<unsigned>(kDualBrokerMinLargestHeap),
           static_cast<unsigned>(enabled_count),
           static_cast<unsigned>(connected_count));
  return false;
}

bool MQTTUplink::preflightBroker(BrokerState& broker) const {
  if (broker.spec == nullptr) {
    return false;
  }

  logMqttMemorySnapshot("preflight-pre", broker.spec->label);
  WiFiClientSecure client;
  client.setCACert(brokerCaCert(*broker.spec));
  client.setTimeout(kWsPreflightTimeoutMillis);
  if (!client.connect(broker.spec->host, 443)) {
    MQTT_LOG("%s preflight failed: tcp/tls connect", broker.spec->label);
    client.stop();
    logMqttMemorySnapshot("preflight-failed", broker.spec->label);
    return false;
  }

  client.print("GET /mqtt HTTP/1.1\r\n"
               "Connection: Upgrade\r\n"
               "Upgrade: websocket\r\n"
               "Sec-WebSocket-Version: 13\r\n"
               "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
               "User-Agent: MeshCore-EastMesh\r\n"
               "Host: ");
  client.print(broker.spec->host);
  client.print(":443\r\n\r\n");

  char response[kWsPreflightResponseSize];
  size_t used = 0;
  bool header_complete = false;
  unsigned long deadline_ms = millis() + kWsPreflightTimeoutMillis;
  response[0] = 0;

  while (static_cast<long>(millis() - deadline_ms) < 0) {
    while (client.available() > 0) {
      int c = client.read();
      if (c < 0) {
        break;
      }
      if (used + 1 >= sizeof(response)) {
        response[used] = 0;
        MQTT_LOG("%s preflight failed: response header too large bytes=%u", broker.spec->label,
                 static_cast<unsigned>(used));
        client.stop();
        logMqttMemorySnapshot("preflight-failed", broker.spec->label);
        return false;
      }
      response[used++] = static_cast<char>(c);
      response[used] = 0;
      if (strstr(response, "\r\n\r\n") != nullptr) {
        header_complete = true;
        break;
      }
    }
    if (header_complete) {
      break;
    }
    if (!client.connected() && client.available() <= 0) {
      break;
    }
    delay(10);
  }

  client.stop();
  if (!header_complete) {
    MQTT_LOG("%s preflight failed: incomplete response bytes=%u", broker.spec->label, static_cast<unsigned>(used));
    logMqttMemorySnapshot("preflight-failed", broker.spec->label);
    return false;
  }

  char status_line[48];
  size_t status_len = 0;
  while (status_len + 1 < sizeof(status_line) && response[status_len] != 0 && response[status_len] != '\r' &&
         response[status_len] != '\n') {
    status_line[status_len] = response[status_len];
    ++status_len;
  }
  status_line[status_len] = 0;

  bool ok = strncmp(response, "HTTP/", 5) == 0 && strstr(status_line, " 101") != nullptr &&
            containsIgnoreCase(response, "\r\nSec-WebSocket-Accept:");
  if (!ok) {
    MQTT_LOG("%s preflight failed: status=\"%s\" accept=%d", broker.spec->label, status_line,
             containsIgnoreCase(response, "\r\nSec-WebSocket-Accept:") ? 1 : 0);
    logMqttMemorySnapshot("preflight-failed", broker.spec->label);
    return false;
  }

  MQTT_LOG("%s preflight ok: status=\"%s\"", broker.spec->label, status_line);
  logMqttMemorySnapshot("preflight-ok", broker.spec->label);
  return true;
}

void MQTTUplink::formatTopic(char* dst, size_t dst_size, const char* leaf) const {
  if (dst == nullptr || dst_size == 0) {
    return;
  }
  snprintf(dst, dst_size, "meshcore/%s/%s/%s", _prefs.iata, _device_id, leaf);
}

bool MQTTUplink::isActive() const {
  return _running && hasEnabledBroker();
}

bool MQTTUplink::sendStatusNow() {
  if (!_running || _network == nullptr || !_network->hasTimeSync() || !_network->isWifiConnected()) {
    return false;
  }

  bool any_connected = false;
  for (const BrokerState& broker : _brokers) {
    if (broker.spec != nullptr && broker.connected && broker.client != nullptr) {
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

void MQTTUplink::refreshIdentityStrings() {
  bytesToHexUpper(_identity->pub_key, PUB_KEY_SIZE, _device_id, sizeof(_device_id));
  for (BrokerState& broker : _brokers) {
    refreshBrokerIdentity(broker);
  }
}

void MQTTUplink::refreshBrokerIdentity(BrokerState& broker) {
  if (broker.spec == nullptr) {
    return;
  }
  snprintf(broker.username, sizeof(broker.username), "v1_%s", _device_id);
  snprintf(broker.client_id, sizeof(broker.client_id), "mqtt_%s-%.6s", broker.spec->key, _device_id);
  formatTopic(broker.status_topic, sizeof(broker.status_topic), "status");
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
    MQTT_LOG("%s token skipped: clock not ready (%lu)", broker.spec->label, static_cast<unsigned long>(now));
    return false;
  }

  if (broker.token == nullptr) {
    broker.token = allocScratchBuffer(kBrokerTokenSize);
    if (broker.token == nullptr) {
      MQTT_LOG("%s token alloc failed", broker.spec->label);
      return false;
    }
  }

  time_t expires_at = now + kTokenLifetimeSecs;
  const char* owner = _prefs.owner_public_key[0] ? _prefs.owner_public_key : nullptr;
  const char* email = _prefs.owner_email[0] ? _prefs.owner_email : nullptr;
  if (!JWTHelper::createAuthToken(*_identity, broker.spec->host, now, expires_at, broker.token, kBrokerTokenSize,
                                  owner, email)) {
    freeScratchBuffer(broker.token);
    broker.token = nullptr;
    MQTT_LOG("%s token creation failed", broker.spec->label);
    return false;
  }
  broker.token_expires_at = expires_at;
  MQTT_LOG("%s token ready exp=%lu owner=%s email=%s", broker.spec->label,
           static_cast<unsigned long>(expires_at), owner != nullptr ? "yes" : "no", email != nullptr ? "yes" : "no");
  return true;
}

void MQTTUplink::destroyBroker(BrokerState& broker, bool reset_retry_state) {
  bool had_runtime_state = broker.client != nullptr || broker.token != nullptr || broker.connected || broker.started ||
                           broker.connect_announced || broker.reconnect_pending || broker.next_connect_attempt != 0 ||
                           broker.last_connect_attempt != 0 || broker.reconnect_failures != 0 ||
                           broker.token_expires_at != 0;
  if (!had_runtime_state) {
    return;
  }
  logMqttMemorySnapshot("destroy-pre", broker.spec != nullptr ? broker.spec->label : nullptr);
  if (broker.client != nullptr) {
    if (broker.started) {
      MQTT_LOG("%s stop broker client", broker.spec->label);
      esp_err_t stop_rc = esp_mqtt_client_stop(broker.client);
      MQTT_LOG("%s stop broker client rc=0x%x", broker.spec->label, stop_rc);
      if (heap_caps_check_integrity_all(true)) {
        MQTT_LOG("%s destroy broker client", broker.spec->label);
        esp_err_t destroy_rc = esp_mqtt_client_destroy(broker.client);
        MQTT_LOG("%s destroy broker client rc=0x%x", broker.spec->label, destroy_rc);
      } else {
        // Avoid freeing through esp-mqtt after the IDF 4.4 WSS transport has already poisoned the heap.
        MQTT_LOG("%s abandon stopped broker client: heap corrupt", broker.spec->label);
      }
    } else {
      if (heap_caps_check_integrity_all(true)) {
        MQTT_LOG("%s destroy broker client", broker.spec->label);
        esp_err_t destroy_rc = esp_mqtt_client_destroy(broker.client);
        MQTT_LOG("%s destroy broker client rc=0x%x", broker.spec->label, destroy_rc);
      } else {
        MQTT_LOG("%s abandon broker client: heap corrupt", broker.spec->label);
      }
    }
    broker.client = nullptr;
  }
  freeScratchBuffer(broker.token);
  broker.token = nullptr;
  broker.connected = false;
  broker.started = false;
  broker.connect_announced = false;
  broker.connected_since_ms = 0;
  broker.token_expires_at = 0;
  if (reset_retry_state) {
    broker.reconnect_pending = false;
    broker.next_connect_attempt = 0;
    broker.reconnect_failures = 0;
    broker.last_connect_attempt = 0;
  }
  logMqttMemorySnapshot("destroy-post", broker.spec != nullptr ? broker.spec->label : nullptr);
}

void MQTTUplink::queuePublish(BrokerState& broker, const char* topic, const char* payload, bool retain) {
  if (broker.client == nullptr || !broker.connected) {
    return;
  }
  MQTT_LOG("%s publish topic=%s retain=%d bytes=%u", broker.spec->label, topic, retain ? 1 : 0,
           static_cast<unsigned>(strlen(payload)));
  int enqueue_rc = esp_mqtt_client_enqueue(broker.client, topic, payload, 0, 1, retain, true);
  MQTT_LOG("%s enqueue topic=%s rc=%d connected=%d", broker.spec->label, topic, enqueue_rc, broker.connected ? 1 : 0);
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
                                float snr, int score, int duration) const {
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
    int len;
    if (score >= 0) {
      len = snprintf(buffer, buffer_size,
                     "{\"origin\":\"%s\",\"origin_id\":\"%s\",\"timestamp\":\"%s\",\"type\":\"PACKET\","
                     "\"direction\":\"%s\",\"time\":\"%s\",\"date\":\"%s\",\"len\":\"%d\",\"packet_type\":\"%u\","
                     "\"route\":\"D\",\"payload_len\":\"%u\",\"raw\":\"%s\",\"SNR\":\"%.1f\",\"RSSI\":\"%d\","
                     "\"score\":\"%d\",\"duration\":\"%d\",\"hash\":\"%s\",\"path\":\"%s\"}",
                     origin, _device_id, ts, is_tx ? "tx" : "rx", time_only, date_only, raw_len,
                     packet.getPayloadType(), packet.payload_len, raw_hex, snr, rssi, score, duration, hash_hex,
                     path_info);
    } else {
      len = snprintf(buffer, buffer_size,
                     "{\"origin\":\"%s\",\"origin_id\":\"%s\",\"timestamp\":\"%s\",\"type\":\"PACKET\","
                     "\"direction\":\"%s\",\"time\":\"%s\",\"date\":\"%s\",\"len\":\"%d\",\"packet_type\":\"%u\","
                     "\"route\":\"D\",\"payload_len\":\"%u\",\"raw\":\"%s\",\"SNR\":\"%.1f\",\"RSSI\":\"%d\","
                     "\"hash\":\"%s\",\"path\":\"%s\"}",
                     origin, _device_id, ts, is_tx ? "tx" : "rx", time_only, date_only, raw_len,
                     packet.getPayloadType(), packet.payload_len, raw_hex, snr, rssi, hash_hex, path_info);
    }
    freeScratchBuffer(raw_hex);
    return len;
  }

  int len;
  if (score >= 0) {
    len = snprintf(buffer, buffer_size,
                   "{\"origin\":\"%s\",\"origin_id\":\"%s\",\"timestamp\":\"%s\",\"type\":\"PACKET\","
                   "\"direction\":\"%s\",\"time\":\"%s\",\"date\":\"%s\",\"len\":\"%d\",\"packet_type\":\"%u\","
                   "\"route\":\"F\",\"payload_len\":\"%u\",\"raw\":\"%s\",\"SNR\":\"%.1f\",\"RSSI\":\"%d\","
                   "\"score\":\"%d\",\"duration\":\"%d\",\"hash\":\"%s\"}",
                   origin, _device_id, ts, is_tx ? "tx" : "rx", time_only, date_only, raw_len,
                   packet.getPayloadType(), packet.payload_len, raw_hex, snr, rssi, score, duration, hash_hex);
  } else {
    len = snprintf(buffer, buffer_size,
                   "{\"origin\":\"%s\",\"origin_id\":\"%s\",\"timestamp\":\"%s\",\"type\":\"PACKET\","
                   "\"direction\":\"%s\",\"time\":\"%s\",\"date\":\"%s\",\"len\":\"%d\",\"packet_type\":\"%u\","
                   "\"route\":\"F\",\"payload_len\":\"%u\",\"raw\":\"%s\",\"SNR\":\"%.1f\",\"RSSI\":\"%d\","
                   "\"hash\":\"%s\"}",
                   origin, _device_id, ts, is_tx ? "tx" : "rx", time_only, date_only, raw_len,
                   packet.getPayloadType(), packet.payload_len, raw_hex, snr, rssi, hash_hex);
  }
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
  logMqttMemorySnapshot(online ? "status-pre" : "status-offline-pre");
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
    if (broker.spec != nullptr) {
      queuePublish(broker, broker.status_topic, payload, true);
    }
  }
  freeScratchBuffer(payload);
  logMqttMemorySnapshot(online ? "status-post" : "status-offline-post");
}

void MQTTUplink::scheduleBrokerRetry(BrokerState& broker, unsigned long now_ms, bool count_failure) {
  if (count_failure && broker.reconnect_failures < 10) {
    broker.reconnect_failures++;
  }
  broker.reconnect_pending = true;
  broker.next_connect_attempt = now_ms + getBrokerRetryDelayMillis(broker.reconnect_failures);
  MQTT_LOG("%s reconnect in %lu ms (failures=%u)", broker.spec != nullptr ? broker.spec->label : "-",
           getBrokerRetryDelayMillis(broker.reconnect_failures),
           static_cast<unsigned>(broker.reconnect_failures));
}

void MQTTUplink::handleMqttEvent(void* handler_args, esp_event_base_t, int32_t event_id, void* event_data) {
  auto* broker = static_cast<BrokerState*>(handler_args);
  if (broker == nullptr) {
    return;
  }
  auto* event = static_cast<esp_mqtt_event_handle_t>(event_data);
  unsigned long now_ms = millis();
  unsigned long connected_for_ms = broker->connected_since_ms != 0 ? (now_ms - broker->connected_since_ms) : 0;

  switch (event_id) {
    case MQTT_EVENT_CONNECTED:
      broker->connected = true;
      broker->reconnect_pending = false;
      broker->next_connect_attempt = 0;
      broker->reconnect_failures = 0;
      broker->connected_since_ms = now_ms;
      MQTT_LOG("%s connected", broker->spec->label);
      logMqttMemorySnapshot("connected", broker->spec->label);
      break;
    case MQTT_EVENT_DISCONNECTED:
      broker->connected = false;
      broker->connected_since_ms = 0;
      MQTT_LOG("%s disconnected wifi_status=%d rssi=%d connected_for_ms=%lu", broker->spec->label,
               static_cast<int>(WiFi.status()), WiFi.RSSI(), connected_for_ms);
      logMqttMemorySnapshot("disconnected", broker->spec->label);
      if (!broker->reconnect_pending) {
        scheduleBrokerRetry(*broker, now_ms, true);
      }
      break;
    case MQTT_EVENT_ERROR:
      broker->connected = false;
      broker->connected_since_ms = 0;
      if (event != nullptr && event->error_handle != nullptr) {
        MQTT_LOG("%s error type=%d tls_esp=0x%x tls_stack=0x%x cert_flags=0x%x sock_errno=%d conn_refused=%d "
                 "connected_for_ms=%lu",
                 broker->spec->label, event->error_handle->error_type, event->error_handle->esp_tls_last_esp_err,
                 event->error_handle->esp_tls_stack_err, event->error_handle->esp_tls_cert_verify_flags,
                 event->error_handle->esp_transport_sock_errno, event->error_handle->connect_return_code,
                 connected_for_ms);
      } else {
        MQTT_LOG("%s error event connected_for_ms=%lu", broker->spec->label, connected_for_ms);
      }
      MQTT_LOG("%s wifi_status=%d rssi=%d", broker->spec->label, static_cast<int>(WiFi.status()), WiFi.RSSI());
      logMqttMemorySnapshot("error", broker->spec->label);
      if (!broker->reconnect_pending) {
        scheduleBrokerRetry(*broker, now_ms, true);
      }
      break;
    case MQTT_EVENT_BEFORE_CONNECT:
      MQTT_LOG("%s before connect", broker->spec->label);
      logMqttMemorySnapshot("before-connect", broker->spec->label);
      break;
    default:
      break;
  }
}

void MQTTUplink::ensureBroker(BrokerState& broker, bool allow_new_connect) {
  if (broker.spec == nullptr) {
    return;
  }
  bool enabled = (_prefs.enabled_mask & broker.spec->bit) != 0;
  bool iata_configured = !isUnsetIataValue(_prefs.iata);
  if (!enabled || !iata_configured) {
    if (broker.client != nullptr || broker.token != nullptr || broker.connected || broker.connect_announced ||
        broker.reconnect_pending || broker.next_connect_attempt != 0 || broker.last_connect_attempt != 0 ||
        broker.reconnect_failures != 0 || broker.token_expires_at != 0) {
      destroyBroker(broker);
    }
    return;
  }

  if (_network == nullptr || !_network->hasTimeSync() || !_network->isWifiConnected()) {
    return;
  }

  time_t now = time(nullptr);
  unsigned long now_ms = millis();
  if (broker.client != nullptr && broker.token_expires_at > 0 && now + kTokenRefreshSlackSecs >= broker.token_expires_at) {
    _token_refresh_count++;
    _token_refresh_active_until_ms = now_ms + kTokenRefreshEventWindowMs;
    MQTT_LOG("%s token refresh reconnect count=%lu", broker.spec->label,
             static_cast<unsigned long>(_token_refresh_count));
    destroyBroker(broker, false);
  }

  if (broker.client != nullptr) {
    if (broker.connected) {
      return;
    }
    if (!broker.reconnect_pending || now_ms < broker.next_connect_attempt) {
      return;
    }
    if (!allow_new_connect) {
      return;
    }
    broker.last_connect_attempt = now_ms;
    broker.reconnect_pending = false;
    if (!preflightBroker(broker)) {
      scheduleBrokerRetry(broker, now_ms, true);
      return;
    }
    MQTT_LOG("%s mqtt reconnect requested", broker.spec->label);
    logMqttMemorySnapshot("reconnect-pre", broker.spec->label);
    esp_err_t reconnect_rc = esp_mqtt_client_reconnect(broker.client);
    if (reconnect_rc != ESP_OK) {
      MQTT_LOG("%s mqtt reconnect failed rc=0x%x", broker.spec->label, reconnect_rc);
      logMqttMemorySnapshot("reconnect-failed", broker.spec->label);
      scheduleBrokerRetry(broker, now_ms, true);
    } else {
      logMqttMemorySnapshot("reconnect-requested", broker.spec->label);
    }
    return;
  }

  if (broker.next_connect_attempt != 0 && now_ms < broker.next_connect_attempt) {
    return;
  }
  if (!allow_new_connect) {
    return;
  }
  if (!hasConnectHeadroom(broker)) {
    broker.reconnect_pending = true;
    broker.next_connect_attempt = now_ms + kHeapHeadroomRetryMillis;
    return;
  }
  broker.last_connect_attempt = now_ms;
  broker.reconnect_pending = false;

  if (!preflightBroker(broker)) {
    scheduleBrokerRetry(broker, now_ms, true);
    return;
  }

  if (!refreshToken(broker)) {
    broker.reconnect_pending = true;
    broker.next_connect_attempt = now_ms + kBrokerRetryBaseMillis;
    return;
  }

  refreshBrokerState(broker);
  MQTT_LOG("%s mqtt init host=%s port=%d path=%s client_id=%s", broker.spec->label, broker.spec->host, 443, "/mqtt",
           broker.client_id);
  logMqttMemorySnapshot("init-pre", broker.spec->label);
  esp_mqtt_client_config_t cfg = {};
#if ESP_IDF_VERSION_MAJOR >= 5
  cfg.broker.address.hostname = broker.spec->host;
  cfg.broker.address.port = 443;
  cfg.broker.address.transport = MQTT_TRANSPORT_OVER_WSS;
  cfg.broker.address.path = "/mqtt";
#if defined(MQTT_USE_CRT_BUNDLE)
  cfg.broker.verification.crt_bundle_attach = MQTT_CRT_BUNDLE_ATTACH;
#else
  cfg.broker.verification.certificate = brokerCaCert(*broker.spec);
#endif
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
  cfg.network.disable_auto_reconnect = true;
  cfg.buffer.size = 768;
  cfg.buffer.out_size = 1280;
#else
  cfg.host = broker.spec->host;
  cfg.port = 443;
  cfg.username = broker.username;
  cfg.password = broker.token;
  cfg.client_id = broker.client_id;
  cfg.keepalive = 30;
  cfg.buffer_size = 768;
  cfg.out_buffer_size = 1280;
  cfg.reconnect_timeout_ms = 10000;
  cfg.network_timeout_ms = 10000;
  cfg.disable_auto_reconnect = true;
  cfg.transport = MQTT_TRANSPORT_OVER_WSS;
#if defined(MQTT_USE_CRT_BUNDLE)
  cfg.crt_bundle_attach = MQTT_CRT_BUNDLE_ATTACH;
#else
  cfg.cert_pem = brokerCaCert(*broker.spec);
#endif
  cfg.lwt_topic = broker.status_topic;
  cfg.lwt_msg = broker.offline_payload;
  cfg.lwt_qos = 1;
  cfg.lwt_retain = 1;
  cfg.path = "/mqtt";
#endif

  broker.client = esp_mqtt_client_init(&cfg);
  if (broker.client == nullptr) {
    MQTT_LOG("%s mqtt init failed", broker.spec->label);
    logMqttMemorySnapshot("init-failed", broker.spec->label);
    return;
  }
  logMqttMemorySnapshot("init-post", broker.spec->label);

  esp_mqtt_client_register_event(broker.client, MQTT_EVENT_ANY, &MQTTUplink::handleMqttEvent, &broker);
  if (esp_mqtt_client_start(broker.client) != ESP_OK) {
    MQTT_LOG("%s mqtt start failed", broker.spec->label);
    logMqttMemorySnapshot("start-failed", broker.spec->label);
    broker.reconnect_pending = true;
    broker.next_connect_attempt = now_ms + kBrokerRetryBaseMillis;
    destroyBroker(broker, false);
  } else {
    broker.started = true;
    MQTT_LOG("%s mqtt start requested", broker.spec->label);
    logMqttMemorySnapshot("start-requested", broker.spec->label);
  }
}

void MQTTUplink::begin(FILESYSTEM* fs) {
  _fs = fs;
  MQTTPrefsStore::load(_fs, _prefs);
  uint8_t normalized_mask = normalizeEnabledMask(_prefs.enabled_mask & 0x07);
  if (normalized_mask != _prefs.enabled_mask) {
    _prefs.enabled_mask = normalized_mask;
    savePrefs();
  }
  refreshIdentityStrings();
  _running = true;
  _last_status_publish = millis();
  MQTT_LOG("begin iata=%s enabled_mask=0x%02X", _prefs.iata, _prefs.enabled_mask);
}

void MQTTUplink::end() {
  MQTT_LOG("end");
  publishStatus(false);
  for (BrokerState& broker : _brokers) {
    destroyBroker(broker);
  }
  _running = false;
}

void MQTTUplink::loop(const MQTTStatusSnapshot& snapshot) {
  if (!_running) {
    return;
  }

  _last_status = snapshot;

  BrokerState* active_connecting_broker = nullptr;
  for (BrokerState& broker : _brokers) {
    if (broker.client != nullptr && !broker.connected && !broker.reconnect_pending) {
      active_connecting_broker = &broker;
      break;
    }
  }

  bool connect_started = false;
  for (BrokerState& broker : _brokers) {
    bool allow_new_connect = active_connecting_broker == nullptr && !connect_started;
    ensureBroker(broker, allow_new_connect);
    if (active_connecting_broker == nullptr && broker.client != nullptr && !broker.connected && !broker.reconnect_pending) {
      connect_started = true;
    }
    if (broker.connected && !broker.connect_announced) {
      publishOnlineStatus(broker);
      broker.connect_announced = true;
      _last_status_publish = millis();
    } else if (!broker.connected) {
      broker.connect_announced = false;
    }
  }

  if (_prefs.status_enabled && hasEnabledBroker() && _network != nullptr && _network->hasTimeSync() &&
      millis() - _last_status_publish >= _prefs.status_interval_ms) {
    publishStatus(true);
    _last_status_publish = millis();
  }
}

void MQTTUplink::publishPacket(const mesh::Packet& packet, bool is_tx, int rssi, float snr, int score, int duration) {
  if (!_running || _network == nullptr || !_network->hasTimeSync() || !_network->isWifiConnected() || !hasEnabledBroker() ||
      !_prefs.packets_enabled) {
    return;
  }
  if (is_tx && !_prefs.tx_enabled) {
    return;
  }
  MQTT_LOG("packet dir=%s type=%u payload_len=%u rssi=%d snr=%.1f score=%d duration=%d",
           is_tx ? "tx" : "rx", packet.getPayloadType(), packet.payload_len, rssi, snr, score, duration);

  char* payload = allocScratchBuffer(1280);
  if (payload == nullptr) {
    return;
  }
  int len = buildPacketJson(payload, 1280, packet, is_tx, rssi, snr, score, duration);
  if (len <= 0 || static_cast<size_t>(len) >= 1280) {
    freeScratchBuffer(payload);
    return;
  }

  char topic[128];
  formatTopic(topic, sizeof(topic), "packets");
  for (BrokerState& broker : _brokers) {
    if (broker.spec != nullptr) {
      queuePublish(broker, topic, payload, false);
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

  formatTopic(topic, sizeof(topic), "raw");
  for (BrokerState& broker : _brokers) {
    if (broker.spec != nullptr) {
      queuePublish(broker, topic, raw_payload, false);
    }
  }
  freeScratchBuffer(raw_payload);
}

void MQTTUplink::formatStatusReply(char* reply, size_t reply_size) const {
  auto broker_state = [this](uint8_t bit) -> const char* {
    if ((_prefs.enabled_mask & bit) == 0) {
      return "off";
    }
    if (isUnsetIataValue(_prefs.iata)) {
      return "invalid iata";
    }
    const BrokerState* broker = nullptr;
    for (const BrokerState& candidate : _brokers) {
      if (candidate.spec != nullptr && candidate.spec->bit == bit) {
        broker = &candidate;
        break;
      }
    }
    if (broker == nullptr) {
      return "retry";
    }
    if (broker->connected) {
      return "up";
    }
    if (_network == nullptr || !_network->isWifiConnected() || !_network->hasTimeSync()) {
      return "wait";
    }
    if (broker->client != nullptr) {
      return "conn";
    }
    if (broker->next_connect_attempt != 0 && broker->next_connect_attempt > millis()) {
      return "backoff";
    }
    return "retry";
  };

  snprintf(reply, reply_size, "> wifi:%s ntp:%s iata:%s eastmesh-au:%s letsmesh-eu:%s letsmesh-us:%s status:%s tx:%s",
           (_network != nullptr && _network->isWifiConnected()) ? "up" : "down",
           (_network != nullptr && _network->hasTimeSync()) ? "up" : "wait",
           _prefs.iata,
           broker_state(kEastmeshBit), broker_state(kLetsmeshEuBit), broker_state(kLetsmeshUsBit),
           _prefs.status_enabled ? "on" : "off", _prefs.tx_enabled ? "on" : "off");
}

bool MQTTUplink::setEndpointEnabled(uint8_t bit, bool enabled) {
  uint8_t next_mask = _prefs.enabled_mask & 0x07;
  if (enabled) {
    next_mask = normalizeEnabledMask(next_mask | bit);
    if ((next_mask & bit) == 0) {
      return false;
    }
  } else {
    next_mask &= ~bit;
  }
  _prefs.enabled_mask = next_mask;
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
  if (strcmp(cleaned, MQTT_UNSET_IATA) == 0) {
    StrHelper::strncpy(_prefs.iata, MQTT_UNSET_IATA, sizeof(_prefs.iata));
    refreshIdentityStrings();
    return savePrefs();
  }
  StrHelper::strncpy(_prefs.iata, cleaned, sizeof(_prefs.iata));
  refreshIdentityStrings();
  return savePrefs();
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

bool MQTTUplink::isAnyBrokerConnected() const {
  for (const BrokerState& broker : _brokers) {
    if (broker.spec != nullptr && broker.connected) {
      return true;
    }
  }
  return false;
}

const char* MQTTUplink::getAggregateBrokerState() const {
  uint8_t enabled_count = 0;
  uint8_t connected_count = 0;

  for (const BrokerState& broker : _brokers) {
    if (broker.spec == nullptr || (broker.spec->bit & _prefs.enabled_mask) == 0) {
      continue;
    }
    enabled_count++;
    if (broker.connected) {
      connected_count++;
    }
  }

  if (enabled_count == 0 || connected_count == 0) {
    return "down";
  }
  if (connected_count < enabled_count) {
    return "degraded";
  }
  return "up";
}

#else

MQTTUplink::MQTTUplink(mesh::RTCClock&, mesh::LocalIdentity&)
    : _fs(nullptr), _rtc(nullptr), _identity(nullptr), _running(false), _last_status_publish(0),
      _token_refresh_count(0), _token_refresh_active_until_ms(0), _last_status{}, _node_name(nullptr), _network(nullptr) {
  MQTTPrefsStore::setDefaults(_prefs);
}

bool MQTTUplink::savePrefs() { return false; }
void MQTTUplink::begin(FILESYSTEM*) {}
void MQTTUplink::end() {}
void MQTTUplink::loop(const MQTTStatusSnapshot&) {}
void MQTTUplink::publishPacket(const mesh::Packet&, bool, int, float, int, int) {}
void MQTTUplink::formatStatusReply(char* reply, size_t reply_size) const { snprintf(reply, reply_size, "> unsupported"); }
bool MQTTUplink::setEndpointEnabled(uint8_t, bool) { return false; }
bool MQTTUplink::isEndpointEnabled(uint8_t) const { return false; }
bool MQTTUplink::setPacketsEnabled(bool) { return false; }
bool MQTTUplink::setRawEnabled(bool) { return false; }
bool MQTTUplink::setStatusEnabled(bool) { return false; }
bool MQTTUplink::setTxEnabled(bool) { return false; }
bool MQTTUplink::setIata(const char*) { return false; }
bool MQTTUplink::isActive() const { return false; }
bool MQTTUplink::setOwnerPublicKey(const char*) { return false; }
bool MQTTUplink::setOwnerEmail(const char*) { return false; }
bool MQTTUplink::sendStatusNow() { return false; }
bool MQTTUplink::isAnyBrokerConnected() const { return false; }
const char* MQTTUplink::getAggregateBrokerState() const { return "down"; }
bool MQTTUplink::isTokenRefreshInProgress() const { return false; }

#endif

#endif
