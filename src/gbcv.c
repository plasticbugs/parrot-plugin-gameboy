// gbcv.c: see gbcv.h. Each part names the function in coleco/gbcv/src it
// ports; keep them in step (test/run.sh compares the lists).
#include <math.h>
#include <string.h>

#include "gbcv.h"

const gbcv_look_t gbcv_looks[] = {
    {"pea", "Pea soup", {11, 3, 12, 1}, 1, false},
    {"grey", "Grey", {15, 14, 4, 1}, 1, false},
    {"sky", "Sky", {15, 7, 4, 1}, 4, false},
    {"sunset", "Sunset", {11, 9, 6, 1}, 6, false},
    {"colour", "Colour", {15, 14, 4, 1}, 1, true},
};
const int gbcv_nlooks = sizeof gbcv_looks / sizeof gbcv_looks[0];

// ---- cv.rs: cells(), row() -----------------------------------------------------

// TMS9918 colours as cvengine::video::PALETTE has them (0 = transparent, as black)
static const uint8_t RGB[16][3] = {
    {0, 0, 0},       {0, 0, 0},       {33, 200, 66},   {94, 220, 120},
    {84, 85, 237},   {125, 118, 252}, {212, 82, 77},   {66, 235, 245},
    {252, 85, 84},   {255, 121, 120}, {212, 193, 84},  {230, 206, 128},
    {33, 176, 59},   {201, 91, 186},  {204, 204, 204}, {255, 255, 255},
};

static uint32_t DIST[16][16];   // luma-weighted: 3 dr^2 + 6 dg^2 + db^2

static void dist_init(void) {
    static bool done;
    if (done) return;
    for (int a = 0; a < 16; a++)
        for (int b = 0; b < 16; b++) {
            int32_t r = RGB[a][0] - RGB[b][0], g = RGB[a][1] - RGB[b][1], u = RGB[a][2] - RGB[b][2];
            DIST[a][b] = (uint32_t)(3 * r * r + 6 * g * g + u * u);
        }
    done = true;
}

#ifdef GBCV_TEST_ROWS
// cv.rs row(), pixel by pixel: the reference row_planes is tested against.
// One 8-pixel row in two colours; a row with three or four keeps the pair
// nearest overall. Returns 1 if it had to (a clash).
static int row(const uint8_t *col, uint8_t *pat, uint8_t *colour) {
    uint8_t uniq[8];
    int nu = 0;
    for (int i = 0; i < 8; i++) {
        int k = 0;
        while (k < nu && uniq[k] != col[i]) k++;
        if (k == nu) uniq[nu++] = col[i];
    }
    uint8_t fg, bg;
    int clash = 0;
    if (nu == 1) {
        fg = bg = uniq[0];
    } else if (nu == 2) {
        fg = uniq[0] > uniq[1] ? uniq[0] : uniq[1];
        bg = uniq[0] > uniq[1] ? uniq[1] : uniq[0];
    } else {
        uint32_t best = UINT32_MAX;
        fg = bg = 0;
        for (int i = 0; i < nu; i++)
            for (int j = i + 1; j < nu; j++) {
                uint8_t a = uniq[i], b = uniq[j];
                uint32_t e = 0;
                for (int p = 0; p < 8; p++) {
                    uint32_t da = DIST[col[p]][a], db = DIST[col[p]][b];
                    e += da < db ? da : db;
                }
                if (e < best) {
                    best = e;
                    fg = a > b ? a : b;
                    bg = a > b ? b : a;
                }
            }
        clash = 1;
    }
    uint8_t p = 0;
    if (fg != bg)
        for (int i = 0; i < 8; i++)
            if (DIST[col[i]][fg] < DIST[col[i]][bg]) p |= 0x80 >> i;
    *pat = p;
    *colour = (uint8_t)(fg << 4 | bg);
    return clash;
}
#endif

