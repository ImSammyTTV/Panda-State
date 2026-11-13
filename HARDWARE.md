# Panda Status P2: what the hardware is

BIQU / BigTreeTech publish no source code or hardware documentation for the
Panda Status P2. Everything here was read out of a P2's own stock firmware
(V1.0.0, read off the device over USB) and then confirmed on a working bar.
Each line says how it is known. "Not found yet" means exactly that.

## Measured

| What | Value | How it is known |
|---|---|---|
| Chip | Espressif ESP32-C3 (RISC-V, single core, 160 MHz, Wi-Fi 2.4 GHz), rev 1.1 | esptool; image header chip id 5 |
| Module | ESP32-C3-MINI | BIQU's product page |
| Board and power | 96 × 17 mm control board; USB-C, 5 V (rated 5 V 3 A); 3.3 V logic | BIQU's product page |
| Flash | 4 MB | esptool |
| USB | CH340 USB-serial bridge on the USB-C port (not the chip's native USB) | enumerates as a WCH serial port; 115200 baud reliable, 460800 gave data errors |
| Stock firmware | project `panda_status_p2`, version 1 (V1.0.0), built 15 Apr 2026, ESP-IDF v5.3.1, ESP-ADF | app description block in the image |
| Signing | none: a plain ESP-IDF image with the standard appended hash, no secure boot | image header (`hash_appended` 1, no signature block) |
| Partitions | `nvs` 0x9000, `otadata` 0xC000, `app0` 0x10000, `app1` 0x200000 (each 0x1F0000 = 1.94 MB), `coredump` 0x3F0000 | partition table; `partitions.csv` here is the same table |
| Slots | stock runs from `app0`; `app1` is empty on a bar that has never been updated | first bytes of each slot |
| LEDs | 25 addressable LEDs, WS2812-style, colour order GRB, LED 0 at the end progress fills from | stock's LED code sends 75 bytes a frame and loops 0..24; order and direction checked by eye |
| LED pin | GPIO 5, driven by the RMT peripheral at 10 MHz | `rmt_tx_channel_config_t.gpio_num = 5` in stock's LED set-up |
| Microphone | ES8311 audio codec (this is what Music mode listens with) | source paths in the stock image; the chip answers on I2C |
| Codec I2C | SDA GPIO 2, SCL GPIO 3, address 0x18 | stock's `get_i2c_pins`; confirmed on the bar |
| Codec I2S | MCLK GPIO 0, BCK GPIO 1, WS GPIO 4, DOUT GPIO 7, DIN GPIO 10 | stock's `get_i2s_pins`; confirmed on the bar: the level follows the room (about -42 to -26 dB beside a running printer) |
| Button | GPIO 9, to ground when pressed (pull-up on), 5 s hold threshold in stock | stock's button set-up |
| Speaker | none audible: an 880 Hz tone at every DAC volume made no sound. No amplifier-enable pin in stock's board file either | test tone in 0.5.0 (removed again in 0.5.2) |
| Stock Wi-Fi record | NVS namespace `app_nvs`, key `wifi_info`, 228 bytes: network name at 0 (33), password at 33 (64), then AP name, AP password and host name | field positions only; this firmware reads it once so an updated bar stays on its network |
| Stock pairing record | NVS `app_nvs`, key `bambu_mqtt_info`, 57 bytes: printer address at 0 (16), access code at 16 (9), serial at 25 (16) | field positions; this firmware reads it so an updated bar stays paired |
| Printer feed | MQTT over TLS, port 8883, user `bblp`, password = the printer's access code, topic `device/<serial>/report` | an X2D sends one message a second of about 31 KB |
| Stock updater | `POST /ota` with the raw `.bin` as the body and an `OTA-Type: ota_fw` header; accepts an image that is not BTT's | used to install this firmware |
| Web page | built into the firmware image; there is no separate file system | no data partition in the table |

## Not found yet

- **What stock's second kind of button press and its 5 s hold do.** This firmware's choices for those are its own.
- **Whether a failed first boot rolls back to stock by itself.** The boot data marks the running image valid, which suggests the bootloader's rollback is enabled. This firmware adds its own rollback anyway (see the README).

## Why a custom firmware is possible at all

- The chip is a stock ESP32-C3 with nothing locked: no secure boot, no flash encryption, no image signing.
- The flash has two firmware slots, and stock's own updater writes a new image into whichever one is not running. So a new firmware can go on without a cable, and stock stays whole in the other slot to go back to.
- Every pin the bar uses could be read out of stock's own set-up code, so this firmware drives the same LEDs, microphone and button.
