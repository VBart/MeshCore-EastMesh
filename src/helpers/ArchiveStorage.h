#pragma once

#include <Arduino.h>
#include <helpers/IdentityStore.h>

#if defined(ESP32)
  #include <FS.h>
  #include <SD.h>
  #include <SPI.h>
#endif

class ArchiveStorage {
public:
  ArchiveStorage();

  void begin();
  bool recover();

  bool isSupported() const;
  bool isAttempted() const { return _attempted; }
  bool isMounted() const { return _mounted; }
  bool hadMountFailure() const { return _mount_failed; }

  const char* getLogicalName() const { return "archive"; }
  const char* getLogicalStatsPath() const { return "archive:/stats"; }
  const char* getFsStatsPath() const { return "/stats"; }

  FILESYSTEM* getFS();

  uint64_t getCardSizeBytes() const { return _card_size_bytes; }
  uint64_t getTotalBytes() const { return _total_bytes; }
  uint64_t getUsedBytes() const { return _used_bytes; }
  const char* getCardTypeName() const;
  uint8_t getChipSelectPin() const { return _cs_pin; }

private:
#if defined(ESP32)
  bool resolvePins();
  void prepareBusForArchive() const;
  void refreshCardInfo();
#endif

  bool _attempted;
  bool _mounted;
  bool _mount_failed;
  bool _supported;
  uint8_t _card_type;
  uint8_t _spi_bus;
  uint8_t _cs_pin;
  uint8_t _sck_pin;
  uint8_t _miso_pin;
  uint8_t _mosi_pin;
  uint64_t _card_size_bytes;
  uint64_t _total_bytes;
  uint64_t _used_bytes;

#if defined(ESP32)
  SPIClass* _spi;
  bool _owns_spi;
#endif
};
