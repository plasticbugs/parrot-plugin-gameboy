// gbplay.c: see gbplay.h. The model's Session::step (coleco/gbcv/src/lib.rs)
// in C, with Peanut-GB for gb-core.
#include <string.h>

#include "gbplay.h"

#ifndef MINIGB_APU_AUDIO_FORMAT_S16SYS
#define MINIGB_APU_AUDIO_FORMAT_S16SYS 1
#endif
#include "vendor/minigb_apu/minigb_apu.h"

// Peanut-GB's per-instruction functions go where the encoder's do (see
// GBCV_HOT): a section on an earlier declaration holds for the definition
struct gb_s;
uint8_t __gb_read(struct gb_s *gb, uint16_t addr) GBCV_HOT;
void __gb_write(struct gb_s *gb, uint_fast16_t addr, uint8_t val) GBCV_HOT;
uint8_t __gb_execute_cb(struct gb_s *gb) GBCV_HOT;
void __gb_draw_line(struct gb_s *gb) GBCV_HOT;
void __gb_step_cpu(struct gb_s *gb) GBCV_HOT;
// (the wait-loop skip runs on JRs back only: small beats fast, as RAM is short)
#if defined(__GNUC__) && !defined(__clang__)
uint_fast16_t __gb_idle_skip(struct gb_s *gb, uint_fast16_t top, uint_fast16_t jump_cycles)
    GBCV_HOT __attribute__((optimize("Os")));
#else
uint_fast16_t __gb_idle_skip(struct gb_s *gb, uint_fast16_t top, uint_fast16_t jump_cycles) GBCV_HOT;
#endif
void gb_run_frame(struct gb_s *gb) GBCV_HOT;
uint8_t audio_read(uint16_t addr) GBCV_HOT;
void audio_write(uint16_t addr, uint8_t val) GBCV_HOT;

#define ENABLE_SOUND 1
#define ENABLE_LCD 1
#define PEANUT_GB_12_COLOUR 0
#include "vendor/peanut_gb.h"

// Game Boy and TV frame lengths: 70224 cycles at 4,194,304 Hz, and 262 x 228
// T at 3,579,545 Hz. A Game Boy frame n is due by the end of TV frame f when
// n x GB_T x Z80_HZ < f x TV_T x GB_HZ (the model's loop, in integers).
#define GB_T   70224ull
#define GB_HZ  4194304ull
#define TV_T   59736ull
#define Z80_HZ 3579545ull
#define MAX_CATCHUP 3            // Game Boy frames in one TV frame, at most

gbplay_stats_t gbplay_stats;
uint32_t (*gbplay_clock)(void);
void (*gbplay_on_line)(const uint8_t *px, unsigned line, const uint16_t *palette);
gbcv_enc_t gbplay_enc;
uint8_t gbplay_shades[GBCV_W * GBCV_H];

static struct gb_s *gbp;       // in the caller's memory (gbplay_init)
static struct minigb_apu_ctx apu;
static gbcv_tr_t tr;
static const uint8_t *rom;
static uint32_t rom_size, rom_mask;
static uint8_t *cram;
static bool drawn;
static uint64_t frame_base, last_cycle;   // Game Boy cycles at the start of this frame; last write's
static uint8_t last_key = 0x0F;
static uint8_t pad_held;                  // gbplay_buttons
bool gbplay_keep_enc;

// the .sav (gbplay_sav_*): how much cart RAM it keeps, and the clock
static uint32_t sav_ram;
static bool sav_rtc;
static uint64_t rtc_time, rtc_frame;      // the clock's UNIX time as of Game Boy frame rtc_frame
#define RTC_EPOCH 1767268800ull           // 2026-01-01 12:00 UTC: a game's clock with no time of its own

// the border (gbplay_border) and the held keys (gbplay_request)
static uint8_t border_col;
static uint32_t border_left, hold_n;
static int request;
#define HOLD_HINT 15                      // TV frames held: the border says "keep holding"
#define HOLD_GO   60                      // ...and here it's a request
#define HINT_COL  10                      // dark yellow

