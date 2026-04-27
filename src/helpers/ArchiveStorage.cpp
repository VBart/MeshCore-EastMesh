#include "ArchiveStorage.h"

#include <MeshCore.h>

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

#if defined(ESP32)
const char* cardTypeName(uint8_t type) {
  switch (type) {
    case CARD_MMC:
      return "mmc";
    case CARD_SD:
      return "sdsc";
    case CARD_SDHC:
      return "sdhc";
    default:
      return "unknown";
  }
}

uint32_t archiveSpiFrequencyHz() {
#if defined(TBEAM_1W)
  // The 1W board shares SD and LoRa on one SPI bus; keep SD conservative.
  return 4000000U;
#else
  return 10000000U;
#endif
}

void deassertChipSelect(int pin, uint8_t archive_cs_pin) {
  if (pin < 0 || pin == static_cast<int>(archive_cs_pin)) {
    return;
  }
  pinMode(static_cast<uint8_t>(pin), OUTPUT);
  digitalWrite(static_cast<uint8_t>(pin), HIGH);
}
#endif

}  // namespace

#if defined(ESP32)
SPIClass* getBoardSharedArchiveSPI() __attribute__((weak));
SPIClass* getBoardSharedArchiveSPI() {
  return nullptr;
}
#endif

ArchiveStorage::ArchiveStorage()
    : _attempted(false), _mounted(false), _mount_failed(false), _supported(false), _card_type(0), _spi_bus(HSPI),
      _cs_pin(0xFF), _sck_pin(0xFF),
      _miso_pin(0xFF), _mosi_pin(0xFF), _card_size_bytes(0), _total_bytes(0), _used_bytes(0)
#if defined(ESP32)
      , _spi(nullptr), _owns_spi(false)
#endif
{
}

namespace {

bool mountArchiveSd(SPIClass* spi,
                    uint8_t spi_bus,
                    uint8_t cs_pin,
                    uint8_t sck_pin,
                    uint8_t miso_pin,
                    uint8_t mosi_pin,
                    bool initialize_spi) {
  if (spi == nullptr) {
    return false;
  }
  pinMode(cs_pin, OUTPUT);
  digitalWrite(cs_pin, HIGH);
  if (initialize_spi) {
    spi->begin(sck_pin, miso_pin, mosi_pin, cs_pin);
  }
  delayMicroseconds(2);
  return SD.begin(cs_pin, *spi, archiveSpiFrequencyHz());
}

}  // namespace

void ArchiveStorage::begin() {
  if (_attempted) {
    return;
  }
  _attempted = true;

#if defined(ESP32)
  _supported = resolvePins();
  if (!_supported) {
    ARCHIVE_LOG("unsupported: no SD pin mapping");
    return;
  }

  ARCHIVE_LOG("init bus=%u cs=%u sck=%u miso=%u mosi=%u",
              static_cast<unsigned>(_spi_bus),
              static_cast<unsigned>(_cs_pin),
              static_cast<unsigned>(_sck_pin),
              static_cast<unsigned>(_miso_pin),
              static_cast<unsigned>(_mosi_pin));

  _spi = getBoardSharedArchiveSPI();
  _owns_spi = (_spi == nullptr);
  if (_spi == nullptr) {
    _spi = new SPIClass(_spi_bus);
    if (_spi == nullptr) {
      _mount_failed = true;
      ARCHIVE_LOG("mount failed: SPI alloc failed");
      return;
    }
  }

  prepareBusForArchive();
  if (!mountArchiveSd(_spi, _spi_bus, _cs_pin, _sck_pin, _miso_pin, _mosi_pin, true)) {
    _mount_failed = true;
    ARCHIVE_LOG("mount failed bus=%u cs=%u sck=%u miso=%u mosi=%u",
                static_cast<unsigned>(_spi_bus),
                static_cast<unsigned>(_cs_pin),
                static_cast<unsigned>(_sck_pin),
                static_cast<unsigned>(_miso_pin),
                static_cast<unsigned>(_mosi_pin));
    return;
  }

  _mounted = true;
  prepareBusForArchive();
  if (!SD.exists(getFsStatsPath())) {
    SD.mkdir(getFsStatsPath());
  }
  refreshCardInfo();
  ARCHIVE_LOG("mounted type=%s card=%llu total=%llu used=%llu path=%s freq=%lu shared=%s",
              getCardTypeName(),
              static_cast<unsigned long long>(_card_size_bytes),
              static_cast<unsigned long long>(_total_bytes),
              static_cast<unsigned long long>(_used_bytes),
              getLogicalStatsPath(),
              static_cast<unsigned long>(archiveSpiFrequencyHz()),
              _owns_spi ? "no" : "yes");
#else
  _supported = false;
#endif
}

