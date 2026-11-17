// gb -- Game Boy and Game Boy Color games on the ColecoVision: pick a .gb or
// .gbc on the card in the menu and press A. Peanut-GB runs it on the cart
// and gbcv sends each picture and the sound to the console as the Game Boy
// kernel's frame lists: the menu's own Game Boy mode (sdplayer.c) as a
// plugin, with USB game pads. RESET goes back to the menu.
//
//   core 1  serves the kernel and a list a TV frame (cvframes.c)
//   core 0  this loop: for each TV frame the console starts, the Game Boy up
//           to the end of it and the list for the next (gbplay_frame), the
//           saves when they're due, the pads
//
// Memory: past .bss, up to the launch block, is free (no heap: nothing here
// mallocs). The Game Boy's state (49K with Color support) goes there, then
// the ROM if it fits (rounded up to a power of two, as Peanut-GB masks its
// addresses); else the ROM goes in the PSRAM and is read through the XIP
// cache, which is the PSRAM's alone (this plugin runs from SRAM).
//
// The controller: joystick = D-pad, left fire = B, right fire = A, # =
// Start, * = Select; keypad 1-5 pick the look; 7 held for a second saves a
// state, 9 loads it (the border turns yellow while it's held, then green,
// cyan or red).
//
// USB game pads (cvpad.h; a Switch Pro Controller or a plain HID pad,
// through a powered hub on the cart's USB port): D-pad or left stick =
// D-pad, the right face button = A and the bottom one = B (where Nintendo
// puts them), + = Start, - = Select; L and R step the look back and on; ZL
// held for a second saves a state, ZR loads it. A second pad plays as well.
//
// Saves, as the menu's Game Boy mode keeps them, next to the ROM by its full
// name: "<name>.sav", the cart's battery RAM (and an MBC3's clock) as other
// emulators keep it, read when the game starts and written a sector at a
// time, only the sectors the game changed (a second after it stops writing
// to its RAM, every ten seconds while it keeps on, the clock once a minute,
// and at RESET); and "<name>.state", the whole Game Boy (keypad 7 / 9). A
// card taken out and put back is written to again only if it holds the ROM
// where it was.
#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "hardware/watchdog.h"
#include "hardware/xip_cache.h"

#include "cvframes.h"
#include "cvpad.h"
#include "cvpsram.h"
#include "cvsd.h"
#include "font8.h"
#include "gbkernel.h"
#include "gbplay.h"
#include "handoff.h"

#define OPEN_US        20000        // serve the console this long before the card's (blocking) start
#define WATCHDOG_MS    4000         // a hang reboots into the menu (with where the Game Boy was)
#define NOTE_US        1000000      // the menu's note is brought up to date this often
#define CARD_SETTLE_US 500000       // a card put back: in this long before it's mounted

extern uint8_t __bss_end__[];       // (the linker's) the free RAM: from here to the launch block
#ifndef GB_FREE_END                 // (test/gb_host.c has its own)
#define GB_FREE_END ((uint8_t *)CVL_ADDR)
#endif

static uint8_t cart_ram[0x8000] __attribute__((aligned(4)));
static uint8_t list[GBCV_LIST_MAX] __attribute__((aligned(4)));
static size_t list_len;
static bool list_pending;           // built, not published yet (cvf_publish said wait)
static uint32_t seen_frames;
static fs_t fs;
static fs_file_t file;
static char path[sizeof CVL_BLOCK->path];
static char gb_dir[sizeof CVL_BLOCK->path];   // the ROM's folder ("" the root)
static char name[FS_LNAME], note[33];
static uint8_t *free_at;

static void *take(uint32_t n) {     // n bytes of the free RAM, or NULL
    uint8_t *p = (uint8_t *)(((uintptr_t)free_at + 3) & ~(uintptr_t)3);
    if (n > (uint32_t)(GB_FREE_END - p)) return NULL;
    free_at = p + n;
    return p;
}