// A Color game's palettes as TMS9918 colours in the current look, kept up
// to date line by line (a game may change a palette partway down the picture)
static uint16_t pal_seen[64];
static uint8_t pal_tms[64];
static bool pal_ok;

// ---- Peanut-GB's callbacks ---------------------------------------------------------

GBCV_HOT static uint8_t rom_read(struct gb_s *g, const uint_fast32_t a) {
    (void)g;
    return rom[a & rom_mask];
}
GBCV_HOT static uint8_t ram_read(struct gb_s *g, const uint_fast32_t a) {
    (void)g;
    return cram[a & 0x7FFF];
}
// the cart's RAM: each change marks its 512-byte piece for the .sav
static uint64_t ram_dirty;
static uint32_t ram_changes;
GBCV_HOT static void ram_write(struct gb_s *g, const uint_fast32_t a, const uint8_t v) {
    (void)g;
    uint32_t i = a & 0x7FFF;
    if (cram[i] == v) return;
    cram[i] = v;
    ram_dirty |= 1ull << (i >> 9);
    ram_changes++;
}
static void on_error(struct gb_s *g, const enum gb_error_e e, const uint16_t a) {
    (void)g; (void)e; (void)a;
    gbplay_stats.errors++;
}
GBCV_HOT static void draw_line(struct gb_s *g, const uint8_t *px, const uint_fast8_t line) {
    uint8_t *d = &gbplay_shades[line * GBCV_W];
#if PEANUT_FULL_GBC_SUPPORT
    if (g->cgb.cgbMode) {
        // px: palette entries (background 0-31, sprites 32-63)
        const uint16_t *fp = g->cgb.fixPalette;
        if (!pal_ok || memcmp(fp, pal_seen, sizeof pal_seen)) {
            for (int i = 0; i < 64; i++)
                if (!pal_ok || fp[i] != pal_seen[i]) {
                    pal_seen[i] = fp[i];
                    pal_tms[i] = gbcv_map_rgb(&gbplay_enc.look, fp[i]);
                }
            pal_ok = true;
        }
        for (int x = 0; x < GBCV_W; x++) d[x] = pal_tms[px[x] & 63];
        if (gbplay_on_line) gbplay_on_line(px, line, fp);
    } else
#endif
    for (int x = 0; x < GBCV_W; x++) d[x] = px[x] & 3;
    if (line == GBCV_H - 1) drawn = true;
}

// The sound registers: minigb_apu keeps them (what the game reads back);
// the translator gets each write with its cycle. Peanut-GB's frame ends as
// line 144 begins, so the cycle within a frame is (LY + 10) mod 154 lines on.
uint8_t audio_read(uint16_t addr) { return minigb_apu_audio_read(&apu, addr); }
void audio_write(uint16_t addr, uint8_t val) {
    minigb_apu_audio_write(&apu, addr, val);
    uint64_t c = frame_base + ((gbp->hram_io[IO_LY] + 10u) % 154u) * 456u + gbp->counter.lcd_count;
    if (c < last_cycle) c = last_cycle;   // never backwards
    last_cycle = c;
    gbcv_tr_write(&tr, c, addr, val);
}

// ---- the cart ----------------------------------------------------------------------

bool gbplay_check(const uint8_t *h, uint32_t size, const char **why) {
    static const int8_t mbc[0x20] = {0, 1, 1, 1, -1, 2, 2, -1, 0, 0, -1, 0, 0, 0, -1, 3,
                                     3, 3, 3, 3, -1, -1, -1, -1, -1, 5, 5, 5, 5, 5, 5, -1};
    uint8_t x = 0;
    for (int i = 0x134; i <= 0x14C; i++) x = (uint8_t)(x - h[i] - 1);
    if (x != h[0x14D]) { *why = "Not a Game Boy ROM"; return false; }
#if !PEANUT_FULL_GBC_SUPPORT
    if (h[0x143] == 0xC0) { *why = "Game Boy Color only"; return false; }
#endif
    if (h[0x149] == 4 || h[0x149] == 5) { *why = "Cartridge RAM too big"; return false; }
    if (h[0x147] >= 0x20 || mbc[h[0x147]] < 0) { *why = "Cartridge type not supported"; return false; }
    if (size < 0x8000 || size > (8u << 20)) { *why = "ROM size not supported"; return false; }
    *why = "";
    return true;
}

