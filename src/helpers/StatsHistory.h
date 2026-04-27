#pragma once

#include <Arduino.h>
#include <MeshCore.h>

#include "ArchiveStorage.h"

struct HistorySample {
  uint32_t epoch_secs;
  uint32_t uptime_secs;
  uint32_t packets_sent;
  uint32_t packets_recv;
  uint32_t heap_free;
  uint32_t heap_min;
  uint32_t psram_free;
  uint32_t psram_min;
  uint16_t battery_mv;
  uint16_t queue_len;
  uint16_t error_flags;
  uint16_t recv_errors;
  uint16_t neighbour_count;
  uint16_t direct_dups;
  uint16_t flood_dups;
  int16_t last_rssi_x4;
  int16_t last_snr_x4;
  int16_t noise_floor;
  uint16_t supply_voltage_centi_v;
  int16_t sensor_temp_deci_c;
  int16_t mcu_temp_deci_c;
  uint16_t humidity_deci_pct;
  uint16_t pressure_deci_hpa;
  int16_t pressure_altitude_m;
  int16_t gps_altitude_m;
  int32_t gps_lat_e6;
  int32_t gps_lon_e6;
  uint16_t sensor_flags;
  uint8_t gps_satellites;
  uint8_t flags;
  int8_t battery_pct;
  uint8_t reserved;
};

struct HistoryEvent {
  uint32_t epoch_secs;
  uint32_t uptime_secs;
  uint8_t type;
  int16_t value;
  uint8_t reserved;
};

enum HistorySampleFlags : uint8_t {
  HISTORY_FLAG_EXTERNAL_POWER = 1 << 0,
  HISTORY_FLAG_CHARGING = 1 << 1,
  HISTORY_FLAG_VBUS = 1 << 2,
  HISTORY_FLAG_WIFI_CONNECTED = 1 << 3,
  HISTORY_FLAG_MQTT_CONNECTED = 1 << 4,
  HISTORY_FLAG_WEB_ENABLED = 1 << 5,
  HISTORY_FLAG_WEB_PANEL_UP = 1 << 6,
  HISTORY_FLAG_ARCHIVE_MOUNTED = 1 << 7,
};

enum HistorySampleSensorFlags : uint16_t {
  HISTORY_SENSOR_SUPPLY_VOLTAGE = 1 << 0,
  HISTORY_SENSOR_TEMP = 1 << 1,
  HISTORY_SENSOR_MCU_TEMP = 1 << 2,
  HISTORY_SENSOR_HUMIDITY = 1 << 3,
  HISTORY_SENSOR_PRESSURE = 1 << 4,
  HISTORY_SENSOR_PRESSURE_ALTITUDE = 1 << 5,
  HISTORY_SENSOR_GPS_PRESENT = 1 << 6,
  HISTORY_SENSOR_GPS_ENABLED = 1 << 7,
  HISTORY_SENSOR_GPS_FIX = 1 << 8,
  HISTORY_SENSOR_GPS_LAT = 1 << 9,
  HISTORY_SENSOR_GPS_LON = 1 << 10,
  HISTORY_SENSOR_GPS_ALTITUDE = 1 << 11,
  HISTORY_SENSOR_GPS_SATELLITES = 1 << 12,
};

enum HistoryEventType : uint8_t {
  HISTORY_EVENT_BOOT = 1,
  HISTORY_EVENT_WEB_STARTED = 2,
  HISTORY_EVENT_WEB_STOPPED = 3,
  HISTORY_EVENT_MQTT_CONNECTED = 4,
  HISTORY_EVENT_MQTT_DISCONNECTED = 5,
  HISTORY_EVENT_ARCHIVE_MOUNTED = 6,
  HISTORY_EVENT_ARCHIVE_UNAVAILABLE = 7,
  HISTORY_EVENT_LOW_MEMORY = 8,
  HISTORY_EVENT_STATS_ENABLED = 9,
  HISTORY_EVENT_STATS_DISABLED = 10,
  HISTORY_EVENT_MQTT_TOKEN_REFRESHED = 11,
};

class StatsHistory {
public:
  static constexpr uint32_t kSampleIntervalSecs = 60;
  static constexpr uint32_t kArchiveSummaryIntervalSecs = 300;
  static constexpr uint32_t kLiveOnlyIdleTimeoutMs = 2UL * 60UL * 1000UL;

  StatsHistory();
  ~StatsHistory();

  void begin(bool enabled, ArchiveStorage* archive);
  void setArchive(ArchiveStorage* archive);
  void setEnabled(bool enabled);

  bool isEnabled() const { return _enabled; }
  bool isRecentHistoryAvailable() const { return _samples != nullptr; }
  bool isPsramBacked() const { return _psram_backed; }
  bool isDegraded() const { return _degraded; }
  bool isLiveOnly() const { return _live_only; }
  bool isArchiveAvailable() const;
  bool hasArchiveRestore() const { return _restored_sample_count > 0; }
  bool isBootAutoCaptureExpected() const;
  uint32_t getDetectedPsramSizeBytes() const;

  size_t getSampleCapacity() const { return _sample_capacity; }
  size_t getSampleCount() const { return _sample_count; }
  size_t getEventCapacity() const { return _event_capacity; }
  size_t getEventCount() const { return _event_count; }
  size_t getRestoredSampleCount() const { return _restored_sample_count; }

  void pushSample(const HistorySample& sample);
  void recordEvent(uint8_t type, uint32_t epoch_secs, uint32_t uptime_secs, int16_t value = 0);
  void maybeFlush(uint32_t now_ms);
  void noteAccess(uint32_t now_ms);
  void maybeReleaseIdleBuffers(uint32_t now_ms);

  bool buildSeriesJson(const char* series, char* buffer, size_t buffer_size, uint32_t now_epoch_secs, uint32_t now_uptime_secs) const;
  bool getRecentEvent(size_t reverse_index, HistoryEvent& event) const;

  static const char* getEventTypeName(uint8_t type);
  static constexpr uint32_t getSampleIntervalSecs() { return kSampleIntervalSecs; }
  static constexpr uint32_t getArchiveSummaryIntervalSecs() { return kArchiveSummaryIntervalSecs; }

private:
  bool activate();
  bool ensureBuffers();
  void releaseBuffers();
  bool supportsPersistence() const;
  bool shouldAutoActivateFromBoot() const;
  bool isAccessActive(uint32_t now_ms) const;
  bool restoreSummaryLog();
  bool restoreEventsLog();
  bool parseSummaryLine(const char* line, HistorySample& sample) const;
  bool parseEventLine(const char* line, HistoryEvent& event) const;
  void storeSample(const HistorySample& sample, bool mark_dirty);
  void storeEvent(const HistoryEvent& event, bool queue_pending);
  void appendPendingEvent(const HistoryEvent& event);
  void flushSummaryLog();
  void flushEventsLog();
  void writeMetaFile() const;
  bool getSampleFromOldest(size_t index, HistorySample& sample) const;

  bool _enabled;
  bool _activated;
  bool _psram_backed;
  bool _degraded;
  bool _live_only;
  bool _summary_dirty;
  uint32_t _next_summary_flush_ms;
  uint32_t _next_event_flush_ms;
  uint32_t _last_access_ms;
  size_t _sample_capacity;
  size_t _sample_head;
  size_t _sample_count;
  size_t _event_capacity;
  size_t _event_head;
  size_t _event_count;
  HistorySample* _samples;
  HistoryEvent* _events;
  ArchiveStorage* _archive;
  size_t _restored_sample_count;
  HistoryEvent _pending_events[16];
  size_t _pending_event_count;
};
