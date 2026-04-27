#include "MyMesh.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <helpers/sensors/LPPDataHelpers.h>

#if defined(TBEAM_1W)
  #include <TBeam1WBoard.h>
#endif

#if defined(ESP32) && WITH_WEB_PANEL
  #include <WiFi.h>
#endif

#ifndef ARCHIVE_DEBUG
  #if defined(MQTT_DEBUG) && MQTT_DEBUG
    #define ARCHIVE_DEBUG 1
  #else
    #define ARCHIVE_DEBUG 0
  #endif
#endif

#if ARCHIVE_DEBUG
  #define ARCHIVE_LOG(fmt, ...) Serial.printf("[ARCHIVE] " fmt "\n", ##__VA_ARGS__)
#else
  #define ARCHIVE_LOG(...) do { } while (0)
#endif

namespace {

int clampBatteryPercentFromRange(uint16_t battery_mv, uint16_t min_mv, uint16_t max_mv) {
  if (max_mv <= min_mv) {
    return 0;
  }
  const long scaled = (static_cast<long>(battery_mv) - static_cast<long>(min_mv)) * 100L /
                      static_cast<long>(max_mv - min_mv);
  return std::max(0L, std::min(100L, scaled));
}

WebSensorSnapshot collectWebSensorSnapshot(mesh::MainBoard& board, SensorManager& sensors, uint16_t battery_mv) {
  WebSensorSnapshot snapshot;
  snapshot.has_battery = true;
  snapshot.battery_mv = battery_mv;

  CayenneLPP sensor_telemetry(200);
  sensor_telemetry.reset();
  sensor_telemetry.addVoltage(TELEM_CHANNEL_SELF, static_cast<float>(battery_mv) / 1000.0f);
  sensors.querySensors(0xFF, sensor_telemetry);

  const float board_temp_c = board.getMCUTemperature();
  if (!isnan(board_temp_c)) {
    sensor_telemetry.addTemperature(TELEM_CHANNEL_SELF, board_temp_c);
  }

  LocationProvider* location = sensors.getLocationProvider();
  if (location != nullptr) {
    snapshot.has_gps = true;
    snapshot.gps_enabled = location->isEnabled();
    snapshot.gps_fix = location->isValid();
    const long satellites = location->satellitesCount();
    if (snapshot.gps_enabled || satellites > 0) {
      snapshot.has_satellites = true;
      snapshot.satellites = max<long>(satellites, 0);
    }
    if (snapshot.gps_fix) {
      snapshot.has_gps_lat = true;
      snapshot.gps_lat = static_cast<float>(location->getLatitude()) / 1000000.0f;
      snapshot.has_gps_lon = true;
      snapshot.gps_lon = static_cast<float>(location->getLongitude()) / 1000000.0f;
      snapshot.has_gps_altitude = true;
      snapshot.gps_altitude_m = static_cast<float>(location->getAltitude()) / 1000.0f;
    }
  }

  float temperatures[4] = {NAN, NAN, NAN, NAN};
  size_t temperature_count = 0;

  LPPReader reader(sensor_telemetry.getBuffer(), sensor_telemetry.getSize());
  uint8_t channel = 0;
  uint8_t type = 0;
  while (reader.readHeader(channel, type)) {
    float value = NAN;
    switch (type) {
      case LPP_GPS: {
        float lat = NAN;
        float lon = NAN;
        float alt = NAN;
        if (reader.readGPS(lat, lon, alt) && !snapshot.has_gps && std::isfinite(lat) && std::isfinite(lon) && std::isfinite(alt)) {
          snapshot.has_gps = true;
          snapshot.gps_enabled = true;
          snapshot.gps_fix = true;
          snapshot.has_gps_lat = true;
          snapshot.gps_lat = lat;
          snapshot.has_gps_lon = true;
          snapshot.gps_lon = lon;
          snapshot.has_gps_altitude = true;
          snapshot.gps_altitude_m = alt;
        }
        break;
      }
      case LPP_VOLTAGE:
        if (reader.readVoltage(value) && channel == TELEM_CHANNEL_SELF && !snapshot.has_supply_voltage) {
          snapshot.has_supply_voltage = std::isfinite(value);
          snapshot.supply_voltage_v = value;
        }
        break;
      case LPP_TEMPERATURE:
        if (reader.readTemperature(value) && temperature_count < 4 && std::isfinite(value)) {
          temperatures[temperature_count++] = value;
        }
        break;
      case LPP_RELATIVE_HUMIDITY:
        if (reader.readRelativeHumidity(value) && !snapshot.has_humidity) {
          snapshot.has_humidity = std::isfinite(value);
          snapshot.humidity_pct = value;
        }
        break;
      case LPP_BAROMETRIC_PRESSURE:
        if (reader.readPressure(value) && !snapshot.has_pressure) {
          snapshot.has_pressure = std::isfinite(value);
          snapshot.pressure_hpa = value;
        }
        break;
      case LPP_ALTITUDE:
        if (reader.readAltitude(value) && !snapshot.has_pressure_altitude) {
          snapshot.has_pressure_altitude = std::isfinite(value);
          snapshot.pressure_altitude_m = value;
        }
        break;
      default:
        reader.skipData(type);
        break;
    }
  }

  if (temperature_count >= 2) {
    snapshot.has_sensor_temp = true;
    snapshot.sensor_temp_c = temperatures[0];
    snapshot.has_mcu_temp = true;
    snapshot.mcu_temp_c = temperatures[temperature_count - 1];
  } else if (temperature_count == 1) {
    if (snapshot.has_humidity || snapshot.has_pressure || snapshot.has_pressure_altitude) {
      snapshot.has_sensor_temp = true;
      snapshot.sensor_temp_c = temperatures[0];
    } else {
      snapshot.has_mcu_temp = true;
      snapshot.mcu_temp_c = temperatures[0];
    }
  }

  return snapshot;
}

bool appendJsonBoolField(char* reply, size_t reply_size, size_t& offset, bool& needs_comma, const char* key, bool value) {
  const int written = snprintf(&reply[offset], reply_size - offset, "%s\"%s\":%s", needs_comma ? "," : "", key, value ? "true" : "false");
  if (written < 0 || static_cast<size_t>(written) >= (reply_size - offset)) {
    return false;
  }
  offset += static_cast<size_t>(written);
  needs_comma = true;
  return true;
}

bool appendJsonLongField(char* reply, size_t reply_size, size_t& offset, bool& needs_comma, const char* key, long value) {
  const int written = snprintf(&reply[offset], reply_size - offset, "%s\"%s\":%ld", needs_comma ? "," : "", key, value);
  if (written < 0 || static_cast<size_t>(written) >= (reply_size - offset)) {
    return false;
  }
  offset += static_cast<size_t>(written);
  needs_comma = true;
  return true;
}

bool appendJsonFloatField(char* reply, size_t reply_size, size_t& offset, bool& needs_comma, const char* key, float value, int precision) {
  if (!std::isfinite(value)) {
    return true;
  }
  const int written = snprintf(&reply[offset], reply_size - offset, "%s\"%s\":%.*f", needs_comma ? "," : "", key, precision, value);
  if (written < 0 || static_cast<size_t>(written) >= (reply_size - offset)) {
    return false;
  }
  offset += static_cast<size_t>(written);
  needs_comma = true;
  return true;
}

constexpr unsigned long kArchiveNeighboursFlushIntervalMs = 60UL * 1000UL;
constexpr const char* kArchiveNeighboursSnapshotPath = "/stats/neighbours.snapshot";
constexpr const char* kArchiveNeighboursLatestPath = "/stats/neighbours.latest";

bool buildUtcDailyArchivePath(const char* prefix, uint32_t epoch_secs, char* path, size_t path_size) {
  if (prefix == nullptr || path == nullptr || path_size == 0) {
    return false;
  }
  if (epoch_secs == 0) {
    snprintf(path, path_size, "/stats/%s-unknown.log", prefix);
    return true;
  }
  const DateTime dt(epoch_secs);
  snprintf(path, path_size, "/stats/%s-%04d-%02d-%02d.log", prefix, dt.year(), dt.month(), dt.day());
  return true;
}

File openArchiveWrite(FILESYSTEM* fs, const char* filename) {
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  fs->remove(filename);
  return fs->open(filename, FILE_O_WRITE);
#elif defined(RP2040_PLATFORM)
  return fs->open(filename, "w");
#else
  fs->remove(filename);
  return fs->open(filename, FILE_WRITE);
#endif
}

File openArchiveRead(FILESYSTEM* fs, const char* filename) {
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM) || defined(RP2040_PLATFORM)
  return fs->open(filename, "r");
#else
  return fs->open(filename, FILE_READ);
#endif
}

File openArchiveWriteWithRecovery(ArchiveStorage* archive, const char* filename) {
  if (archive == nullptr) {
    return File();
  }
  FILESYSTEM* fs = archive->getFS();
  if (fs == nullptr) {
    return File();
  }
  File file = openArchiveWrite(fs, filename);
  if (file) {
    return file;
  }
  if (!archive->recover()) {
    return File();
  }
  fs = archive->getFS();
  return fs != nullptr ? openArchiveWrite(fs, filename) : File();
}

File openArchiveAppend(FILESYSTEM* fs, const char* filename) {
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM) || defined(RP2040_PLATFORM)
  return fs->open(filename, "a");
#else
  return fs->open(filename, FILE_APPEND, true);
#endif
}

File openArchiveAppendWithRecovery(ArchiveStorage* archive, const char* filename) {
  if (archive == nullptr) {
    return File();
  }
  FILESYSTEM* fs = archive->getFS();
  if (fs == nullptr) {
    return File();
  }
  File file = openArchiveAppend(fs, filename);
  if (file) {
    return file;
  }
  if (!archive->recover()) {
    return File();
  }
  fs = archive->getFS();
  return fs != nullptr ? openArchiveAppend(fs, filename) : File();
}

File openArchiveReadWithRecovery(ArchiveStorage* archive, const char* filename) {
  if (archive == nullptr) {
    return File();
  }
  FILESYSTEM* fs = archive->getFS();
  if (fs == nullptr) {
    return File();
  }
  File file = openArchiveRead(fs, filename);
  if (file) {
    return file;
  }
  if (!archive->recover()) {
    return File();
  }
  fs = archive->getFS();
  return fs != nullptr ? openArchiveRead(fs, filename) : File();
}

void escapeJsonString(const char* input, char* output, size_t output_size) {
  if (output == nullptr || output_size == 0) {
    return;
  }

  size_t oi = 0;
  for (size_t i = 0; input != nullptr && input[i] != 0 && oi + 1 < output_size; ++i) {
    const char c = input[i];
    const char* escape = nullptr;
    switch (c) {
      case '\\':
        escape = "\\\\";
        break;
      case '"':
        escape = "\\\"";
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
      while (*escape != 0 && oi + 1 < output_size) {
        output[oi++] = *escape++;
      }
    } else {
      output[oi++] = c;
    }
  }
  output[oi] = 0;
}

}  // namespace

/* ------------------------------ Config -------------------------------- */

#ifndef LORA_FREQ
  #define LORA_FREQ 915.0
#endif
#ifndef LORA_BW
  #define LORA_BW 250
#endif
#ifndef LORA_SF
  #define LORA_SF 10
#endif
#ifndef LORA_CR
  #define LORA_CR 5
#endif
#ifndef LORA_TX_POWER
  #define LORA_TX_POWER 20
#endif

#ifndef ADVERT_NAME
  #define ADVERT_NAME "repeater"
#endif
#ifndef ADVERT_LAT
  #define ADVERT_LAT 0.0
#endif
#ifndef ADVERT_LON
  #define ADVERT_LON 0.0
#endif

#ifndef ADMIN_PASSWORD
  #define ADMIN_PASSWORD "password"
#endif

#ifndef SERVER_RESPONSE_DELAY
  #define SERVER_RESPONSE_DELAY 300
#endif

#ifndef TXT_ACK_DELAY
  #define TXT_ACK_DELAY 200
#endif

#define FIRMWARE_VER_LEVEL       2

#define REQ_TYPE_GET_STATUS         0x01 // same as _GET_STATS
#define REQ_TYPE_KEEP_ALIVE         0x02
#define REQ_TYPE_GET_TELEMETRY_DATA 0x03
#define REQ_TYPE_GET_ACCESS_LIST    0x05
#define REQ_TYPE_GET_NEIGHBOURS     0x06
#define REQ_TYPE_GET_OWNER_INFO     0x07     // FIRMWARE_VER_LEVEL >= 2

#define RESP_SERVER_LOGIN_OK        0 // response to ANON_REQ

#define ANON_REQ_TYPE_REGIONS      0x01
#define ANON_REQ_TYPE_OWNER        0x02
#define ANON_REQ_TYPE_BASIC        0x03   // just remote clock

#define CLI_REPLY_DELAY_MILLIS      600

#define LAZY_CONTACTS_WRITE_DELAY    5000

void MyMesh::putNeighbour(const mesh::Identity &id, uint32_t timestamp, float snr) {
#if MAX_NEIGHBOURS // check if neighbours enabled
  // find existing neighbour, else use least recently updated
  uint32_t oldest_timestamp = 0xFFFFFFFF;
  NeighbourInfo *neighbour = &neighbours[0];
  for (int i = 0; i < MAX_NEIGHBOURS; i++) {
    // if neighbour already known, we should update it
    if (id.matches(neighbours[i].id)) {
      neighbour = &neighbours[i];
      break;
    }

    // otherwise we should update the least recently updated neighbour
    if (neighbours[i].heard_timestamp < oldest_timestamp) {
      neighbour = &neighbours[i];
      oldest_timestamp = neighbour->heard_timestamp;
    }
  }

  // update neighbour info
  neighbour->id = id;
  neighbour->advert_timestamp = timestamp;
  neighbour->heard_timestamp = getRTCClock()->getCurrentTime();
  neighbour->snr = (int8_t)(snr * 4);
  _archive_neighbours_dirty = true;
#endif
}

uint8_t MyMesh::handleLoginReq(const mesh::Identity& sender, const uint8_t* secret, uint32_t sender_timestamp, const uint8_t* data, bool is_flood) {
  ClientInfo* client = NULL;
  if (data[0] == 0) {   // blank password, just check if sender is in ACL
    client = acl.getClient(sender.pub_key, PUB_KEY_SIZE);
    if (client == NULL) {
    #if MESH_DEBUG
      MESH_DEBUG_PRINTLN("Login, sender not in ACL");
    #endif
    }
  }
  if (client == NULL) {
    uint8_t perms;
    if (strcmp((char *)data, _prefs.password) == 0) { // check for valid admin password
      perms = PERM_ACL_ADMIN;
    } else if (strcmp((char *)data, _prefs.guest_password) == 0) { // check guest password
      perms = PERM_ACL_GUEST;
    } else {
#if MESH_DEBUG
      MESH_DEBUG_PRINTLN("Invalid password: %s", data);
#endif
      return 0;
    }

    client = acl.putClient(sender, 0);  // add to contacts (if not already known)
    if (sender_timestamp <= client->last_timestamp) {
      MESH_DEBUG_PRINTLN("Possible login replay attack!");
      return 0;  // FATAL: client table is full -OR- replay attack
    }

    MESH_DEBUG_PRINTLN("Login success!");
    client->last_timestamp = sender_timestamp;
    client->last_activity = getRTCClock()->getCurrentTime();
    client->permissions &= ~0x03;
    client->permissions |= perms;
    memcpy(client->shared_secret, secret, PUB_KEY_SIZE);

    if (perms != PERM_ACL_GUEST) {   // keep number of FS writes to a minimum
      dirty_contacts_expiry = futureMillis(LAZY_CONTACTS_WRITE_DELAY);
    }
  }

  if (is_flood) {
    client->out_path_len = OUT_PATH_UNKNOWN;  // need to rediscover out_path
  }

  uint32_t now = getRTCClock()->getCurrentTimeUnique();
  memcpy(reply_data, &now, 4);   // response packets always prefixed with timestamp
  reply_data[4] = RESP_SERVER_LOGIN_OK;
  reply_data[5] = 0;  // Legacy: was recommended keep-alive interval (secs / 16)
  reply_data[6] = client->isAdmin() ? 1 : 0;
  reply_data[7] = client->permissions;
  getRNG()->random(&reply_data[8], 4);   // random blob to help packet-hash uniqueness
  reply_data[12] = FIRMWARE_VER_LEVEL;  // New field

  return 13;  // reply length
}

