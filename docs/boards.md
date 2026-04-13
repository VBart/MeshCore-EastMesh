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

If a board enables PSRAM but the repo does not declare the exact size, it is shown as `Yes (size not declared)`.

## Start Here

- Pick an `ESP32-S3` board with `16MB` flash and PSRAM if you want the most headroom for MQTT plus UI: `heltec_v4_tft`, `heltec_v4`, `Station_G2`, `T_Beam_S3_Supreme_SX1262`.
- Pick a TFT board if this will be used as a human-facing companion or field node: `heltec_v4_tft`, `heltec_tracker_v2`, `LilyGo_TDeck`, `Heltec_T190`.
- Pick e-paper if you want a status screen with lower idle draw and less frequent refresh: `Heltec_E213`, `Heltec_E290`, `Heltec_Wireless_Paper`, `ThinkNode_M5`.
- Pick a headless board if this is mainly a fixed MQTT gateway and screen space is not useful: `Xiao_C3`, `RAK_3112`, `Tenstar_C3`, `Heltec_ct62`, `Generic_E22`.
- Pick a headless Wi-Fi companion if the phone app will be the primary UI anyway: `Xiao_C3_companion_radio_wifi`, `RAK_3112_companion_radio_wifi`, `Xiao_S3_WIO_companion_radio_wifi`, `Station_G2_companion_radio_wifi`.
- Pick a GPS-capable board if location-aware/mobile use matters: the T-Beam family, `heltec_tracker_v2`, `Heltec_v3`, `heltec_v4`, `Station_G2`, `ThinkNode_M5`.
- If you want the most conservative, older radio family choices, the `SX1276` boards are `LilyGo_TLora_V2_1_1_6`, `Tbeam_SX1276`, and `Heltec_v2`.

## `repeater_mqtt` Boards

This table includes all repeater MQTT targets currently defined in `variants/eastmesh_mqtt/platformio.ini`.