// row() for a row given as bit planes: the same choice, made per colour
// rather than per pixel. sc = the look's colour for each shade.
// - the colours are taken in the order they first appear (pixel 0 = bit 7;
//   the masks are disjoint, so the larger mask has the earlier pixel), as
//   row() meets them, so ties go the same way;
// - a pair's error is each colour's pixel count times its distance to the
//   nearer of the pair.
// The pair for a row of nu colours ec[] (in the order row() meets them)
// covering the pixels in em[]: row_planes' and row_cols' choice.
GBCV_HOT static inline int pair(const uint8_t *ec, const uint8_t *em, int nu, uint8_t *pat, uint8_t *colour) {
    uint8_t fg, bg;
    int clash = 0;
    if (nu == 1) {
        fg = bg = ec[0];
    } else if (nu == 2) {
        fg = ec[0] > ec[1] ? ec[0] : ec[1];
        bg = ec[0] > ec[1] ? ec[1] : ec[0];
    } else {
        uint32_t cnt[8], best = UINT32_MAX;
        for (int k = 0; k < nu; k++) cnt[k] = (uint32_t)__builtin_popcount(em[k]);
        fg = bg = 0;
        for (int i = 0; i < nu; i++)
            for (int j = i + 1; j < nu; j++) {
                uint8_t a = ec[i], b = ec[j];
                uint32_t e = 0;
                for (int k = 0; k < nu; k++) {
                    uint32_t da = DIST[ec[k]][a], db = DIST[ec[k]][b];
                    e += cnt[k] * (da < db ? da : db);
                }
                if (e < best) {
                    best = e;
                    fg = a > b ? a : b;
                    bg = a > b ? b : a;
                }
            }
        clash = 1;
    }
    uint8_t p = 0;
    if (fg != bg)
        for (int k = 0; k < nu; k++)
            if (DIST[ec[k]][fg] < DIST[ec[k]][bg]) p |= em[k];
    *pat = p;
    *colour = (uint8_t)(fg << 4 | bg);
    return clash;
}

GBCV_HOT static int row_planes(const uint8_t *sc, uint8_t lo, uint8_t hi, uint8_t *pat, uint8_t *colour) {
    const uint8_t m[4] = {(uint8_t)~(lo | hi), (uint8_t)(lo & ~hi), (uint8_t)(~lo & hi), (uint8_t)(lo & hi)};
    uint8_t ec[4], em[4];
    int nu = 0;
    for (int s = 0; s < 4; s++) {
        if (!m[s]) continue;
        int k = 0;
        while (k < nu && ec[k] != sc[s]) k++;
        if (k == nu) { ec[nu] = sc[s]; em[nu] = m[s]; nu++; }
        else em[k] |= m[s];
    }
    for (int i = 1; i < nu; i++)
        for (int j = i; j > 0 && em[j] > em[j - 1]; j--) {
            uint8_t t = em[j]; em[j] = em[j - 1]; em[j - 1] = t;
            t = ec[j]; ec[j] = ec[j - 1]; ec[j - 1] = t;
        }
    return pair(ec, em, nu, pat, colour);
}

// row() for a row of eight colours (a Color game's): the same choice
GBCV_HOT static int row_cols(const uint8_t *px, uint8_t *pat, uint8_t *colour) {
    uint8_t ec[8], em[8];
    int nu = 0;
    for (int i = 0; i < 8; i++) {
        uint8_t c = px[i];
        int k = 0;
        while (k < nu && ec[k] != c) k++;
        if (k == nu) { ec[nu] = c; em[nu] = 0; nu++; }
        em[k] |= (uint8_t)(0x80 >> i);
    }
    return pair(ec, em, nu, pat, colour);
}

// ---- Game Boy Color colours ----------------------------------------------------
// Nearest in OKLab (Björn Ottosson's perceptual space), where equal steps
// look equal, so a colour goes to the TMS9918 colour that looks most like it.

static float LIN[32];          // a 5-bit sRGB level, linear
static float LAB[16][3];       // the TMS9918 colours in OKLab

static void oklab(float r, float g, float b, float *o) {
    float l = cbrtf(0.4122214708f * r + 0.5363325363f * g + 0.0514459929f * b);
    float m = cbrtf(0.2119034982f * r + 0.6806995451f * g + 0.1073969566f * b);
    float s = cbrtf(0.0883024619f * r + 0.2817188376f * g + 0.6299787005f * b);
    o[0] = 0.2104542553f * l + 0.7936177850f * m - 0.0040720468f * s;
    o[1] = 1.9779984951f * l - 2.4285922050f * m + 0.4505937099f * s;
    o[2] = 0.0259040371f * l + 0.7827717662f * m - 0.8086757660f * s;
}

static float srgb_lin(float v) { return v <= 0.04045f ? v / 12.92f : powf((v + 0.055f) / 1.055f, 2.4f); }

static void lab_init(void) {
    static bool done;
    if (done) return;
    for (int i = 0; i < 32; i++) LIN[i] = srgb_lin(i / 31.0f);
    for (int c = 1; c < 16; c++)
        oklab(srgb_lin(RGB[c][0] / 255.0f), srgb_lin(RGB[c][1] / 255.0f), srgb_lin(RGB[c][2] / 255.0f), LAB[c]);
    done = true;
}