uint8_t MyMesh::handleAnonRegionsReq(const mesh::Identity& sender, uint32_t sender_timestamp, const uint8_t* data) {
  if (anon_limiter.allow(rtc_clock.getCurrentTime())) {
    // request data has: {reply-path-len}{reply-path}
    reply_path_len = *data & 63;
    reply_path_hash_size = (*data >> 6) + 1;
    data++;

    memcpy(reply_path, data, ((uint8_t)reply_path_len) * reply_path_hash_size);
    // data += (uint8_t)reply_path_len * reply_path_hash_size;

    memcpy(reply_data, &sender_timestamp, 4);   // prefix with sender_timestamp, like a tag
    uint32_t now = getRTCClock()->getCurrentTime();
    memcpy(&reply_data[4], &now, 4);     // include our clock (for easy clock sync, and packet hash uniqueness)

    return 8 + region_map.exportNamesTo((char *) &reply_data[8], sizeof(reply_data) - 12, REGION_DENY_FLOOD);   // reply length
  }
  return 0;
}

uint8_t MyMesh::handleAnonOwnerReq(const mesh::Identity& sender, uint32_t sender_timestamp, const uint8_t* data) {
  if (anon_limiter.allow(rtc_clock.getCurrentTime())) {
    // request data has: {reply-path-len}{reply-path}
    reply_path_len = *data & 63;
    reply_path_hash_size = (*data >> 6) + 1;
    data++;

    memcpy(reply_path, data, ((uint8_t)reply_path_len) * reply_path_hash_size);
    // data += (uint8_t)reply_path_len * reply_path_hash_size;

    memcpy(reply_data, &sender_timestamp, 4);   // prefix with sender_timestamp, like a tag
    uint32_t now = getRTCClock()->getCurrentTime();
    memcpy(&reply_data[4], &now, 4);     // include our clock (for easy clock sync, and packet hash uniqueness)
    sprintf((char *) &reply_data[8], "%s\n%s", _prefs.node_name, _prefs.owner_info);

    return 8 + strlen((char *) &reply_data[8]);   // reply length
  }
  return 0;
}

uint8_t MyMesh::handleAnonClockReq(const mesh::Identity& sender, uint32_t sender_timestamp, const uint8_t* data) {
  if (anon_limiter.allow(rtc_clock.getCurrentTime())) {
    // request data has: {reply-path-len}{reply-path}
    reply_path_len = *data & 63;
    reply_path_hash_size = (*data >> 6) + 1;
    data++;

    memcpy(reply_path, data, ((uint8_t)reply_path_len) * reply_path_hash_size);
    // data += (uint8_t)reply_path_len * reply_path_hash_size;

    memcpy(reply_data, &sender_timestamp, 4);   // prefix with sender_timestamp, like a tag
    uint32_t now = getRTCClock()->getCurrentTime();
    memcpy(&reply_data[4], &now, 4);     // include our clock (for easy clock sync, and packet hash uniqueness)
    reply_data[8] = 0;  // features
#ifdef WITH_RS232_BRIDGE
    reply_data[8] |= 0x01;  // is bridge, type UART
#elif WITH_ESPNOW_BRIDGE
    reply_data[8] |= 0x03;  // is bridge, type ESP-NOW
#endif
    if (_prefs.disable_fwd) {   // is this repeater currently disabled
      reply_data[8] |= 0x80;  // is disabled
    }
    // TODO:  add some kind of moving-window utilisation metric, so can query 'how busy' is this repeater
    return 9;   // reply length
  }
  return 0;
}

int MyMesh::handleRequest(ClientInfo *sender, uint32_t sender_timestamp, uint8_t *payload, size_t payload_len) {
  // uint32_t now = getRTCClock()->getCurrentTimeUnique();
  // memcpy(reply_data, &now, 4);   // response packets always prefixed with timestamp
  memcpy(reply_data, &sender_timestamp, 4); // reflect sender_timestamp back in response packet (kind of like a 'tag')

  if (payload[0] == REQ_TYPE_GET_STATUS) {  // guests can also access this now
    RepeaterStats stats;
    stats.batt_milli_volts = getBatteryMilliVolts(true);
    stats.curr_tx_queue_len = _mgr->getOutboundTotal();
    stats.noise_floor = (int16_t)_radio->getNoiseFloor();
    stats.last_rssi = (int16_t)radio_driver.getLastRSSI();
    stats.n_packets_recv = radio_driver.getPacketsRecv();
    stats.n_packets_sent = radio_driver.getPacketsSent();
    stats.total_air_time_secs = getTotalAirTime() / 1000;
    stats.total_up_time_secs = uptime_millis / 1000;
    stats.n_sent_flood = getNumSentFlood();
    stats.n_sent_direct = getNumSentDirect();
    stats.n_recv_flood = getNumRecvFlood();
    stats.n_recv_direct = getNumRecvDirect();
    stats.err_events = _err_flags;
    stats.last_snr = (int16_t)(radio_driver.getLastSNR() * 4);
    stats.n_direct_dups = ((SimpleMeshTables *)getTables())->getNumDirectDups();
    stats.n_flood_dups = ((SimpleMeshTables *)getTables())->getNumFloodDups();
    stats.total_rx_air_time_secs = getReceiveAirTime() / 1000;
    stats.n_recv_errors = radio_driver.getPacketsRecvErrors();
    memcpy(&reply_data[4], &stats, sizeof(stats));

    return 4 + sizeof(stats); //  reply_len
  }
  if (payload[0] == REQ_TYPE_GET_TELEMETRY_DATA) {
    uint8_t perm_mask = ~(payload[1]); // NEW: first reserved byte (of 4), is now inverse mask to apply to permissions

    telemetry.reset();
    telemetry.addVoltage(TELEM_CHANNEL_SELF, (float)getBatteryMilliVolts(true) / 1000.0f);

    // query other sensors -- target specific
    if ((sender->permissions & PERM_ACL_ROLE_MASK) == PERM_ACL_GUEST) {
      perm_mask = 0x00;  // just base telemetry allowed
    }
    sensors.querySensors(perm_mask, telemetry);

	// This default temperature will be overridden by external sensors (if any)
    float temperature = board.getMCUTemperature();
    if(!isnan(temperature)) { // Supported boards with built-in temperature sensor. ESP32-C3 may return NAN
      telemetry.addTemperature(TELEM_CHANNEL_SELF, temperature); // Built-in MCU Temperature
    }

    uint8_t tlen = telemetry.getSize();
    memcpy(&reply_data[4], telemetry.getBuffer(), tlen);
    return 4 + tlen; // reply_len
  }
  if (payload[0] == REQ_TYPE_GET_ACCESS_LIST && sender->isAdmin()) {
    uint8_t res1 = payload[1];   // reserved for future  (extra query params)
    uint8_t res2 = payload[2];
    if (res1 == 0 && res2 == 0) {
      uint8_t ofs = 4;
      for (int i = 0; i < acl.getNumClients() && ofs + 7 <= sizeof(reply_data) - 4; i++) {
        auto c = acl.getClientByIdx(i);
        if (c->permissions == 0) continue;  // skip deleted entries
        memcpy(&reply_data[ofs], c->id.pub_key, 6); ofs += 6;  // just 6-byte pub_key prefix
        reply_data[ofs++] = c->permissions;
      }
      return ofs;
    }
  }
  if (payload[0] == REQ_TYPE_GET_NEIGHBOURS) {
    uint8_t request_version = payload[1];
    if (request_version == 0) {

      // reply data offset (after response sender_timestamp/tag)
      int reply_offset = 4;

      // get request params
      uint8_t count = payload[2]; // how many neighbours to fetch (0-255)
      uint16_t offset;
      memcpy(&offset, &payload[3], 2); // offset from start of neighbours list (0-65535)
      uint8_t order_by = payload[5]; // how to order neighbours. 0=newest_to_oldest, 1=oldest_to_newest, 2=strongest_to_weakest, 3=weakest_to_strongest
      uint8_t pubkey_prefix_length = payload[6]; // how many bytes of neighbour pub key we want
      // we also send a 4 byte random blob in payload[7...10] to help packet uniqueness

      MESH_DEBUG_PRINTLN("REQ_TYPE_GET_NEIGHBOURS count=%d, offset=%d, order_by=%d, pubkey_prefix_length=%d", count, offset, order_by, pubkey_prefix_length);

      // clamp pub key prefix length to max pub key length
      if(pubkey_prefix_length > PUB_KEY_SIZE){
        pubkey_prefix_length = PUB_KEY_SIZE;
        MESH_DEBUG_PRINTLN("REQ_TYPE_GET_NEIGHBOURS invalid pubkey_prefix_length=%d clamping to %d", pubkey_prefix_length, PUB_KEY_SIZE);
      }

      // create copy of neighbours list, skipping empty entries so we can sort it separately from main list
      int16_t neighbours_count = 0;
#if MAX_NEIGHBOURS
      NeighbourInfo* sorted_neighbours[MAX_NEIGHBOURS];
      for (int i = 0; i < MAX_NEIGHBOURS; i++) {
        auto neighbour = &neighbours[i];
        if (neighbour->heard_timestamp > 0) {
          sorted_neighbours[neighbours_count] = neighbour;
          neighbours_count++;
        }
      }

      // sort neighbours based on order
      if (order_by == 0) {
        // sort by newest to oldest
        MESH_DEBUG_PRINTLN("REQ_TYPE_GET_NEIGHBOURS sorting newest to oldest");
        std::sort(sorted_neighbours, sorted_neighbours + neighbours_count, [](const NeighbourInfo* a, const NeighbourInfo* b) {
          return a->heard_timestamp > b->heard_timestamp; // desc
        });
      } else if (order_by == 1) {
        // sort by oldest to newest
        MESH_DEBUG_PRINTLN("REQ_TYPE_GET_NEIGHBOURS sorting oldest to newest");
        std::sort(sorted_neighbours, sorted_neighbours + neighbours_count, [](const NeighbourInfo* a, const NeighbourInfo* b) {
          return a->heard_timestamp < b->heard_timestamp; // asc
        });
      } else if (order_by == 2) {
        // sort by strongest to weakest
        MESH_DEBUG_PRINTLN("REQ_TYPE_GET_NEIGHBOURS sorting strongest to weakest");
        std::sort(sorted_neighbours, sorted_neighbours + neighbours_count, [](const NeighbourInfo* a, const NeighbourInfo* b) {
          return a->snr > b->snr; // desc
        });
      } else if (order_by == 3) {
        // sort by weakest to strongest
        MESH_DEBUG_PRINTLN("REQ_TYPE_GET_NEIGHBOURS sorting weakest to strongest");
        std::sort(sorted_neighbours, sorted_neighbours + neighbours_count, [](const NeighbourInfo* a, const NeighbourInfo* b) {
          return a->snr < b->snr; // asc
        });
      }
#endif

      // build results buffer
      int results_count = 0;
      int results_offset = 0;
      uint8_t results_buffer[130];
      for(int index = 0; index < count && index + offset < neighbours_count; index++){
        
        // stop if we can't fit another entry in results
        int entry_size = pubkey_prefix_length + 4 + 1;
        if(results_offset + entry_size > sizeof(results_buffer)){
          MESH_DEBUG_PRINTLN("REQ_TYPE_GET_NEIGHBOURS no more entries can fit in results buffer");
          break;
        }

#if MAX_NEIGHBOURS
        // add next neighbour to results
        auto neighbour = sorted_neighbours[index + offset];
        uint32_t heard_seconds_ago = getRTCClock()->getCurrentTime() - neighbour->heard_timestamp;
        memcpy(&results_buffer[results_offset], neighbour->id.pub_key, pubkey_prefix_length); results_offset += pubkey_prefix_length;
        memcpy(&results_buffer[results_offset], &heard_seconds_ago, 4); results_offset += 4;
        memcpy(&results_buffer[results_offset], &neighbour->snr, 1); results_offset += 1;
        results_count++;
#endif

      }

      // build reply
      MESH_DEBUG_PRINTLN("REQ_TYPE_GET_NEIGHBOURS neighbours_count=%d results_count=%d", neighbours_count, results_count);
      memcpy(&reply_data[reply_offset], &neighbours_count, 2); reply_offset += 2;
      memcpy(&reply_data[reply_offset], &results_count, 2); reply_offset += 2;
      memcpy(&reply_data[reply_offset], &results_buffer, results_offset); reply_offset += results_offset;

      return reply_offset;
    }
  } else if (payload[0] == REQ_TYPE_GET_OWNER_INFO) {
    sprintf((char *) &reply_data[4], "%s\n%s\n%s", FIRMWARE_VERSION, _prefs.node_name, _prefs.owner_info);
    return 4 + strlen((char *) &reply_data[4]);
  }
  return 0; // unknown command
}

mesh::Packet *MyMesh::createSelfAdvert() {
  uint8_t app_data[MAX_ADVERT_DATA_SIZE];
  uint8_t app_data_len = _cli.buildAdvertData(ADV_TYPE_REPEATER, app_data);

  return createAdvert(self_id, app_data, app_data_len);
}

File MyMesh::openAppend(const char *fname) {
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  return _fs->open(fname, FILE_O_WRITE);
#elif defined(RP2040_PLATFORM)
  return _fs->open(fname, "a");
#else
  return _fs->open(fname, "a", true);
#endif
}

static uint8_t max_loop_minimal[] =  { 0, /* 1-byte */  4, /* 2-byte */  2, /* 3-byte */  1 };
static uint8_t max_loop_moderate[] = { 0, /* 1-byte */  2, /* 2-byte */  1, /* 3-byte */  1 };
static uint8_t max_loop_strict[] =   { 0, /* 1-byte */  1, /* 2-byte */  1, /* 3-byte */  1 };

bool MyMesh::isLooped(const mesh::Packet* packet, const uint8_t max_counters[]) {
  uint8_t hash_size = packet->getPathHashSize();
  uint8_t hash_count = packet->getPathHashCount();
  uint8_t n = 0;
  const uint8_t* path = packet->path;
  while (hash_count > 0) {      // count how many times this node is already in the path
    if (self_id.isHashMatch(path, hash_size)) n++;
    hash_count--;
    path += hash_size;
  }
  return n >= max_counters[hash_size];
}

void MyMesh::sendFloodReply(mesh::Packet* packet, unsigned long delay_millis, uint8_t path_hash_size) {
  if (recv_pkt_region && !recv_pkt_region->isWildcard()) {  // if _request_ packet scope is known, send reply with same scope
    TransportKey scope;
    if (region_map.getTransportKeysFor(*recv_pkt_region, &scope, 1) > 0) {
      sendFloodScoped(scope, packet, delay_millis, path_hash_size);
    } else {
      sendFlood(packet, delay_millis, path_hash_size);  // send un-scoped
    }
  } else {
    sendFlood(packet, delay_millis, path_hash_size);  // send un-scoped
  }
}

bool MyMesh::allowPacketForward(const mesh::Packet *packet) {
  if (_prefs.disable_fwd) return false;
  if (packet->isRouteFlood() && packet->getPathHashCount() >= _prefs.flood_max) return false;
  if (packet->isRouteFlood() && recv_pkt_region == NULL) {
    MESH_DEBUG_PRINTLN("allowPacketForward: unknown transport code, or wildcard not allowed for FLOOD packet");
    return false;
  }
  if (packet->isRouteFlood() && _prefs.loop_detect != LOOP_DETECT_OFF) {
    const uint8_t* maximums;
    if (_prefs.loop_detect == LOOP_DETECT_MINIMAL) {
      maximums = max_loop_minimal;
    } else if (_prefs.loop_detect == LOOP_DETECT_MODERATE) {
      maximums = max_loop_moderate;
    } else {
      maximums = max_loop_strict;
    }
    if (isLooped(packet, maximums)) {
      MESH_DEBUG_PRINTLN("allowPacketForward: FLOOD packet loop detected!");
      return false;
    }
  }
  return true;
}

const char *MyMesh::getLogDateTime() {
  static char tmp[32];
  uint32_t now = getRTCClock()->getCurrentTime();
  DateTime dt = DateTime(now);
  sprintf(tmp, "%02d:%02d:%02d - %d/%d/%d U", dt.hour(), dt.minute(), dt.second(), dt.day(), dt.month(),
          dt.year());
  return tmp;
}

