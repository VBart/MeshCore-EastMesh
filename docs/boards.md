# Board Comparison

This page is meant to help you choose a board, not just list every technical detail.

If you only want a quick answer:

- for a roof-mounted or set-and-forget MQTT repeater, start with `heltec_v4`, `heltec_v4_tft`, `Station_G2`, or `T_Beam_S3_Supreme_SX1262`
- for an app-first Wi-Fi companion, headless boards are fine and often simpler
- if you want an onboard screen people will actually use, prefer TFT boards
- if you want a low-power status screen, prefer e-paper boards

The tables below are built from the repo's PlatformIO board metadata and variant build flags.

- `Target` is the short board name used in this comparison page. Release filenames and local build commands still use the full env names such as `heltec_v4_repeater_mqtt`.
- `MCU` shows the chip family only. Actual runtime clock can vary by env and board configuration.
- `RAM` is the MCU's built-in RAM.
- `PSRAM` is extra memory on some boards. More PSRAM usually means more headroom for UI, MQTT, and future features.
- `GPS` uses `✅` when present and is blank when absent.

## Start Here

- Pick an `ESP32-S3` board with `16MB` flash and PSRAM if you want strong overall headroom for MQTT plus UI: `heltec_v4_tft`, `heltec_v4`, `Station_G2`, `LilyGo_TBeam_1W`.
- Pick a TFT board if this will be used as a human-facing companion or field node: `heltec_v4_tft`, `heltec_tracker_v2`, `LilyGo_TDeck`, `Heltec_T190`.
- Pick e-paper if you want a status screen with lower idle draw and less frequent refresh: `Heltec_E213`, `Heltec_E290`, `Heltec_Wireless_Paper`, `ThinkNode_M5`.
- Pick a headless board if this is mainly a fixed MQTT gateway and screen space is not useful: `Xiao_C3`, `RAK_3112`, `Tenstar_C3`, `Heltec_ct62`, `Generic_E22`.
- Pick a headless Wi-Fi companion if the phone app will be the primary UI anyway: `Xiao_C3_companion_radio_wifi`, `RAK_3112_companion_radio_wifi`, `Xiao_S3_WIO_companion_radio_wifi`, `Station_G2_companion_radio_wifi`.
- Pick a GPS-capable board if location-aware/mobile use matters: the T-Beam family, `heltec_tracker_v2`, `Heltec_v3`, `heltec_v4`, `Station_G2`, `ThinkNode_M5`.
- If you want the most conservative, older radio family choices, the `SX1276` boards are `LilyGo_TLora_V2_1_1_6`, `Tbeam_SX1276`, and `Heltec_v2`.

## Additional Signals

If you are deciding between otherwise similar boards, these target-level settings often matter more than flash alone.

- `CPU cfg` is the runtime clock MeshCore sets for that target. `default` means MeshCore does not override the board's default clock.
- `TX cfg` is MeshCore's configured `LORA_TX_POWER`, not guaranteed antenna output. Boards with a PA or RF front-end may radiate much more than the configured value suggests.
- `Power` is how the target exposes power in the repo: `ADC batt`, `PMU`, `custom 2S`, `fixed/ext`, or `none`.
- `Extras` lists notable RF or peripheral extras exposed by the target, such as `PA/FEM`, `RF switch`, `BME280`, `NeoPixel`, or `ext ant`.

