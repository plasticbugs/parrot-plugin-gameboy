// gbcv: the cart's side of a Game Boy on the ColecoVision. Portable C (no
// pico-sdk), used by sdplayer's Game Boy mode and by the host test.
//
// It is a port of coleco/gbcv/src/cv.rs, list.rs and psg.rs (the model),
// and makes the same choices byte for byte: test/run.sh feeds it the
// model's inputs (gbcv-sim --lists writes trace.bin) and compares every
// command list with the model's lists.bin.
//
//   each Game Boy picture:   gbcv_offer(e, shades, index)
//   (a Game Boy Color one:   gbcv_offer_colours(e, colours, index))
//   each sound register write: gbcv_tr_write(t, cycle, addr, value)
//   each TV frame:           gbcv_tr_until(t, cycle);
//                            n = gbcv_tr_psg(t, psg);
//                            len = gbcv_tv_frame(e, psg, n, list);
//
// The list is what kernel/gbkernel.asm runs in that frame (format and
// costs in its header).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GBCV_W     160
#define GBCV_H     144
#define GBCV_COLS  20
#define GBCV_ROWS  18
#define GBCV_CELLS (GBCV_COLS * GBCV_ROWS)
#define GBCV_COL0  6
#define GBCV_ROW0  3
#define GBCV_LIST_MAX 0x4000   // a list never outgrows its 16K bank

// on the cart, the per-frame code runs from SRAM: from flash it would share
// the 16K XIP cache with a ROM held in the PSRAM
#if defined(PICO_ON_DEVICE) && PICO_ON_DEVICE
#define GBCV_HOT __attribute__((section(".time_critical.gbcv")))
#else
#define GBCV_HOT
#endif

// ---- looks ---------------------------------------------------------------------
typedef struct {
    const char *id, *label;
    uint8_t shades[4];   // hardware colour for Game Boy shade 0 (lightest) .. 3
    uint8_t border;      // backdrop around the picture (R7)
    bool colour;         // a Color game in its own colours (a DMG game: the shades)
} gbcv_look_t;

extern const gbcv_look_t gbcv_looks[];
extern const int gbcv_nlooks;
#define GBCV_LOOK_GREY   1
#define GBCV_LOOK_COLOUR 4

// A Game Boy Color colour (RGB555, red in bits 10-14, as Peanut-GB's
// fixPalette has it) as a TMS9918 colour (1-15): the nearest one for a
// colour look, else the look's colour for its shade of grey.
uint8_t gbcv_map_rgb(const gbcv_look_t *look, uint16_t rgb555);

// ---- the encoder: pictures to tiles, the pattern cache, the lists -------------
typedef struct {
    uint8_t key[16];
    uint8_t slot;
    bool used;
} gbcv_ent_t;

#define GBCV_HASH 512   // per third, a power of two

typedef struct {
    uint8_t third, slot;
    uint8_t cell[16];
} gbcv_upload_t;

typedef struct {
    uint32_t tv_frames, offered, shown, uploads, hits, overflow, clash_rows, index_resets;
    uint64_t t_used;
} gbcv_stats_t;

typedef struct {
    gbcv_look_t look;
    uint8_t names[768];        // the name table the picture should show
    uint8_t vram_names[768];   // the name table in VRAM
    int vram_r7;               // R7 in VRAM, -1 before the first list
    uint8_t pat[3][256][16];   // what VRAM holds in each slot
    uint32_t age[3][256];      // TV frame each slot was last put on screen
    gbcv_ent_t index[3][GBCV_HASH];
    uint32_t now;

    // the newest Game Boy picture, as cells
    bool has_latest;
    uint64_t latest_index;
    uint32_t latest_clash;
    uint8_t latest[GBCV_CELLS][16];

    // the picture being uploaded
    bool has_target;
    uint64_t target_index;
    uint32_t target_clash;
    uint8_t target_names[GBCV_CELLS];
    gbcv_upload_t uploads[GBCV_CELLS];
    uint16_t up_head, up_tail;

    uint64_t shown_gb;
    gbcv_stats_t stats;

    // gbcv_offer's memory of the last picture: each 8-pixel row as its two
    // bit planes (lo | hi << 8, pixel 0 in bit 7), or for gbcv_offer_colours
    // its eight colours (a nibble each), and which rows clashed (bit y of a
    // cell), so rows that haven't changed keep their cells
    bool prev_ok, prev_colours;
    uint32_t prev[GBCV_CELLS][8];
    uint8_t prev_clash[GBCV_CELLS];
} gbcv_enc_t;

void gbcv_init(gbcv_enc_t *e, const gbcv_look_t *look);
void gbcv_set_look(gbcv_enc_t *e, const gbcv_look_t *look);
// shades: 160 x 144, one byte a pixel, 0 (lightest) .. 3
void gbcv_offer(gbcv_enc_t *e, const uint8_t *shades, uint64_t gb_index);
// colours: 160 x 144, one byte a pixel, each a TMS9918 colour 1-15
void gbcv_offer_colours(gbcv_enc_t *e, const uint8_t *colours, uint64_t gb_index);
// this TV frame's list into out (GBCV_LIST_MAX bytes); returns its length
size_t gbcv_tv_frame(gbcv_enc_t *e, const uint8_t *psg, int npsg, uint8_t *out);
// tests: index entries whose slot doesn't hold their cell (0 when no
// picture is being uploaded)
int gbcv_check_index(const gbcv_enc_t *e);

// ---- the sound: Game Boy sound registers to PSG bytes -------------------------
typedef struct {
    uint8_t initial, period, vol, timer;
    bool up;
} gbcv_env_t;

typedef struct {
    bool on, dac, len_on, sw_neg, sw_on;
    uint16_t freq, len, shadow;
    gbcv_env_t env;
    uint8_t sw_period, sw_shift, sw_timer, level, nr43;
} gbcv_chan_t;

typedef struct {
    bool power;
    uint8_t nr51;
    gbcv_chan_t ch[4];
    uint64_t seq_at;
    uint8_t seq_step;
    int32_t sent_period[3];   // -1: never sent
    int16_t sent_att[4];
    int16_t sent_noise;
} gbcv_tr_t;

void gbcv_tr_init(gbcv_tr_t *t);
// a write to $FF10-$FF26 at this Game Boy cycle (cycles never go back)
void gbcv_tr_write(gbcv_tr_t *t, uint64_t cycle, uint16_t addr, uint8_t v);
// run the frame sequencer up to this cycle
void gbcv_tr_until(gbcv_tr_t *t, uint64_t cycle);
// the PSG bytes that bring the PSG to the Game Boy's state; returns how many (<= 16)
int gbcv_tr_psg(gbcv_tr_t *t, uint8_t *out);