| Target                   | MCU     | RAM    | PSRAM | Flash | LoRa   | Display         | GPS | Notes                                   |
| ------------------------ | ------- | ------ | ----- | ----- | ------ | --------------- | --- | --------------------------------------- |
| Ebyte_EoRa-S3            | ESP32S3 | 512 KB | 2 MB  | 4MB   | SX1262 | OLED (SSD1306)  | No  | more memory headroom                    |
| Generic_E22_sx1262       | ESP32   | 520 KB | No    | 4 MB  | SX1262 | None            | No  | headless                                |
| Generic_E22_sx1268       | ESP32   | 520 KB | No    | 4 MB  | SX1268 | None            | No  | headless                                |
| Heltec_ct62              | ESP32C3 | 400 KB | No    | 4 MB  | SX1262 | None            | No  | headless                                |
| Heltec_E213              | ESP32S3 | 512 KB | 8 MB  | 16MB  | SX1262 | E-paper (2.13") | No  | low-power display, more memory headroom |
| Heltec_E290              | ESP32S3 | 512 KB | 8 MB  | 16MB  | SX1262 | E-paper (2.9")  | No  | low-power display, more memory headroom |
| Heltec_T190              | ESP32S3 | 512 KB | 8 MB  | 16MB  | SX1262 | TFT (ST7789)    | No  | richer UI, more memory headroom         |
| heltec_tracker_v2        | ESP32S3 | 512 KB | No    | 8MB   | SX1262 | TFT (ST7735)    | Yes | richer UI, GPS                          |
| Heltec_v2                | ESP32   | 520 KB | No    | 8 MB  | SX1276 | OLED (SSD1306)  | No  |                                         |
| Heltec_v3                | ESP32S3 | 512 KB | No    | 8 MB  | SX1262 | OLED (SSD1306)  | Yes | GPS                                     |
| heltec_v4                | ESP32S3 | 512 KB | 2 MB  | 16MB  | SX1262 | OLED (SSD1306)  | Yes | GPS, more memory headroom               |
| heltec_v4_tft            | ESP32S3 | 512 KB | 2 MB  | 16MB  | SX1262 | TFT (ST7789)    | Yes | richer UI, GPS, more memory headroom    |
| Heltec_Wireless_Paper    | ESP32S3 | 512 KB | No    | 8 MB  | SX1262 | E-paper (2.13") | No  | low-power display                       |
| Heltec_Wireless_Tracker  | ESP32S3 | 512 KB | No    | 8 MB  | SX1262 | TFT (ST7735)    | Yes | richer UI, GPS                          |
| Heltec_WSL3              | ESP32S3 | 512 KB | No    | 8 MB  | SX1262 | None            | Yes | headless, GPS                           |
| LilyGo_T3S3_sx1262       | ESP32S3 | 512 KB | 2 MB  | 4MB   | SX1262 | OLED (SSD1306)  | No  | more memory headroom                    |
| LilyGo_T3S3_sx1276       | ESP32S3 | 512 KB | 2 MB  | 4MB   | SX1276 | OLED (SSD1306)  | No  | more memory headroom                    |
| LilyGo_TBeam_1W          | ESP32S3 | 512 KB | 8 MB  | 16MB  | SX1262 | OLED (SH1106)   | Yes | GPS, more memory headroom               |
| LilyGo_TDeck             | ESP32S3 | 512 KB | No    | 16MB  | SX1262 | TFT (ST7789)    | Yes | richer UI, GPS                          |
| LilyGo_Tlora_C6          | ESP32C6 | 512 KB | No    | 4 MB  | SX1262 | None            | No  | headless                                |
| LilyGo_TLora_V2_1_1_6    | ESP32   | 520 KB | No    | 4 MB  | SX1276 | OLED (SSD1306)  | Yes | GPS                                     |
| M5Stack_Unit_C6L         | ESP32C6 | 512 KB | No    | 4 MB  | SX1262 | None            | Yes | headless, GPS                           |
| Meshadventurer_sx1262    | ESP32   | 520 KB | No    | 4 MB  | SX1262 | OLED (SSD1306)  | Yes | GPS                                     |
| Meshadventurer_sx1268    | ESP32   | 520 KB | No    | 4 MB  | SX1268 | OLED (SSD1306)  | Yes | GPS                                     |
| Meshimi                  | ESP32C6 | 512 KB | No    | 4 MB  | SX1262 | None            | No  | headless                                |
| nibble_screen_connect    | ESP32S3 | 512 KB | No    | 4MB   | SX1262 | OLED (SSD1306)  | No  |                                         |
| RAK_3112                 | ESP32S3 | 512 KB | No    | 8 MB  | SX1262 | None            | Yes | headless, GPS                           |
| Station_G2               | ESP32S3 | 512 KB | 8 MB  | 16MB  | SX1262 | OLED (SH1106)   | Yes | GPS, more memory headroom               |
| Station_G2_logging       | ESP32S3 | 512 KB | 8 MB  | 16MB  | SX1262 | OLED (SH1106)   | Yes | GPS, more memory headroom               |
| T_Beam_S3_Supreme_SX1262 | ESP32S3 | 512 KB | 8 MB  | 8MB   | SX1262 | OLED (SH1106)   | Yes | GPS, more memory headroom               |
| Tbeam_SX1262             | ESP32   | 520 KB | No    | 4 MB  | SX1262 | OLED (SSD1306)  | Yes | GPS                                     |
| Tbeam_SX1276             | ESP32   | 520 KB | No    | 4 MB  | SX1276 | OLED (SSD1306)  | Yes | GPS                                     |
| Tenstar_C3_sx1262        | ESP32C3 | 400 KB | No    | 4 MB  | SX1262 | None            | No  | headless                                |
| Tenstar_C3_sx1268        | ESP32C3 | 400 KB | No    | 4 MB  | SX1268 | None            | No  | headless                                |
| ThinkNode_M2             | ESP32S3 | 512 KB | No    | 4MB   | SX1262 | OLED (SH1106)   | No  |                                         |
| ThinkNode_M5             | ESP32S3 | 512 KB | No    | 4MB   | SX1262 | E-paper (GxEPD) | Yes | low-power display, GPS                  |
| WHY2025_badge            | ESP32C6 | 512 KB | No    | 4 MB  | SX1262 | None            | No  | headless                                |
| Xiao_C3                  | ESP32C3 | 400 KB | No    | 4 MB  | SX1262 | None            | Yes | headless, GPS, web panel off            |
| Xiao_C6                  | ESP32C6 | 512 KB | No    | 4 MB  | SX1262 | None            | No  | headless                                |
| Xiao_S3_WIO              | ESP32S3 | 512 KB | No    | 8 MB  | SX1262 | None            | Yes | headless, GPS                           |

Note: all current `repeater_mqtt` boards support the local web panel except `Xiao_C3`, where it is disabled to preserve limited board resources.

## `companion_radio_wifi` Boards With A Display

These are the Wi-Fi companion targets that have a display configured for local status, setup help, or occasional direct interaction.

| Target                   | MCU     | RAM    | PSRAM | Flash | LoRa   | Display         | GPS | Notes                                |
| ------------------------ | ------- | ------ | ----- | ----- | ------ | --------------- | --- | ------------------------------------ |
| Heltec_v2                | ESP32   | 520 KB | No    | 8 MB  | SX1276 | OLED (SSD1306)  | No  |                                      |
| Heltec_v3                | ESP32S3 | 512 KB | No    | 8 MB  | SX1262 | OLED (SSD1306)  | Yes | GPS                                  |
| heltec_tracker_v2        | ESP32S3 | 512 KB | No    | 8MB   | SX1262 | TFT (ST7735)    | Yes | richer UI, GPS                       |
| heltec_v4                | ESP32S3 | 512 KB | 2 MB  | 16MB  | SX1262 | OLED (SSD1306)  | Yes | GPS, more memory headroom            |
| heltec_v4_tft            | ESP32S3 | 512 KB | 2 MB  | 16MB  | SX1262 | TFT (ST7789)    | Yes | richer UI, GPS, more memory headroom |
| LilyGo_TBeam_1W          | ESP32S3 | 512 KB | 8 MB  | 16MB  | SX1262 | OLED (SH1106)   | Yes | GPS, more memory headroom            |
| LilyGo_TLora_V2_1_1_6    | ESP32   | 520 KB | No    | 4 MB  | SX1276 | OLED (SSD1306)  | Yes | GPS                                  |
| nibble_screen_connect    | ESP32S3 | 512 KB | No    | 4MB   | SX1262 | OLED (SSD1306)  | No  |                                      |
| Station_G2               | ESP32S3 | 512 KB | 8 MB  | 16MB  | SX1262 | OLED (SH1106)   | Yes | GPS, more memory headroom            |
| T_Beam_S3_Supreme_SX1262 | ESP32S3 | 512 KB | 8 MB  | 8MB   | SX1262 | OLED (SH1106)   | Yes | GPS, more memory headroom            |
| ThinkNode_M2             | ESP32S3 | 512 KB | No    | 4MB   | SX1262 | OLED (SH1106)   | No  |                                      |
| ThinkNode_M5             | ESP32S3 | 512 KB | No    | 4MB   | SX1262 | E-paper (GxEPD) | Yes | low-power display, GPS               |

## `companion_radio_wifi` Headless Boards

These are the Wi-Fi companion targets that rely on the companion app as the primary interface. For many users, this is the best everyday option.

| Target      | MCU     | RAM    | PSRAM | Flash | LoRa   | GPS | Notes                                   |
| ----------- | ------- | ------ | ----- | ----- | ------ | --- | --------------------------------------- |
| Heltec_WSL3 | ESP32S3 | 512 KB | No    | 8 MB  | SX1262 | Yes | headless, GPS                           |
| RAK_3112    | ESP32S3 | 512 KB | No    | 8 MB  | SX1262 | Yes | headless, GPS                           |
| Xiao_C3     | ESP32C3 | 400 KB | No    | 4 MB  | SX1262 | Yes | smallest headless option, web panel off |
| Xiao_S3_WIO | ESP32S3 | 512 KB | No    | 8 MB  | SX1262 | Yes | headless, GPS                           |

## Practical Picks

- Best all-round MQTT repeater with screen and memory headroom: `heltec_v4_repeater_mqtt`, `heltec_v4_tft_repeater_mqtt`, `T_Beam_S3_Supreme_SX1262_repeater_mqtt`, `Station_G2_repeater_mqtt`.
- Best MQTT repeater if you want a simple headless install: `RAK_3112_repeater_mqtt`, `Tenstar_C3_sx1262_repeater_mqtt`, `Heltec_ct62_repeater_mqtt`, `Generic_E22_sx1262_repeater_mqtt`.
- Best low-power display MQTT builds: `Heltec_E213_repeater_mqtt`, `Heltec_E290_repeater_mqtt`, `Heltec_Wireless_Paper_repeater_mqtt`, `ThinkNode_M5_Repeater_mqtt`.
- Best companion choices if you want the richest local UI: `heltec_v4_tft_companion_radio_wifi` and `heltec_tracker_v2_companion_radio_wifi`.
- Best companion choices if you want maximum memory headroom: `T_Beam_S3_Supreme_SX1262_companion_radio_wifi`, `heltec_v4_companion_radio_wifi`, `heltec_v4_tft_companion_radio_wifi`, `Station_G2_companion_radio_wifi`.
- Best companion choices if you are happy with an app-first, mostly headless setup: `Xiao_C3_companion_radio_wifi`, `RAK_3112_companion_radio_wifi`, `Xiao_S3_WIO_companion_radio_wifi`, `Station_G2_companion_radio_wifi`.
- Best companion choices if you prefer OLED over TFT: `T_Beam_S3_Supreme_SX1262_companion_radio_wifi`, `heltec_v4_companion_radio_wifi`, `LilyGo_TBeam_1W_companion_radio_wifi`.