| Target                   | CPU cfg | TX cfg       | Power     | Extras    |
| ------------------------ | ------- | ------------ | --------- | --------- |
| Ebyte_EoRa-S3            | default | 22 dBm cfg   | ADC batt  |           |
| Generic_E22_sx1262       | default | 22 dBm cfg   | ADC batt  | RF switch |
| Generic_E22_sx1268       | default | 22 dBm cfg   | ADC batt  | RF switch |
| Heltec_ct62              | 80 MHz  | 22 dBm cfg   | ADC batt  |           |
| Heltec_E213              | default | 22 dBm cfg   | ADC batt  |           |
| Heltec_E290              | default | 22 dBm cfg   | ADC batt  |           |
| Heltec_T190              | default | 22 dBm cfg   | ADC batt  |           |
| heltec_tracker_v2        | 160 MHz | 9 dBm cfg\*  | ADC batt  | PA/FEM    |
| Heltec_v2                | default | 20 dBm cfg   | ADC batt  |           |
| Heltec_v3                | 80 MHz  | 22 dBm cfg   | ADC batt  |           |
| heltec_v4                | 80 MHz  | 10 dBm cfg\* | ADC batt  | PA/FEM    |
| heltec_v4_tft            | 80 MHz  | 10 dBm cfg\* | ADC batt  | PA/FEM    |
| Heltec_Wireless_Paper    | default | 22 dBm cfg   | ADC batt  |           |
| Heltec_Wireless_Tracker  | 80 MHz  | 22 dBm cfg   | none      |           |
| Heltec_WSL3              | 80 MHz  | 22 dBm cfg   | ADC batt  |           |
| LilyGo_T3S3_sx1262       | default | 22 dBm cfg   | ADC batt  |           |
| LilyGo_T3S3_sx1276       | default | 20 dBm cfg   | ADC batt  | RF switch |
| LilyGo_TBeam_1W          | default | 22 dBm cfg\* | custom 2S | PA/FEM    |
| LilyGo_TDeck             | default | 22 dBm cfg   | ADC batt  |           |
| LilyGo_Tlora_C6          | default | 22 dBm cfg   | none      | RF switch |
| LilyGo_TLora_V2_1_1_6    | default | 20 dBm cfg   | ADC batt  |           |
| M5Stack_Unit_C6L         | default | 22 dBm cfg   | none      | RF switch |
| Meshadventurer_sx1262    | default | 22 dBm cfg   | ADC batt  | RF switch |
| Meshadventurer_sx1268    | default | 22 dBm cfg   | ADC batt  | RF switch |
| Meshimi                  | default | 22 dBm cfg   | none      | ext ant   |
| nibble_screen_connect    | default | 22 dBm cfg   | none      | NeoPixel  |
| RAK_3112                 | 80 MHz  | 22 dBm cfg   | ADC batt  |           |
| Station_G2               | default | 7 dBm cfg\*  | fixed/ext | PA/FEM    |
| Station_G2_logging       | default | 7 dBm cfg\*  | fixed/ext | PA/FEM    |
| T_Beam_S3_Supreme_SX1262 | default | 22 dBm cfg   | PMU       | BME280    |
| Tbeam_SX1262             | default | 22 dBm cfg   | PMU       |           |
| Tbeam_SX1276             | default | 20 dBm cfg   | PMU       |           |
| Tenstar_C3_sx1262        | 80 MHz  | 22 dBm cfg   | ADC batt  |           |
| Tenstar_C3_sx1268        | 80 MHz  | 22 dBm cfg   | ADC batt  |           |
| ThinkNode_M2             | default | 22 dBm cfg   | ADC batt  |           |
| ThinkNode_M5             | default | 22 dBm cfg   | ADC batt  |           |
| WHY2025_badge            | default | 22 dBm cfg   | none      |           |
| Xiao_C3                  | 80 MHz  | 22 dBm cfg   | ADC batt  |           |
| Xiao_C6                  | default | 22 dBm cfg   | none      | RF switch |
| Xiao_S3_WIO              | default | 22 dBm cfg   | none      | RF switch |

\* These targets use a PA or RF front-end, so effective output can be much higher than the configured `LORA_TX_POWER`.

## `repeater_mqtt` Boards

This table includes all repeater MQTT targets currently defined in `variants/eastmesh_mqtt/platformio.ini`.