bool gbplay_colour(const uint8_t *hdr) { return PEANUT_FULL_GBC_SUPPORT && (hdr[0x143] & 0x80); }

size_t gbplay_gb_size(void) { return sizeof(struct gb_s); }

bool gbplay_init(const uint8_t *r, uint32_t size, uint8_t *cart_ram, int look, void *gb_mem) {
    gbp = gb_mem;
    memset(gbp, 0, sizeof *gbp);
    pal_ok = false;
    rom = r;
    rom_size = size;
    rom_mask = 1;
    while (rom_mask < size) rom_mask <<= 1;
    rom_mask--;
    cram = cart_ram;
    memset(cram, 0xFF, 0x8000);
    memset(&gbplay_stats, 0, sizeof gbplay_stats);
    if (look < 0 || look >= gbcv_nlooks) look = gbplay_colour(r) ? GBCV_LOOK_COLOUR : GBCV_LOOK_GREY;
    gbplay_stats.look = look;
    minigb_apu_audio_init(&apu);
    gbcv_tr_init(&tr);
    if (gbplay_keep_enc) gbcv_set_look(&gbplay_enc, &gbcv_looks[look]);
    else gbcv_init(&gbplay_enc, &gbcv_looks[look]);
    frame_base = last_cycle = 0;
    last_key = 0x0F;
    pad_held = 0;
    gbplay_sav_size(r, &sav_ram);
    sav_rtc = r[0x147] == 0x0F || r[0x147] == 0x10;
    rtc_time = RTC_EPOCH;
    rtc_frame = 0;
    ram_dirty = 0;
    ram_changes = 0;
    border_left = hold_n = 0;
    request = 0;
    if (gb_init(gbp, rom_read, ram_read, ram_write, on_error, NULL) != GB_INIT_NO_ERROR) return false;
    gb_init_lcd(gbp, draw_line);
    return true;
}

// keypad code as read -> key: '1'..'5' pick a look
static int look_key(uint8_t key) {
    switch (key) {
    case 0xD: return 0;   // 1
    case 0x7: return 1;   // 2
    case 0xC: return 2;   // 3
    case 0x2: return 3;   // 4
    case 0x3: return 4;   // 5
    default: return -1;
    }
}

_Static_assert(GBPLAY_A == JOYPAD_A && GBPLAY_B == JOYPAD_B && GBPLAY_SELECT == JOYPAD_SELECT &&
               GBPLAY_START == JOYPAD_START && GBPLAY_RIGHT == JOYPAD_RIGHT && GBPLAY_LEFT == JOYPAD_LEFT &&
               GBPLAY_UP == JOYPAD_UP && GBPLAY_DOWN == JOYPAD_DOWN, "gbplay.h's buttons are Peanut-GB's");

void gbplay_buttons(uint8_t held) { pad_held = held; }

void gbplay_look(int lk) {
    if (lk < 0 || lk >= gbcv_nlooks || lk == gbplay_stats.look) return;
    gbplay_stats.look = lk;
    gbcv_set_look(&gbplay_enc, &gbcv_looks[lk]);
    pal_ok = false;
}