static void feed(void) { if (watchdog_hw->ctrl & WATCHDOG_CTRL_ENABLE_BITS) watchdog_update(); }

// ---- the console, a list a TV frame ------------------------------------------------

static void publish(void) {
    if (list_pending && cvf_publish(list, list_len)) list_pending = false;
}

// a new TV frame started since the last call (and the last list is out)
static bool new_frame(void) {
    cvpad_task();
    cvf_task(time_us_32());
    feed();
    publish();
    if (list_pending) return false;
    uint32_t f = cvf_frames();
    if (f == seen_frames) return false;
    seen_frames = f;
    return true;
}

// ---- messages: a Game Boy picture of text ------------------------------------------
// The grey look's shades: 0 white, 1 grey, 2 dark blue, 3 black. 20
// characters a line (the 8x8 font, upper case).

#define MSG_COLS (GBCV_W / 8)
static uint64_t pic_index;

static void text(int x, int y, const char *s, int n, uint8_t shade, int scale) {
    for (int i = 0; i < n && s[i]; i++, x += 8 * scale) {
        int ch = (unsigned char)s[i];
        if (ch >= 'a' && ch <= 'z') ch -= 32;
        if (ch < 32 || ch > 95) ch = '?';
        const uint8_t *g = font8[ch - 32];
        for (int r = 0; r < 8 * scale; r++)
            for (int b = 0; b < 8 * scale; b++) {
                int px = x + b, py = y + r;
                if (px >= 0 && px < GBCV_W && py >= 0 && py < GBCV_H && (g[r / scale] >> (7 - b / scale) & 1))
                    gbplay_shades[py * GBCV_W + px] = shade;
            }
    }
}

static void centre(int y, const char *s, int n, uint8_t shade, int scale) {
    int w = n * 8 * scale;
    text(w < GBCV_W ? (GBCV_W - w) / 2 : 0, y, s, n, shade, scale);
}

// s in up to `lines` centred lines, broken at spaces; the y after them
static int wrap(int y, const char *s, uint8_t shade, int lines) {
    while (s && *s && lines-- > 0) {
        int n = (int)strlen(s);
        if (n > MSG_COLS) {
            int k = MSG_COLS;
            while (k > 0 && s[k] != ' ') k--;
            n = k ? k : MSG_COLS;
        }
        centre(y, s, n, shade, 1);
        s += n;
        while (*s == ' ') s++;
        y += 10;
    }
    return y;
}

static int bar = -1;                // loading: a bar this many percent full (-1: none)

static void message(const char *head, const char *l1, const char *l2) {
    memset(gbplay_shades, 3, sizeof gbplay_shades);
    centre(6, "GAME BOY", 8, 0, 2);
    wrap(30, name, 1, 3);
    if (head) centre(68, head, (int)strlen(head), 0, 1);
    int y = wrap(84, l1, 1, 2);
    wrap(y, l2, 1, 2);
    if (bar >= 0)                   // (the font has no %)
        for (int r = 84; r < 92; r++)
            for (int x = 28; x < 132; x++) {
                bool edge = r == 84 || r == 91 || x == 28 || x == 131;
                gbplay_shades[r * GBCV_W + x] = edge ? 1 : x - 30 < bar && r > 85 && r < 90 && x > 29 ? 0 : 3;
            }
    centre(130, "RESET: THE MENU", 15, 2, 1);
    gbcv_offer(&gbplay_enc, gbplay_shades, ++pic_index);
}

static void send_message_frame(void) {
    static const uint8_t no_psg[1];
    list_len = gbcv_tv_frame(&gbplay_enc, no_psg, 0, list);
    list_pending = true;
    publish();
}

// serve the console, a list a frame, for us microseconds (forever: 0)
static void serve_for(uint32_t us) {
    uint32_t t0 = time_us_32();
    while (!us || time_us_32() - t0 < us)
        if (new_frame()) send_message_frame();
}