uint8_t gbcv_map_rgb(const gbcv_look_t *look, uint16_t rgb) {
    lab_init();
    float o[3];
    oklab(LIN[rgb >> 10 & 31], LIN[rgb >> 5 & 31], LIN[rgb & 31], o);
    if (!look->colour) {
        int shade = 3 - (int)(o[0] * 4.0f);   // OKLab L: 0 black .. 1 white
        return look->shades[shade < 0 ? 0 : shade > 3 ? 3 : shade];
    }
    uint8_t best = 1;
    float bd = 1e9f;
    for (int c = 1; c < 16; c++) {
        float d0 = o[0] - LAB[c][0], d1 = o[1] - LAB[c][1], d2 = o[2] - LAB[c][2];
        float d = d0 * d0 + d1 * d1 + d2 * d2;
        if (d < bd) { bd = d; best = (uint8_t)c; }
    }
    return best;
}

// bit 0 of each of 8 shade bytes (w0 = pixels 0-3, w1 = 4-7, little-endian)
// as one byte, pixel 0 in bit 7: the multiply moves byte i's bit to bit 31-i
static inline uint8_t gather(uint32_t w0, uint32_t w1) {
    return (uint8_t)(((w0 & 0x01010101u) * 0x80402010u) >> 28 << 4 | ((w1 & 0x01010101u) * 0x80402010u) >> 28);
}

#ifdef GBCV_TEST_ROWS
// test/rows_test.c: row_planes against row() for every row in every look
int gbcv_test_rows(void) {
    dist_init();
    int bad = 0;
    for (int l = 0; l < gbcv_nlooks; l++)
        for (uint32_t k = 0; k < 0x10000; k++) {
            uint8_t lo = (uint8_t)k, hi = (uint8_t)(k >> 8), col[8], p1, c1, p2, c2;
            uint8_t px[8];
            for (int i = 0; i < 8; i++) {
                px[i] = (uint8_t)((lo >> (7 - i) & 1) | (hi >> (7 - i) & 1) << 1);
                col[i] = gbcv_looks[l].shades[px[i]];
            }
            uint32_t w0, w1;
            memcpy(&w0, px, 4);
            memcpy(&w1, px + 4, 4);
            if (gather(w0, w1) != lo || gather(w0 >> 1, w1 >> 1) != hi) bad++;
            int x1 = row(col, &p1, &c1), x2 = row_planes(gbcv_looks[l].shades, lo, hi, &p2, &c2);
            if (x1 != x2 || p1 != p2 || c1 != c2) bad++;
        }
    return bad;
}

// test/rows_test.c: row_cols against row() for rows of up to eight colours
int gbcv_test_cols(int n) {
    dist_init();
    int bad = 0;
    uint32_t x = 12345;
    for (int t = 0; t < n; t++) {
        uint8_t set[8], col[8], p1, c1, p2, c2;
        x = x * 1664525u + 1013904223u;
        int k = 1 + (int)(x >> 29);               // draw from 1-8 colours
        for (int i = 0; i < k; i++) { x = x * 1664525u + 1013904223u; set[i] = (uint8_t)(1 + (x >> 24) % 15); }
        for (int i = 0; i < 8; i++) { x = x * 1664525u + 1013904223u; col[i] = set[(x >> 24) % k]; }
        int x1 = row(col, &p1, &c1), x2 = row_cols(col, &p2, &c2);
        if (x1 != x2 || p1 != p2 || c1 != c2) bad++;
    }
    return bad;
}
#endif

// ---- cv.rs: the pattern cache index (HashMap<Cell, u8> per third) ------------

static const uint8_t BORDER[16] = {0};  // slot 0: all zero, colour 0 shows the backdrop

GBCV_HOT static uint32_t hash(const uint8_t *k) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < 16; i++) h = (h ^ k[i]) * 16777619u;
    return h;
}

// At most 257 entries (a cell per slot, and slot 0's) in 512, so a probe
// always meets an empty entry. The probes are bounded all the same: a full
// table once looped forever here and locked the cart up (see start()).
GBCV_HOT static int idx_get(gbcv_enc_t *e, int t, const uint8_t *k) {
    gbcv_ent_t *tab = e->index[t];
    uint32_t i = hash(k) & (GBCV_HASH - 1);
    for (int n = 0; n < GBCV_HASH; n++, i = (i + 1) & (GBCV_HASH - 1)) {
        if (!tab[i].used) return -1;
        if (!memcmp(tab[i].key, k, 16)) return tab[i].slot;
    }
    return -1;
}