void MyMesh::logRxRaw(float snr, float rssi, const uint8_t raw[], int len) {
#if MESH_PACKET_LOGGING
  Serial.print(getLogDateTime());
  Serial.print(" RAW: ");
  mesh::Utils::printHex(Serial, raw, len);
  Serial.println();
#endif
}

void MyMesh::logRx(mesh::Packet *pkt, int len, float score) {
#ifdef WITH_BRIDGE
  if (_prefs.bridge_pkt_src == 1) {
    bridge.sendPacket(pkt);
  }
#endif
#ifdef WITH_MQTT_UPLINK
  mqtt.publishPacket(*pkt, false, (int)_radio->getLastRSSI(), _radio->getLastSNR(), (int)(score * 1000),
                     (int)_radio->getEstAirtimeFor(len));
#endif

  if (_logging) {
    File f = openAppend(PACKET_LOG_FILE);
    if (f) {
      f.print(getLogDateTime());
      f.printf(": RX, len=%d (type=%d, route=%s, payload_len=%d) SNR=%d RSSI=%d score=%d", len,
               pkt->getPayloadType(), pkt->isRouteDirect() ? "D" : "F", pkt->payload_len,
               (int)_radio->getLastSNR(), (int)_radio->getLastRSSI(), (int)(score * 1000));

      if (pkt->getPayloadType() == PAYLOAD_TYPE_PATH || pkt->getPayloadType() == PAYLOAD_TYPE_REQ ||
          pkt->getPayloadType() == PAYLOAD_TYPE_RESPONSE || pkt->getPayloadType() == PAYLOAD_TYPE_TXT_MSG) {
        f.printf(" [%02X -> %02X]\n", (uint32_t)pkt->payload[1], (uint32_t)pkt->payload[0]);
      } else {
        f.printf("\n");
      }
      f.close();
    }
  }
}

void MyMesh::logTx(mesh::Packet *pkt, int len) {
#ifdef WITH_BRIDGE
  if (_prefs.bridge_pkt_src == 0) {
    bridge.sendPacket(pkt);
  }
#endif
#ifdef WITH_MQTT_UPLINK
  mqtt.publishPacket(*pkt, true, (int)_radio->getLastRSSI(), _radio->getLastSNR());
#endif

  if (_logging) {
    File f = openAppend(PACKET_LOG_FILE);
    if (f) {
      f.print(getLogDateTime());
      f.printf(": TX, len=%d (type=%d, route=%s, payload_len=%d)", len, pkt->getPayloadType(),
               pkt->isRouteDirect() ? "D" : "F", pkt->payload_len);

      if (pkt->getPayloadType() == PAYLOAD_TYPE_PATH || pkt->getPayloadType() == PAYLOAD_TYPE_REQ ||
          pkt->getPayloadType() == PAYLOAD_TYPE_RESPONSE || pkt->getPayloadType() == PAYLOAD_TYPE_TXT_MSG) {
        f.printf(" [%02X -> %02X]\n", (uint32_t)pkt->payload[1], (uint32_t)pkt->payload[0]);
      } else {
        f.printf("\n");
      }
      f.close();
    }
  }
}

void MyMesh::logTxFail(mesh::Packet *pkt, int len) {
  if (_logging) {
    File f = openAppend(PACKET_LOG_FILE);
    if (f) {
      f.print(getLogDateTime());
      f.printf(": TX FAIL!, len=%d (type=%d, route=%s, payload_len=%d)\n", len, pkt->getPayloadType(),
               pkt->isRouteDirect() ? "D" : "F", pkt->payload_len);
      f.close();
    }
  }
}

int MyMesh::calcRxDelay(float score, uint32_t air_time) const {
  if (_prefs.rx_delay_base <= 0.0f) return 0;
  return (int)((pow(_prefs.rx_delay_base, 0.85f - score) - 1.0) * air_time);
}

uint32_t MyMesh::getRetransmitDelay(const mesh::Packet *packet) {
  uint32_t t = (_radio->getEstAirtimeFor(packet->getPathByteLen() + packet->payload_len + 2) * _prefs.tx_delay_factor);
  return getRNG()->nextInt(0, 5*t + 1);
}
uint32_t MyMesh::getDirectRetransmitDelay(const mesh::Packet *packet) {
  uint32_t t = (_radio->getEstAirtimeFor(packet->getPathByteLen() + packet->payload_len + 2) * _prefs.direct_tx_delay_factor);
  return getRNG()->nextInt(0, 5*t + 1);
}

bool MyMesh::filterRecvFloodPacket(mesh::Packet* pkt) {
  // just try to determine region for packet (apply later in allowPacketForward())
  if (pkt->getRouteType() == ROUTE_TYPE_TRANSPORT_FLOOD) {
    recv_pkt_region = region_map.findMatch(pkt, REGION_DENY_FLOOD);
  } else if (pkt->getRouteType() == ROUTE_TYPE_FLOOD) {
    if (region_map.getWildcard().flags & REGION_DENY_FLOOD) {
      recv_pkt_region = NULL;
    } else {
      recv_pkt_region =  &region_map.getWildcard();
    }
  } else {
    recv_pkt_region = NULL;
  }
  // do normal processing
  return false;
}

void MyMesh::onAnonDataRecv(mesh::Packet *packet, const uint8_t *secret, const mesh::Identity &sender,
                            uint8_t *data, size_t len) {
  if (packet->getPayloadType() == PAYLOAD_TYPE_ANON_REQ) { // received an initial request by a possible admin
                                                           // client (unknown at this stage)
    uint32_t timestamp;
    memcpy(&timestamp, data, 4);

    data[len] = 0;  // ensure null terminator
    uint8_t reply_len;

    reply_path_len = -1;
    if (data[4] == 0 || data[4] >= ' ') {   // is password, ie. a login request
      reply_len = handleLoginReq(sender, secret, timestamp, &data[4], packet->isRouteFlood());
    } else if (data[4] == ANON_REQ_TYPE_REGIONS && packet->isRouteDirect()) {
      reply_len = handleAnonRegionsReq(sender, timestamp, &data[5]);
    } else if (data[4] == ANON_REQ_TYPE_OWNER && packet->isRouteDirect()) {
      reply_len = handleAnonOwnerReq(sender, timestamp, &data[5]);
    } else if (data[4] == ANON_REQ_TYPE_BASIC && packet->isRouteDirect()) {
      reply_len = handleAnonClockReq(sender, timestamp, &data[5]);
    } else {
      reply_len = 0;  // unknown/invalid request type
    }

    if (reply_len == 0) return;   // invalid request

    if (packet->isRouteFlood()) {
      // let this sender know path TO here, so they can use sendDirect(), and ALSO encode the response
      mesh::Packet* path = createPathReturn(sender, secret, packet->path, packet->path_len,
                                            PAYLOAD_TYPE_RESPONSE, reply_data, reply_len);
      if (path) sendFloodReply(path, SERVER_RESPONSE_DELAY, packet->getPathHashSize());
    } else if (reply_path_len < 0) {
      mesh::Packet* reply = createDatagram(PAYLOAD_TYPE_RESPONSE, sender, secret, reply_data, reply_len);
      if (reply) sendFloodReply(reply, SERVER_RESPONSE_DELAY, packet->getPathHashSize());
    } else {
      mesh::Packet* reply = createDatagram(PAYLOAD_TYPE_RESPONSE, sender, secret, reply_data, reply_len);
      uint8_t path_len = ((reply_path_hash_size - 1) << 6) | (reply_path_len & 63);
      if (reply) sendDirect(reply, reply_path,  path_len, SERVER_RESPONSE_DELAY);
    }
  }
}

int MyMesh::searchPeersByHash(const uint8_t *hash) {
  int n = 0;
  for (int i = 0; i < acl.getNumClients(); i++) {
    if (acl.getClientByIdx(i)->id.isHashMatch(hash)) {
      matching_peer_indexes[n++] = i; // store the INDEXES of matching contacts (for subsequent 'peer' methods)
    }
  }
  return n;
}

void MyMesh::getPeerSharedSecret(uint8_t *dest_secret, int peer_idx) {
  int i = matching_peer_indexes[peer_idx];
  if (i >= 0 && i < acl.getNumClients()) {
    // lookup pre-calculated shared_secret
    memcpy(dest_secret, acl.getClientByIdx(i)->shared_secret, PUB_KEY_SIZE);
  } else {
    MESH_DEBUG_PRINTLN("getPeerSharedSecret: Invalid peer idx: %d", i);
  }
}

static bool isShare(const mesh::Packet *packet) {
  if (packet->hasTransportCodes()) {
    return packet->transport_codes[0] == 0 && packet->transport_codes[1] == 0;  // codes { 0, 0 } means 'send to nowhere'
  }
  return false;
}

void MyMesh::onAdvertRecv(mesh::Packet *packet, const mesh::Identity &id, uint32_t timestamp,
                          const uint8_t *app_data, size_t app_data_len) {
  mesh::Mesh::onAdvertRecv(packet, id, timestamp, app_data, app_data_len); // chain to super impl

  // if this a zero hop advert (and not via 'Share'), add it to neighbours
  if (packet->path_len == 0 && !isShare(packet)) {
    AdvertDataParser parser(app_data, app_data_len);
    if (parser.isValid() && parser.getType() == ADV_TYPE_REPEATER) { // just keep neigbouring Repeaters
      putNeighbour(id, timestamp, packet->getSNR());
    }
  }
}

void MyMesh::onPeerDataRecv(mesh::Packet *packet, uint8_t type, int sender_idx, const uint8_t *secret,
                            uint8_t *data, size_t len) {
  int i = matching_peer_indexes[sender_idx];
  if (i < 0 || i >= acl.getNumClients()) { // get from our known_clients table (sender SHOULD already be known in this context)
    MESH_DEBUG_PRINTLN("onPeerDataRecv: invalid peer idx: %d", i);
    return;
  }
  ClientInfo* client = acl.getClientByIdx(i);

  if (type == PAYLOAD_TYPE_REQ) { // request (from a Known admin client!)
    uint32_t timestamp;
    memcpy(&timestamp, data, 4);

    if (timestamp > client->last_timestamp) { // prevent replay attacks
      int reply_len = handleRequest(client, timestamp, &data[4], len - 4);
      if (reply_len == 0) return; // invalid command

      client->last_timestamp = timestamp;
      client->last_activity = getRTCClock()->getCurrentTime();

      if (packet->isRouteFlood()) {
        // let this sender know path TO here, so they can use sendDirect(), and ALSO encode the response
        mesh::Packet *path = createPathReturn(client->id, secret, packet->path, packet->path_len,
                                              PAYLOAD_TYPE_RESPONSE, reply_data, reply_len);
        if (path) sendFloodReply(path, SERVER_RESPONSE_DELAY, packet->getPathHashSize());
      } else {
        mesh::Packet *reply =
            createDatagram(PAYLOAD_TYPE_RESPONSE, client->id, secret, reply_data, reply_len);
        if (reply) {
          if (client->out_path_len != OUT_PATH_UNKNOWN) { // we have an out_path, so send DIRECT
            sendDirect(reply, client->out_path, client->out_path_len, SERVER_RESPONSE_DELAY);
          } else {
            sendFloodReply(reply, SERVER_RESPONSE_DELAY, packet->getPathHashSize());
          }
        }
      }
    } else {
      MESH_DEBUG_PRINTLN("onPeerDataRecv: possible replay attack detected");
    }
  } else if (type == PAYLOAD_TYPE_TXT_MSG && len > 5 && client->isAdmin()) { // a CLI command
    uint32_t sender_timestamp;
    memcpy(&sender_timestamp, data, 4); // timestamp (by sender's RTC clock - which could be wrong)
    uint8_t flags = (data[4] >> 2);        // message attempt number, and other flags

    if (!(flags == TXT_TYPE_PLAIN || flags == TXT_TYPE_CLI_DATA)) {
      MESH_DEBUG_PRINTLN("onPeerDataRecv: unsupported text type received: flags=%02x", (uint32_t)flags);
    } else if (sender_timestamp >= client->last_timestamp) { // prevent replay attacks
      bool is_retry = (sender_timestamp == client->last_timestamp);
      client->last_timestamp = sender_timestamp;
      client->last_activity = getRTCClock()->getCurrentTime();

      // len can be > original length, but 'text' will be padded with zeroes
      data[len] = 0; // need to make a C string again, with null terminator

      if (flags == TXT_TYPE_PLAIN) { // for legacy CLI, send Acks
        uint32_t ack_hash; // calc truncated hash of the message timestamp + text + sender pub_key, to prove
                           // to sender that we got it
        mesh::Utils::sha256((uint8_t *)&ack_hash, 4, data, 5 + strlen((char *)&data[5]), client->id.pub_key,
                            PUB_KEY_SIZE);

        mesh::Packet *ack = createAck(ack_hash);
        if (ack) {
          if (client->out_path_len == OUT_PATH_UNKNOWN) {
            sendFloodReply(ack, TXT_ACK_DELAY, packet->getPathHashSize());
          } else {
            sendDirect(ack, client->out_path, client->out_path_len, TXT_ACK_DELAY);
          }
        }
      }

      uint8_t temp[166];
      char *command = (char *)&data[5];
      char *reply = (char *)&temp[5];
      if (is_retry) {
        *reply = 0;
      } else {
        handleCommand(sender_timestamp, command, reply);
      }
      int text_len = strlen(reply);
      if (text_len > 0) {
        uint32_t timestamp = getRTCClock()->getCurrentTimeUnique();
        if (timestamp == sender_timestamp) {
          // WORKAROUND: the two timestamps need to be different, in the CLI view
          timestamp++;
        }
        memcpy(temp, &timestamp, 4);        // mostly an extra blob to help make packet_hash unique
        temp[4] = (TXT_TYPE_CLI_DATA << 2); // NOTE: legacy was: TXT_TYPE_PLAIN

        auto reply = createDatagram(PAYLOAD_TYPE_TXT_MSG, client->id, secret, temp, 5 + text_len);
        if (reply) {
          if (client->out_path_len == OUT_PATH_UNKNOWN) {
            sendFloodReply(reply, CLI_REPLY_DELAY_MILLIS, packet->getPathHashSize());
          } else {
            sendDirect(reply, client->out_path, client->out_path_len, CLI_REPLY_DELAY_MILLIS);
          }
        }
      }
    } else {
      MESH_DEBUG_PRINTLN("onPeerDataRecv: possible replay attack detected");
    }
  }
}

bool MyMesh::onPeerPathRecv(mesh::Packet *packet, int sender_idx, const uint8_t *secret, uint8_t *path,
                            uint8_t path_len, uint8_t extra_type, uint8_t *extra, uint8_t extra_len) {
  // TODO: prevent replay attacks
  int i = matching_peer_indexes[sender_idx];

  if (i >= 0 && i < acl.getNumClients()) { // get from our known_clients table (sender SHOULD already be known in this context)
    MESH_DEBUG_PRINTLN("PATH to client, path_len=%d", (uint32_t)path_len);
    auto client = acl.getClientByIdx(i);

    // store a copy of path, for sendDirect()
    client->out_path_len = mesh::Packet::copyPath(client->out_path, path, path_len);
    client->last_activity = getRTCClock()->getCurrentTime();
  } else {
    MESH_DEBUG_PRINTLN("onPeerPathRecv: invalid peer idx: %d", i);
  }

  // NOTE: no reciprocal path send!!
  return false;
}

#define CTL_TYPE_NODE_DISCOVER_REQ   0x80
#define CTL_TYPE_NODE_DISCOVER_RESP  0x90