static void __attribute__((noreturn)) fail(const char *why, const char *more, const char *menu_note) {
    message("IT CAN'T RUN", why, more);
    cvf_note(menu_note);
    serve_for(0);
    for (;;) tight_loop_contents();
}

static const char *fs_why(int r) {
    switch (r) {
    case FS_EIO:     return cvsd_card_in() ? "THE CARD DIDN'T ANSWER" : "NO CARD IN THE SLOT";
    case FS_ENOFS:   return "NO FAT OR EXFAT ON THE CARD";
    case FS_ENOFILE: return "THE FILE IS EMPTY OR GONE";
    case FS_ECHAIN:  return "ITS CLUSTERS ARE BROKEN";
    case FS_EFRAG:   return "IT'S IN TOO MANY PIECES";
    default:         return "THE CARD'S FILE SYSTEM IS ODD";
    }
}

// ---- the ROM ------------------------------------------------------------------------

static const uint8_t *rom;
static bool colour_game;

// the file into SRAM (dst) or the PSRAM (dst NULL), a sector between TV frames
static void load_rom(uint8_t *dst) {
    static uint32_t sec[128];
    uint32_t n = (uint32_t)file.size, last_pc = 101;
    for (uint32_t at = 0; at < n; at += 512) {
        uint32_t lba = fs_lba(&fs, at / 512);
        if (!lba || !cvsd_read(lba, (uint8_t *)sec)) fail("A CARD READ FAILED", NULL, "GB: a card read failed");
        if (dst) memcpy(dst + at, sec, 512);   // (the buffer is a power of two, 32K at least)
        else {
            volatile uint32_t *d = CVPSRAM_UNCACHED + at / 4;
            for (int i = 0; i < 128; i++) d[i] = sec[i];
        }
        uint32_t pc = (uint32_t)((uint64_t)at * 100 / n);
        if (pc / 5 != last_pc / 5) {
            last_pc = pc;
            bar = (int)pc;
            message("LOADING", NULL, NULL);
        }
        if (new_frame()) send_message_frame();
    }
    if (!dst) xip_cache_invalidate_all();
    bar = -1;
}

// ---- saves (sdplayer.c's Game Boy saves, on the plugin's card) ----------------------
#define SAV_QUIET_US   1000000      // no changes for this long: written
#define SAV_EVERY_US   10000000     // ...or changing for this long
#define SAV_CLOCK_US   60000000     // the clock, alone
#define SAV_RETRY_US   5000000      // after a write failed
#define COL_BUSY 10                 // TMS9918: dark yellow
#define COL_SAVED 3                 // light green
#define COL_LOADED 7                // cyan
#define COL_FAIL 8                  // medium red

static uint16_t rom_nm[FS_LNAME], sav_nm[FS_LNAME], st_nm[FS_LNAME];
static int rom_nm_n, sav_n, st_n;  // 0: no name (unreadable, or too long)
static uint32_t sav_size, sav_ram;  // gbplay_sav_size
static fs_file_t sav_file;
static uint32_t card_gen = 1;       // cards mounted so far (the .sav is looked for again on a new one)
static uint32_t sav_gen;            // card_gen sav_file is from (0: none)
static uint64_t sav_pending;        // sectors changed, not written yet
static bool sav_clock_due;
static uint32_t sav_seen, sav_change_t, sav_dirty_t, sav_clock_t, sav_retry_t;
static uint32_t sav_writes, sav_fails, st_saves, st_loads, st_fails;
static int gb_req;                  // a state request, done once the console shows COL_BUSY
static uint32_t gb_req_frame;
static char sav_msg[80] = "";
static uint8_t sav_buf[512] __attribute__((aligned(4)));

// the card: the one the game came from, mounted (card_watch)
static bool card_here = true, card_tried;
static uint32_t card_in_t;