size_t gbplay_frame(uint32_t f, uint8_t joy, uint8_t fire, uint8_t key, uint8_t *list) {
    gbplay_stats.tv = f;

    // the controller: joystick N E S W in bits 0-3, left fire = B, right
    // fire = A, # = Start, * = Select (all low = pressed, as Peanut-GB wants)
    uint8_t b = 0xFF;
    if (!(joy & 1)) b &= (uint8_t)~JOYPAD_UP;
    if (!(joy & 2)) b &= (uint8_t)~JOYPAD_RIGHT;
    if (!(joy & 4)) b &= (uint8_t)~JOYPAD_DOWN;
    if (!(joy & 8)) b &= (uint8_t)~JOYPAD_LEFT;
    if (!(fire & 1)) b &= (uint8_t)~JOYPAD_B;
    if (!(fire & 2)) b &= (uint8_t)~JOYPAD_A;
    if (key == 0x6) b &= (uint8_t)~JOYPAD_START;
    if (key == 0x9) b &= (uint8_t)~JOYPAD_SELECT;
    b &= (uint8_t)~pad_held;
    if (!(b & (JOYPAD_UP | JOYPAD_DOWN))) b |= JOYPAD_UP | JOYPAD_DOWN;
    if (!(b & (JOYPAD_LEFT | JOYPAD_RIGHT))) b |= JOYPAD_LEFT | JOYPAD_RIGHT;
    // a button going down requests the joypad interrupt, which wakes a
    // game waiting for one in HALT (Peanut-GB never raises it itself)
    if (gbp->direct.joypad & (uint8_t)~b) gbp->hram_io[IO_IF] |= CONTROL_INTR;
    gbp->direct.joypad = b;

    // keypad 1-5: the picture, on the press
    if (key != last_key) gbplay_look(look_key(key));
    last_key = key;

    // keypad 7 / 9 held for a second: save / load a state (once a hold)
    if (key == 0x5 || key == 0xB) {       // '7', '9'
        if (++hold_n == HOLD_GO) request = key == 0x5 ? GBPLAY_SAVE : GBPLAY_LOAD;
    } else hold_n = 0;

    // the Game Boy, up to the end of this TV frame
    // (less any frames skipped before to catch up)
    uint64_t due = ((uint64_t)f * TV_T * GB_HZ + GB_T * Z80_HZ - 1) / (GB_T * Z80_HZ) - gbplay_stats.slipped;
    if (due > gbplay_stats.gb_frames + MAX_CATCHUP) {
        gbplay_stats.slipped += (uint32_t)(due - gbplay_stats.gb_frames - MAX_CATCHUP);
        due = gbplay_stats.gb_frames + MAX_CATCHUP;
    }
    uint32_t (*clk)(void) = gbplay_clock;
    uint32_t t0 = clk ? clk() : 0, t1;
    while (gbplay_stats.gb_frames < due) {
        // catching up: only the last frame is shown, so only it is drawn
        // (Peanut-GB skips a whole frame's lines; the window restarts at LY 0)
        gbp->direct.frame_skip = gbplay_stats.gb_frames + 1 < due;
        gbp->display.frame_skip_count = 0;
        drawn = false;
        gb_run_frame(gbp);
        gbplay_stats.gb_frames++;
        frame_base += GB_T;
        if (last_cycle < frame_base) last_cycle = frame_base;
        if (clk) { t1 = clk(); gbplay_stats.t_emulate += t1 - t0; t0 = t1; }
        if (drawn) {
#if PEANUT_FULL_GBC_SUPPORT
            if (gbp->cgb.cgbMode) gbcv_offer_colours(&gbplay_enc, gbplay_shades, gbplay_stats.gb_frames);
            else
#endif
            gbcv_offer(&gbplay_enc, gbplay_shades, gbplay_stats.gb_frames);
            gbplay_stats.pictures++;
            if (clk) { t1 = clk(); gbplay_stats.t_offer += t1 - t0; t0 = t1; }
        }
    }

    // the next list: the PSG to the Game Boy's sound now, then the picture
    gbcv_tr_until(&tr, frame_base);
    if (border_left) { border_left--; gbplay_enc.look.border = border_col; }
    else if (hold_n >= HOLD_HINT && hold_n < HOLD_GO) gbplay_enc.look.border = HINT_COL;
    else gbplay_enc.look.border = gbcv_looks[gbplay_stats.look].border;
    uint8_t psg[32];
    int np = gbcv_tr_psg(&tr, psg);
    size_t len = gbcv_tv_frame(&gbplay_enc, psg, np, list);
    if (clk) gbplay_stats.t_list += clk() - t0;
    return len;
}