void MyMesh::onControlDataRecv(mesh::Packet* packet) {
  uint8_t type = packet->payload[0] & 0xF0;    // just test upper 4 bits
  if (type == CTL_TYPE_NODE_DISCOVER_REQ && packet->payload_len >= 6
      && !_prefs.disable_fwd && discover_limiter.allow(rtc_clock.getCurrentTime())
  ) {
    int i = 1;
    uint8_t  filter = packet->payload[i++];
    uint32_t tag;
    memcpy(&tag, &packet->payload[i], 4); i += 4;
    uint32_t since;
    if (packet->payload_len >= i+4) {   // optional since field
      memcpy(&since, &packet->payload[i], 4); i += 4;
    } else {
      since = 0;
    }

    if ((filter & (1 << ADV_TYPE_REPEATER)) != 0 && _prefs.discovery_mod_timestamp >= since) {
      bool prefix_only = packet->payload[0] & 1;
      uint8_t data[6 + PUB_KEY_SIZE];
      data[0] = CTL_TYPE_NODE_DISCOVER_RESP | ADV_TYPE_REPEATER;   // low 4-bits for node type
      data[1] = packet->_snr;   // let sender know the inbound SNR ( x 4)
      memcpy(&data[2], &tag, 4);     // include tag from request, for client to match to
      memcpy(&data[6], self_id.pub_key, PUB_KEY_SIZE);
      auto resp = createControlData(data, prefix_only ? 6 + 8 : 6 + PUB_KEY_SIZE);
      if (resp) {
        sendZeroHop(resp, getRetransmitDelay(resp)*4);  // apply random delay (widened x4), as multiple nodes can respond to this
      }
    }
  } else if (type == CTL_TYPE_NODE_DISCOVER_RESP && packet->payload_len >= 6) {
    uint8_t node_type = packet->payload[0] & 0x0F;
    if (node_type != ADV_TYPE_REPEATER) {
      return;
    }
    if (packet->payload_len < 6 + PUB_KEY_SIZE) {
      MESH_DEBUG_PRINTLN("onControlDataRecv: DISCOVER_RESP pubkey too short: %d", (uint32_t)packet->payload_len);
      return;
    }

    if (pending_discover_tag == 0 || millisHasNowPassed(pending_discover_until)) {
      pending_discover_tag = 0;
      return;
    }
    uint32_t tag;
    memcpy(&tag, &packet->payload[2], 4);
    if (tag != pending_discover_tag) {
      return;
    }

    mesh::Identity id(&packet->payload[6]);
    if (id.matches(self_id)) {
      return;
    }
    putNeighbour(id, rtc_clock.getCurrentTime(), packet->getSNR());
  }
}

void MyMesh::sendNodeDiscoverReq() {
  uint8_t data[10];
  data[0] = CTL_TYPE_NODE_DISCOVER_REQ; // prefix_only=0
  data[1] = (1 << ADV_TYPE_REPEATER);
  getRNG()->random(&data[2], 4); // tag
  memcpy(&pending_discover_tag, &data[2], 4);
  pending_discover_until = futureMillis(60000);
  uint32_t since = 0;
  memcpy(&data[6], &since, 4);

  auto pkt = createControlData(data, sizeof(data));
  if (pkt) {
    sendZeroHop(pkt);
  }
}

MyMesh::MyMesh(mesh::MainBoard &board, mesh::Radio &radio, mesh::MillisecondClock &ms, mesh::RNG &rng,
               mesh::RTCClock &rtc, mesh::MeshTables &tables)
    : mesh::Mesh(radio, ms, rng, rtc, *new StaticPoolPacketManager(32), tables),
      region_map(key_store), temp_map(key_store),
      _cli(board, rtc, sensors, region_map, acl, &_prefs, this),
      telemetry(MAX_PACKET_PAYLOAD - 4),
      discover_limiter(4, 120),  // max 4 every 2 minutes
      anon_limiter(4, 180)   // max 4 every 3 minutes
#if defined(WITH_RS232_BRIDGE)
      , bridge(&_prefs, WITH_RS232_BRIDGE, _mgr, &rtc)
#endif
#if defined(WITH_ESPNOW_BRIDGE)
      , bridge(&_prefs, _mgr, &rtc)
#endif
#if defined(WITH_MQTT_UPLINK)
      , mqtt(rtc, self_id)
#endif
{
  last_millis = 0;
  _archive = nullptr;
  uptime_millis = 0;
  next_archive_neighbours_flush_ms = 0;
  next_battery_sample_ms = 0;
  next_history_sample_ms = 0;
  next_local_advert = next_flood_advert = 0;
  dirty_contacts_expiry = 0;
  set_radio_at = revert_radio_at = 0;
  _logging = false;
  _archive_neighbours_dirty = false;
  _battery_sample_valid = false;
  _battery_mv_cache = 0;
  region_load_active = false;
  memset(&_stats_state, 0, sizeof(_stats_state));

#if MAX_NEIGHBOURS
  memset(neighbours, 0, sizeof(neighbours));
#endif

  // defaults
  memset(&_prefs, 0, sizeof(_prefs));
  _prefs.airtime_factor = 1.0;
  _prefs.rx_delay_base = 0.0f;   // turn off by default, was 10.0;
  _prefs.tx_delay_factor = 0.5f; // was 0.25f
  _prefs.direct_tx_delay_factor = 0.3f; // was 0.2
  StrHelper::strncpy(_prefs.node_name, ADVERT_NAME, sizeof(_prefs.node_name));
  _prefs.node_lat = ADVERT_LAT;
  _prefs.node_lon = ADVERT_LON;
  StrHelper::strncpy(_prefs.password, ADMIN_PASSWORD, sizeof(_prefs.password));
  _prefs.freq = LORA_FREQ;
  _prefs.sf = LORA_SF;
  _prefs.bw = LORA_BW;
  _prefs.cr = LORA_CR;
  _prefs.tx_power_dbm = LORA_TX_POWER;
  _prefs.advert_interval = 1;        // default to 2 minutes for NEW installs
  _prefs.flood_advert_interval = 12; // 12 hours
  _prefs.flood_max = 64;
  _prefs.interference_threshold = 0; // disabled

  // bridge defaults
  _prefs.bridge_enabled = 1;    // enabled
  _prefs.bridge_delay   = 500;  // milliseconds
  _prefs.bridge_pkt_src = 0;    // logTx
  _prefs.bridge_baud = 115200;  // baud rate
  _prefs.bridge_channel = 1;    // channel 1

  StrHelper::strncpy(_prefs.bridge_secret, "LVSITANOS", sizeof(_prefs.bridge_secret));

  // GPS defaults
  _prefs.gps_enabled = 0;
  _prefs.gps_interval = 0;
  _prefs.advert_loc_policy = ADVERT_LOC_PREFS;

  _prefs.adc_multiplier = 0.0f; // 0.0f means use default board multiplier
  _prefs.reserved_290 = 0;
  _prefs.fan_mode = 0; // auto
  _prefs.fan_timeout_secs = 30;

#if defined(USE_SX1262) || defined(USE_SX1268)
#ifdef SX126X_RX_BOOSTED_GAIN
  _prefs.rx_boosted_gain = SX126X_RX_BOOSTED_GAIN;
#else
  _prefs.rx_boosted_gain = 1; // enabled by default;
#endif
#endif

  pending_discover_tag = 0;
  pending_discover_until = 0;

  memset(default_scope.key, 0, sizeof(default_scope.key));
}

void MyMesh::begin(FILESYSTEM *fs, ArchiveStorage* archive) {
  mesh::Mesh::begin();
  _fs = fs;
  _archive = archive;
  last_millis = millis();
  // load persisted prefs
  _cli.loadPrefs(_fs);
  acl.load(_fs, self_id);
  // TODO: key_store.begin();
  region_map.load(_fs);

  // establish default-scope
  {
    RegionEntry* r = region_map.getDefaultRegion();
    if (r) {
      region_map.getTransportKeysFor(*r, &default_scope, 1);
    } else {
#ifdef DEFAULT_FLOOD_SCOPE_NAME
      r = region_map.findByName(DEFAULT_FLOOD_SCOPE_NAME);
      if (r == NULL) {
        r = region_map.putRegion(DEFAULT_FLOOD_SCOPE_NAME, 0);  // auto-create the default scope region
        if (r) { r->flags = 0; }   // Allow-flood
      }
      if (r) {
        region_map.setDefaultRegion(r);
        region_map.getTransportKeysFor(*r, &default_scope, 1);
      }
#endif
    }
  }

#if defined(WITH_BRIDGE)
  if (_prefs.bridge_enabled) {
    bridge.begin();
  }
#endif
#if defined(ESP_PLATFORM)
  uint8_t legacy_wifi_powersave = 0;
  const char* legacy_wifi_ssid = nullptr;
  const char* legacy_wifi_pwd = nullptr;
#ifdef WITH_MQTT_UPLINK
  MQTTPrefs legacy_mqtt_prefs{};
  MQTTPrefsStore::setDefaults(legacy_mqtt_prefs);
  MQTTPrefsStore::load(_fs, legacy_mqtt_prefs);
  legacy_wifi_powersave = legacy_mqtt_prefs.legacy_wifi_powersave;
  legacy_wifi_ssid = legacy_mqtt_prefs.legacy_wifi_ssid;
  legacy_wifi_pwd = legacy_mqtt_prefs.legacy_wifi_pwd;
#endif
  network.begin(_fs, legacy_wifi_powersave, legacy_wifi_ssid, legacy_wifi_pwd);
#endif
#if defined(ESP_PLATFORM) && WITH_WEB_PANEL
  board.setInhibitSleep(true);
  web.setCommandRunner(this);
  web.setNetworkStateProvider(&network);
  web.begin(_fs);
  _stats_history.begin(web.isWebStatsEnabled(), _archive);
  if (web.isWebStatsEnabled() && !_stats_history.isLiveOnly() && _archive != nullptr && _archive->isMounted()) {
    restoreArchiveNeighbours();
    next_archive_neighbours_flush_ms = millis() + kArchiveNeighboursFlushIntervalMs;
  }
  if (web.isWebStatsEnabled()) {
    recordStatsEvent(HISTORY_EVENT_BOOT);
    if (_archive != nullptr) {
      recordStatsEvent(_archive->isMounted() ? HISTORY_EVENT_ARCHIVE_MOUNTED : HISTORY_EVENT_ARCHIVE_UNAVAILABLE);
    }
  }
#endif
#if defined(WITH_MQTT_UPLINK) && !(defined(ESP_PLATFORM) && WITH_WEB_PANEL)
  board.setInhibitSleep(true);
#endif
#ifdef WITH_MQTT_UPLINK
  mqtt.setNodeNameSource(_prefs.node_name);
#if defined(ESP_PLATFORM)
  mqtt.setNetworkStateProvider(&network);
#endif
  mqtt.begin(_fs);
#endif

  radio_set_params(_prefs.freq, _prefs.bw, _prefs.sf, _prefs.cr);
  radio_set_tx_power(_prefs.tx_power_dbm);

  radio_driver.setRxBoostedGainMode(_prefs.rx_boosted_gain);
  MESH_DEBUG_PRINTLN("RX Boosted Gain Mode: %s",
                     radio_driver.getRxBoostedGainMode() ? "Enabled" : "Disabled");

  updateAdvertTimer();
  updateFloodAdvertTimer();

  board.setAdcMultiplier(_prefs.adc_multiplier);
#if defined(TBEAM_1W)
  auto& tbeam1w_board = static_cast<TBeam1WBoard&>(board);
  tbeam1w_board.setFanPostTxHoldMs(static_cast<uint32_t>(_prefs.fan_timeout_secs) * 1000UL);
  tbeam1w_board.setFanMode(static_cast<TBeam1WBoard::FanMode>(_prefs.fan_mode));
#endif

#if ENV_INCLUDE_GPS == 1
  applyGpsPrefs();
#endif

  next_history_sample_ms = futureMillis(1000);
}

void MyMesh::sendFloodScoped(const TransportKey& scope, mesh::Packet* pkt, uint32_t delay_millis, uint8_t path_hash_size) {
  if (scope.isNull()) {
    sendFlood(pkt, delay_millis, path_hash_size);
  } else {
    uint16_t codes[2];
    codes[0] = scope.calcTransportCode(pkt);
    codes[1] = 0;  // REVISIT: set to 'home' Region, for sender/return region?
    sendFlood(pkt, codes, delay_millis, path_hash_size);
  }
}

uint16_t MyMesh::getBatteryMilliVolts(bool force_refresh) {
  constexpr unsigned long kBatterySampleIntervalMs = 60000UL;

  if (force_refresh || !_battery_sample_valid || next_battery_sample_ms == 0 || millisHasNowPassed(next_battery_sample_ms)) {
    _battery_mv_cache = board.getBattMilliVolts();
    _battery_sample_valid = true;
    next_battery_sample_ms = millis() + kBatterySampleIntervalMs;
  }

  return _battery_mv_cache;
}

void MyMesh::applyTempRadioParams(float freq, float bw, uint8_t sf, uint8_t cr, int timeout_mins) {
  set_radio_at = futureMillis(2000); // give CLI reply some time to be sent back, before applying temp radio params
  pending_freq = freq;
  pending_bw = bw;
  pending_sf = sf;
  pending_cr = cr;

  revert_radio_at = futureMillis(2000 + timeout_mins * 60 * 1000); // schedule when to revert radio params
}

bool MyMesh::formatFileSystem() {
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  return InternalFS.format();
#elif defined(RP2040_PLATFORM)
  return LittleFS.format();
#elif defined(ESP32)
  return SPIFFS.format();
#else
#error "need to implement file system erase"
  return false;
#endif
}

void MyMesh::sendSelfAdvertisement(int delay_millis, bool flood) {
  mesh::Packet *pkt = createSelfAdvert();
  if (pkt) {
    if (flood) {
      sendFloodScoped(default_scope, pkt, delay_millis, _prefs.path_hash_mode + 1);
    } else {
      sendZeroHop(pkt, delay_millis);
    }
  } else {
    MESH_DEBUG_PRINTLN("ERROR: unable to create advertisement packet!");
  }
}

void MyMesh::updateAdvertTimer() {
  if (_prefs.advert_interval > 0) { // schedule local advert timer
    next_local_advert = futureMillis(((uint32_t)_prefs.advert_interval) * 2 * 60 * 1000);
  } else {
    next_local_advert = 0; // stop the timer
  }
}

void MyMesh::updateFloodAdvertTimer() {
  if (_prefs.flood_advert_interval > 0) { // schedule flood advert timer
    next_flood_advert = futureMillis(((uint32_t)_prefs.flood_advert_interval) * 60 * 60 * 1000);
  } else {
    next_flood_advert = 0; // stop the timer
  }
}

void MyMesh::dumpLogFile() {
#if defined(RP2040_PLATFORM)
  File f = _fs->open(PACKET_LOG_FILE, "r");
#else
  File f = _fs->open(PACKET_LOG_FILE);
#endif
  if (f) {
    while (f.available()) {
      int c = f.read();
      if (c < 0) break;
      Serial.print((char)c);
    }
    f.close();
  }
}

void MyMesh::setTxPower(int8_t power_dbm) {
  radio_set_tx_power(power_dbm);
}

#if defined(USE_SX1262) || defined(USE_SX1268)
void MyMesh::setRxBoostedGain(bool enable) {
  radio_driver.setRxBoostedGainMode(enable);
}
#endif

void MyMesh::formatNeighborsReply(char *reply) {
  char *dp = reply;

#if MAX_NEIGHBOURS
  // create copy of neighbours list, skipping empty entries so we can sort it separately from main list
  int16_t neighbours_count = 0;
  NeighbourInfo* sorted_neighbours[MAX_NEIGHBOURS];
  for (int i = 0; i < MAX_NEIGHBOURS; i++) {
    auto neighbour = &neighbours[i];
    if (neighbour->heard_timestamp > 0) {
      sorted_neighbours[neighbours_count] = neighbour;
      neighbours_count++;
    }
  }

  // sort neighbours newest to oldest
  std::sort(sorted_neighbours, sorted_neighbours + neighbours_count, [](const NeighbourInfo* a, const NeighbourInfo* b) {
    return a->heard_timestamp > b->heard_timestamp; // desc
  });

  for (int i = 0; i < neighbours_count && dp - reply < 134; i++) {
    NeighbourInfo *neighbour = sorted_neighbours[i];

    // add new line if not first item
    if (i > 0) *dp++ = '\n';

    char hex[10];
    // get 4 bytes of neighbour id as hex
    mesh::Utils::toHex(hex, neighbour->id.pub_key, 4);

    // add next neighbour
    uint32_t secs_ago = getRTCClock()->getCurrentTime() - neighbour->heard_timestamp;
    sprintf(dp, "%s:%d:%d", hex, secs_ago, neighbour->snr);
    while (*dp)
      dp++; // find end of string
  }
#endif
  if (dp == reply) { // no neighbours, need empty response
    strcpy(dp, "-none-");
    dp += 6;
  }
  *dp = 0; // null terminator
}

