// gbplay: a Game Boy (Peanut-GB) behind the cart's encoder, one ColecoVision
// TV frame at a time. Portable C: sdplayer's Game Boy mode runs it on core 0,
// and test/gbplay_host.c runs it on the Mac to make lists MAME can play.
//
// Each TV frame the console reports its controller (the kernel's reads of
// $FFC0/$FFD0/$FFE0 + bits) and takes a list ($FFF0 + seq). For the frame
// that read starts, the cart calls gbplay_frame() with the report: it runs
// the Game Boy up to the end of that TV frame and returns the next list.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gbcv.h"

typedef struct {
    uint32_t tv;            // TV frames played
    uint64_t gb_frames;     // Game Boy frames run
    uint32_t pictures;      // Game Boy pictures offered (frames with the LCD on)
    uint32_t slipped;       // Game Boy frames skipped to catch up
    uint32_t errors;        // Peanut-GB errors (bad opcode, ...)
    int look;
    // time spent, in gbplay_clock's units (only if it is set)
    uint64_t t_emulate;     // Peanut-GB: gb_run_frame
    uint64_t t_offer;       // pictures to cells (gbcv_offer)
    uint64_t t_list;        // the sound and the list (gbcv_tv_frame)
} gbplay_stats_t;

// a clock for the times in gbplay_stats (us on the cart, ns on the host);
// NULL (the default) skips them
extern uint32_t (*gbplay_clock)(void);

// for the Mac's previews: each line of a Color game as Peanut-GB draws it
// (palette entries) with the palettes then (RGB555); NULL (the default) on the cart
extern void (*gbplay_on_line)(const uint8_t *px, unsigned line, const uint16_t *palette);

// Can this cart run it? hdr = the first 0x150 bytes of the ROM. On false,
// why says so in a few capital words (the menu font).
bool gbplay_check(const uint8_t *hdr, uint32_t size, const char **why);

// A Game Boy Color game (or one with Color extras), which this build runs in
// colour (PEANUT_FULL_GBC_SUPPORT). hdr as for gbplay_check.
bool gbplay_colour(const uint8_t *hdr);

// The emulator's state: gbplay_init wants this many bytes for it (about
// 17K, or 49K with Color support), 4-byte aligned.
size_t gbplay_gb_size(void);

// rom stays where it is (SRAM or PSRAM); the cart's RAM is cart_ram (32K).
// look: an index into gbcv_looks, or -1 for the game's own (colour for a
// Color game, grey for the rest). gb_mem: gbplay_gb_size() bytes.
bool gbplay_init(const uint8_t *rom, uint32_t size, uint8_t *cart_ram, int look, void *gb_mem);

// Set it before gbplay_init when gbplay_enc is running already (a plugin
// showed its messages through it): gbplay_init then keeps it, and so its
// idea of the console's VRAM, rather than starting it again; the look is set.
extern bool gbplay_keep_enc;

// f = TV frames since the console started taking lists (1 for the first);
// joy, fire, key: the report, as read (low = pressed). Writes the list the
// console runs in the next frame; returns its length.
size_t gbplay_frame(uint32_t f, uint8_t joy, uint8_t fire, uint8_t key, uint8_t *list);

// Game Boy buttons held elsewhere (a USB pad), 1 = held, for the frames
// from the next: they add to the controller's. (Both of a pair of
// directions at once, from the two together: neither.)
enum { GBPLAY_A = 0x01, GBPLAY_B = 0x02, GBPLAY_SELECT = 0x04, GBPLAY_START = 0x08,
       GBPLAY_RIGHT = 0x10, GBPLAY_LEFT = 0x20, GBPLAY_UP = 0x40, GBPLAY_DOWN = 0x80 };
void gbplay_buttons(uint8_t held);

// the look (an index into gbcv_looks), as keypad 1-5 pick it
void gbplay_look(int look);

// work RAM ($C000-$DFFF), for test bots
uint8_t gbplay_peek(uint16_t addr);

// where the Game Boy is, for a report if it locks up: PC | ROM bank << 16,
// and IE | IF << 8 | LCDC << 16 | flags << 24 (bit 0 HALT, 1 IME, 2 double speed)
void gbplay_where(uint32_t *pc_bank, uint32_t *regs);

// ---- the border, and the keypad's requests ------------------------------------
// The border shows colour (TMS9918, 0-15) for this many TV frames, then the
// look's again.
void gbplay_border(uint8_t colour, uint32_t frames);
// Keypad 7 held for a second asks to save a state, 9 to load one (the border
// turns dark yellow while it is held): GBPLAY_SAVE / GBPLAY_LOAD, once a
// hold, or 0.
enum { GBPLAY_SAVE = 1, GBPLAY_LOAD = 2 };
int gbplay_request(void);

// ---- the cart's battery save (a .sav, as other emulators keep it) --------------
// The cart RAM (MBC2: 512 bytes, a nibble a byte), then for an MBC3 with a
// clock 48 bytes of clock (VBA-M's: the five registers and the five latched
// ones as 32-bit words, then a 64-bit UNIX time). The size, 0 if the cart
// keeps nothing; *ram: of which cart RAM, a multiple of 512. hdr as for
// gbplay_check.
//
// The cart has no clock of its own: a game's clock runs while it is played
// and stands still between, and the time saved is the time loaded (or
// 2026-01-01 for a new save) plus the time played.
uint32_t gbplay_sav_size(const uint8_t *hdr, uint32_t *ram);
// a 512-byte sector of the .sav as the game has it now (the clock's sector,
// past the RAM, is 48 bytes and zeros)
void gbplay_sav_get(uint32_t sec, uint8_t *buf);
// after gbplay_init, before the first frame: a sector of a .sav file_size
// bytes long (a clock with a 32-bit time, 44 bytes, is read too)
void gbplay_sav_put(uint32_t sec, const uint8_t *buf, uint32_t file_size);
// the cart RAM's 512-byte sectors changed since the last call (bit n:
// sector n), which it then forgets
uint64_t gbplay_sav_dirty(void);
uint32_t gbplay_sav_changes(void);        // a count of changes to the cart RAM

// ---- save states: the whole Game Boy, between two frames -------------------------
// About 84K with Color support (the cart's .state files are 128K): a header
// sector (this build, this game, the payload's CRC-32), then Peanut-GB's
// context, the cart RAM and the sound's state. Only this build reads it.
#define GBPLAY_STATE_FILE (128u * 1024)
uint32_t gbplay_state_sectors(void);
// saving: sector 0 first (it sums the rest), then the others, all between
// the same two frames
void gbplay_state_get(uint32_t sec, uint8_t *buf);
// loading: each sector from 0 through check, which returns 1 for more, 0
// when the state is whole and good, or why not:
enum { GBPLAY_ST_NONE = -1,   // not a state
       GBPLAY_ST_OLD = -2,    // another build's
       GBPLAY_ST_GAME = -3,   // another game's
       GBPLAY_ST_BAD = -4 };  // damaged
int gbplay_state_check(uint32_t sec, const uint8_t *buf);
// then each sector again from 0 through put, and done: true if they summed
// as checked (if not, the Game Boy is a mix: start it again with gbplay_init).
// The cart RAM it brings counts as changed (gbplay_sav_dirty).
void gbplay_state_put(uint32_t sec, const uint8_t *buf);
bool gbplay_state_done(void);

extern gbplay_stats_t gbplay_stats;
extern gbcv_enc_t gbplay_enc;
// the newest Game Boy picture, 160 x 144 shades (0-3), or for a Color game
// TMS9918 colours (1-15)
extern uint8_t gbplay_shades[GBCV_W * GBCV_H];
