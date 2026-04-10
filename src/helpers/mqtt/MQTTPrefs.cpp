#include "MQTTPrefs.h"

#ifdef WITH_MQTT_UPLINK

#include <helpers/TxtDataHelpers.h>
#include <string.h>

namespace {
constexpr uint32_t kFixedStatusIntervalMs = 300000;
}

void MQTTPrefsStore::setDefaults(MQTTPrefs& prefs) {
  memset(&prefs, 0, sizeof(prefs));
  prefs.magic = kMagic;
  prefs.enabled_mask = 0x01;
  prefs.packets_enabled = 1;
  prefs.raw_enabled = 0;
  prefs.status_enabled = 1;
  prefs.tx_enabled = 0;
  prefs.web_enabled = 1;
  prefs.wifi_powersave = 0;
  prefs.status_interval_ms = kFixedStatusIntervalMs;
  StrHelper::strncpy(prefs.iata, MQTT_DEFAULT_IATA, sizeof(prefs.iata));
#ifdef WIFI_SSID
  StrHelper::strncpy(prefs.wifi_ssid, WIFI_SSID, sizeof(prefs.wifi_ssid));
#endif
#ifdef WIFI_PWD
  StrHelper::strncpy(prefs.wifi_pwd, WIFI_PWD, sizeof(prefs.wifi_pwd));
#endif
}

bool MQTTPrefsStore::load(FILESYSTEM* fs, MQTTPrefs& prefs) {
  setDefaults(prefs);
  if (fs == nullptr || !fs->exists(kFilename)) {
    return false;
  }

#if defined(RP2040_PLATFORM)
  File file = fs->open(kFilename, "r");
#else
  File file = fs->open(kFilename);
#endif
  if (!file) {
    return false;
  }

  MQTTPrefs persisted{};
  size_t bytes_to_read = min(static_cast<size_t>(file.size()), sizeof(persisted));
  bool ok = bytes_to_read >= sizeof(persisted.magic) &&
            file.read(reinterpret_cast<uint8_t*>(&persisted), bytes_to_read) == bytes_to_read;
  file.close();

  if (!ok || persisted.magic != kMagic) {
    // Legacy or corrupt MQTT prefs should not survive migration. Remove the
    // stale file and persist a clean current-format default copy immediately.
    fs->remove(kFilename);
    save(fs, prefs);
    return false;
  }
  prefs = persisted;
  if (prefs.wifi_powersave > 2) {
    prefs.wifi_powersave = 0;
  }
  prefs.web_enabled = prefs.web_enabled ? 1 : 0;
  prefs.status_interval_ms = kFixedStatusIntervalMs;
  prefs.enabled_mask &= 0x07;
  return true;
}

bool MQTTPrefsStore::save(FILESYSTEM* fs, const MQTTPrefs& prefs) {
  if (fs == nullptr) {
    return false;
  }
  if (fs->exists(kFilename) && !fs->remove(kFilename)) {
    return false;
  }
#if defined(RP2040_PLATFORM)
  File file = fs->open(kFilename, "w");
#else
  File file = fs->open(kFilename, "w", true);
#endif
  if (!file) {
    return false;
  }
  bool ok = file.write(reinterpret_cast<const uint8_t*>(&prefs), sizeof(prefs)) == sizeof(prefs);
  file.close();
  return ok;
}

#endif