void MyMesh::removeNeighbor(const uint8_t *pubkey, int key_len) {
#if MAX_NEIGHBOURS
  for (int i = 0; i < MAX_NEIGHBOURS; i++) {
    NeighbourInfo *neighbour = &neighbours[i];
    if (memcmp(neighbour->id.pub_key, pubkey, key_len) == 0) {
      neighbours[i] = NeighbourInfo(); // clear neighbour entry
      _archive_neighbours_dirty = true;
    }
  }
#endif
}

void MyMesh::formatStatsReply(char *reply, size_t reply_size) {
  snprintf(reply,
           reply_size,
           "{\"battery_mv\":%u,\"uptime_secs\":%u,\"errors\":%u,\"queue_len\":%u}",
           getBatteryMilliVolts(true),
           _ms->getMillis() / 1000,
           _err_flags,
           _mgr->getOutboundTotal());
}

void MyMesh::startRegionsLoad() {
  temp_map.resetFrom(region_map);   // rebuild regions in a temp instance
  memset(load_stack, 0, sizeof(load_stack));
  load_stack[0] = &temp_map.getWildcard();
  region_load_active = true;
}

bool MyMesh::saveRegions() {
  return region_map.save(_fs);
}

void MyMesh::onDefaultRegionChanged(const RegionEntry* r) {
  if (r) {
    region_map.getTransportKeysFor(*r, &default_scope, 1);
  } else {
    memset(default_scope.key, 0, sizeof(default_scope.key));
  }
}

void MyMesh::formatRadioStatsReply(char *reply, size_t reply_size) {
  StatsFormatHelper::formatRadioStats(reply, reply_size, _radio, radio_driver, getTotalAirTime(), getReceiveAirTime());
}

void MyMesh::formatPacketStatsReply(char *reply, size_t reply_size) {
  StatsFormatHelper::formatPacketStats(reply, reply_size, radio_driver, getNumSentFlood(), getNumSentDirect(),
                                       getNumRecvFlood(), getNumRecvDirect());
}

void MyMesh::formatMemoryReply(char *reply, size_t reply_size) {
  StatsFormatHelper::formatMemoryStats(reply, reply_size);
}

size_t MyMesh::getNeighbourCount() const {
#if MAX_NEIGHBOURS
  size_t count = 0;
  for (int i = 0; i < MAX_NEIGHBOURS; i++) {
    if (neighbours[i].heard_timestamp > 0) {
      count++;
    }
  }
  return count;
#else
  return 0;
#endif
}

bool MyMesh::restoreArchiveNeighbours() {
#if MAX_NEIGHBOURS
  if (_archive == nullptr || !_archive->isMounted()) {
    return false;
  }

  FILESYSTEM* fs = _archive->getFS();
  if (fs == nullptr) {
    return false;
  }

  const char* restore_path = nullptr;
  if (fs->exists(kArchiveNeighboursLatestPath)) {
    restore_path = kArchiveNeighboursLatestPath;
  } else if (fs->exists(kArchiveNeighboursSnapshotPath)) {
    restore_path = kArchiveNeighboursSnapshotPath;
  } else {
    return false;
  }

  File file = openArchiveReadWithRecovery(_archive, restore_path);
  if (!file) {
    ARCHIVE_LOG("neighbours restore open failed path=%s", restore_path);
    return false;
  }

  memset(neighbours, 0, sizeof(neighbours));
  char line[128];
  size_t line_len = 0;
  size_t restored = 0;
  while (file.available()) {
    const int raw = file.read();
    if (raw < 0) {
      break;
    }

    const char ch = static_cast<char>(raw);
    if (ch == '\r') {
      continue;
    }
    if (ch == '\n') {
      line[line_len] = 0;
      if (line_len > 0 && restored < MAX_NEIGHBOURS) {
        char full_hex[65];
        unsigned long advert_timestamp = 0;
        unsigned long heard_timestamp = 0;
        int snr = 0;
        memset(full_hex, 0, sizeof(full_hex));
        if (sscanf(line, "%64[^,],%lu,%lu,%d", full_hex, &advert_timestamp, &heard_timestamp, &snr) == 4) {
          uint8_t pub_key[PUB_KEY_SIZE];
          if (mesh::Utils::fromHex(pub_key, PUB_KEY_SIZE, full_hex)) {
            neighbours[restored].id = mesh::Identity(pub_key);
            neighbours[restored].advert_timestamp = static_cast<uint32_t>(advert_timestamp);
            neighbours[restored].heard_timestamp = static_cast<uint32_t>(heard_timestamp);
            neighbours[restored].snr = static_cast<int8_t>(constrain(snr, -128, 127));
            restored++;
          }
        }
      }
      line_len = 0;
      continue;
    }

    if (line_len + 1 < sizeof(line)) {
      line[line_len++] = ch;
    }
  }

  if (line_len > 0 && restored < MAX_NEIGHBOURS) {
    line[line_len] = 0;
    char full_hex[65];
    unsigned long advert_timestamp = 0;
    unsigned long heard_timestamp = 0;
    int snr = 0;
    memset(full_hex, 0, sizeof(full_hex));
    if (sscanf(line, "%64[^,],%lu,%lu,%d", full_hex, &advert_timestamp, &heard_timestamp, &snr) == 4) {
      uint8_t pub_key[PUB_KEY_SIZE];
      if (mesh::Utils::fromHex(pub_key, PUB_KEY_SIZE, full_hex)) {
        neighbours[restored].id = mesh::Identity(pub_key);
        neighbours[restored].advert_timestamp = static_cast<uint32_t>(advert_timestamp);
        neighbours[restored].heard_timestamp = static_cast<uint32_t>(heard_timestamp);
        neighbours[restored].snr = static_cast<int8_t>(constrain(snr, -128, 127));
        restored++;
      }
    }
  }

  file.close();
  _archive_neighbours_dirty = false;
  return restored > 0;
#else
  return false;
#endif
}

void MyMesh::flushArchiveNeighbours() {
#if MAX_NEIGHBOURS
  if (_archive == nullptr || !_archive->isMounted()) {
    return;
  }

  FILESYSTEM* fs = _archive->getFS();
  if (fs == nullptr) {
    return;
  }

  int16_t neighbours_count = 0;
  NeighbourInfo* sorted_neighbours[MAX_NEIGHBOURS];
  for (int i = 0; i < MAX_NEIGHBOURS; i++) {
    if (neighbours[i].heard_timestamp > 0) {
      sorted_neighbours[neighbours_count++] = &neighbours[i];
    }
  }

  std::sort(sorted_neighbours, sorted_neighbours + neighbours_count, [](const NeighbourInfo* a, const NeighbourInfo* b) {
    return a->heard_timestamp > b->heard_timestamp;
  });

  char latest_block[1536];
  size_t latest_len = 0;
  uint32_t latest_epoch_secs = 0;
  for (int i = 0; i < neighbours_count; ++i) {
    char full_hex[65];
    mesh::Utils::toHex(full_hex, sorted_neighbours[i]->id.pub_key, PUB_KEY_SIZE);
    latest_len += snprintf(&latest_block[latest_len], sizeof(latest_block) - latest_len,
                           "%s,%lu,%lu,%d\n",
                           full_hex,
                           static_cast<unsigned long>(sorted_neighbours[i]->advert_timestamp),
                           static_cast<unsigned long>(sorted_neighbours[i]->heard_timestamp),
                           static_cast<int>(sorted_neighbours[i]->snr));
    latest_epoch_secs = max<uint32_t>(latest_epoch_secs, sorted_neighbours[i]->heard_timestamp);
    if (latest_len >= sizeof(latest_block)) {
      latest_len = sizeof(latest_block) - 1;
      break;
    }
  }

  File latest_file = openArchiveWriteWithRecovery(_archive, kArchiveNeighboursLatestPath);
  if (!latest_file) {
    ARCHIVE_LOG("neighbours open failed path=%s", kArchiveNeighboursLatestPath);
    return;
  }
  const size_t latest_written = latest_file.print(latest_block);
  latest_file.flush();
  latest_file.close();

  char daily_path[52];
  buildUtcDailyArchivePath("neighbours", latest_epoch_secs, daily_path, sizeof(daily_path));
  File file = openArchiveAppendWithRecovery(_archive, daily_path);
  if (!file) {
    ARCHIVE_LOG("neighbours open failed path=%s", daily_path);
    return;
  }

  size_t total_written = 0;
  for (int i = 0; i < neighbours_count; ++i) {
    char full_hex[65];
    mesh::Utils::toHex(full_hex, sorted_neighbours[i]->id.pub_key, PUB_KEY_SIZE);
    total_written += file.printf("%s,%lu,%lu,%d\n",
                                 full_hex,
                                 static_cast<unsigned long>(sorted_neighbours[i]->advert_timestamp),
                                 static_cast<unsigned long>(sorted_neighbours[i]->heard_timestamp),
                                 static_cast<int>(sorted_neighbours[i]->snr));
  }
  file.flush();
  file.close();
  ARCHIVE_LOG("neighbours flushed latest=%s bytes=%u log=%s log_bytes=%u count=%d",
              kArchiveNeighboursLatestPath,
              static_cast<unsigned>(latest_written),
              daily_path,
              static_cast<unsigned>(total_written),
              static_cast<int>(neighbours_count));
  _archive_neighbours_dirty = false;
#endif
}

void MyMesh::maybeFlushArchiveNeighbours(unsigned long now_ms) {
#if MAX_NEIGHBOURS
  if (!_archive_neighbours_dirty || _archive == nullptr || !_archive->isMounted()) {
    return;
  }
  if (next_archive_neighbours_flush_ms == 0 || millisHasNowPassed(next_archive_neighbours_flush_ms)) {
    flushArchiveNeighbours();
    next_archive_neighbours_flush_ms = now_ms + kArchiveNeighboursFlushIntervalMs;
  }
#else
  (void)now_ms;
#endif
}

void MyMesh::recordStatsEvent(uint8_t type, int16_t value) {
  _stats_history.recordEvent(type, getRTCClock()->getCurrentTime(), static_cast<uint32_t>(uptime_millis / 1000), value);
}