static int with_ext(uint16_t *out, const uint16_t *nm, int stem, const char *ext) {
    size_t e = strlen(ext);
    if (stem + (int)e > 255) return 0;
    memcpy(out, nm, (size_t)stem * 2);
    for (size_t i = 0; i < e; i++) out[stem + (int)i] = (uint16_t)ext[i];
    return stem + (int)e;
}

// the names, and the .sav into the cart RAM (before the first frame)
static void saves_start(void) {
    sav_n = st_n = 0;
    sav_gen = 0;
    sav_pending = 0;
    sav_clock_due = false;
    sav_seen = gbplay_sav_changes();
    sav_dirty_t = sav_retry_t = 0;
    sav_clock_t = time_us_32();
    gb_req = 0;
    sav_size = gbplay_sav_size(rom, &sav_ram);
    int n = rom_nm_n = fs_name16(&fs, gb_dir, file.first_clus, rom_nm, FS_LNAME - 1);
    if (n <= 0) { rom_nm_n = 0; snprintf(sav_msg, sizeof sav_msg, "no saves: the ROM's name is unreadable (%d)", n); return; }
    int stem = n;
    for (int i = 0; i < n; i++) if (rom_nm[i] == '.') stem = i;
    sav_n = with_ext(sav_nm, rom_nm, stem, ".sav");
    st_n = with_ext(st_nm, rom_nm, stem, ".state");
    if (!sav_size) { snprintf(sav_msg, sizeof sav_msg, "the cart keeps no save"); return; }
    fs_file_t f;
    bool made;
    int r = fs_create(&fs, gb_dir, sav_nm, sav_n, 0, &f, &made);
    if (r == FS_ENOFILE) { snprintf(sav_msg, sizeof sav_msg, "no .sav yet (%lu bytes when made)", (unsigned long)sav_size); return; }
    if (r || f.size < sav_ram) {    // never written over: it isn't this game's
        if (r) snprintf(sav_msg, sizeof sav_msg, "the .sav can't be read (%d): not used", r);
        else snprintf(sav_msg, sizeof sav_msg, "the .sav is too small (%lu bytes): not used", (unsigned long)f.size);
        sav_n = 0;
        return;
    }
    if (fs_open(&fs, &f)) { snprintf(sav_msg, sizeof sav_msg, "the .sav can't be opened: not used"); sav_n = 0; return; }
    uint32_t n_sec = (uint32_t)((f.size < sav_size ? f.size : sav_size) + 511) / 512;
    for (uint32_t k = 0; k < n_sec; k++) {
        uint32_t lba = fs_lba(&fs, k);
        if (!lba || !cvsd_read(lba, sav_buf)) { snprintf(sav_msg, sizeof sav_msg, "the .sav didn't read: not used"); sav_n = 0; return; }
        gbplay_sav_put(k, sav_buf, (uint32_t)f.size);
    }
    sav_file = f;
    sav_gen = card_gen;
    snprintf(sav_msg, sizeof sav_msg, "%lu-byte .sav loaded", (unsigned long)f.size);
}

// the .sav, opened for writing: found again on a new card, or made
static bool sav_open(void) {
    if (sav_gen != card_gen) {
        fs_file_t f;
        bool made;
        int r = fs_create(&fs, gb_dir, sav_nm, sav_n, sav_size, &f, &made);
        if (r || f.size < sav_ram) {
            snprintf(sav_msg, sizeof sav_msg, r ? "the .sav can't be made (%d)" : "the .sav on the card is too small", r);
            if (!r) sav_n = 0;      // someone else's: left alone
            return false;
        }
        if (made) {                 // all of it: the card held anything there
            sav_pending = sav_ram ? ~0ull >> (64 - sav_ram / 512) : 0;
            sav_clock_due = true;
            snprintf(sav_msg, sizeof sav_msg, ".sav made (%lu bytes)", (unsigned long)f.size);
        }
        sav_file = f;
        sav_gen = card_gen;
    }
    return fs_open(&fs, &sav_file) == FS_OK;
}