GBCV_HOT static void idx_put(gbcv_enc_t *e, int t, const uint8_t *k, uint8_t slot) {
    gbcv_ent_t *tab = e->index[t];
    uint32_t i = hash(k) & (GBCV_HASH - 1);
    int n = 0;
    while (tab[i].used && memcmp(tab[i].key, k, 16)) {
        i = (i + 1) & (GBCV_HASH - 1);
        if (++n == GBCV_HASH) {            // full (it can't be): start the index again
            memset(tab, 0, sizeof e->index[t]);
            e->stats.index_resets++;
            idx_put(e, t, BORDER, 0);
            idx_put(e, t, k, slot);
            return;
        }
    }
    memcpy(tab[i].key, k, 16);
    tab[i].slot = slot;
    tab[i].used = true;
}

// linear probing, deletion by shifting later entries back
GBCV_HOT static void idx_del(gbcv_enc_t *e, int t, const uint8_t *k) {
    gbcv_ent_t *tab = e->index[t];
    uint32_t i = hash(k) & (GBCV_HASH - 1);
    for (int n = 0; tab[i].used && memcmp(tab[i].key, k, 16); n++) {
        if (n == GBCV_HASH) return;
        i = (i + 1) & (GBCV_HASH - 1);
    }
    if (!tab[i].used) return;
    for (uint32_t j = i;;) {
        tab[i].used = false;
        for (;;) {
            j = (j + 1) & (GBCV_HASH - 1);
            if (!tab[j].used) return;
            uint32_t h = hash(tab[j].key) & (GBCV_HASH - 1);
            // move j back to i unless its home lies cyclically in (i, j]
            if (i <= j ? (i < h && h <= j) : (i < h || h <= j)) continue;
            break;
        }
        tab[i] = tab[j];
        i = j;
    }
}

// the index against VRAM: entries whose slot doesn't hold their cell (for
// tests, between pictures: an upload in progress makes a few)
int gbcv_check_index(const gbcv_enc_t *e) {
    int bad = 0;
    for (int t = 0; t < 3; t++)
        for (int i = 0; i < GBCV_HASH; i++) {
            const gbcv_ent_t *x = &e->index[t][i];
            if (x->used && memcmp(e->pat[t][x->slot], x->key, 16)) bad++;
        }
    return bad;
}

// ---- list.rs ------------------------------------------------------------------------

#define FRAME_T    (262 * 228)
#define VBLANK_T   (70 * 228)
#define C_START    480
#define C_RUN_VB   106
#define C_BYTE_VB  23
#define C_RUN_ACT  106
#define C_BYTE_ACT 31
#define C_PSG      75
#define C_PSG_BYTE 52
#define C_REG      114
#define C_END_VB   54
#define C_END      52
#define VB_LIMIT   (VBLANK_T - 600)
#define LIST_LIMIT (FRAME_T - 1200)

typedef struct {
    uint8_t *b;
    size_t n;
    uint32_t t, data;
    bool vblank;
} list_t;

static bool fits(const list_t *l, uint32_t runs, uint32_t bytes) {
    if (l->vblank) return l->t + runs * C_RUN_VB + bytes * C_BYTE_VB + C_END_VB <= VB_LIMIT;
    return l->t + runs * C_RUN_ACT + bytes * C_BYTE_ACT + C_END <= LIST_LIMIT;
}

static bool fits_active(const list_t *l, uint32_t runs, uint32_t bytes) {
    uint32_t end_vb = l->vblank ? C_END_VB : 0;
    return l->t + end_vb + runs * C_RUN_ACT + bytes * C_BYTE_ACT + C_END <= LIST_LIMIT;
}

static void l_run(list_t *l, uint16_t addr, const uint8_t *d, int n) {
    l->t += l->vblank ? C_RUN_VB + n * C_BYTE_VB : C_RUN_ACT + n * C_BYTE_ACT;
    l->data += n;
    l->b[l->n++] = 1;
    l->b[l->n++] = (uint8_t)addr;
    l->b[l->n++] = (uint8_t)((addr >> 8) & 0x3F) | 0x40;
    l->b[l->n++] = (uint8_t)n;
    memcpy(l->b + l->n, d, n);
    l->n += n;
}