void MyMesh::updateStatsHistory(unsigned long now_ms) {
#if defined(ESP_PLATFORM) && WITH_WEB_PANEL
  constexpr uint32_t kLowMemoryEnterBytes = 32UL * 1024UL;
  constexpr uint32_t kLowMemoryClearBytes = 48UL * 1024UL;
  constexpr uint32_t kLowMemoryEventCooldownSecs = 5UL * 60UL;
  _stats_history.setArchive(_archive);
  _stats_history.setEnabled(web.isWebStatsEnabled());
  if (!_stats_history.isEnabled()) {
    _stats_state.initialized = false;
    _archive_neighbours_dirty = false;
    return;
  }
  _stats_history.maybeReleaseIdleBuffers(now_ms);

  const bool wifi_connected = network.isWifiConnected();
#ifdef WITH_MQTT_UPLINK
  const bool mqtt_connected = mqtt.isAnyBrokerConnected();
  const uint32_t mqtt_token_refresh_count = mqtt.getTokenRefreshCount();
  const bool mqtt_token_refreshing = mqtt.isTokenRefreshInProgress();
#else
  const bool mqtt_connected = false;
  const uint32_t mqtt_token_refresh_count = 0;
  const bool mqtt_token_refreshing = false;
#endif
  const bool web_panel_up = web.isPanelRunning();
  const bool archive_mounted = (_archive != nullptr) && _archive->isMounted();
#if defined(ESP32)
  const uint32_t free_heap = ESP.getFreeHeap();
  const uint32_t max_alloc_heap = ESP.getMaxAllocHeap();
  const uint32_t uptime_secs = static_cast<uint32_t>(uptime_millis / 1000);
  bool low_memory = _stats_state.low_memory;
  if (!_stats_state.initialized) {
    low_memory = free_heap <= kLowMemoryEnterBytes;
  } else if (low_memory) {
    low_memory = free_heap <= kLowMemoryClearBytes;
  } else {
    low_memory = free_heap <= kLowMemoryEnterBytes;
  }
#else
  const bool low_memory = false;
#endif

  if (!_stats_state.initialized) {
    _stats_state.initialized = true;
    _stats_state.wifi_connected = wifi_connected;
    _stats_state.mqtt_connected = mqtt_connected;
    _stats_state.web_panel_up = web_panel_up;
    _stats_state.archive_mounted = archive_mounted;
    _stats_state.low_memory = low_memory;
    _stats_state.mqtt_token_refresh_count = mqtt_token_refresh_count;
    _stats_state.last_low_memory_event_uptime_secs = 0;
  } else {
    if (_stats_state.mqtt_token_refresh_count != mqtt_token_refresh_count) {
      const uint32_t refresh_delta = mqtt_token_refresh_count - _stats_state.mqtt_token_refresh_count;
      recordStatsEvent(HISTORY_EVENT_MQTT_TOKEN_REFRESHED,
                       static_cast<int16_t>(min<uint32_t>(refresh_delta, 32767)));
      _stats_state.mqtt_token_refresh_count = mqtt_token_refresh_count;
    }
    if (_stats_state.mqtt_connected != mqtt_connected) {
      if (_stats_state.mqtt_connected && !mqtt_connected && mqtt_token_refreshing) {
        // Planned JWT rotation briefly drops MQTT; keep the event stream focused on real outages.
      } else {
        recordStatsEvent(mqtt_connected ? HISTORY_EVENT_MQTT_CONNECTED : HISTORY_EVENT_MQTT_DISCONNECTED);
        _stats_state.mqtt_connected = mqtt_connected;
      }
    }
    if (_stats_state.web_panel_up != web_panel_up) {
      recordStatsEvent(web_panel_up ? HISTORY_EVENT_WEB_STARTED : HISTORY_EVENT_WEB_STOPPED);
      _stats_state.web_panel_up = web_panel_up;
    }
    if (_stats_state.archive_mounted != archive_mounted) {
      recordStatsEvent(archive_mounted ? HISTORY_EVENT_ARCHIVE_MOUNTED : HISTORY_EVENT_ARCHIVE_UNAVAILABLE);
      _stats_state.archive_mounted = archive_mounted;
      if (!_stats_history.isLiveOnly() && archive_mounted) {
        if (getNeighbourCount() == 0) {
          restoreArchiveNeighbours();
        }
        next_archive_neighbours_flush_ms = now_ms + kArchiveNeighboursFlushIntervalMs;
      }
    }
    if (!_stats_history.isLiveOnly() && !_stats_state.low_memory && low_memory) {
#if defined(ESP32)
      if (_stats_state.last_low_memory_event_uptime_secs == 0 ||
          (uptime_secs - _stats_state.last_low_memory_event_uptime_secs) >= kLowMemoryEventCooldownSecs) {
        recordStatsEvent(HISTORY_EVENT_LOW_MEMORY, static_cast<int16_t>(min<uint32_t>(free_heap / 1024, 32767)));
        _stats_state.last_low_memory_event_uptime_secs = uptime_secs;
      }
#endif
    }
    _stats_state.wifi_connected = wifi_connected;
    _stats_state.low_memory = low_memory;
  }

#if defined(ESP32)
  const bool live_stats_headroom_low =
      _stats_history.isLiveOnly() && (free_heap <= kLowMemoryClearBytes || max_alloc_heap <= (24UL * 1024UL));
#else
  const bool live_stats_headroom_low = false;
#endif
  if (next_history_sample_ms == 0 || millisHasNowPassed(next_history_sample_ms)) {
    if (!live_stats_headroom_low) {
      const uint16_t battery_mv = getBatteryMilliVolts();
      WebSensorSnapshot sensor_snapshot = collectWebSensorSnapshot(board, sensors, battery_mv);
      HistorySample sample{};
      sample.epoch_secs = getRTCClock()->getCurrentTime();
      sample.uptime_secs = static_cast<uint32_t>(uptime_millis / 1000);
      sample.packets_sent = radio_driver.getPacketsSent();
      sample.packets_recv = radio_driver.getPacketsRecv();
      sample.battery_mv = battery_mv;
      sample.queue_len = static_cast<uint16_t>(_mgr->getOutboundTotal());
      sample.error_flags = _err_flags;
      sample.recv_errors = static_cast<uint16_t>(min<uint32_t>(radio_driver.getPacketsRecvErrors(), 0xFFFF));
      sample.neighbour_count = static_cast<uint16_t>(min<size_t>(getNeighbourCount(), 0xFFFF));
      sample.direct_dups =
          static_cast<uint16_t>(min<uint32_t>(((SimpleMeshTables *)getTables())->getNumDirectDups(), 0xFFFF));
      sample.flood_dups =
          static_cast<uint16_t>(min<uint32_t>(((SimpleMeshTables *)getTables())->getNumFloodDups(), 0xFFFF));
      sample.last_rssi_x4 = static_cast<int16_t>(radio_driver.getLastRSSI() * 4.0f);
      sample.last_snr_x4 = static_cast<int16_t>(radio_driver.getLastSNR() * 4.0f);
      sample.noise_floor = static_cast<int16_t>(_radio->getNoiseFloor());
      sample.battery_pct = static_cast<int8_t>(board.getBatteryPercent());
      if (sensor_snapshot.has_supply_voltage && std::isfinite(sensor_snapshot.supply_voltage_v)) {
        sample.sensor_flags |= HISTORY_SENSOR_SUPPLY_VOLTAGE;
        sample.supply_voltage_centi_v =
            static_cast<uint16_t>(min<int>(lroundf(sensor_snapshot.supply_voltage_v * 100.0f), 0xFFFF));
      }
      if (sensor_snapshot.has_sensor_temp && std::isfinite(sensor_snapshot.sensor_temp_c)) {
        sample.sensor_flags |= HISTORY_SENSOR_TEMP;
        sample.sensor_temp_deci_c =
            static_cast<int16_t>(max<long>(-32768L, min<long>(32767L, lroundf(sensor_snapshot.sensor_temp_c * 10.0f))));
      }
      if (sensor_snapshot.has_mcu_temp && std::isfinite(sensor_snapshot.mcu_temp_c)) {
        sample.sensor_flags |= HISTORY_SENSOR_MCU_TEMP;
        sample.mcu_temp_deci_c =
            static_cast<int16_t>(max<long>(-32768L, min<long>(32767L, lroundf(sensor_snapshot.mcu_temp_c * 10.0f))));
      }
      if (sensor_snapshot.has_humidity && std::isfinite(sensor_snapshot.humidity_pct)) {
        sample.sensor_flags |= HISTORY_SENSOR_HUMIDITY;
        sample.humidity_deci_pct =
            static_cast<uint16_t>(min<int>(lroundf(sensor_snapshot.humidity_pct * 10.0f), 0xFFFF));
      }
      if (sensor_snapshot.has_pressure && std::isfinite(sensor_snapshot.pressure_hpa)) {
        sample.sensor_flags |= HISTORY_SENSOR_PRESSURE;
        sample.pressure_deci_hpa =
            static_cast<uint16_t>(min<int>(lroundf(sensor_snapshot.pressure_hpa * 10.0f), 0xFFFF));
      }
      if (sensor_snapshot.has_pressure_altitude && std::isfinite(sensor_snapshot.pressure_altitude_m)) {
        sample.sensor_flags |= HISTORY_SENSOR_PRESSURE_ALTITUDE;
        sample.pressure_altitude_m =
            static_cast<int16_t>(max<long>(-32768L, min<long>(32767L, lroundf(sensor_snapshot.pressure_altitude_m))));
      }
      if (sensor_snapshot.has_gps) sample.sensor_flags |= HISTORY_SENSOR_GPS_PRESENT;
      if (sensor_snapshot.gps_enabled) sample.sensor_flags |= HISTORY_SENSOR_GPS_ENABLED;
      if (sensor_snapshot.gps_fix) sample.sensor_flags |= HISTORY_SENSOR_GPS_FIX;
      if (sensor_snapshot.has_gps_lat && std::isfinite(sensor_snapshot.gps_lat)) {
        sample.sensor_flags |= HISTORY_SENSOR_GPS_LAT;
        sample.gps_lat_e6 = static_cast<int32_t>(lroundf(sensor_snapshot.gps_lat * 1000000.0f));
      }
      if (sensor_snapshot.has_gps_lon && std::isfinite(sensor_snapshot.gps_lon)) {
        sample.sensor_flags |= HISTORY_SENSOR_GPS_LON;
        sample.gps_lon_e6 = static_cast<int32_t>(lroundf(sensor_snapshot.gps_lon * 1000000.0f));
      }
      if (sensor_snapshot.has_gps_altitude && std::isfinite(sensor_snapshot.gps_altitude_m)) {
        sample.sensor_flags |= HISTORY_SENSOR_GPS_ALTITUDE;
        sample.gps_altitude_m =
            static_cast<int16_t>(max<long>(-32768L, min<long>(32767L, lroundf(sensor_snapshot.gps_altitude_m))));
      }
      if (sensor_snapshot.has_satellites) {
        sample.sensor_flags |= HISTORY_SENSOR_GPS_SATELLITES;
        sample.gps_satellites = static_cast<uint8_t>(min<long>(sensor_snapshot.satellites, 255));
      }
#if defined(ESP32)
      sample.heap_free = free_heap;
      sample.heap_min = ESP.getMinFreeHeap();
      sample.psram_free = ESP.getFreePsram();
      sample.psram_min = ESP.getMinFreePsram();
#endif
      if (board.isExternalPowered()) sample.flags |= HISTORY_FLAG_EXTERNAL_POWER;
      if (board.isCharging()) sample.flags |= HISTORY_FLAG_CHARGING;
      if (board.isVbusPresent()) sample.flags |= HISTORY_FLAG_VBUS;
      if (wifi_connected) sample.flags |= HISTORY_FLAG_WIFI_CONNECTED;
      if (mqtt_connected) sample.flags |= HISTORY_FLAG_MQTT_CONNECTED;
      if (web.isWebEnabled()) sample.flags |= HISTORY_FLAG_WEB_ENABLED;
      if (web_panel_up) sample.flags |= HISTORY_FLAG_WEB_PANEL_UP;
      if (archive_mounted) sample.flags |= HISTORY_FLAG_ARCHIVE_MOUNTED;
      _stats_history.pushSample(sample);
    }
    next_history_sample_ms = now_ms + 60000UL;
  }

  _stats_history.maybeFlush(now_ms);
  if (!_stats_history.isLiveOnly()) {
    maybeFlushArchiveNeighbours(now_ms);
  }
#else
  (void)now_ms;
#endif
}

bool MyMesh::appendJsonEvents(char* reply, size_t reply_size, size_t& offset) const {
  offset += snprintf(&reply[offset], reply_size - offset, "\"events\":[");
  const size_t max_events = min<size_t>(_stats_history.getEventCount(), 6);
  const uint32_t now_epoch_secs = getRTCClock()->getCurrentTime();
  const uint32_t now_uptime_secs = static_cast<uint32_t>(uptime_millis / 1000);
  for (size_t i = 0; i < max_events; ++i) {
    HistoryEvent event{};
    if (!_stats_history.getRecentEvent(i, event)) {
      break;
    }
    const uint32_t age_secs = (now_epoch_secs >= event.epoch_secs && event.epoch_secs > 0)
        ? (now_epoch_secs - event.epoch_secs)
        : ((now_uptime_secs >= event.uptime_secs) ? (now_uptime_secs - event.uptime_secs) : event.uptime_secs);
    offset += snprintf(&reply[offset], reply_size - offset,
                       "%s{\"t\":%lu,\"type\":\"%s\",\"value\":%d}",
                       i == 0 ? "" : ",",
                       static_cast<unsigned long>(age_secs),
                       StatsHistory::getEventTypeName(event.type),
                       static_cast<int>(event.value));
    if (offset >= reply_size) {
      return false;
    }
  }
  offset += snprintf(&reply[offset], reply_size - offset, "]");
  return offset < reply_size;
}

bool MyMesh::appendJsonNeighbours(char* reply, size_t reply_size, size_t& offset) const {
  offset += snprintf(&reply[offset], reply_size - offset, "\"neighbors_detail\":[");
  if (offset >= reply_size) {
    return false;
  }

#if MAX_NEIGHBOURS
  constexpr size_t kMaxNeighboursJson = 10;
  int16_t neighbours_count = 0;
  NeighbourInfo* sorted_neighbours[MAX_NEIGHBOURS];
  for (int i = 0; i < MAX_NEIGHBOURS; i++) {
    auto neighbour = const_cast<NeighbourInfo*>(&neighbours[i]);
    if (neighbour->heard_timestamp > 0) {
      sorted_neighbours[neighbours_count++] = neighbour;
    }
  }

  std::sort(sorted_neighbours, sorted_neighbours + neighbours_count, [](const NeighbourInfo* a, const NeighbourInfo* b) {
    return a->heard_timestamp > b->heard_timestamp;
  });

  const size_t emit_count = min<size_t>(neighbours_count, kMaxNeighboursJson);
  const uint32_t now_secs = getRTCClock()->getCurrentTime();
  for (size_t i = 0; i < emit_count; ++i) {
    const NeighbourInfo* neighbour = sorted_neighbours[i];
    char hex[7];
    char full_hex[65];
    mesh::Utils::toHex(hex, neighbour->id.pub_key, 3);
    mesh::Utils::toHex(full_hex, neighbour->id.pub_key, PUB_KEY_SIZE);
    const uint32_t heard_secs_ago = now_secs - neighbour->heard_timestamp;
    const uint32_t advert_secs_ago = now_secs - neighbour->advert_timestamp;
    offset += snprintf(&reply[offset], reply_size - offset,
                       "%s{\"id\":\"%s\",\"full_id\":\"%s\",\"heard_secs_ago\":%lu,\"advert_secs_ago\":%lu,\"snr_db\":%.2f}",
                       i == 0 ? "" : ",",
                       hex,
                       full_hex,
                       static_cast<unsigned long>(heard_secs_ago),
                       static_cast<unsigned long>(advert_secs_ago),
                       static_cast<double>(neighbour->snr) / 4.0);
    if (offset >= reply_size) {
      return false;
    }
  }
#endif

  offset += snprintf(&reply[offset], reply_size - offset, "]");
  return offset < reply_size;
}

void MyMesh::saveIdentity(const mesh::LocalIdentity &new_id) {
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  IdentityStore store(*_fs, "");
#elif defined(ESP32)
  IdentityStore store(*_fs, "/identity");
#elif defined(RP2040_PLATFORM)
  IdentityStore store(*_fs, "/identity");
#else
#error "need to define saveIdentity()"
#endif
  store.save("_main", new_id);
}

void MyMesh::clearStats() {
  radio_driver.resetStats();
  resetStats();
  ((SimpleMeshTables *)getTables())->resetStats();
}

void MyMesh::prepareForOTAStart() {
#if defined(ESP_PLATFORM) && WITH_WEB_PANEL
  web.prepareForOTAStart();
#endif
}

