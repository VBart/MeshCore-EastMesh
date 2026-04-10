#pragma once

#ifdef WITH_MQTT_UPLINK

#include <Mesh.h>
#include <helpers/CommonCLI.h>
#include <helpers/web/WebPanelServer.h>
#include <target.h>

#include "JWTHelper.h"
#include "MQTTPrefs.h"

#if defined(ESP_PLATFORM)
#include <WiFi.h>
#if !defined(WITH_WEB_PANEL)
  #define WITH_WEB_PANEL 1
#endif
#include <mqtt_client.h>
#endif

using MQTTWebCommandRunner = WebPanelCommandRunner;

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
  void publishPacket(const mesh::Packet& packet, bool is_tx, int rssi, float snr);

  bool isRunning() const { return _running; }
  bool isActive() const;

  void formatStatusReply(char* reply, size_t reply_size) const;
  void formatWebStatusReply(char* reply, size_t reply_size) const;

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
  bool setWebEnabled(bool enabled);
  bool isWebEnabled() const { return _prefs.web_enabled != 0; }
  bool setIata(const char* iata);
  const char* getIata() const { return _prefs.iata; }
  void setNodeNameSource(const char* node_name) { _node_name = node_name; }
  void setWebCommandRunner(MQTTWebCommandRunner* runner);
  bool setWifiSSID(const char* ssid);
  bool setWifiPassword(const char* pwd);
  const char* getWifiSSID() const { return _prefs.wifi_ssid; }
  bool setWifiPowerSave(const char* mode);
  const char* getWifiPowerSave() const;
  bool setOwnerPublicKey(const char* owner_public_key);
  const char* getOwnerPublicKey() const { return _prefs.owner_public_key; }
  bool setOwnerEmail(const char* owner_email);
  const char* getOwnerEmail() const { return _prefs.owner_email; }
  void formatWifiStatusReply(char* reply, size_t reply_size) const;
  void reconnectWifi();
  bool sendStatusNow();

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
    unsigned long last_connect_attempt;
    time_t token_expires_at;
    char username[70];
    char token[512];
    char client_id[48];
    char status_topic[128];
    char packets_topic[128];
    char raw_topic[128];
    char offline_payload[256];
  };
#endif

  FILESYSTEM* _fs;
  mesh::RTCClock* _rtc;
  mesh::LocalIdentity* _identity;
  MQTTPrefs _prefs;
  bool _running;
  bool _wifi_started;
  bool _sntp_started;
  bool _have_time_sync;
  unsigned long _last_wifi_attempt;
  unsigned long _last_status_publish;
  MQTTStatusSnapshot _last_status;
  char _device_id[65];
  const char* _node_name;
  MQTTWebCommandRunner* _web_runner;
  WebPanelServer _web_panel;

#if defined(ESP_PLATFORM)
  static constexpr uint8_t kEastmeshBit = 0x01;
  static constexpr uint8_t kLetsmeshEuBit = 0x02;
  static constexpr uint8_t kLetsmeshUsBit = 0x04;
  static const BrokerSpec kBrokerSpecs[3];

  BrokerState _brokers[3];

  static void handleMqttEvent(void* handler_args, esp_event_base_t base, int32_t event_id, void* event_data);
  void ensureWebServer();
  void stopWebServer();
  void ensureWifi();
  void updateTimeSync();
  bool hasEnabledBroker() const;
  void refreshIdentityStrings();
  void refreshBrokerState(BrokerState& broker);
  void ensureBroker(BrokerState& broker);
  void destroyBroker(BrokerState& broker);
  bool refreshToken(BrokerState& broker);
  void publishStatus(bool online);
  void publishOnlineStatus(BrokerState& broker);
  void queuePublish(BrokerState& broker, const char* topic, const char* payload, bool retain);
  int buildStatusJson(char* buffer, size_t buffer_size, bool online) const;
  int buildPacketJson(char* buffer, size_t buffer_size, const mesh::Packet& packet, bool is_tx, int rssi,
                      float snr) const;
  int buildRawJson(char* buffer, size_t buffer_size, const mesh::Packet& packet, bool is_tx, int rssi, float snr) const;
  static wifi_ps_type_t toEspPowerSave(uint8_t mode);
  static const char* getPowerSaveLabel(uint8_t mode);
  static void escapeJsonString(const char* input, char* output, size_t output_size);
  static void makeSafeToken(const char* input, char* output, size_t output_size);
  static void bytesToHexUpper(const uint8_t* src, size_t len, char* dst, size_t dst_size);
  static void formatIsoTimestamp(time_t ts, char* dst, size_t dst_size);
#endif

  bool savePrefs();
};

#endif