bool ArchiveStorage::recover() {
#if defined(ESP32)
  if (!_supported || _spi == nullptr) {
    return false;
  }

  ARCHIVE_LOG("recover begin bus=%u cs=%u", static_cast<unsigned>(_spi_bus), static_cast<unsigned>(_cs_pin));
  SD.end();
  if (_owns_spi) {
    _spi->end();
  }
  _mounted = false;
  _mount_failed = false;
  _card_size_bytes = 0;
  _total_bytes = 0;
  _used_bytes = 0;

  prepareBusForArchive();
  if (!mountArchiveSd(_spi, _spi_bus, _cs_pin, _sck_pin, _miso_pin, _mosi_pin, _owns_spi)) {
    _mount_failed = true;
    ARCHIVE_LOG("recover failed bus=%u cs=%u", static_cast<unsigned>(_spi_bus), static_cast<unsigned>(_cs_pin));
    return false;
  }

  _mounted = true;
  prepareBusForArchive();
  if (!SD.exists(getFsStatsPath())) {
    SD.mkdir(getFsStatsPath());
  }
  refreshCardInfo();
  ARCHIVE_LOG("recover mounted type=%s card=%llu total=%llu used=%llu path=%s freq=%lu shared=%s",
              getCardTypeName(),
              static_cast<unsigned long long>(_card_size_bytes),
              static_cast<unsigned long long>(_total_bytes),
              static_cast<unsigned long long>(_used_bytes),
              getLogicalStatsPath(),
              static_cast<unsigned long>(archiveSpiFrequencyHz()),
              _owns_spi ? "no" : "yes");
  return true;
#else
  return false;
#endif
}

bool ArchiveStorage::isSupported() const {
  return _supported;
}

FILESYSTEM* ArchiveStorage::getFS() {
#if defined(ESP32)
  if (!_mounted) {
    return nullptr;
  }
  prepareBusForArchive();
  return &SD;
#else
  return nullptr;
#endif
}

const char* ArchiveStorage::getCardTypeName() const {
#if defined(ESP32)
  if (!_mounted) {
    return "unavailable";
  }
  return cardTypeName(_card_type);
#else
  return "unsupported";
#endif
}

#if defined(ESP32)
void ArchiveStorage::prepareBusForArchive() const {
#if defined(P_BOARD_IMU_CS)
  deassertChipSelect(P_BOARD_IMU_CS, _cs_pin);
#endif
#if defined(P_LORA_NSS)
  deassertChipSelect(P_LORA_NSS, _cs_pin);
#endif
#if defined(SX126X_CS)
  deassertChipSelect(SX126X_CS, _cs_pin);
#endif
  pinMode(_cs_pin, OUTPUT);
  digitalWrite(_cs_pin, HIGH);
  delayMicroseconds(2);
}

bool ArchiveStorage::resolvePins() {
#if defined(P_BOARD_SPI_SCK) && defined(P_BOARD_SPI_MISO) && defined(P_BOARD_SPI_MOSI) && defined(P_BOARD_SPI_CS)
  _sck_pin = static_cast<uint8_t>(P_BOARD_SPI_SCK);
  _miso_pin = static_cast<uint8_t>(P_BOARD_SPI_MISO);
  _mosi_pin = static_cast<uint8_t>(P_BOARD_SPI_MOSI);
  _cs_pin = static_cast<uint8_t>(P_BOARD_SPI_CS);
#if defined(TBEAM_SUPREME_SX1262) && defined(FSPI)
  _spi_bus = FSPI;
#endif
  return true;
#elif defined(TBEAM_SUPREME_SX1262)
  // T-Beam S3 Supreme SD/IMU shared SPI bus pins.
  _sck_pin = 36;
  _miso_pin = 37;
  _mosi_pin = 35;
  _cs_pin = 47;
#if defined(FSPI)
  _spi_bus = FSPI;
#endif
  return true;
#elif defined(TBEAM_1W)
  // T-Beam 1W microSD shares the board SPI pins but uses its own chip select.
  _sck_pin = 13;
  _miso_pin = 12;
  _mosi_pin = 11;
  _cs_pin = 10;
#if defined(FSPI)
  _spi_bus = FSPI;
#endif
  return true;
#elif defined(HAS_SDCARD) && defined(SDCARD_CS)
  _cs_pin = static_cast<uint8_t>(SDCARD_CS);
  #if defined(SPI_SCK) && defined(SPI_MISO) && defined(SPI_MOSI)
    _sck_pin = static_cast<uint8_t>(SPI_SCK);
    _miso_pin = static_cast<uint8_t>(SPI_MISO);
    _mosi_pin = static_cast<uint8_t>(SPI_MOSI);
    return true;
  #elif defined(P_LORA_SCLK) && defined(P_LORA_MISO) && defined(P_LORA_MOSI)
    _sck_pin = static_cast<uint8_t>(P_LORA_SCLK);
    _miso_pin = static_cast<uint8_t>(P_LORA_MISO);
    _mosi_pin = static_cast<uint8_t>(P_LORA_MOSI);
    return true;
  #endif
#endif
  return false;
}

void ArchiveStorage::refreshCardInfo() {
  if (!_mounted) {
    _card_type = 0;
    _card_size_bytes = 0;
    _total_bytes = 0;
    _used_bytes = 0;
    return;
  }

  if (_card_type == 0) {
    _card_type = SD.cardType();
  }
  _card_size_bytes = SD.cardSize();
  _total_bytes = SD.totalBytes();
  _used_bytes = SD.usedBytes();
  if (_total_bytes == 0) {
    _total_bytes = _card_size_bytes;
  }
}
#endif