void MyMesh::handleCommand(uint32_t sender_timestamp, char *command, char *reply) {
  if (region_load_active) {
    if (StrHelper::isBlank(command)) {  // empty/blank line, signal to terminate 'load' operation
      region_map = temp_map;  // copy over the temp instance as new current map
      region_load_active = false;

      sprintf(reply, "OK - loaded %d regions", region_map.getCount());
    } else {
      char *np = command;
      while (*np == ' ') np++;   // skip indent
      int indent = np - command;

      char *ep = np;
      while (RegionMap::is_name_char(*ep)) ep++;
      if (*ep) { *ep++ = 0; }  // set null terminator for end of name

      while (*ep && *ep != 'F') ep++;  // look for (optional) flags

      if (indent > 0 && indent < 8 && strlen(np) > 0) {
        auto parent = load_stack[indent - 1];
        if (parent) {
          auto old = region_map.findByName(np);
          auto nw = temp_map.putRegion(np, parent->id, old ? old->id : 0);  // carry-over the current ID (if name already exists)
          if (nw) {
            nw->flags = old ? old->flags : (*ep == 'F' ? 0 : REGION_DENY_FLOOD);   // carry-over flags from curr

            load_stack[indent] = nw;  // keep pointers to parent regions, to resolve parent_id's
          }
        }
      }
      reply[0] = 0;
    }
    return;
  }

  while (*command == ' ') command++; // skip leading spaces

  if (strlen(command) > 4 && command[2] == '|') { // optional prefix (for companion radio CLI)
    memcpy(reply, command, 3);                    // reflect the prefix back
    reply += 3;
    command += 3;
  }

  // handle ACL related commands
  if (memcmp(command, "setperm ", 8) == 0) {   // format:  setperm {pubkey-hex} {permissions-int8}
    char* hex = &command[8];
    char* sp = strchr(hex, ' ');   // look for separator char
    if (sp == NULL) {
      strcpy(reply, "Err - bad params");
    } else {
      *sp++ = 0;   // replace space with null terminator

      uint8_t pubkey[PUB_KEY_SIZE];
      int hex_len = min(sp - hex, PUB_KEY_SIZE*2);
      if (mesh::Utils::fromHex(pubkey, hex_len / 2, hex)) {
        uint8_t perms = atoi(sp);
        if (acl.applyPermissions(self_id, pubkey, hex_len / 2, perms)) {
          dirty_contacts_expiry = futureMillis(LAZY_CONTACTS_WRITE_DELAY);   // trigger acl.save()
          strcpy(reply, "OK");
        } else {
          strcpy(reply, "Err - invalid params");
        }
      } else {
        strcpy(reply, "Err - bad pubkey");
      }
    }
  } else if (sender_timestamp == 0 && strcmp(command, "get acl") == 0) {
    Serial.println("ACL:");
    for (int i = 0; i < acl.getNumClients(); i++) {
      auto c = acl.getClientByIdx(i);
      if (c->permissions == 0) continue;  // skip deleted (or guest) entries

      Serial.printf("%02X ", c->permissions);
      mesh::Utils::printHex(Serial, c->id.pub_key, PUB_KEY_SIZE);
      Serial.printf("\n");
    }
    reply[0] = 0;
  } else if (memcmp(command, "discover.neighbors", 18) == 0) {
    const char* sub = command + 18;
    while (*sub == ' ') sub++;
    if (*sub != 0) {
      strcpy(reply, "Err - discover.neighbors has no options");
    } else {
      sendNodeDiscoverReq();
      strcpy(reply, "OK - Discover sent");
    }
#if defined(ESP_PLATFORM) && WITH_WEB_PANEL
  } else if (strcmp(command, "get web.status") == 0 || strcmp(command, "get web") == 0) {
    web.formatWebStatusReply(reply, 160);
  } else if (strcmp(command, "get web.stats.status") == 0) {
    snprintf(reply, 160,
             "> enabled:%s history:%s mode:%s psram:%s psram_bytes:%lu boot_auto:%s samples:%u/%u events:%u/%u archive:%s",
             web.isWebStatsEnabled() ? "on" : "off",
             (_stats_history.isEnabled() && _stats_history.isRecentHistoryAvailable()) ? "active" : "inactive",
             _stats_history.isLiveOnly() ? "live" : "full",
             _stats_history.isPsramBacked() ? "yes" : "no",
             static_cast<unsigned long>(_stats_history.getDetectedPsramSizeBytes()),
             _stats_history.isBootAutoCaptureExpected() ? "yes" : "no",
             static_cast<unsigned>(_stats_history.getSampleCount()),
             static_cast<unsigned>(_stats_history.getSampleCapacity()),
             static_cast<unsigned>(_stats_history.getEventCount()),
             static_cast<unsigned>(_stats_history.getEventCapacity()),
             (_archive != nullptr && _archive->isMounted()) ? "mounted" : "unavailable");
#endif
#if defined(TBEAM_1W)
  } else if (strcmp(command, "get fan") == 0) {
    auto& tbeam1w_board = static_cast<TBeam1WBoard&>(board);
    const float temp_c = tbeam1w_board.getLastBoardTemperatureC();
    if (isnan(temp_c)) {
      snprintf(reply, 160, "> mode:%s state:%s timeout:%lus temp:unavailable",
               tbeam1w_board.getFanModeName(),
               tbeam1w_board.isFanEnabled() ? "on" : "off",
               static_cast<unsigned long>(tbeam1w_board.getFanPostTxHoldMs() / 1000UL));
    } else {
      snprintf(reply, 160, "> mode:%s state:%s timeout:%lus temp:%.2fC",
               tbeam1w_board.getFanModeName(),
               tbeam1w_board.isFanEnabled() ? "on" : "off",
               static_cast<unsigned long>(tbeam1w_board.getFanPostTxHoldMs() / 1000UL),
               temp_c);
    }
  } else if (memcmp(command, "set fan ", 8) == 0) {
    auto& tbeam1w_board = static_cast<TBeam1WBoard&>(board);
    const char* mode = &command[8];
    if (memcmp(mode, "auto", 4) == 0) {
      _prefs.fan_mode = static_cast<uint8_t>(TBeam1WBoard::FanMode::Auto);
      tbeam1w_board.setFanMode(TBeam1WBoard::FanMode::Auto);
      savePrefs();
      strcpy(reply, "OK - fan auto");
    } else if (memcmp(mode, "on", 2) == 0) {
      _prefs.fan_mode = static_cast<uint8_t>(TBeam1WBoard::FanMode::On);
      tbeam1w_board.setFanMode(TBeam1WBoard::FanMode::On);
      savePrefs();
      strcpy(reply, "OK - fan on");
    } else if (memcmp(mode, "off", 3) == 0) {
      _prefs.fan_mode = static_cast<uint8_t>(TBeam1WBoard::FanMode::Off);
      tbeam1w_board.setFanMode(TBeam1WBoard::FanMode::Off);
      savePrefs();
      strcpy(reply, "OK - fan off");
    } else if (memcmp(mode, "timeout ", 8) == 0) {
      char* end = nullptr;
      const unsigned long timeout_s = strtoul(&mode[8], &end, 10);
      while (end != nullptr && *end == ' ') end++;
      if (end != nullptr && *end == 's' && *(end + 1) == 0 && tbeam1w_board.setFanPostTxHoldMs(static_cast<uint32_t>(timeout_s * 1000UL))) {
        _prefs.fan_timeout_secs = static_cast<uint16_t>(timeout_s);
        savePrefs();
        snprintf(reply, 160, "OK - fan timeout %lus", timeout_s);
      } else {
        strcpy(reply, "Err - use 0s..600s");
      }
    } else {
      strcpy(reply, "Err - use auto|on|off|timeout <Ns>");
    }
#endif
#if defined(ESP_PLATFORM)
  } else if (memcmp(command, "get wifi.status", 15) == 0) {
    network.formatWifiStatusReply(reply, 160);
  } else if (memcmp(command, "get wifi.ssid", 13) == 0) {
    sprintf(reply, "> %s", network.getWifiSSID()[0] ? network.getWifiSSID() : "-");
  } else if (memcmp(command, "get wifi.powersaving", 20) == 0) {
    sprintf(reply, "> %s", network.getWifiPowerSave());
#endif
#if defined(ESP_PLATFORM) && WITH_WEB_PANEL
  } else if (memcmp(command, "set web ", 8) == 0) {
    web.setWebEnabled(memcmp(&command[8], "on", 2) == 0);
    strcpy(reply, "OK");
  } else if (memcmp(command, "set.web ", 8) == 0) {
    web.setWebEnabled(memcmp(&command[8], "on", 2) == 0);
    strcpy(reply, "OK");
  } else if (memcmp(command, "set web.stats ", 14) == 0 || memcmp(command, "set.web.stats ", 15) == 0) {
    const char* value = (memcmp(command, "set web.stats ", 14) == 0) ? &command[14] : &command[15];
    const bool enabled = memcmp(value, "on", 2) == 0;
    if (web.setWebStatsEnabled(enabled)) {
      _stats_history.setEnabled(enabled);
      recordStatsEvent(enabled ? HISTORY_EVENT_STATS_ENABLED : HISTORY_EVENT_STATS_DISABLED);
      if (enabled) {
        next_history_sample_ms = millis();
      } else {
        _stats_state.initialized = false;
      }
      strcpy(reply, enabled ? "OK - web.stats on" : "OK - web.stats off");
    } else {
      strcpy(reply, "Err - unable to update web.stats");
    }
#endif
#if defined(ESP_PLATFORM)
  } else if (memcmp(command, "set wifi.ssid ", 14) == 0) {
    if (network.setWifiSSID(&command[14])) {
      strcpy(reply, "OK");
    } else {
      strcpy(reply, "Err - bad wifi.ssid");
    }
  } else if (memcmp(command, "set wifi.pwd ", 13) == 0) {
    if (network.setWifiPassword(&command[13])) {
      strcpy(reply, "OK");
    } else {
      strcpy(reply, "Err - bad wifi.pwd");
    }
  } else if (memcmp(command, "set wifi.powersaving ", 21) == 0) {
    if (network.setWifiPowerSave(&command[21])) {
      strcpy(reply, "OK");
    } else {
      strcpy(reply, "Err - use none|min|max");
    }
#endif
#ifdef WITH_MQTT_UPLINK
  } else if (memcmp(command, "mqtt.owner ", 11) == 0) {
    if (mqtt.setOwnerPublicKey(&command[11])) {
      strcpy(reply, "OK");
    } else {
      strcpy(reply, "Err - owner must be 64 hex chars");
    }
  } else if (memcmp(command, "mqtt.email ", 11) == 0) {
    if (mqtt.setOwnerEmail(&command[11])) {
      strcpy(reply, "OK");
    } else {
      strcpy(reply, "Err - bad mqtt.email");
    }
  } else if (strcmp(command, "send mqtt.status") == 0) {
    if (mqtt.sendStatusNow()) {
      strcpy(reply, "OK");
    } else {
      strcpy(reply, "Err - mqtt status unavailable");
    }
  } else if (strcmp(command, "get mqtt.statuscfg") == 0) {
    sprintf(reply, "> %s", mqtt.isStatusEnabled() ? "on" : "off");
  } else if (strcmp(command, "get mqtt.status") == 0) {
    mqtt.formatStatusReply(reply, 160);
  } else if (strcmp(command, "get mqtt.client_version") == 0) {
    sprintf(reply, "> %s", mqtt.getClientVersion());
  } else if (memcmp(command, "get mqtt.iata", 13) == 0) {
    sprintf(reply, "> %s", mqtt.getIata());
  } else if (memcmp(command, "get mqtt.owner", 14) == 0) {
    sprintf(reply, "> %s", mqtt.getOwnerPublicKey()[0] ? mqtt.getOwnerPublicKey() : "-");
  } else if (memcmp(command, "get mqtt.email", 14) == 0) {
    sprintf(reply, "> %s", mqtt.getOwnerEmail()[0] ? mqtt.getOwnerEmail() : "-");
  } else if (memcmp(command, "get mqtt.packets", 16) == 0) {
    sprintf(reply, "> %s", mqtt.isPacketsEnabled() ? "on" : "off");
  } else if (memcmp(command, "get mqtt.raw", 12) == 0) {
    sprintf(reply, "> %s", mqtt.isRawEnabled() ? "on" : "off");
  } else if (memcmp(command, "get mqtt.tx", 11) == 0) {
    sprintf(reply, "> %s", mqtt.isTxEnabled() ? "on" : "off");
  } else if (memcmp(command, "get mqtt.eastmesh-au", 20) == 0 || memcmp(command, "get mqtt.eastmesh.au", 20) == 0) {
    sprintf(reply, "> %s", mqtt.isEndpointEnabled(0x01) ? "on" : "off");
  } else if (memcmp(command, "get mqtt.letsmesh-eu", 21) == 0 || memcmp(command, "get mqtt.letsmesh.eu", 21) == 0) {
    sprintf(reply, "> %s", mqtt.isEndpointEnabled(0x02) ? "on" : "off");
  } else if (memcmp(command, "get mqtt.letsmesh-us", 21) == 0 || memcmp(command, "get mqtt.letsmesh.us", 21) == 0) {
    sprintf(reply, "> %s", mqtt.isEndpointEnabled(0x04) ? "on" : "off");
  } else if (memcmp(command, "set mqtt.tx ", 12) == 0) {
    mqtt.setTxEnabled(memcmp(&command[12], "on", 2) == 0);
    strcpy(reply, "OK");
  } else if (memcmp(command, "set mqtt.iata ", 14) == 0) {
    if (mqtt.setIata(&command[14])) {
      strcpy(reply, "OK");
    } else {
      strcpy(reply, "Err - bad mqtt.iata");
    }
  } else if (memcmp(command, "set mqtt.owner ", 15) == 0) {
    if (mqtt.setOwnerPublicKey(&command[15])) {
      strcpy(reply, "OK");
    } else {
      strcpy(reply, "Err - owner must be 64 hex chars");
    }
  } else if (memcmp(command, "set mqtt.email ", 15) == 0) {
    if (mqtt.setOwnerEmail(&command[15])) {
      strcpy(reply, "OK");
    } else {
      strcpy(reply, "Err - bad mqtt.email");
    }
  } else if (memcmp(command, "set mqtt.packets ", 17) == 0) {
    mqtt.setPacketsEnabled(memcmp(&command[17], "on", 2) == 0);
    strcpy(reply, "OK");
  } else if (memcmp(command, "set mqtt.raw ", 13) == 0) {
    mqtt.setRawEnabled(memcmp(&command[13], "on", 2) == 0);
    strcpy(reply, "OK");
  } else if (memcmp(command, "set mqtt.status ", 16) == 0) {
    mqtt.setStatusEnabled(memcmp(&command[16], "on", 2) == 0);
    strcpy(reply, "OK");
  } else if (memcmp(command, "set mqtt.eastmesh-au ", 21) == 0 || memcmp(command, "set mqtt.eastmesh.au ", 21) == 0) {
    if (mqtt.setEndpointEnabled(0x01, memcmp(&command[21], "on", 2) == 0)) {
      strcpy(reply, "OK");
    } else {
      strcpy(reply, "Err - max 2 mqtt brokers");
    }
  } else if (memcmp(command, "set mqtt.letsmesh-eu ", 21) == 0 || memcmp(command, "set mqtt.letsmesh.eu ", 21) == 0) {
    if (mqtt.setEndpointEnabled(0x02, memcmp(&command[21], "on", 2) == 0)) {
      strcpy(reply, "OK");
    } else {
      strcpy(reply, "Err - max 2 mqtt brokers");
    }
  } else if (memcmp(command, "set mqtt.letsmesh-us ", 21) == 0 || memcmp(command, "set mqtt.letsmesh.us ", 21) == 0) {
    if (mqtt.setEndpointEnabled(0x04, memcmp(&command[21], "on", 2) == 0)) {
      strcpy(reply, "OK");
    } else {
      strcpy(reply, "Err - max 2 mqtt brokers");
    }
#endif
  } else{
    _cli.handleCommand(sender_timestamp, command, reply);  // common CLI commands
  }
}

void MyMesh::runWebCommand(const char* command, char* reply, size_t reply_size) {
  if (reply_size == 0) {
    return;
  }
  reply[0] = 0;
  if (command == nullptr) {
    strncpy(reply, "Err - empty command", reply_size - 1);
    reply[reply_size - 1] = 0;
    return;
  }

  char command_buf[192];
  StrHelper::strncpy(command_buf, command, sizeof(command_buf));
  handleCommand(0, command_buf, reply);
  reply[reply_size - 1] = 0;
}

bool MyMesh::isWebStatsEnabled() const {
#if defined(ESP_PLATFORM) && WITH_WEB_PANEL
  return web.isWebStatsEnabled();
#else
  return false;
#endif
}

