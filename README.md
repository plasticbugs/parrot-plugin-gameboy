# Game Boy

This plugin plays Game Boy and Game Boy Color games on the ColecoVision.

## Setup

Copy `GB.CVP` into the `/PLUGINS` folder on the card. Put your `.gb` and `.gbc` files in `/gb`, or anywhere else on the card. Pick one in the menu to play it.

USB pads need a powered hub on the cart's USB port, because the cart can't power them. A Switch Pro Controller or a plain HID pad works.

## Controls

On the ColecoVision controller:

- The joystick is the D-pad.
- Left fire is B, and right fire is A.
- Keypad # is Start, and keypad * is Select.
- Keypad 1 to 5 pick the colors. 1 is pea soup, 2 is gray, 3 is sky and 4 is sunset. 5 is full color, for Game Boy Color games.
- Hold keypad 7 for a second to save a state, or 9 to load it.
- Press RESET on the console to go back to the menu, or hold both fire buttons and \* for 2 seconds (Select and Start on a USB pad).

On a USB pad:

- The D-pad or the left stick is the D-pad.
- The right face button is A, and the bottom one is B, as on a Nintendo pad.
- Plus is Start, and minus is Select.
- L and R step back and forward through the colors.
- Hold ZL for a second to save a state, or ZR to load it.

A second pad controls the game too.

## Saves

- Battery saves go next to the game, with the same name, so `Zelda.gb` saves to `Zelda.sav`. Other emulators use the same files. The cart writes them as the game changes them and when you press RESET.
- A save state holds the whole Game Boy. It goes next to the game as a `.state` file, one for each game.
- While you hold 7 or 9, the border turns yellow. Then it turns green when the state is saved, cyan when it's loaded, or red if it didn't work.

## Credits

- Peanut-GB 1.3.0 by Mahyar Koshkouei, under the MIT license. The source is at https://github.com/deltabeard/Peanut-GB
- Game Boy Color support from Peanut-GB's cgb branch, by froggestspirit, adapted by Tomas Vecera.
- minigb_apu by Alex Baines and Mahyar Koshkouei, from Peanut-GB's examples, under the MIT license.
- TinyUSB by Ha Thach, under the MIT license. The source is at https://github.com/hathach/tinyusb
- The plugin itself is by Parrot Cart.

## Build it yourself

You need the Raspberry Pi Pico SDK 2.1.1, an Arm GNU toolchain, CMake,
Ninja, Python 3, sjasmplus and picotool. Releases are built with the Arm
GNU Toolchain 9-2019-q4 (GCC 9.2.1) and picotool 2.3.1. With those you get
the same file, byte for byte.

```sh
cmake -B build -G Ninja -DPICO_SDK_PATH=$HOME/pico-sdk
ninja -C build
```

This makes `build/GB.CVP`. Copy it into the `/PLUGINS` folder on the card.

On Ubuntu, `tools/ci/setup.sh` installs the same tools that GitHub uses to
build each release.

`CART-README.md` is the text the cart shows on the plugin's page. It is
the start of this README.

## License

The plugin is under the MIT license (`LICENSE`). `THIRD-PARTY.md` lists
the parts that came from elsewhere, and their licenses.