static void l_psg(list_t *l, const uint8_t *d, int n) {
    while (n > 0) {
        int k = n > 255 ? 255 : n;
        l->t += C_PSG + C_PSG_BYTE * k;
        l->b[l->n++] = 2;
        l->b[l->n++] = (uint8_t)k;
        memcpy(l->b + l->n, d, k);
        l->n += k;
        d += k;
        n -= k;
    }
}

static void l_reg(list_t *l, uint8_t r, uint8_t v) {
    l->t += C_REG;
    l->b[l->n++] = 3;
    l->b[l->n++] = v;
    l->b[l->n++] = 0x80 | r;
}

static void l_end_vblank(list_t *l) {
    if (!l->vblank) return;
    l->vblank = false;
    l->t += C_END_VB;
    l->b[l->n++] = 0;
}

// ---- cv.rs: Console ---------------------------------------------------------------

#define NAMES    0x1800
#define PATTERNS 0x0000
#define COLOURS  0x2000

static int third(int r) { return (GBCV_ROW0 + r) / 8; }

void gbcv_set_look(gbcv_enc_t *e, const gbcv_look_t *look) {
    e->look = *look;
    for (int t = 0; t < 3; t++) {
        memset(e->age[t], 0, sizeof e->age[t]);
        memset(e->index[t], 0, sizeof e->index[t]);
        idx_put(e, t, BORDER, 0);
    }
    memset(e->names, 0, sizeof e->names);
    e->prev_ok = false;                    // new colours: every row again
    e->has_target = false;
    e->has_latest = false;
    e->shown_gb = 0;
}

void gbcv_init(gbcv_enc_t *e, const gbcv_look_t *look) {
    dist_init();
    memset(e, 0, sizeof *e);
    e->vram_r7 = -1;
    e->now = 1;
    gbcv_set_look(e, look);
}

// cv.rs: cells(). Each 8-pixel row becomes two bit planes; a row whose
// planes match the last picture's keeps its pattern and colour bytes.
GBCV_HOT void gbcv_offer(gbcv_enc_t *e, const uint8_t *shades, uint64_t gb_index) {
    const uint8_t *sc = e->look.shades;
    const bool ok = e->prev_ok && !e->prev_colours;
    uint32_t clash = 0;
    for (int r = 0; r < GBCV_ROWS; r++)
        for (int c = 0; c < GBCV_COLS; c++) {
            int i = r * GBCV_COLS + c;
            uint8_t *cell = e->latest[i];
            uint32_t *pv = e->prev[i];
            uint32_t cb = ok ? e->prev_clash[i] : 0;
            const uint8_t *px = shades + r * 8 * GBCV_W + c * 8;
            for (int y = 0; y < 8; y++, px += GBCV_W) {
                uint32_t w0, w1;
                memcpy(&w0, px, 4);
                memcpy(&w1, px + 4, 4);
                uint8_t lo = gather(w0, w1), hi = gather(w0 >> 1, w1 >> 1);
                uint16_t key = (uint16_t)(lo | hi << 8);
                if (ok && pv[y] == key) continue;
                pv[y] = key;
                uint32_t x = (uint32_t)row_planes(sc, lo, hi, &cell[y], &cell[8 + y]);
                cb = (cb & ~(1u << y)) | x << y;
            }
            e->prev_clash[i] = (uint8_t)cb;
            clash += (uint32_t)__builtin_popcount(cb);
        }
    e->prev_ok = true;
    e->prev_colours = false;
    e->stats.offered++;
    e->latest_clash = clash;
    e->latest_index = gb_index;
    e->has_latest = true;
}

// gbcv_offer for a picture already in TMS9918 colours (a Color game's).
// A row's key is its eight colours, a nibble each.
GBCV_HOT void gbcv_offer_colours(gbcv_enc_t *e, const uint8_t *colours, uint64_t gb_index) {
    const bool ok = e->prev_ok && e->prev_colours;
    uint32_t clash = 0;
    for (int r = 0; r < GBCV_ROWS; r++)
        for (int c = 0; c < GBCV_COLS; c++) {
            int i = r * GBCV_COLS + c;
            uint8_t *cell = e->latest[i];
            uint32_t *pv = e->prev[i];
            uint32_t cb = ok ? e->prev_clash[i] : 0;
            const uint8_t *px = colours + r * 8 * GBCV_W + c * 8;
            for (int y = 0; y < 8; y++, px += GBCV_W) {
                uint32_t w0, w1;
                memcpy(&w0, px, 4);
                memcpy(&w1, px + 4, 4);
                uint32_t key = (w0 & 0x0F0F0F0Fu) | (w1 & 0x0F0F0F0Fu) << 4;
                if (ok && pv[y] == key) continue;
                pv[y] = key;
                uint32_t x = (uint32_t)row_cols(px, &cell[y], &cell[8 + y]);
                cb = (cb & ~(1u << y)) | x << y;
            }
            e->prev_clash[i] = (uint8_t)cb;
            clash += (uint32_t)__builtin_popcount(cb);
        }
    e->prev_ok = true;
    e->prev_colours = true;
    e->stats.offered++;
    e->latest_clash = clash;
    e->latest_index = gb_index;
    e->has_latest = true;
}