uint8_t gbplay_peek(uint16_t addr) {
    return addr >= 0xC000 && addr < 0xE000 ? gbp->wram[addr - 0xC000] : 0xFF;
}

void gbplay_where(uint32_t *pc_bank, uint32_t *regs) {
    const struct gb_s *g = gbp;
    uint32_t flags = (uint32_t)g->gb_halt | (uint32_t)g->gb_ime << 1;
#if PEANUT_FULL_GBC_SUPPORT
    flags |= (uint32_t)(g->cgb.doubleSpeed & 1) << 2;
#endif
    *pc_bank = g->cpu_reg.pc.reg | (uint32_t)(g->selected_rom_bank & 0xFFFF) << 16;
    *regs = g->hram_io[IO_IE] | (uint32_t)g->hram_io[IO_IF] << 8 | (uint32_t)g->hram_io[IO_LCDC] << 16 | flags << 24;
}

// ---- the border, and requests from the keypad ----------------------------------------

void gbplay_border(uint8_t colour, uint32_t frames) {
    border_col = colour;
    border_left = frames;
}

int gbplay_request(void) {
    int r = request;
    request = 0;
    return r;
}

// ---- the cart's battery save ----------------------------------------------------------

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

uint32_t gbplay_sav_size(const uint8_t *h, uint32_t *ram) {
    uint8_t t = h[0x147];
    bool battery = t == 0x03 || t == 0x06 || t == 0x09 || t == 0x0F || t == 0x10 || t == 0x13 || t == 0x1B || t == 0x1E;
    static const uint32_t sizes[4] = {0, 0x800, 0x2000, 0x8000};
    *ram = !battery ? 0 : t == 0x05 || t == 0x06 ? 0x200 : h[0x149] < 4 ? sizes[h[0x149]] : 0;
    if (!battery) return 0;
    return *ram + (t == 0x0F || t == 0x10 ? 48 : 0);
}

static uint64_t rtc_now(void) {             // the clock's UNIX time, as the game has run
    return rtc_time + (gbplay_stats.gb_frames - rtc_frame) * GB_T / GB_HZ;
}

void gbplay_sav_get(uint32_t sec, uint8_t *buf) {
    memset(buf, 0, 512);
    uint32_t off = sec * 512;
    if (off < sav_ram) { memcpy(buf, cram + off, 512); return; }
    if (!sav_rtc || off != sav_ram) return;
    for (int i = 0; i < 5; i++) {
        wr32(buf + 4 * i, gbp->rtc_real.bytes[i]);
        wr32(buf + 20 + 4 * i, gbp->rtc_latched.bytes[i]);
    }
    uint64_t t = rtc_now();
    wr32(buf + 40, (uint32_t)t);
    wr32(buf + 44, (uint32_t)(t >> 32));
}

void gbplay_sav_put(uint32_t sec, const uint8_t *buf, uint32_t file_size) {
    uint32_t off = sec * 512;
    if (off >= file_size) return;
    if (off < sav_ram) {
        uint32_t n = file_size - off < 512 ? file_size - off : 512;
        memcpy(cram + off, buf, n);
        if (sav_ram == 0x200)               // MBC2: a nibble a byte, the top one reads as 1s
            for (uint32_t i = 0; i < 0x200; i++) cram[i] |= 0xF0;
        return;
    }
    if (!sav_rtc || off != sav_ram || file_size < sav_ram + 44) return;
    static const uint8_t mask[5] = {0x3F, 0x3F, 0x1F, 0xFF, 0xC1};
    for (int i = 0; i < 5; i++) {
        gbp->rtc_real.bytes[i] = (uint8_t)(rd32(buf + 4 * i) & mask[i]);
        gbp->rtc_latched.bytes[i] = (uint8_t)(rd32(buf + 20 + 4 * i) & mask[i]);
    }
    rtc_time = rd32(buf + 40) | (file_size >= sav_ram + 48 ? (uint64_t)rd32(buf + 44) << 32 : 0);
    rtc_frame = gbplay_stats.gb_frames;
}