// write what changed (and the clock, if due); true when nothing is left
static bool sav_flush(void) {
    sav_pending |= gbplay_sav_dirty();
    if (!sav_n || !sav_size || (!sav_pending && !sav_clock_due)) return true;
    if (!card_here) return false;
    if (!sav_open()) { sav_fails++; return false; }
    bool clock = sav_size > sav_ram;
    for (uint32_t k = 0; k < 64; k++) {
        if (!(sav_pending >> k & 1)) continue;
        gbplay_sav_get(k, sav_buf);
        if (fs_write_sec(&fs, k, sav_buf)) { sav_fails++; snprintf(sav_msg, sizeof sav_msg, "the .sav didn't write"); return false; }
        sav_pending &= ~(1ull << k);
        feed();
    }
    if (clock && sav_file.size >= sav_ram + 44) {   // the clock goes with every write
        gbplay_sav_get(sav_ram / 512, sav_buf);
        if (fs_write_sec(&fs, sav_ram / 512, sav_buf)) { sav_fails++; return false; }
        sav_clock_t = time_us_32();
    }
    sav_clock_due = false;
    sav_writes++;
    return true;
}

// each pass while the game runs: the .sav when it's due
static void saves_service(uint32_t now) {
    uint32_t c = gbplay_sav_changes();
    if (c != sav_seen) {
        sav_seen = c;
        sav_change_t = now;
        if (!sav_dirty_t) sav_dirty_t = now | 1;
    }
    if (sav_size > sav_ram && now - sav_clock_t > SAV_CLOCK_US) { sav_clock_due = true; sav_clock_t = now; }
    bool due = sav_clock_due || (sav_dirty_t && (now - sav_change_t > SAV_QUIET_US || now - sav_dirty_t > SAV_EVERY_US));
    if (!due || (sav_retry_t && (int32_t)(now - sav_retry_t) < 0)) return;
    if (sav_flush()) sav_dirty_t = sav_retry_t = 0;
    else sav_retry_t = (now + SAV_RETRY_US) | 1;
}

static bool state_file(bool make, fs_file_t *f, bool *made) {
    if (!st_n) { snprintf(sav_msg, sizeof sav_msg, "no states: the ROM's name is unreadable"); return false; }
    if (!card_here) { snprintf(sav_msg, sizeof sav_msg, "no states: the game's card isn't in"); return false; }
    int r = fs_create(&fs, gb_dir, st_nm, st_n, make ? GBPLAY_STATE_FILE : 0, f, made);
    if (r) { snprintf(sav_msg, sizeof sav_msg, r == FS_ENOFILE ? "no .state to load" : "the .state can't be %s (%d)", make ? "made" : "found", r); return false; }
    if (f->size < gbplay_state_sectors() * 512u) { snprintf(sav_msg, sizeof sav_msg, "the .state on the card is too small"); return false; }
    return fs_open(&fs, f) == FS_OK;
}

static bool state_save(void) {
    fs_file_t f;
    bool made;
    if (!state_file(true, &f, &made)) return false;
    uint32_t lba;
    if (!made) {                    // someone else's file of that name: left alone
        if (!(lba = fs_lba(&fs, 0)) || !cvsd_read(lba, sav_buf)) return false;
        if (gbplay_state_check(0, sav_buf) == GBPLAY_ST_NONE) { snprintf(sav_msg, sizeof sav_msg, "the .state isn't one of ours: left alone"); return false; }
    }
    uint32_t n = gbplay_state_sectors();
    for (uint32_t k = 0; k < n; k++) {   // (sector 0 first: it sums the rest)
        gbplay_state_get(k, sav_buf);
        if (fs_write_sec(&fs, k, sav_buf)) { snprintf(sav_msg, sizeof sav_msg, "the .state didn't write"); return false; }
        feed();
    }
    snprintf(sav_msg, sizeof sav_msg, "state saved (%lu sectors)", (unsigned long)n);
    return true;
}