GBCV_HOT static void start(gbcv_enc_t *e) {
    // slots on screen now must not change until the flip
    static bool busy[3][256], mine[3][256];
    memset(busy, 0, sizeof busy);
    memset(mine, 0, sizeof mine);
    for (int r = 0; r < GBCV_ROWS; r++)
        for (int c = 0; c < GBCV_COLS; c++)
            busy[third(r)][e->names[(GBCV_ROW0 + r) * 32 + GBCV_COL0 + c]] = true;
    for (int t = 0; t < 3; t++) busy[t][0] = true;
    e->up_head = e->up_tail = 0;
    for (int r = 0; r < GBCV_ROWS; r++) {
        int t = third(r);
        for (int c = 0; c < GBCV_COLS; c++) {
            int i = r * GBCV_COLS + c;
            const uint8_t *cell = e->latest[i];
            int s = idx_get(e, t, cell);
            if (s >= 0) {
                e->target_names[i] = (uint8_t)s;
                mine[t][s] = true;
                e->stats.hits++;
                continue;
            }
            // least recently shown slot that is neither on screen nor taken
            int pick = -1;
            for (int k = 1; k < 256; k++)
                if (!busy[t][k] && !mine[t][k] && (pick < 0 || e->age[t][k] < e->age[t][pick])) pick = k;
            if (pick < 0) {
                // out of free slots: overwrite one on screen (wrong until the
                // flip), but never one this picture has taken already. (Up to
                // 2026-11-12 this started from slot 1 even when it was taken:
                // two cells then shared a slot, the index kept a stale entry,
                // which showed wrong cells, and the stale entries piled up
                // until the index was full and idx_get looped forever.)
                e->stats.overflow++;
                for (int k = 1; k < 256; k++)    // a third has at most 160 cells: there is one
                    if (!mine[t][k] && (pick < 0 || e->age[t][k] < e->age[t][pick])) pick = k;
            }
            const uint8_t *old = e->pat[t][pick];
            if (idx_get(e, t, old) == pick) idx_del(e, t, old);
            idx_put(e, t, cell, (uint8_t)pick);
            mine[t][pick] = true;
            e->target_names[i] = (uint8_t)pick;
            gbcv_upload_t *u = &e->uploads[e->up_tail++];
            u->third = (uint8_t)t;
            u->slot = (uint8_t)pick;
            memcpy(u->cell, cell, 16);
        }
    }
    e->target_index = e->latest_index;
    e->target_clash = e->latest_clash;
    e->has_target = true;
}

GBCV_HOT size_t gbcv_tv_frame(gbcv_enc_t *e, const uint8_t *psg, int npsg, uint8_t *out) {
    e->now++;
    list_t l = {.b = out, .n = 0, .t = C_START, .data = 0, .vblank = true};

    if (e->vram_r7 != e->look.border) {
        l_reg(&l, 7, e->look.border);
        e->vram_r7 = e->look.border;
    }

    if (e->has_target && e->up_head == e->up_tail) {
        for (int r = 0; r < GBCV_ROWS; r++) {
            int at = (GBCV_ROW0 + r) * 32 + GBCV_COL0;
            memcpy(&e->names[at], &e->target_names[r * GBCV_COLS], GBCV_COLS);
            for (int c = 0; c < GBCV_COLS; c++) e->age[third(r)][e->target_names[r * GBCV_COLS + c]] = e->now;
        }
        e->has_target = false;
        e->shown_gb = e->target_index;
        e->stats.shown++;
        e->stats.clash_rows += e->target_clash;
    }
    // name rows that differ from VRAM: the flip, or a look change's clear
    for (int r = 0; r < GBCV_ROWS; r++) {
        int at = (GBCV_ROW0 + r) * 32 + GBCV_COL0;
        if (memcmp(&e->vram_names[at], &e->names[at], GBCV_COLS)) {
            l_run(&l, (uint16_t)(NAMES + at), &e->names[at], GBCV_COLS);
            memcpy(&e->vram_names[at], &e->names[at], GBCV_COLS);
        }
    }
    if (npsg > 0) l_psg(&l, psg, npsg);

    if (!e->has_target && e->has_latest) {
        e->has_latest = false;
        if (e->latest_index > e->shown_gb) start(e);
    }

    if (e->has_target) {
        while (e->up_head != e->up_tail) {
            if (!fits(&l, 2, 16)) {
                if (l.vblank && fits_active(&l, 2, 16)) l_end_vblank(&l);
                else break;
            }
            gbcv_upload_t *u = &e->uploads[e->up_head];
            uint16_t at = (uint16_t)(u->third * 0x800 + u->slot * 8);
            l_run(&l, PATTERNS + at, u->cell, 8);
            l_run(&l, COLOURS + at, u->cell + 8, 8);
            memcpy(e->pat[u->third][u->slot], u->cell, 16);
            e->up_head++;
            e->stats.uploads++;
        }
    }

    l_end_vblank(&l);
    l.t += C_END;
    l.b[l.n++] = 0;
    e->stats.tv_frames++;
    e->stats.t_used += l.t;
    return l.n;
}