uint64_t gbplay_sav_dirty(void) {
    uint64_t d = ram_dirty;
    ram_dirty = 0;
    return d;
}

uint32_t gbplay_sav_changes(void) { return ram_changes; }

// ---- save states ------------------------------------------------------------------------
// Sector 0 is the header; the payload follows from sector 1: Peanut-GB's
// context (its function pointers are this build's, put back on loading),
// the cart's 32K of RAM, minigb_apu's registers, the sound translator.
// The encoder isn't kept: it mirrors the console's VRAM, which stays as it is.
#define ST_MAGIC   "GBCVSTAT"
#define ST_VERSION 1u

typedef struct { uint8_t *p; uint32_t n; } seg_t;

static int segs(seg_t *s) {
    s[0] = (seg_t){(uint8_t *)gbp, (uint32_t)sizeof *gbp};
    s[1] = (seg_t){cram, 0x8000};
    s[2] = (seg_t){(uint8_t *)&apu, (uint32_t)sizeof apu};
    s[3] = (seg_t){(uint8_t *)&tr, (uint32_t)sizeof tr};
    return 4;
}

static uint32_t payload_len(void) {
    seg_t s[4];
    uint32_t n = 0;
    for (int i = 0, k = segs(s); i < k; i++) n += s[i].n;
    return n;
}

// 512 bytes of the payload at off, out of (to buf) or into the state
static void payload(uint32_t off, uint8_t *buf, bool to_buf) {
    seg_t s[4];
    int k = segs(s);
    uint32_t base = 0;
    if (to_buf) memset(buf, 0, 512);
    for (int i = 0; i < k; base += s[i++].n) {
        if (off + 512 <= base || off >= base + s[i].n) continue;
        uint32_t a = off > base ? off - base : 0;               // in the segment
        uint32_t b = base > off ? base - off : 0;               // in buf
        uint32_t n = s[i].n - a < 512 - b ? s[i].n - a : 512 - b;
        if (to_buf) memcpy(buf + b, s[i].p + a, n);
        else memcpy(s[i].p + a, buf + b, n);
    }
}

static uint32_t crc32_add(uint32_t c, const uint8_t *p, uint32_t n) {
    c = ~c;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++) c = c >> 1 ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}

static void id(uint8_t *h) {               // the game: title, header and global checksums
    memcpy(h, rom + 0x134, 16);
    h[16] = rom[0x14D]; h[17] = rom[0x14E]; h[18] = rom[0x14F];
}

uint32_t gbplay_state_sectors(void) { return 1 + (payload_len() + 511) / 512; }

void gbplay_state_get(uint32_t sec, uint8_t *buf) {
    if (sec) { payload((sec - 1) * 512, buf, true); return; }
    // the header: computed over the whole payload as it is now
    uint32_t c = 0, n = payload_len();
    static uint8_t tmp[512];
    for (uint32_t off = 0; off < n; off += 512) {
        payload(off, tmp, true);
        c = crc32_add(c, tmp, n - off < 512 ? n - off : 512);
    }
    memset(buf, 0, 512);
    memcpy(buf, ST_MAGIC, 8);
    wr32(buf + 8, ST_VERSION);
    wr32(buf + 12, n);
    wr32(buf + 16, c);
    wr32(buf + 20, (uint32_t)sizeof *gbp);
    wr32(buf + 24, (uint32_t)sizeof apu);
    wr32(buf + 28, (uint32_t)sizeof tr);
    id(buf + 32);
    wr32(buf + 52, (uint32_t)frame_base); wr32(buf + 56, (uint32_t)(frame_base >> 32));
}

// loading: what sector 0 said
static uint32_t ld_len, ld_crc, ld_c, ld_sec;
static uint64_t ld_base;
static bool ld_ok;