static bool state_load(void) {
    fs_file_t f;
    bool made;
    if (!state_file(false, &f, &made)) return false;
    uint32_t n = 0, lba;
    int r = 1;
    for (; r == 1 && n * 512 < f.size; n++) {   // all of it, checked, before any goes in
        if (!(lba = fs_lba(&fs, n)) || !cvsd_read(lba, sav_buf)) { snprintf(sav_msg, sizeof sav_msg, "the .state didn't read"); return false; }
        r = gbplay_state_check(n, sav_buf);
        feed();
    }
    if (r) {
        static const char *const why[] = {"", "isn't a state", "is from another firmware", "is another game's", "is damaged"};
        snprintf(sav_msg, sizeof sav_msg, "the .state %s", r < 0 && r >= -4 ? why[-r] : "is short");
        return false;
    }
    for (int tries = 0; tries < 3; tries++) {
        bool read = true;
        for (uint32_t k = 0; k < n && read; k++) {
            read = (lba = fs_lba(&fs, k)) && cvsd_read(lba, sav_buf);
            if (read) gbplay_state_put(k, sav_buf);
            feed();
        }
        if (read && gbplay_state_done()) {
            snprintf(sav_msg, sizeof sav_msg, "state loaded");
            return true;
        }
    }
    snprintf(sav_msg, sizeof sav_msg, "the .state read differently the second time: the game may misbehave (RESET)");
    return false;
}

// a request from the keypad or a pad: the border turns COL_BUSY, and once
// the console shows it (two TV frames on) the state is saved or loaded, the
// picture standing still meanwhile
static void requests(uint32_t frames) {
    if (!gb_req) {
        int r = gbplay_request();
        if (!r) return;
        gb_req = r;
        gb_req_frame = frames + 2;
        gbplay_border(COL_BUSY, 600);
        return;
    }
    if ((int32_t)(frames - gb_req_frame) < 0) return;
    bool ok = gb_req == GBPLAY_SAVE ? state_save() : state_load();
    if (gb_req == GBPLAY_SAVE) ok ? st_saves++ : st_fails++;
    else ok ? st_loads++ : st_fails++;
    gbplay_border(ok ? (gb_req == GBPLAY_SAVE ? COL_SAVED : COL_LOADED) : COL_FAIL, ok ? 45 : 120);
    gb_req = 0;
}

// the card out and back: mounted again, and written to only if it's the one
// the game came from (the ROM's name at its first cluster in its folder)
static void card_watch(uint32_t now) {
    if (!cvsd_card_in()) {
        card_here = card_tried = false;
        card_in_t = 0;
        return;
    }
    if (card_here || card_tried) return;
    if (!card_in_t) { card_in_t = now | 1; return; }
    if (now - card_in_t < CARD_SETTLE_US) return;
    card_tried = true;              // once a time it goes in
    if (!cvsd_init() || fs_mount(&fs, cvsd_read) != FS_OK) { snprintf(sav_msg, sizeof sav_msg, "the card put back didn't mount"); return; }
    fs_set_writer(&fs, cvsd_write);
    static uint16_t nm[FS_LNAME];
    int n = rom_nm_n ? fs_name16(&fs, gb_dir, file.first_clus, nm, FS_LNAME - 1) : -1;
    if (n != rom_nm_n || memcmp(nm, rom_nm, (size_t)n * 2)) {
        snprintf(sav_msg, sizeof sav_msg, "not the game's card: no saves to it");
        return;
    }
    card_here = true;
    card_gen++;
    snprintf(sav_msg, sizeof sav_msg, "the game's card is back");
}

// ---- the game -----------------------------------------------------------------------