// ---- psg.rs: Translator ------------------------------------------------------------

#define SEQ_CYCLES 8192   // 512 Hz

static const uint8_t ATT[16] = {15, 12, 9, 7, 6, 5, 4, 3, 3, 2, 2, 1, 1, 1, 0, 0};
static const uint8_t WAVE_VOL[4] = {0, 12, 6, 3};

static uint16_t period(uint16_t x, bool wave) {
    uint64_t q = 85352ull * (2048 - (x > 2047 ? 2047 : x)) * (wave ? 2 : 1);
    uint64_t d = 100000;
    while (q > 1023 * d) d *= 2;
    uint64_t n = (q + d / 2) / d;
    return (uint16_t)(n < 1 ? 1 : n > 1023 ? 1023 : n);
}

static uint8_t noise_rate(uint8_t nr43) {
    uint32_t r = nr43 & 7;
    uint32_t d = (r == 0 ? 1 : 2 * r) << (nr43 >> 4);
    return d <= 106 ? 0 : d <= 212 ? 1 : 2;
}

static void env_set(gbcv_env_t *v, uint8_t x) {
    v->initial = x >> 4;
    v->up = (x & 8) != 0;
    v->period = x & 7;
}
static void env_trigger(gbcv_env_t *v) {
    v->vol = v->initial;
    v->timer = v->period == 0 ? 8 : v->period;
}
static void env_clock(gbcv_env_t *v) {
    if (v->period == 0) return;
    if (v->timer) v->timer--;
    if (v->timer == 0) {
        v->timer = v->period;
        if (v->up && v->vol < 15) v->vol++;
        else if (!v->up && v->vol > 0) v->vol--;
    }
}

void gbcv_tr_init(gbcv_tr_t *t) {
    memset(t, 0, sizeof *t);
    t->nr51 = 0xFF;
    t->seq_at = SEQ_CYCLES;
    for (int i = 0; i < 3; i++) t->sent_period[i] = -1;
    for (int i = 0; i < 4; i++) t->sent_att[i] = -1;
    t->sent_noise = -1;
}

static void sequencer(gbcv_tr_t *t) {
    uint8_t step = t->seq_step;
    t->seq_step = (step + 1) & 7;
    if (step % 2 == 0)
        for (int i = 0; i < 4; i++) {
            gbcv_chan_t *c = &t->ch[i];
            if (c->len_on && c->len > 0) {
                c->len--;
                if (c->len == 0) c->on = false;
            }
        }
    if (step == 2 || step == 6) {
        gbcv_chan_t *c = &t->ch[0];
        if (c->sw_timer) c->sw_timer--;
        if (c->sw_timer == 0) {
            c->sw_timer = c->sw_period == 0 ? 8 : c->sw_period;
            if (c->sw_on && c->sw_period != 0) {
                uint16_t d = c->shadow >> c->sw_shift;
                uint16_t n = c->sw_neg ? (uint16_t)(c->shadow - d) : (uint16_t)(c->shadow + d);
                if (n > 2047) c->on = false;
                else if (c->sw_shift != 0) c->shadow = c->freq = n;
            }
        }
    }
    if (step == 7) {
        env_clock(&t->ch[0].env);
        env_clock(&t->ch[1].env);
        env_clock(&t->ch[3].env);
    }
}