bool MyMesh::formatWebStatsSummaryJson(char* reply, size_t reply_size) {
  if (reply == nullptr || reply_size == 0) {
    return false;
  }
  reply[0] = 0;

#if !defined(ESP_PLATFORM) || !WITH_WEB_PANEL
  return false;
#else
  char wifi_ssid[48];
  char wifi_status[20];
  char wifi_state[24];
  char wifi_ip[20];
  char wifi_signal[16];
  char wifi_powersave[12];
  escapeJsonString(network.getWifiSSID()[0] ? network.getWifiSSID() : "-", wifi_ssid, sizeof(wifi_ssid));
  escapeJsonString(network.getWifiPowerSave(), wifi_powersave, sizeof(wifi_powersave));
  int wifi_rssi = 0;
  int wifi_quality = 0;
  int wifi_code = 0;

#if defined(ESP32)
  if (network.getWifiSSID()[0] == 0) {
    strncpy(wifi_status, "unconfigured", sizeof(wifi_status) - 1);
    wifi_status[sizeof(wifi_status) - 1] = 0;
    strncpy(wifi_state, "unconfigured", sizeof(wifi_state) - 1);
    wifi_state[sizeof(wifi_state) - 1] = 0;
    strncpy(wifi_ip, "--", sizeof(wifi_ip) - 1);
    wifi_ip[sizeof(wifi_ip) - 1] = 0;
    strncpy(wifi_signal, "--", sizeof(wifi_signal) - 1);
    wifi_signal[sizeof(wifi_signal) - 1] = 0;
  } else if (network.isWifiConnected()) {
    strncpy(wifi_status, "connected", sizeof(wifi_status) - 1);
    wifi_status[sizeof(wifi_status) - 1] = 0;
    strncpy(wifi_state, "connected", sizeof(wifi_state) - 1);
    wifi_state[sizeof(wifi_state) - 1] = 0;
    String ip = WiFi.localIP().toString();
    escapeJsonString(ip.c_str(), wifi_ip, sizeof(wifi_ip));
    wifi_rssi = WiFi.RSSI();
    wifi_code = static_cast<int>(WiFi.status());
    if (wifi_rssi <= -100) {
      wifi_quality = 0;
      strncpy(wifi_signal, "poor", sizeof(wifi_signal) - 1);
    } else if (wifi_rssi >= -50) {
      wifi_quality = 100;
      strncpy(wifi_signal, "excellent", sizeof(wifi_signal) - 1);
    } else {
      wifi_quality = 2 * (wifi_rssi + 100);
      if (wifi_rssi >= -60) {
        strncpy(wifi_signal, "excellent", sizeof(wifi_signal) - 1);
      } else if (wifi_rssi >= -67) {
        strncpy(wifi_signal, "good", sizeof(wifi_signal) - 1);
      } else if (wifi_rssi >= -75) {
        strncpy(wifi_signal, "fair", sizeof(wifi_signal) - 1);
      } else {
        strncpy(wifi_signal, "poor", sizeof(wifi_signal) - 1);
      }
    }
    wifi_signal[sizeof(wifi_signal) - 1] = 0;
  } else {
    strncpy(wifi_status, "connecting", sizeof(wifi_status) - 1);
    wifi_status[sizeof(wifi_status) - 1] = 0;
    wifi_code = static_cast<int>(WiFi.status());
    switch (WiFi.status()) {
      case WL_IDLE_STATUS:
        strncpy(wifi_state, "idle", sizeof(wifi_state) - 1);
        break;
      case WL_NO_SSID_AVAIL:
        strncpy(wifi_state, "no_ssid", sizeof(wifi_state) - 1);
        break;
      case WL_SCAN_COMPLETED:
        strncpy(wifi_state, "scan_completed", sizeof(wifi_state) - 1);
        break;
      case WL_CONNECT_FAILED:
        strncpy(wifi_state, "connect_failed", sizeof(wifi_state) - 1);
        break;
      case WL_CONNECTION_LOST:
        strncpy(wifi_state, "connection_lost", sizeof(wifi_state) - 1);
        break;
      case WL_DISCONNECTED:
        strncpy(wifi_state, "disconnected", sizeof(wifi_state) - 1);
        break;
      default:
        strncpy(wifi_state, "unknown", sizeof(wifi_state) - 1);
        break;
    }
    wifi_state[sizeof(wifi_state) - 1] = 0;
    strncpy(wifi_ip, "--", sizeof(wifi_ip) - 1);
    wifi_ip[sizeof(wifi_ip) - 1] = 0;
    strncpy(wifi_signal, "--", sizeof(wifi_signal) - 1);
    wifi_signal[sizeof(wifi_signal) - 1] = 0;
  }
#else
  strncpy(wifi_status, "unsupported", sizeof(wifi_status) - 1);
  wifi_status[sizeof(wifi_status) - 1] = 0;
  strncpy(wifi_state, "unsupported", sizeof(wifi_state) - 1);
  wifi_state[sizeof(wifi_state) - 1] = 0;
  strncpy(wifi_ip, "--", sizeof(wifi_ip) - 1);
  wifi_ip[sizeof(wifi_ip) - 1] = 0;
  strncpy(wifi_signal, "--", sizeof(wifi_signal) - 1);
  wifi_signal[sizeof(wifi_signal) - 1] = 0;
#endif

  const uint16_t battery_mv = getBatteryMilliVolts(true);
  const int battery_pct = board.getBatteryPercent();
  const uint16_t battery_min_mv = board.getBatteryMinMilliVolts();
  const uint16_t battery_max_mv = board.getBatteryMaxMilliVolts();
  const int battery_display_pct =
      (battery_pct >= 0) ? std::max(0, std::min(100, battery_pct))
                         : clampBatteryPercentFromRange(battery_mv, battery_min_mv, battery_max_mv);
  const WebSensorSnapshot sensor_snapshot = collectWebSensorSnapshot(board, sensors, battery_mv);
  const bool archive_available = (_archive != nullptr) && _archive->isMounted();
#ifdef WITH_MQTT_UPLINK
  const bool mqtt_connected = mqtt.isAnyBrokerConnected();
  const char* mqtt_state = mqtt.getAggregateBrokerState();
#else
  const bool mqtt_connected = false;
  const char* mqtt_state = "down";
#endif
  const bool web_panel_up = web.isPanelRunning();
  const char* archive_name = (_archive != nullptr) ? _archive->getLogicalName() : "archive";
  const char* archive_path = (_archive != nullptr) ? _archive->getLogicalStatsPath() : "archive:/stats";
  const char* archive_type = (_archive != nullptr) ? _archive->getCardTypeName() : "unavailable";
  _stats_history.noteAccess(millis());
  const uint32_t heap_free = ESP.getFreeHeap();
  const uint32_t heap_min = ESP.getMinFreeHeap();
  const uint32_t heap_max = ESP.getMaxAllocHeap();
  const uint32_t psram_free = ESP.getFreePsram();
  const uint32_t psram_min = ESP.getMinFreePsram();
  const uint32_t psram_max = ESP.getMaxAllocPsram();

  size_t offset = 0;
  offset += snprintf(&reply[offset], reply_size - offset,
                     "{\"enabled\":true,"
                     "\"history\":{\"active\":%s,\"psram\":%s,\"degraded\":%s,\"live_only\":%s,\"samples\":%u,\"sample_capacity\":%u,\"sample_interval_secs\":%lu,"
                     "\"archive_restored\":%s,\"archive_restored_samples\":%u,\"archive_summary_interval_secs\":%lu,"
                     "\"events\":%u,\"event_capacity\":%u},"
                     "\"archive\":{\"logical\":\"%s\",\"available\":%s,\"path\":\"%s\",\"type\":\"%s\","
                     "\"total_bytes\":%llu,\"used_bytes\":%llu},"
                     "\"core\":{\"battery_mv\":%u,\"battery_pct\":%d,\"battery_display_pct\":%d,\"battery_min_mv\":%u,\"battery_max_mv\":%u,"
                     "\"uptime_secs\":%lu,\"errors\":%u,\"queue_len\":%u,"
                     "\"external_power\":%s,\"charging\":%s,\"vbus\":%s},"
                     "\"radio\":{\"noise_floor\":%d,\"last_rssi\":%.2f,\"last_snr\":%.2f,\"tx_air_secs\":%lu,\"rx_air_secs\":%lu},"
                     "\"packets\":{\"recv\":%u,\"sent\":%u,\"flood_tx\":%u,\"direct_tx\":%u,\"flood_rx\":%u,\"direct_rx\":%u,"
                     "\"recv_errors\":%u,\"direct_dups\":%u,\"flood_dups\":%u,\"neighbors\":%u},"
                     "\"memory\":{\"heap_free\":%u,\"heap_min\":%u,\"heap_max\":%u,\"psram_free\":%u,\"psram_min\":%u,\"psram_max\":%u},"
                     "\"wifi\":{\"ssid\":\"%s\",\"status\":\"%s\",\"connected\":%s,\"state\":\"%s\",\"code\":%d,\"ip\":\"%s\",\"rssi\":%d,\"quality\":%d,\"signal\":\"%s\",\"powersave\":\"%s\"},"
                     "\"services\":{\"mqtt_connected\":%s,\"mqtt_state\":\"%s\",\"web_enabled\":%s,\"web_panel_up\":%s,\"web_auth\":\"%s\","
                     "\"archive_available\":%s}",
                     (_stats_history.isEnabled() && _stats_history.isRecentHistoryAvailable()) ? "true" : "false",
                     _stats_history.isPsramBacked() ? "true" : "false",
                     _stats_history.isDegraded() ? "true" : "false",
                     _stats_history.isLiveOnly() ? "true" : "false",
                     static_cast<unsigned>(_stats_history.getSampleCount()),
                     static_cast<unsigned>(_stats_history.getSampleCapacity()),
                     static_cast<unsigned long>(StatsHistory::getSampleIntervalSecs()),
                     _stats_history.hasArchiveRestore() ? "true" : "false",
                     static_cast<unsigned>(_stats_history.getRestoredSampleCount()),
                     static_cast<unsigned long>(StatsHistory::getArchiveSummaryIntervalSecs()),
                     static_cast<unsigned>(_stats_history.getEventCount()),
                     static_cast<unsigned>(_stats_history.getEventCapacity()),
                     archive_name,
                     archive_available ? "true" : "false",
                     archive_path,
                     archive_type,
                     static_cast<unsigned long long>(_archive != nullptr ? _archive->getTotalBytes() : 0),
                     static_cast<unsigned long long>(_archive != nullptr ? _archive->getUsedBytes() : 0),
                     battery_mv,
                     battery_pct,
                     battery_display_pct,
                     battery_min_mv,
                     battery_max_mv,
                     static_cast<unsigned long>(uptime_millis / 1000),
                     _err_flags,
                     static_cast<unsigned>(_mgr->getOutboundTotal()),
                     board.isExternalPowered() ? "true" : "false",
                     board.isCharging() ? "true" : "false",
                     board.isVbusPresent() ? "true" : "false",
                     static_cast<int>(_radio->getNoiseFloor()),
                     radio_driver.getLastRSSI(),
                     radio_driver.getLastSNR(),
                     static_cast<unsigned long>(getTotalAirTime() / 1000),
                     static_cast<unsigned long>(getReceiveAirTime() / 1000),
                     static_cast<unsigned>(radio_driver.getPacketsRecv()),
                     static_cast<unsigned>(radio_driver.getPacketsSent()),
                     static_cast<unsigned>(getNumSentFlood()),
                     static_cast<unsigned>(getNumSentDirect()),
                     static_cast<unsigned>(getNumRecvFlood()),
                     static_cast<unsigned>(getNumRecvDirect()),
                     static_cast<unsigned>(radio_driver.getPacketsRecvErrors()),
                     static_cast<unsigned>(((SimpleMeshTables *)getTables())->getNumDirectDups()),
                     static_cast<unsigned>(((SimpleMeshTables *)getTables())->getNumFloodDups()),
                     static_cast<unsigned>(getNeighbourCount()),
                     heap_free,
                     heap_min,
                     heap_max,
                     psram_free,
                     psram_min,
                     psram_max,
                     wifi_ssid,
                     wifi_status,
                     network.isWifiConnected() ? "true" : "false",
                     wifi_state,
                     wifi_code,
                     wifi_ip,
                     wifi_rssi,
                     wifi_quality,
                     wifi_signal,
                     wifi_powersave,
                     mqtt_connected ? "true" : "false",
                     mqtt_state,
                     web.isWebEnabled() ? "true" : "false",
                     web_panel_up ? "true" : "false",
                     web.isPanelUnlocked() ? "unlocked" : "locked",
                     archive_available ? "true" : "false");
  if (offset >= reply_size) {
    return false;
  }

  offset += snprintf(&reply[offset], reply_size - offset, ",");
  if (!appendJsonSensors(reply, reply_size, offset, sensor_snapshot)) {
    return false;
  }
  offset += snprintf(&reply[offset], reply_size - offset, ",");
  if (!appendJsonEvents(reply, reply_size, offset)) {
    return false;
  }
  offset += snprintf(&reply[offset], reply_size - offset, ",");
  if (!appendJsonNeighbours(reply, reply_size, offset)) {
    return false;
  }
  offset += snprintf(&reply[offset], reply_size - offset, "}");
  return offset < reply_size;
#endif
}

bool MyMesh::formatWebStatsSeriesJson(const char* series, char* reply, size_t reply_size) {
#if defined(ESP_PLATFORM) && WITH_WEB_PANEL
  if (!web.isWebStatsEnabled()) {
    if (reply != nullptr && reply_size > 0) {
      reply[0] = 0;
    }
    return false;
  }
  _stats_history.noteAccess(millis());
  return _stats_history.buildSeriesJson(
      series,
      reply,
      reply_size,
      getRTCClock()->getCurrentTime(),
      static_cast<uint32_t>(uptime_millis / 1000));
#else
  (void)series;
  if (reply != nullptr && reply_size > 0) {
    reply[0] = 0;
  }
  return false;
#endif
}

bool MyMesh::appendJsonSensors(char* reply, size_t reply_size, size_t& offset, const WebSensorSnapshot& snapshot) const {
#if defined(ESP_PLATFORM) && WITH_WEB_PANEL
  bool needs_comma = false;
  const int open_written = snprintf(&reply[offset], reply_size - offset, "\"sensors\":{");
  if (open_written < 0 || static_cast<size_t>(open_written) >= (reply_size - offset)) {
    return false;
  }
  offset += static_cast<size_t>(open_written);

  if (snapshot.has_gps) {
    if (!appendJsonBoolField(reply, reply_size, offset, needs_comma, "gps_enabled", snapshot.gps_enabled)) {
      return false;
    }
    if (!appendJsonBoolField(reply, reply_size, offset, needs_comma, "gps_fix", snapshot.gps_fix)) {
      return false;
    }
  }
  if (snapshot.has_satellites) {
    if (!appendJsonLongField(reply, reply_size, offset, needs_comma, "satellites", snapshot.satellites)) {
      return false;
    }
  }
  if (snapshot.has_gps_lat) {
    if (!appendJsonFloatField(reply, reply_size, offset, needs_comma, "gps_lat", snapshot.gps_lat, 6)) {
      return false;
    }
  }
  if (snapshot.has_gps_lon) {
    if (!appendJsonFloatField(reply, reply_size, offset, needs_comma, "gps_lon", snapshot.gps_lon, 6)) {
      return false;
    }
  }
  if (snapshot.has_gps_altitude) {
    if (!appendJsonFloatField(reply, reply_size, offset, needs_comma, "gps_altitude_m", snapshot.gps_altitude_m, 0)) {
      return false;
    }
  }
  if (snapshot.has_supply_voltage) {
    if (!appendJsonFloatField(reply, reply_size, offset, needs_comma, "supply_voltage_v", snapshot.supply_voltage_v, 2)) {
      return false;
    }
  }
  if (snapshot.has_sensor_temp) {
    if (!appendJsonFloatField(reply, reply_size, offset, needs_comma, "sensor_temp_c", snapshot.sensor_temp_c, 1)) {
      return false;
    }
  }
  if (snapshot.has_humidity) {
    if (!appendJsonFloatField(reply, reply_size, offset, needs_comma, "humidity_pct", snapshot.humidity_pct, 0)) {
      return false;
    }
  }
  if (snapshot.has_pressure) {
    if (!appendJsonFloatField(reply, reply_size, offset, needs_comma, "pressure_hpa", snapshot.pressure_hpa, 1)) {
      return false;
    }
  }
  if (snapshot.has_pressure_altitude) {
    if (!appendJsonFloatField(reply, reply_size, offset, needs_comma, "pressure_altitude_m", snapshot.pressure_altitude_m, 0)) {
      return false;
    }
  }
  if (snapshot.has_mcu_temp) {
    if (!appendJsonFloatField(reply, reply_size, offset, needs_comma, "mcu_temp_c", snapshot.mcu_temp_c, 1)) {
      return false;
    }
  }

  const int close_written = snprintf(&reply[offset], reply_size - offset, "}");
  if (close_written < 0 || static_cast<size_t>(close_written) >= (reply_size - offset)) {
    return false;
  }
  offset += static_cast<size_t>(close_written);
  return true;
#else
  (void)reply;
  (void)reply_size;
  (void)offset;
  return false;
#endif
}

void MyMesh::loop() {
#ifdef WITH_BRIDGE
  bridge.loop();
#endif

  const uint32_t now = millis();
  uptime_millis += now - last_millis;
  last_millis = now;

  mesh::Mesh::loop();

  if (next_flood_advert && millisHasNowPassed(next_flood_advert)) {
    mesh::Packet *pkt = createSelfAdvert();
    uint32_t delay_millis = 0;
    if (pkt) sendFloodScoped(default_scope, pkt, delay_millis, _prefs.path_hash_mode + 1);

    updateFloodAdvertTimer(); // schedule next flood advert
    updateAdvertTimer();      // also schedule local advert (so they don't overlap)
  } else if (next_local_advert && millisHasNowPassed(next_local_advert)) {
    mesh::Packet *pkt = createSelfAdvert();
    if (pkt) sendZeroHop(pkt);

    updateAdvertTimer(); // schedule next local advert
  }

  if (set_radio_at && millisHasNowPassed(set_radio_at)) { // apply pending (temporary) radio params
    set_radio_at = 0;                                     // clear timer
    radio_set_params(pending_freq, pending_bw, pending_sf, pending_cr);
    MESH_DEBUG_PRINTLN("Temp radio params");
  }

  if (revert_radio_at && millisHasNowPassed(revert_radio_at)) { // revert radio params to orig
    revert_radio_at = 0;                                        // clear timer
    radio_set_params(_prefs.freq, _prefs.bw, _prefs.sf, _prefs.cr);
    MESH_DEBUG_PRINTLN("Radio params restored");
  }

  // is pending dirty contacts write needed?
  if (dirty_contacts_expiry && millisHasNowPassed(dirty_contacts_expiry)) {
    acl.save(_fs);
    dirty_contacts_expiry = 0;
  }

#if defined(ESP_PLATFORM)
  bool network_required = false;
#if WITH_WEB_PANEL
  network_required = web.isWebEnabled();
#endif
#ifdef WITH_MQTT_UPLINK
  network_required = network_required || mqtt.isActive();
#endif
  network.loop(network_required);
#if WITH_WEB_PANEL
  web.loop();
#endif
#endif
#ifdef WITH_MQTT_UPLINK
  MQTTStatusSnapshot mqtt_status{};
  mqtt_status.battery_mv = static_cast<int>(getBatteryMilliVolts());
  mqtt_status.uptime_secs = static_cast<uint32_t>(uptime_millis / 1000);
  mqtt_status.error_flags = _err_flags;
  mqtt_status.queue_len = static_cast<uint16_t>(_mgr->getOutboundTotal());
  mqtt_status.noise_floor = static_cast<int>(_radio->getNoiseFloor());
  mqtt_status.tx_air_secs = static_cast<uint32_t>(getTotalAirTime() / 1000);
  mqtt_status.rx_air_secs = static_cast<uint32_t>(getReceiveAirTime() / 1000);
  mqtt_status.recv_errors = radio_driver.getPacketsRecvErrors();
  mqtt_status.radio_freq = _prefs.freq;
  mqtt_status.radio_bw = _prefs.bw;
  mqtt_status.radio_sf = _prefs.sf;
  mqtt_status.radio_cr = _prefs.cr;
  mqtt.loop(mqtt_status);
#endif
#if defined(ESP_PLATFORM) && WITH_WEB_PANEL
  updateStatsHistory(now);
#endif
}

// To check if there is pending work
bool MyMesh::hasPendingWork() const {
#if defined(WITH_BRIDGE)
  if (bridge.isRunning()) return true;  // bridge needs WiFi radio, can't sleep
#endif
#if defined(WITH_MQTT_UPLINK)
  if (mqtt.isActive()) return true;
#endif
#if defined(ESP_PLATFORM) && WITH_WEB_PANEL
  if (web.isWebEnabled()) return true;
#endif
  return _mgr->getOutboundTotal() > 0;
}
