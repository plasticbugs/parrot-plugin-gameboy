# What in this repository came from elsewhere

The plugin's own code, in `src/`, is under the MIT license (`LICENSE`).
The copy of the cart's SDK in `sdk/` is under the MIT license
(`sdk/LICENSE`). These parts came from elsewhere and keep their own terms.

| Where | What | License |
|---|---|---|
| `vendor/peanut_gb.h` | [Peanut-GB](https://github.com/deltabeard/Peanut-GB) 1.3.0 by Mahyar Koshkouei, with Game Boy Color support from its cgb branch (froggestspirit, adapted by Tomáš Večeřa), and the changes listed in `vendor/README.md` | MIT (`vendor/LICENSE-peanut_gb`) |
| `vendor/minigb_apu/` | minigb_apu by Alex Baines and Mahyar Koshkouei, from Peanut-GB's examples | MIT (`vendor/minigb_apu/LICENSE`) |
| `sdk/firmware/revc/sdk/tinyusb/` | [TinyUSB](https://github.com/hathach/tinyusb) (Ha Thach and contributors), with the cart's fixes, listed in its `VENDORED.md` | MIT (`sdk/firmware/revc/sdk/tinyusb/LICENSE`) |
| `cmake/pico_sdk_import.cmake` | from the [Raspberry Pi Pico SDK](https://github.com/raspberrypi/pico-sdk) | BSD 3-Clause |

The built plugin also contains the Raspberry Pi Pico SDK's runtime (BSD
3-Clause) and TinyUSB (MIT).

The 8x8 screen font's letter shapes come from the 1982 arcade game Mr. Do!