GBCV_HOT static void reg_write(gbcv_tr_t *t, uint16_t a, uint8_t v) {
    if (a == 0xFF26) {
        bool on = (v & 0x80) != 0;
        if (!on) {
            memset(t->ch, 0, sizeof t->ch);
            t->nr51 = 0;
        }
        t->power = on;
        return;
    }
    if (!t->power || a < 0xFF10 || a > 0xFF25) return;
    int i, reg;
    if (a <= 0xFF14) i = 0, reg = a - 0xFF10;
    else if (a <= 0xFF19) i = 1, reg = a - 0xFF15;
    else if (a <= 0xFF1E) i = 2, reg = a - 0xFF1A;
    else if (a <= 0xFF23) i = 3, reg = a - 0xFF1F;
    else if (a == 0xFF25) { t->nr51 = v; return; }
    else return;
    gbcv_chan_t *c = &t->ch[i];
    switch (reg) {
    case 0:
        if (i == 0) {
            c->sw_period = (v >> 4) & 7;
            c->sw_neg = (v & 8) != 0;
            c->sw_shift = v & 7;
        } else if (i == 2) {
            c->dac = (v & 0x80) != 0;
            if (!c->dac) c->on = false;
        }
        break;
    case 1:
        c->len = i == 2 ? (uint16_t)(256 - v) : (uint16_t)(64 - (v & 63));
        break;
    case 2:
        if (i == 2) {
            c->level = (v >> 5) & 3;
        } else {
            env_set(&c->env, v);
            c->dac = (v & 0xF8) != 0;
            if (!c->dac) c->on = false;
        }
        break;
    case 3:
        if (i < 3) c->freq = (c->freq & 0x700) | v;
        else c->nr43 = v;
        break;
    case 4:
        if (i < 3) c->freq = (uint16_t)((c->freq & 0xFF) | ((v & 7) << 8));
        c->len_on = (v & 0x40) != 0;
        if (v & 0x80) {
            c->on = c->dac;
            if (c->len == 0) c->len = i == 2 ? 256 : 64;
            if (i != 2) env_trigger(&c->env);
            if (i == 0) {
                c->shadow = c->freq;
                c->sw_timer = c->sw_period == 0 ? 8 : c->sw_period;
                c->sw_on = c->sw_period != 0 || c->sw_shift != 0;
            }
        }
        break;
    }
}

void gbcv_tr_until(gbcv_tr_t *t, uint64_t cycle) {
    while (t->seq_at <= cycle) {
        sequencer(t);
        t->seq_at += SEQ_CYCLES;
    }
}

GBCV_HOT void gbcv_tr_write(gbcv_tr_t *t, uint64_t cycle, uint16_t addr, uint8_t v) {
    gbcv_tr_until(t, cycle);
    reg_write(t, addr, v);
}

static bool audible(const gbcv_tr_t *t, int i) {
    const gbcv_chan_t *c = &t->ch[i];
    return t->power && c->on && c->dac && ((t->nr51 >> i) & 0x11) != 0;
}

int gbcv_tr_psg(gbcv_tr_t *t, uint8_t *out) {
    int n = 0;
    for (int k = 0; k < 3; k++) {
        const gbcv_chan_t *c = &t->ch[k];
        uint8_t vol = !audible(t, k) ? 0 : k == 2 ? WAVE_VOL[c->level] : c->env.vol;
        uint8_t a = ATT[vol];
        if (a != 15) {
            uint16_t p = period(c->freq, k == 2);
            if (t->sent_period[k] != p) {
                out[n++] = (uint8_t)(0x80 | k << 5 | (p & 15));
                out[n++] = (uint8_t)((p >> 4) & 0x3F);
                t->sent_period[k] = p;
            }
        }
        if (t->sent_att[k] != a) {
            out[n++] = (uint8_t)(0x90 | k << 5 | a);
            t->sent_att[k] = a;
        }
    }
    const gbcv_chan_t *c = &t->ch[3];
    uint8_t a = audible(t, 3) ? ATT[c->env.vol] : 15;
    if (a != 15) {
        uint8_t ctl = 0xE4 | noise_rate(c->nr43);   // white noise
        if (t->sent_noise != ctl) {
            out[n++] = ctl;
            t->sent_noise = ctl;
        }
    }
    if (t->sent_att[3] != a) {
        out[n++] = 0xF0 | a;
        t->sent_att[3] = a;
    }
    return n;
}
