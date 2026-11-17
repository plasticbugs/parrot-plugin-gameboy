# Vendored code

Both are MIT-licensed (`LICENSE-peanut_gb`, `minigb_apu/LICENSE`).

## peanut_gb.h: Peanut-GB 1.3.0 with Game Boy Color support merged in

Upstream: <https://github.com/deltabeard/Peanut-GB>.

- **Base:** master at `d0bcca771c83a2638c93a9ae61f3d51f226dc905` (2026-09-21),
  Peanut-GB 1.3.0. Before 2026-11-12 this file was that commit unchanged.
- **Merged:** the `cgb` branch at `33f053b` (2023-09-01): full Game Boy Color
  support by froggestspirit, adapted by Tomáš Večeřa (passes cgb-acid2), behind
  `PEANUT_FULL_GBC_SUPPORT`. The branch split from master at `03c1fcf`; master
  has 142 commits since, the branch 13.

With `PEANUT_FULL_GBC_SUPPORT` 0 (the default) the merged file emulates DMG games
exactly as 1.3.0 did: the gbbench host build gives the same frame hashes for
Super Mario Land, Castlevania Adventure and Mega Man II over 3,600 frames. With
it 1 those hashes are unchanged too (a DMG cartridge runs in DMG mode), and Color
cartridges run in colour: `gb->cgb.fixPalette[64]` holds the palettes as RGB555
(red in bits 10-14), and each pixel `draw_line` gets is an index into it
(background 0-31, sprites 32-63).

How the four conflicts were settled, and what else changed in the merge:

1. **Sprite order.** The branch sorted the line's sprites with `qsort`; master
   now builds the sorted list while it scans OAM. Kept master's, and in CGB
   mode skip the sort: a Color game's sprite priority is OAM order alone.
2. **STOP.** Kept master's (the halt stays commented out) and added the
   branch's speed switch: in CGB mode, STOP after `KEY1` bit 0 toggles double
   speed.
3. **H-blank DMA.** The branch did it at the start of a line; master's LCD
   modes are laid out differently, so it now runs where master enters mode 0
   (H-blank), 16 bytes each H-blank, as on the hardware.
4. **Reset.** Kept master's `IO_BOOT` and sound register start values, then
   the branch's CGB register values (A = $11, ...) in master's field names.

Beyond the conflicts:

- **Double speed.** The branch slowed only the LCD in double speed. The
  cartridge's real-time clock (MBC3, e.g. Pokémon Crystal) and the LCD-off frame
  timer now run at single speed too, as on the hardware; DIV, the timer and
  the serial port stay at the CPU's speed.
- **DMG games in a Color build.** Three of the branch's changes reached DMG
  mode too, and now don't: reads of the Color-only registers ($FF4D, $FF4F,
  $FF51-55, $FF68-6B, $FF70) come from HRAM as without Color support, the
  OAM DMA source is no longer folded with `% 0xF1`, and the fast serial
  clock is SC bit 1 in Color mode only (the branch tested bits 0 and 1, so
  any internally clocked transfer ran 32 times too fast).
- **12-colour mode.** A branch line that clears `LCD_PALETTE_BG` on DMG
  sprites is now compiled only with `PEANUT_GB_12_COLOUR` too: that's the only
  build that defines it, and the cart builds without it.

On the Mac, Link's Awakening DX, Oracle of Seasons, Pokémon Crystal, Shantae,
Super Mario Bros. Deluxe, Tetris DX and Wario Land 3 boot and play for a minute
in colour (with minigb_apu: Oracle of Seasons waits on the sound chip's status
register before it starts).

## A fix to upstream 1.3.0

With ten sprites already on a line, putting an eleventh in its sorted place
shifted the list from index 10, one past the end of `sprites_to_render[10]`,
onto the stack. In 1.3.0 that lands on the line's first two pixels: Mega Man
II, with more than ten sprites on a line, drew garbage at x = 0 and 1. The
shift now starts at index 9 when the list is full, and the last sprite drops
off, as on the hardware.

After that, the gbplay host build gives the same lists, frame for frame, with
Color support compiled out and in, for all three DMG games here over 3,600 TV
frames; Super Mario Land's and Castlevania's are also unchanged from before
the merge, and Mega Man II's differ from the frame the overflow first showed.

## Lock-ups fixed for the cart (2026-11-12)

Running 538 Game Boy Color games for a minute each on the Mac, with
the address and undefined-behaviour sanitizers, found these:

- **HALT that never ends.** Peanut-GB's HALT loops inside one call until an
  enabled interrupt is requested. Nothing here requests the joypad
  interrupt, a link cable never answers, and with the LCD off there is no
  VBlank either: such a HALT looped forever and `gb_run_frame` never
  returned. Now the loop also ends with the frame; if the CPU is still
  halted (and IE isn't 0, where Peanut-GB already wakes at VBlank), it steps
  back onto the HALT so the next call carries on waiting. gbplay requests
  the joypad interrupt when a button goes down, as the hardware does.
- **HALT with nothing to wait for** (LCD off, no timer or serial) jumped
  `INT_FAST16_MAX` cycles: 32,767 on the Mac but 2^31 - 1 on the cart, where
  `int_fast16_t` is 32 bits. A halt now advances a line (456 cycles) at most.
- **An invalid opcode** called the error callback and then
  `PGB_UNREACHABLE()`: undefined behaviour, since ours returns. Now the CPU
  stays on the opcode and time goes on (the hardware locks up the same way).
  An invalid read returns $FF instead.

The SML hashes in `../../gbbench/README.md` are unchanged by these.

## Wait loops skipped (2026-11-12)

Many games wait for the next frame by reading one byte until an interrupt
handler changes it, rather than with HALT. Link's Awakening DX on its intro
and title screen (`ldh a,[$FFD1]; and a; jr z`) spent 76–84% of its
instructions there, twice as many in double speed, which made its title
screen slow on the cart once it ran as a Color game. Kirby's Dream Land and
Castlevania wait the same way in play.

`__gb_idle_skip` is called when a JR jumps back 2 to 16 bytes. It tries one
turn of the loop without running it: loads into A of bytes only the CPU
writes (WRAM, HRAM) or that change only at an event (IF, STAT, LY), AND, OR,
CP, BIT, JRs not taken, and a JR back to the top. If that turn leaves A and
F as they are, every turn will until an interrupt or the next event (an LCD
mode change, a timer overflow, the end of a serial transfer), so the JR also
takes the cycles of all the whole turns that fit before it. The loop then
runs its last turn as usual and meets the event on the same instruction as
before. Nothing that the game can see changes.

Checked on the Mac (`NOSKIP=1` turns it off in `test/gbplay_host.c`): all
686 games that run (536 Color, 150 Game Boy) give the same
lists, picture hashes and cartridge RAM, frame for frame, over 3,600 TV
frames with and without it. Peanut-GB alone, on the Mac:

| Game | Without | With |
|---|---|---|
| Link's Awakening DX, intro and title (no input, 900 frames) | 294 µs | 161 µs |
| Link's Awakening DX, played (4,800 frames) | 151 µs | 139 µs |
| Kirby's Dream Land, played | 148 µs | 134 µs |

Instructions a frame (TV frames 600–900) drop further than time does: Link's
Awakening's intro from 14,079 to 4,687, Kirby's Dream Land from 7,448 to
5,477, Castlevania from 923 to 578. The rest of the time goes to drawing the
lines and the LCD events, which the skip doesn't touch; each event still
costs the loop a turn or two. The SML hashes in `../../gbbench/README.md`
are unchanged.

## minigb_apu

From Peanut-GB `d0bcca7`'s `examples/sdl2`, unchanged.