| Target                   | MCU     | RAM    | PSRAM | Flash | LoRa   | Display         | GPS |
| ------------------------ | ------- | ------ | ----- | ----- | ------ | --------------- | --- |
| Ebyte_EoRa-S3            | ESP32S3 | 512 KB | 2 MB  | 4 MB  | SX1262 | OLED (SSD1306)  |     |
| Generic_E22_sx1262       | ESP32   | 520 KB | No    | 4 MB  | SX1262 | None            |     |
| Generic_E22_sx1268       | ESP32   | 520 KB | No    | 4 MB  | SX1268 | None            |     |
| Heltec_ct62              | ESP32C3 | 400 KB | No    | 4 MB  | SX1262 | None            |     |
| Heltec_E213              | ESP32S3 | 512 KB | 8 MB  | 16 MB | SX1262 | E-paper (2.13") |     |
| Heltec_E290              | ESP32S3 | 512 KB | 8 MB  | 16 MB | SX1262 | E-paper (2.9")  |     |
| Heltec_T190              | ESP32S3 | 512 KB | 8 MB  | 16 MB | SX1262 | TFT (ST7789)    |     |
| heltec_tracker_v2        | ESP32S3 | 512 KB | No    | 8 MB  | SX1262 | TFT (ST7735)    | ✅  |
| Heltec_v2                | ESP32   | 520 KB | No    | 8 MB  | SX1276 | OLED (SSD1306)  |     |
| Heltec_v3                | ESP32S3 | 512 KB | No    | 8 MB  | SX1262 | OLED (SSD1306)  | ✅  |
| heltec_v4                | ESP32S3 | 512 KB | 2 MB  | 16 MB | SX1262 | OLED (SSD1306)  | ✅  |
| heltec_v4_tft            | ESP32S3 | 512 KB | 2 MB  | 16 MB | SX1262 | TFT (ST7789)    | ✅  |
| Heltec_Wireless_Paper    | ESP32S3 | 512 KB | No    | 8 MB  | SX1262 | E-paper (2.13") |     |
| Heltec_Wireless_Tracker  | ESP32S3 | 512 KB | No    | 8 MB  | SX1262 | TFT (ST7735)    | ✅  |
| Heltec_WSL3              | ESP32S3 | 512 KB | No    | 8 MB  | SX1262 | None            | ✅  |
| LilyGo_T3S3_sx1262       | ESP32S3 | 512 KB | 2 MB  | 4 MB  | SX1262 | OLED (SSD1306)  |     |
| LilyGo_T3S3_sx1276       | ESP32S3 | 512 KB | 2 MB  | 4 MB  | SX1276 | OLED (SSD1306)  |     |
| LilyGo_TBeam_1W          | ESP32S3 | 512 KB | 8 MB  | 16 MB | SX1262 | OLED (SH1106)   | ✅  |
| LilyGo_TDeck             | ESP32S3 | 512 KB | No    | 16 MB | SX1262 | TFT (ST7789)    | ✅  |
| LilyGo_Tlora_C6          | ESP32C6 | 512 KB | No    | 4 MB  | SX1262 | None            |     |
| LilyGo_TLora_V2_1_1_6    | ESP32   | 520 KB | No    | 4 MB  | SX1276 | OLED (SSD1306)  | ✅  |
| M5Stack_Unit_C6L         | ESP32C6 | 512 KB | No    | 4 MB  | SX1262 | None            | ✅  |
| Meshadventurer_sx1262    | ESP32   | 520 KB | No    | 4 MB  | SX1262 | OLED (SSD1306)  | ✅  |
| Meshadventurer_sx1268    | ESP32   | 520 KB | No    | 4 MB  | SX1268 | OLED (SSD1306)  | ✅  |
| Meshimi                  | ESP32C6 | 512 KB | No    | 4 MB  | SX1262 | None            |     |
| nibble_screen_connect    | ESP32S3 | 512 KB | No    | 4 MB  | SX1262 | OLED (SSD1306)  |     |
| RAK_3112                 | ESP32S3 | 512 KB | No    | 8 MB  | SX1262 | None            | ✅  |
| Station_G2               | ESP32S3 | 512 KB | 8 MB  | 16 MB | SX1262 | OLED (SH1106)   | ✅  |
| Station_G2_logging       | ESP32S3 | 512 KB | 8 MB  | 16 MB | SX1262 | OLED (SH1106)   | ✅  |
| T_Beam_S3_Supreme_SX1262 | ESP32S3 | 512 KB | 8 MB  | 8 MB  | SX1262 | OLED (SH1106)   | ✅  |
| Tbeam_SX1262             | ESP32   | 520 KB | No    | 4 MB  | SX1262 | OLED (SSD1306)  | ✅  |
| Tbeam_SX1276             | ESP32   | 520 KB | No    | 4 MB  | SX1276 | OLED (SSD1306)  | ✅  |
| Tenstar_C3_sx1262        | ESP32C3 | 400 KB | No    | 4 MB  | SX1262 | None            |     |
| Tenstar_C3_sx1268        | ESP32C3 | 400 KB | No    | 4 MB  | SX1268 | None            |     |
| ThinkNode_M2             | ESP32S3 | 512 KB | No    | 4 MB  | SX1262 | OLED (SH1106)   |     |
| ThinkNode_M5             | ESP32S3 | 512 KB | No    | 4 MB  | SX1262 | E-paper (GxEPD) | ✅  |
| WHY2025_badge            | ESP32C6 | 512 KB | No    | 4 MB  | SX1262 | None            |     |
| Xiao_C3                  | ESP32C3 | 400 KB | No    | 4 MB  | SX1262 | None            | ✅  |
| Xiao_C6                  | ESP32C6 | 512 KB | No    | 4 MB  | SX1262 | None            |     |
| Xiao_S3_WIO              | ESP32S3 | 512 KB | No    | 8 MB  | SX1262 | None            | ✅  |

Note: all current `repeater_mqtt` boards support the local web panel except `Xiao_C3`, where it is disabled to preserve limited board resources.

## `companion_radio_wifi` Boards With A Display

These are the Wi-Fi companion targets that have a display configured for local status, setup help, or occasional direct interaction.

| Target                   | MCU     | RAM    | PSRAM | Flash | LoRa   | Display         | GPS |
| ------------------------ | ------- | ------ | ----- | ----- | ------ | --------------- | --- |
| Heltec_v2                | ESP32   | 520 KB | No    | 8 MB  | SX1276 | OLED (SSD1306)  |     |
| Heltec_v3                | ESP32S3 | 512 KB | No    | 8 MB  | SX1262 | OLED (SSD1306)  | ✅  |
| heltec_tracker_v2        | ESP32S3 | 512 KB | No    | 8MB   | SX1262 | TFT (ST7735)    | ✅  |
| heltec_v4                | ESP32S3 | 512 KB | 2 MB  | 16MB  | SX1262 | OLED (SSD1306)  | ✅  |
| heltec_v4_tft            | ESP32S3 | 512 KB | 2 MB  | 16MB  | SX1262 | TFT (ST7789)    | ✅  |
| LilyGo_TBeam_1W          | ESP32S3 | 512 KB | 8 MB  | 16MB  | SX1262 | OLED (SH1106)   | ✅  |
| LilyGo_TLora_V2_1_1_6    | ESP32   | 520 KB | No    | 4 MB  | SX1276 | OLED (SSD1306)  | ✅  |
| nibble_screen_connect    | ESP32S3 | 512 KB | No    | 4MB   | SX1262 | OLED (SSD1306)  |     |
| Station_G2               | ESP32S3 | 512 KB | 8 MB  | 16MB  | SX1262 | OLED (SH1106)   | ✅  |
| T_Beam_S3_Supreme_SX1262 | ESP32S3 | 512 KB | 8 MB  | 8MB   | SX1262 | OLED (SH1106)   | ✅  |
| ThinkNode_M2             | ESP32S3 | 512 KB | No    | 4MB   | SX1262 | OLED (SH1106)   |     |
| ThinkNode_M5             | ESP32S3 | 512 KB | No    | 4MB   | SX1262 | E-paper (GxEPD) | ✅  |

## `companion_radio_wifi` Headless Boards

These are the Wi-Fi companion targets that rely on the companion app as the primary interface. For many users, this is the best everyday option.

| Target      | MCU     | RAM    | PSRAM | Flash | LoRa   | GPS |
| ----------- | ------- | ------ | ----- | ----- | ------ | --- |
| Heltec_WSL3 | ESP32S3 | 512 KB | No    | 8 MB  | SX1262 | ✅  |
| RAK_3112    | ESP32S3 | 512 KB | No    | 8 MB  | SX1262 | ✅  |
| Xiao_C3     | ESP32C3 | 400 KB | No    | 4 MB  | SX1262 | ✅  |
| Xiao_S3_WIO | ESP32S3 | 512 KB | No    | 8 MB  | SX1262 | ✅  |

## Practical Picks

- Best all-round MQTT repeater with screen and overall headroom: `heltec_v4_repeater_mqtt`, `heltec_v4_tft_repeater_mqtt`, `Station_G2_repeater_mqtt`, `LilyGo_TBeam_1W_repeater_mqtt`.
- Best MQTT repeater if you want a simple headless install: `RAK_3112_repeater_mqtt`, `Tenstar_C3_sx1262_repeater_mqtt`, `Heltec_ct62_repeater_mqtt`, `Generic_E22_sx1262_repeater_mqtt`.
- Best low-power display MQTT builds: `Heltec_E213_repeater_mqtt`, `Heltec_E290_repeater_mqtt`, `Heltec_Wireless_Paper_repeater_mqtt`, `ThinkNode_M5_Repeater_mqtt`.
- Best companion choices if you want the richest local UI: `heltec_v4_tft_companion_radio_wifi` and `heltec_tracker_v2_companion_radio_wifi`.
- Best companion choices if you want maximum memory headroom: `T_Beam_S3_Supreme_SX1262_companion_radio_wifi`, `Station_G2_companion_radio_wifi`, `LilyGo_TBeam_1W_companion_radio_wifi`.
- Best companion choices if you are happy with an app-first, mostly headless setup: `Xiao_C3_companion_radio_wifi`, `RAK_3112_companion_radio_wifi`, `Xiao_S3_WIO_companion_radio_wifi`, `Station_G2_companion_radio_wifi`.
- Best companion choices if you prefer OLED over TFT: `T_Beam_S3_Supreme_SX1262_companion_radio_wifi`, `heltec_v4_companion_radio_wifi`, `LilyGo_TBeam_1W_companion_radio_wifi`.
