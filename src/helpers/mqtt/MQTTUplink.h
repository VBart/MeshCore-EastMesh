#pragma once

#ifdef WITH_MQTT_UPLINK

#include <Mesh.h>
#include <helpers/NetworkStateProvider.h>
#include <target.h>

#include "JWTHelper.h"
#include "MQTTPrefs.h"

#if defined(ESP_PLATFORM)
#include <mqtt_client.h>
#endif

struct MQTTStatusSnapshot {
  int battery_mv;
  uint32_t uptime_secs;
  uint16_t error_flags;
  uint16_t queue_len;
  int noise_floor;
  uint32_t tx_air_secs;
  uint32_t rx_air_secs;
  uint32_t recv_errors;
  float radio_freq;
  float radio_bw;
  uint8_t radio_sf;
  uint8_t radio_cr;
};

class MQTTUplink {
public:
  explicit MQTTUplink(mesh::RTCClock& rtc, mesh::LocalIdentity& identity);

  void begin(FILESYSTEM* fs);
  void end();
  void loop(const MQTTStatusSnapshot& snapshot);
  void publishPacket(const mesh::Packet& packet, bool is_tx, int rssi, float snr, int score = -1,
                     int duration = -1);

  bool isRunning() const { return _running; }
  bool isActive() const;

  void formatStatusReply(char* reply, size_t reply_size) const;

  bool setEndpointEnabled(uint8_t bit, bool enabled);
  bool isEndpointEnabled(uint8_t bit) const;
  bool setPacketsEnabled(bool enabled);
  bool isPacketsEnabled() const { return _prefs.packets_enabled != 0; }
  bool setRawEnabled(bool enabled);
  bool isRawEnabled() const { return _prefs.raw_enabled != 0; }
  bool setStatusEnabled(bool enabled);
  bool isStatusEnabled() const { return _prefs.status_enabled != 0; }
  bool setTxEnabled(bool enabled);
  bool isTxEnabled() const { return _prefs.tx_enabled != 0; }
  bool setIata(const char* iata);
  const char* getIata() const { return _prefs.iata; }
  const char* getClientVersion() const;
  void setNodeNameSource(const char* node_name) { _node_name = node_name; }
  bool setOwnerPublicKey(const char* owner_public_key);
  const char* getOwnerPublicKey() const { return _prefs.owner_public_key; }
  bool setOwnerEmail(const char* owner_email);
  const char* getOwnerEmail() const { return _prefs.owner_email; }
  bool sendStatusNow();
  bool isAnyBrokerConnected() const;
  const char* getAggregateBrokerState() const;
  void setNetworkStateProvider(NetworkStateProvider* network) { _network = network; }
  uint32_t getTokenRefreshCount() const { return _token_refresh_count; }
  bool isTokenRefreshInProgress() const;

private:
#if defined(ESP_PLATFORM)
  struct BrokerSpec {
    const char* key;
    const char* label;
    const char* host;
    const char* uri;
    uint8_t bit;
  };

  struct BrokerState {
    const BrokerSpec* spec;
    esp_mqtt_client_handle_t client;
    bool connected;
    bool connect_announced;
    bool reconnect_pending;
    unsigned long last_connect_attempt;
    unsigned long next_connect_attempt;
    unsigned long connected_since_ms;
    time_t token_expires_at;
    uint8_t reconnect_failures;
    char username[70];
    char* token;
    char client_id[48];
    char status_topic[128];
    char offline_payload[256];
  };
#endif

  FILESYSTEM* _fs;
  mesh::RTCClock* _rtc;
  mesh::LocalIdentity* _identity;
  MQTTPrefs _prefs;
  bool _running;
  unsigned long _last_status_publish;
  uint32_t _token_refresh_count;
  unsigned long _token_refresh_active_until_ms;
  MQTTStatusSnapshot _last_status;
  char _device_id[65];
  const char* _node_name;
  NetworkStateProvider* _network;

#if defined(ESP_PLATFORM)
  static constexpr uint8_t kEastmeshBit = 0x01;
  static constexpr uint8_t kLetsmeshEuBit = 0x02;
  static constexpr uint8_t kLetsmeshUsBit = 0x04;
  static constexpr uint8_t kMaxEnabledBrokers = 2;
  static const BrokerSpec kBrokerSpecs[3];
  static bool isUnsetIataValue(const char* iata);

  BrokerState _brokers[3];

  static void handleMqttEvent(void* handler_args, esp_event_base_t base, int32_t event_id, void* event_data);
  bool hasEnabledBroker() const;
  static uint8_t normalizeEnabledMask(uint8_t mask);
  void formatTopic(char* dst, size_t dst_size, const char* leaf) const;
  void refreshIdentityStrings();
  void refreshBrokerIdentity(BrokerState& broker);
  void refreshBrokerState(BrokerState& broker);
  void ensureBroker(BrokerState& broker, bool allow_new_connect);
  void destroyBroker(BrokerState& broker, bool reset_retry_state = true);
  bool refreshToken(BrokerState& broker);
  void publishStatus(bool online);
  void publishOnlineStatus(BrokerState& broker);
  void queuePublish(BrokerState& broker, const char* topic, const char* payload, bool retain);
  int buildStatusJson(char* buffer, size_t buffer_size, bool online) const;
  int buildPacketJson(char* buffer, size_t buffer_size, const mesh::Packet& packet, bool is_tx, int rssi, float snr,
                      int score, int duration) const;
  int buildRawJson(char* buffer, size_t buffer_size, const mesh::Packet& packet, bool is_tx, int rssi, float snr) const;
  static void escapeJsonString(const char* input, char* output, size_t output_size);
  static void makeSafeToken(const char* input, char* output, size_t output_size);
  static void bytesToHexUpper(const uint8_t* src, size_t len, char* dst, size_t dst_size);
  static void formatIsoTimestamp(time_t ts, char* dst, size_t dst_size);
#endif

  bool savePrefs();
};

#endif