int gbplay_state_check(uint32_t sec, const uint8_t *buf) {
    if (sec == 0) {
        uint8_t me[20];
        id(me);
        ld_ok = false;
        if (memcmp(buf, ST_MAGIC, 8)) return GBPLAY_ST_NONE;
        if (rd32(buf + 8) != ST_VERSION || rd32(buf + 12) != payload_len() || rd32(buf + 20) != sizeof *gbp ||
            rd32(buf + 24) != sizeof apu || rd32(buf + 28) != sizeof tr) return GBPLAY_ST_OLD;
        if (memcmp(buf + 32, me, 19)) return GBPLAY_ST_GAME;
        ld_len = rd32(buf + 12);
        ld_crc = rd32(buf + 16);
        ld_base = rd32(buf + 52) | (uint64_t)rd32(buf + 56) << 32;
        ld_c = 0;
        ld_sec = 1;
        return 1;
    }
    if (sec != ld_sec) return GBPLAY_ST_BAD;
    uint32_t off = (sec - 1) * 512;
    ld_c = crc32_add(ld_c, buf, ld_len - off < 512 ? ld_len - off : 512);
    ld_sec++;
    if (off + 512 < ld_len) return 1;
    ld_ok = ld_c == ld_crc;
    return ld_ok ? 0 : GBPLAY_ST_BAD;
}

// this build's pointers in Peanut-GB's context, kept while a state goes in
static struct {
    uint8_t (*rom_read)(struct gb_s *, const uint_fast32_t);
    uint8_t (*ram_read)(struct gb_s *, const uint_fast32_t);
    void (*ram_write)(struct gb_s *, const uint_fast32_t, const uint8_t);
    void (*error)(struct gb_s *, const enum gb_error_e, const uint16_t);
    void (*serial_tx)(struct gb_s *, const uint8_t);
    enum gb_serial_rx_ret_e (*serial_rx)(struct gb_s *, uint8_t *);
    uint8_t (*bootrom_read)(struct gb_s *, const uint_fast16_t);
    void (*draw)(struct gb_s *, const uint8_t *, const uint_fast8_t);
    void *priv;
} ptrs;

void gbplay_state_put(uint32_t sec, const uint8_t *buf) {
    if (sec == 0) {
        ptrs.rom_read = gbp->gb_rom_read; ptrs.ram_read = gbp->gb_cart_ram_read; ptrs.ram_write = gbp->gb_cart_ram_write;
        ptrs.error = gbp->gb_error; ptrs.serial_tx = gbp->gb_serial_tx; ptrs.serial_rx = gbp->gb_serial_rx;
        ptrs.bootrom_read = gbp->gb_bootrom_read; ptrs.draw = gbp->display.lcd_draw_line; ptrs.priv = gbp->direct.priv;
        ld_c = 0;
        return;
    }
    uint32_t off = (sec - 1) * 512;
    if (off >= ld_len) return;
    ld_c = crc32_add(ld_c, buf, ld_len - off < 512 ? ld_len - off : 512);
    payload(off, (uint8_t *)buf, false);
}

bool gbplay_state_done(void) {
    gbp->gb_rom_read = ptrs.rom_read; gbp->gb_cart_ram_read = ptrs.ram_read; gbp->gb_cart_ram_write = ptrs.ram_write;
    gbp->gb_error = ptrs.error; gbp->gb_serial_tx = ptrs.serial_tx; gbp->gb_serial_rx = ptrs.serial_rx;
    gbp->gb_bootrom_read = ptrs.bootrom_read; gbp->display.lcd_draw_line = ptrs.draw; gbp->direct.priv = ptrs.priv;
    // the sound translator's clock is the Game Boy's cycles since gbplay_init: onto ours
    tr.seq_at = tr.seq_at >= ld_base ? tr.seq_at - ld_base + frame_base : frame_base;
    for (int i = 0; i < 3; i++) tr.sent_period[i] = -1;    // send the PSG all of it again
    for (int i = 0; i < 4; i++) tr.sent_att[i] = -1;
    tr.sent_noise = -1;
    pal_ok = false;
    drawn = false;
    ram_dirty = sav_ram ? ~0ull >> (64 - (sav_ram + 511) / 512) : 0;   // its cart RAM, to the .sav
    ram_changes++;
    return ld_c == ld_crc;
}