static bool playing;
static uint32_t f0;                 // cvf_frames() as the Game Boy started
static uint32_t last_usb;           // pad 0's buttons, for the presses
static uint32_t late, us_max, pics_at_note;

static uint32_t us_clock(void) { return time_us_32(); }

// the look after (step 1) or before (-1) this one: a Color look only for a Color game
static int look_step(int step) {
    int k = gbplay_stats.look;
    for (int i = 0; i < gbcv_nlooks; i++) {
        k = (k + step + gbcv_nlooks) % gbcv_nlooks;
        if (colour_game || !gbcv_looks[k].colour) return k;
    }
    return gbplay_stats.look;
}

static uint8_t gb_from_usb(const gamepad_t *g) {
    const uint32_t b = g->buttons, d = cvpad_dirs(g);
    uint8_t h = 0;
    if (d & GP_UP) h |= GBPLAY_UP;
    if (d & GP_DOWN) h |= GBPLAY_DOWN;
    if (d & GP_LEFT) h |= GBPLAY_LEFT;
    if (d & GP_RIGHT) h |= GBPLAY_RIGHT;
    if (b & GP_EAST) h |= GBPLAY_A;
    if (b & GP_SOUTH) h |= GBPLAY_B;
    if (b & GP_MINUS) h |= GBPLAY_SELECT;
    if (b & GP_PLUS) h |= GBPLAY_START;
    return h;
}

static void game_frame(uint32_t f) {
    uint32_t t0 = time_us_32();
    uint8_t joy, fire, key;
    cvf_input(&joy, &fire, &key);
    uint8_t held = 0;
    gamepad_t g;
    if (cvpad_get(0, &g)) {
        held |= gb_from_usb(&g);
        const uint32_t down = g.buttons & ~last_usb;
        if (down & GP_L) gbplay_look(look_step(-1));
        if (down & GP_R) gbplay_look(look_step(1));
        // ZL / ZR held: as keypad 7 / 9 held (a second: save / load a state)
        if (key == 0xF && (g.buttons & GP_ZL)) key = 0x5;
        else if (key == 0xF && (g.buttons & GP_ZR)) key = 0xB;
        last_usb = g.buttons;
    } else last_usb = 0;
    if (cvpad_get(1, &g)) held |= gb_from_usb(&g);
    gbplay_buttons(held);
    list_len = gbplay_frame(f - f0, joy, fire, key, list);
    list_pending = true;
    publish();
    uint32_t us = time_us_32() - t0;
    if (us > us_max) us_max = us;
}

// on the way back to the menu (cvf_on_exit): the game's last changes
static void going_back(void) {
    if (!playing) return;
    bool ok = sav_flush();
    if (!ok || sav_fails || st_fails) {
        char s[33];
        snprintf(s, sizeof s, "GB: %.28s", !ok && !card_here ? "card out: the .sav not saved" : sav_msg);
        cvf_note(s);
    }
}

// the menu's note, should RESET come (pictures a second, TV frames the cart
// was late for, its slowest frame); and in the return block now, where the
// Game Boy is, should the watchdog reboot into the menu
static void update_note(void) {
    static uint32_t t_last, late_last;
    uint32_t t = time_us_32();
    if (t - t_last < NOTE_US) return;
    uint32_t pc_bank, regs;
    gbplay_where(&pc_bank, &regs);
    (void)regs;
    snprintf(note, sizeof note, "GB stuck: bank %lu pc %04lX", (unsigned long)(pc_bank >> 16 & 0x1FF),
             (unsigned long)(pc_bank & 0xFFFF));
    cvf_note(note);
    cvf_note_now();
    uint32_t pics = gbplay_stats.pictures;
    unsigned per_s = MIN(pics - pics_at_note, 999u), dl = MIN(late - late_last, 999u),
             ms = MIN(us_max / 1000, 99u), tenths = us_max / 100 % 10;
    snprintf(note, sizeof note, "GB %u/s %u late %u.%ums%s", per_s, dl, ms, tenths,
             sav_writes ? " saved" : "");
    cvf_note(note);
    t_last = t;
    late_last = late;
    pics_at_note = pics;
    us_max = 0;
}

int main(void) {
    static const cvf_config_t cfg = {gbkernel_bin, GBKERNEL_BIN_LEN, handoff_bin, HANDOFF_BIN_LEN};
    cvf_start(&cfg);
    cvpad_init();                   // the pads find their way in while the game loads
    gbcv_init(&gbplay_enc, &gbcv_looks[GBCV_LOOK_GREY]);   // the messages', then the game's (gbplay_keep_enc)
    gbplay_keep_enc = true;
    free_at = __bss_end__;

    const cvl_t *b = cvf_launch();
    if (b && b->path[0]) {          // (the block becomes a return block below: what's needed, copied)
        memcpy(path, b->path, sizeof path);
        const char *slash = strrchr(path, '/');
        snprintf(name, sizeof name, "%s", slash ? slash + 1 : path);
        char *dot = strrchr(name, '.');   // the title without .gb / .gbc
        if (dot && dot != name) *dot = 0;
        size_t d = slash ? (size_t)(slash - path) : 0;
        memcpy(gb_dir, path, d);
        gb_dir[d] = 0;
    }
    message(path[0] ? "STARTING" : NULL, path[0] ? NULL : "OPEN A .GB OR .GBC FILE FROM THE MENU", NULL);
    serve_for(OPEN_US);
    if (!path[0]) fail("OPEN A .GB OR .GBC FILE FROM THE MENU", NULL, NULL);

    int r = cvsd_open_launched(&fs, b, true, &file);   // (blocks up to 1.5 s: the last list runs again meanwhile)
    if (r != FS_OK) fail(fs_why(r), NULL, "GB: the file couldn't be read");
    uint32_t lba = fs_lba(&fs, 0);
    static uint8_t hdr[512] __attribute__((aligned(4)));
    if (!lba || !cvsd_read(lba, hdr)) fail("A CARD READ FAILED", NULL, "GB: a card read failed");
    const char *why = "";
    if (file.size < 0x150 || !gbplay_check(hdr, (uint32_t)file.size, &why)) {
        char n1[33];
        snprintf(n1, sizeof n1, "GB: %.28s", why);
        fail(why, NULL, n1);
    }
    void *gb_mem = take((uint32_t)gbplay_gb_size());
    if (!gb_mem) fail("NO ROOM FOR THE GAME BOY", NULL, "GB: no room for its state");
    uint32_t pow2 = 0x8000;
    while (pow2 < file.size) pow2 <<= 1;
    uint8_t *sram = take(pow2);
    if (!sram && !cvpsram_init()) fail("THE PSRAM DIDN'T ANSWER", "(THE ROM NEEDS IT)", "GB: the PSRAM didn't answer");
    watchdog_enable(WATCHDOG_MS, true);
    load_rom(sram);
    rom = sram ? sram : CVPSRAM_CACHED;
    colour_game = gbplay_colour(rom);

    message("STARTING", NULL, NULL);
    if (!gbplay_init(rom, (uint32_t)file.size, cart_ram, -1, gb_mem)) fail("PEANUT-GB REFUSED IT", NULL, "GB: Peanut-GB refused it");
    gbplay_clock = us_clock;
    saves_start();
    snprintf(note, sizeof note, "GB: %.28s", sav_msg);
    cvf_note(note);
    cvf_on_exit(going_back);
    playing = true;
    f0 = cvf_frames();

    for (;;) {
        uint32_t before = seen_frames;
        if (!new_frame()) continue;
        if (before && seen_frames - before > 1) late += seen_frames - before - 1;
        game_frame(seen_frames);
        uint32_t now = time_us_32();
        requests(seen_frames);
        saves_service(now);
        card_watch(now);
        update_note();
    }
}
