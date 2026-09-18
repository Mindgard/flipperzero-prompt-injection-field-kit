/*
 * Minimal QR Code encoder for Flipper Zero.
 *
 * QR Code Model 2, byte mode, ECC level L, versions 1–6.
 * Based on ISO/IEC 18004 with lookup tables for GF(256)
 * arithmetic and format/version info.
 *
 * Uses ECC-L (lowest error correction) to maximise data capacity
 * per version, keeping the module count small enough for 2× pixel
 * rendering on the Flipper’s 128×64 screen.
 *
 * This implementation is deliberately compact for embedded use.
 */

#include "qrcode.h"
#include <string.h>
#include <stdlib.h>

/* ── Version parameters (ECC level L) ───────────────────────── */

/* Data codeword capacity per version at ECC-L */
static const uint8_t VERSION_DATA_CODEWORDS[] = {
    0, /* placeholder for index 0 */
    19, /* V1: 26 total -  7 ecc = 19 data */
    34, /* V2: 44 total - 10 ecc = 34 data */
    55, /* V3: 70 total - 15 ecc = 55 data */
    80, /* V4: 100 total - 20 ecc = 80 data */
    108, /* V5: 134 total - 26 ecc = 108 data */
    136, /* V6: 172 total - 36 ecc = 136 data */
};

/* Total codewords per version */
static const uint8_t VERSION_TOTAL_CODEWORDS[] = {
    0,
    26,
    44,
    70,
    100,
    134,
    172,
};

/* ECC codewords per block at ECC-L */
static const uint8_t VERSION_ECC_PER_BLOCK[] = {
    0,
    7,
    10,
    15,
    20,
    26,
    18,
};

/* Number of ECC blocks at ECC-L */
static const uint8_t VERSION_NUM_BLOCKS[] = {
    0,
    1,
    1,
    1,
    1,
    1,
    2,
};

/* Byte-mode data capacity */
static const uint8_t VERSION_BYTE_CAPACITY[] = {
    0,
    17,
    32,
    53,
    78,
    106,
    134,
};

/* Alignment pattern center coordinate for V1-V6 (0-indexed: V1=index 0) */
static const uint8_t ALIGN_CENTER[] = {
    0, /* V1: none */
    18, /* V2 */
    22, /* V3 */
    26, /* V4 */
    30, /* V5 */
    34, /* V6 */
};

/* ── GF(256) arithmetic ───────────────────────────────────────── */

static uint8_t gf_exp[256];
static uint8_t gf_log[256];
static bool gf_init_done = false;

static void gf_init(void) {
    if(gf_init_done) return;
    int x = 1;
    for(int i = 0; i < 255; i++) {
        gf_exp[i] = (uint8_t)x;
        gf_log[x] = (uint8_t)i;
        x <<= 1;
        if(x & 0x100) x ^= 0x11D; /* primitive polynomial */
    }
    gf_exp[255] = gf_exp[0];
    gf_init_done = true;
}

static uint8_t gf_mul(uint8_t a, uint8_t b) {
    if(a == 0 || b == 0) return 0;
    return gf_exp[(gf_log[a] + gf_log[b]) % 255];
}

/* ── Reed-Solomon ECC generation ──────────────────────────────── */

static void rs_generate_ecc(const uint8_t* data, uint8_t data_len, uint8_t* ecc, uint8_t ecc_len) {
    /* Build generator polynomial (constant-term-first in gen[]) */
    uint8_t gen[65];
    memset(gen, 0, sizeof(gen));
    gen[0] = 1;

    for(uint8_t i = 0; i < ecc_len; i++) {
        for(int j = ecc_len; j >= 1; j--) {
            gen[j] = gen[j - 1] ^ gf_mul(gen[j], gf_exp[i]);
        }
        gen[0] = gf_mul(gen[0], gf_exp[i]);
    }

    /* Polynomial division (shift-register).
     * gen[] is constant-term-first, but the shift register needs
     * leading-term-first (skipping the monic leading coefficient),
     * so we index gen[ecc_len - 1 - j]. */
    uint8_t remainder[65];
    memset(remainder, 0, ecc_len);

    for(uint8_t i = 0; i < data_len; i++) {
        uint8_t coef = data[i] ^ remainder[0];
        memmove(remainder, remainder + 1, ecc_len - 1);
        remainder[ecc_len - 1] = 0;
        if(coef != 0) {
            for(uint8_t j = 0; j < ecc_len; j++) {
                remainder[j] ^= gf_mul(gen[ecc_len - 1 - j], coef);
            }
        }
    }

    memcpy(ecc, remainder, ecc_len);
}

/* ── Module grid helpers ──────────────────────────────────────── */

/*
 * Each cell in the work grid stores a combination of two flags:
 *   bit 0: colour (0 = white, 1 = black)
 *   bit 1: function-pattern flag (1 = function pattern, 0 = data)
 *
 * Values: 0 = data-white, 1 = data-black,
 *         2 = func-white,  3 = func-black
 */
#define IS_FUNC(cell) ((cell) & 2)

typedef struct {
    uint8_t grid[QR_MAX_MODULES][QR_MAX_MODULES];
    int size;
    int version;
} QrWork;

static inline void set_func_module(QrWork* w, int x, int y, bool black) {
    if(x >= 0 && x < w->size && y >= 0 && y < w->size)
        w->grid[y][x] = (uint8_t)(2 | (black ? 1 : 0));
}

/* ── Finder pattern ───────────────────────────────────────────── */

static void place_finder(QrWork* w, int ox, int oy) {
    /* 7x7 finder + 1-wide white separator */
    for(int dy = -1; dy <= 7; dy++) {
        for(int dx = -1; dx <= 7; dx++) {
            int x = ox + dx, y = oy + dy;
            if(x < 0 || x >= w->size || y < 0 || y >= w->size) continue;
            bool black;
            if(dx < 0 || dx > 6 || dy < 0 || dy > 6) {
                black = false; /* separator */
            } else if(dx == 0 || dx == 6 || dy == 0 || dy == 6) {
                black = true;
            } else if(dx >= 2 && dx <= 4 && dy >= 2 && dy <= 4) {
                black = true;
            } else {
                black = false;
            }
            set_func_module(w, x, y, black);
        }
    }
}

/* ── Alignment pattern ────────────────────────────────────────── */

static void place_alignment(QrWork* w, int cx, int cy) {
    for(int dy = -2; dy <= 2; dy++) {
        for(int dx = -2; dx <= 2; dx++) {
            bool black = (dx == -2 || dx == 2 || dy == -2 || dy == 2 || (dx == 0 && dy == 0));
            set_func_module(w, cx + dx, cy + dy, black);
        }
    }
}

/* ── Timing patterns ──────────────────────────────────────────── */

static void place_timing(QrWork* w) {
    for(int i = 8; i < w->size - 8; i++) {
        bool black = (i % 2 == 0);
        if(!IS_FUNC(w->grid[6][i])) set_func_module(w, i, 6, black);
        if(!IS_FUNC(w->grid[i][6])) set_func_module(w, 6, i, black);
    }
}

/* ── Reserve format info + dark module ────────────────────────── */

static void reserve_format_area(QrWork* w) {
    int s = w->size;
    /* First copy: around top-left finder (col 8 rows 0-8, row 8 cols 0-8) */
    for(int i = 0; i <= 8; i++) {
        if(!IS_FUNC(w->grid[i][8])) set_func_module(w, 8, i, false);
        if(!IS_FUNC(w->grid[8][i])) set_func_module(w, i, 8, false);
    }
    /* Second copy: bottom (col 8, rows s-1 to s-7) and
     * right (row 8, cols s-8 to s-1) */
    for(int i = 0; i < 7; i++) {
        if(!IS_FUNC(w->grid[s - 1 - i][8])) set_func_module(w, 8, s - 1 - i, false);
    }
    for(int i = 0; i < 8; i++) {
        if(!IS_FUNC(w->grid[8][s - 1 - i])) set_func_module(w, s - 1 - i, 8, false);
    }
    /* Dark module: always black, at (8, 4V+9) */
    set_func_module(w, 8, 4 * w->version + 9, true);
}

/* ── Setup all function patterns ──────────────────────────────── */

static void setup_function_patterns(QrWork* w) {
    place_finder(w, 0, 0);
    place_finder(w, w->size - 7, 0);
    place_finder(w, 0, w->size - 7);
    place_timing(w);

    /* Alignment patterns for V2+ */
    if(w->version >= 2) {
        /* For V2-6 the alignment pattern positions are {6, ALIGN_CENTER[v]}.
         * Alignment patterns are placed at all intersections of these
         * coordinates EXCEPT where they would overlap a finder pattern. */
        int pos[2] = {6, ALIGN_CENTER[w->version - 1]};
        for(int iy = 0; iy < 2; iy++) {
            for(int ix = 0; ix < 2; ix++) {
                int cx = pos[ix], cy = pos[iy];
                /* Skip if overlapping finder patterns */
                if(cx <= 8 && cy <= 8) continue; /* top-left */
                if(cx >= w->size - 8 && cy <= 8) continue; /* top-right */
                if(cx <= 8 && cy >= w->size - 8) continue; /* bottom-left */
                place_alignment(w, cx, cy);
            }
        }
    }

    reserve_format_area(w);
}

/* ── Data placement (ISO 18004 Section 7.7.3) ────────────────── */

static void place_data_bits(QrWork* w, const uint8_t* data, int data_len) {
    int bit_idx = 0;
    int total_bits = data_len * 8;

    /* Two-column bands, from right to left */
    for(int right = w->size - 1; right >= 1; right -= 2) {
        if(right == 6) right = 5; /* skip vertical timing column */

        /* Column pair index determines direction */
        bool upward = (((w->size - 1 - right) / 2) % 2 == 0);

        for(int vert = 0; vert < w->size; vert++) {
            int y = upward ? (w->size - 1 - vert) : vert;
            for(int dx = 0; dx <= 1; dx++) {
                int x = right - dx;
                if(x < 0 || x >= w->size) continue;
                if(IS_FUNC(w->grid[y][x])) continue;

                bool black = false;
                if(bit_idx < total_bits) {
                    black = ((data[bit_idx >> 3] >> (7 - (bit_idx & 7))) & 1) != 0;
                    bit_idx++;
                }
                w->grid[y][x] = black ? 1 : 0;
            }
        }
    }
}

/* ── Masking (ISO 18004 Table 23) ─────────────────────────────── */

static bool mask_condition(int mask, int x, int y) {
    switch(mask) {
    case 0:
        return (y + x) % 2 == 0;
    case 1:
        return y % 2 == 0;
    case 2:
        return x % 3 == 0;
    case 3:
        return (y + x) % 3 == 0;
    case 4:
        return (y / 2 + x / 3) % 2 == 0;
    case 5:
        return (y * x) % 2 + (y * x) % 3 == 0;
    case 6:
        return ((y * x) % 2 + (y * x) % 3) % 2 == 0;
    case 7:
        return ((y + x) % 2 + (y * x) % 3) % 2 == 0;
    default:
        return false;
    }
}

static void apply_mask(QrWork* w, int mask) {
    for(int y = 0; y < w->size; y++) {
        for(int x = 0; x < w->size; x++) {
            if(!IS_FUNC(w->grid[y][x]) && mask_condition(mask, x, y)) {
                w->grid[y][x] ^= 1;
            }
        }
    }
}

/* ── Penalty scoring (ISO 18004 Section 7.8.3) ───────────────── */

static int score_mask(const QrWork* w) {
    int penalty = 0;
    int s = w->size;

    /* Rule 1: runs of 5+ same colour in a row/column */
    for(int y = 0; y < s; y++) {
        int run = 1;
        for(int x = 1; x < s; x++) {
            if((w->grid[y][x] & 1) == (w->grid[y][x - 1] & 1)) {
                run++;
                if(run == 5)
                    penalty += 3;
                else if(run > 5)
                    penalty += 1;
            } else {
                run = 1;
            }
        }
    }
    for(int x = 0; x < s; x++) {
        int run = 1;
        for(int y = 1; y < s; y++) {
            if((w->grid[y][x] & 1) == (w->grid[y - 1][x] & 1)) {
                run++;
                if(run == 5)
                    penalty += 3;
                else if(run > 5)
                    penalty += 1;
            } else {
                run = 1;
            }
        }
    }

    /* Rule 2: 2x2 blocks of same colour */
    for(int y = 0; y < s - 1; y++) {
        for(int x = 0; x < s - 1; x++) {
            int c = w->grid[y][x] & 1;
            if(c == (w->grid[y][x + 1] & 1) && c == (w->grid[y + 1][x] & 1) &&
               c == (w->grid[y + 1][x + 1] & 1)) {
                penalty += 3;
            }
        }
    }

    /* Rule 3: finder-like patterns (1:1:3:1:1) */
    for(int y = 0; y < s; y++) {
        for(int x = 0; x < s - 10; x++) {
            int b[11];
            for(int k = 0; k < 11; k++)
                b[k] = w->grid[y][x + k] & 1;
            if(b[0] && !b[1] && b[2] && b[3] && b[4] && !b[5] && b[6] && !b[7] && !b[8] && !b[9] &&
               !b[10])
                penalty += 40;
            if(!b[0] && !b[1] && !b[2] && !b[3] && b[4] && !b[5] && b[6] && b[7] && b[8] &&
               !b[9] && b[10])
                penalty += 40;
        }
    }
    for(int x = 0; x < s; x++) {
        for(int y = 0; y < s - 10; y++) {
            int b[11];
            for(int k = 0; k < 11; k++)
                b[k] = w->grid[y + k][x] & 1;
            if(b[0] && !b[1] && b[2] && b[3] && b[4] && !b[5] && b[6] && !b[7] && !b[8] && !b[9] &&
               !b[10])
                penalty += 40;
            if(!b[0] && !b[1] && !b[2] && !b[3] && b[4] && !b[5] && b[6] && b[7] && b[8] &&
               !b[9] && b[10])
                penalty += 40;
        }
    }

    /* Rule 4: proportion of dark modules */
    int dark = 0;
    for(int y = 0; y < s; y++)
        for(int x = 0; x < s; x++)
            dark += (w->grid[y][x] & 1);
    int total = s * s;
    int pct = (dark * 100) / total;
    int prev5 = (pct / 5) * 5;
    int next5 = prev5 + 5;
    int a = prev5 - 50;
    if(a < 0) a = -a;
    int b2 = next5 - 50;
    if(b2 < 0) b2 = -b2;
    a /= 5;
    b2 /= 5;
    penalty += (a < b2 ? a : b2) * 10;

    return penalty;
}

/* ── Format info placement (ISO 18004 Section 7.9) ───────────── */

/* Pre-computed format info bits for ECC-L (indicator 01), masks 0-7,
 * including BCH(15,5) error correction + XOR mask 0x5412. */
static const uint16_t FORMAT_INFO[] = {
    0x77C4, /* mask 0 */
    0x72F3, /* mask 1 */
    0x7DAA, /* mask 2 */
    0x789D, /* mask 3 */
    0x662F, /* mask 4 */
    0x6318, /* mask 5 */
    0x6C41, /* mask 6 */
    0x6976, /* mask 7 */
};

static void place_format_info(QrWork* w, int mask) {
    uint16_t bits = FORMAT_INFO[mask];
    int s = w->size;

    /*
     * First copy around top-left finder (ISO 18004 Figure 25):
     *   bits[0] at (8,0), bits[1] at (8,1), ..., bits[5] at (8,5),
     *   bits[6] at (8,7), bits[7] at (8,8),
     *   bits[8] at (7,8), bits[9] at (5,8), ..., bits[14] at (0,8).
     */
    static const int8_t first_copy[][2] = {
        /* bit 0 .. bit 14, each entry is {col, row} */
        {8, 0},
        {8, 1},
        {8, 2},
        {8, 3},
        {8, 4},
        {8, 5}, /* col 8, rows 0-5 */
        {8, 7},
        {8, 8}, /* col 8, rows 7-8 (skip 6=timing) */
        {7, 8},
        {5, 8},
        {4, 8},
        {3, 8},
        {2, 8},
        {1, 8},
        {0, 8}, /* row 8, cols 7,5..0 (skip 6=timing) */
    };
    for(int i = 0; i < 15; i++) {
        bool black = ((bits >> i) & 1) != 0;
        int x = first_copy[i][0], y = first_copy[i][1];
        w->grid[y][x] = (uint8_t)(2 | (black ? 1 : 0));
    }

    /*
     * Second copy (ISO 18004 Figure 25):
     *   Horizontal (row 8, right side): bits[0] at col s-1, bits[1] at col s-2,
     *     ..., bits[7] at col s-8.
     *   Vertical (col 8, bottom side): bits[8] at row s-7, bits[9] at row s-6,
     *     ..., bits[14] at row s-1.
     */
    for(int i = 0; i < 8; i++) {
        bool black = ((bits >> i) & 1) != 0;
        w->grid[8][s - 1 - i] = (uint8_t)(2 | (black ? 1 : 0));
    }
    for(int i = 8; i < 15; i++) {
        bool black = ((bits >> i) & 1) != 0;
        w->grid[s - 15 + i][8] = (uint8_t)(2 | (black ? 1 : 0));
    }
}

/* ── Public API ───────────────────────────────────────────────── */

QrCode* qrcode_encode(const uint8_t* data, size_t len) {
    QrCode* result = malloc(sizeof(QrCode));
    if(!result) return NULL;
    memset(result, 0, sizeof(QrCode));
    result->ok = false;

    gf_init();

    /* Find smallest version that fits */
    uint8_t ver = 0;
    for(uint8_t v = 1; v <= QR_MAX_VERSION; v++) {
        if(len <= VERSION_BYTE_CAPACITY[v]) {
            ver = v;
            break;
        }
    }
    if(ver == 0) return result; /* too long */

    result->version = ver;
    result->size = 17 + ver * 4;

    /* ── Build data bitstream ──────────────────────────────── */

    uint8_t data_cw = VERSION_DATA_CODEWORDS[ver];
    uint8_t total_cw = VERSION_TOTAL_CODEWORDS[ver];
    uint8_t codewords[172];
    memset(codewords, 0, sizeof(codewords));

    size_t bit_cursor = 0;
#define APPEND_BITS(value, count)                                                                 \
    do {                                                                                          \
        uint16_t _v = (value);                                                                    \
        for(int _i = (count) - 1; _i >= 0; _i--) {                                                \
            if((_v >> _i) & 1) codewords[bit_cursor / 8] |= (uint8_t)(0x80u >> (bit_cursor % 8)); \
            bit_cursor++;                                                                         \
        }                                                                                         \
    } while(0)

    APPEND_BITS(0x4, 4); /* byte mode indicator */
    APPEND_BITS((uint16_t)len, 8); /* character count (8 bits for V1-9) */
    for(size_t i = 0; i < len; i++)
        APPEND_BITS(data[i], 8);

    /* Terminator: up to 4 zero bits */
    size_t cap = (size_t)data_cw * 8;
    size_t term = cap - bit_cursor;
    if(term > 4) term = 4;
    APPEND_BITS(0, term);

    /* Pad to byte boundary */
    while(bit_cursor & 7)
        APPEND_BITS(0, 1);

    /* Pad with alternating 0xEC, 0x11 */
    {
        size_t pad_start = bit_cursor / 8;
        for(size_t k = pad_start; k < data_cw; k++)
            codewords[k] = ((k - pad_start) & 1) ? 0x11 : 0xEC;
    }

#undef APPEND_BITS

    /* ── Generate ECC ──────────────────────────────────────── */

    uint8_t num_blocks = VERSION_NUM_BLOCKS[ver];
    uint8_t ecc_per_block = VERSION_ECC_PER_BLOCK[ver];
    uint8_t data_per_block = data_cw / num_blocks;

    uint8_t ecc_blocks[4][40];
    for(uint8_t b = 0; b < num_blocks; b++) {
        rs_generate_ecc(
            &codewords[b * data_per_block], data_per_block, ecc_blocks[b], ecc_per_block);
    }

    /* ── Interleave data + ECC codewords ───────────────────── */

    uint8_t interleaved[172];
    size_t pos = 0;
    for(uint8_t i = 0; i < data_per_block; i++)
        for(uint8_t b = 0; b < num_blocks; b++)
            interleaved[pos++] = codewords[b * data_per_block + i];
    for(uint8_t i = 0; i < ecc_per_block; i++)
        for(uint8_t b = 0; b < num_blocks; b++)
            interleaved[pos++] = ecc_blocks[b][i];

    /* ── Place modules ─────────────────────────────────────── */

    /* Heap-allocate work grids to avoid blowing the 4KB stack.
     * result->ok stays false so the caller reports the failure. */
    QrWork* work = malloc(sizeof(QrWork));
    QrWork* trial = malloc(sizeof(QrWork));
    if(!work || !trial) {
        free(work);
        free(trial);
        return result;
    }
    memset(work, 0, sizeof(QrWork));
    work->size = result->size;
    work->version = ver;

    setup_function_patterns(work);
    place_data_bits(work, interleaved, (int)total_cw);

    /* ── Select best mask ──────────────────────────────────── */

    int best_mask = 0;
    int best_penalty = 0x7FFFFFFF;

    for(int m = 0; m < 8; m++) {
        memcpy(trial, work, sizeof(QrWork));
        apply_mask(trial, m);
        place_format_info(trial, m);
        int pen = score_mask(trial);
        if(pen < best_penalty) {
            best_penalty = pen;
            best_mask = m;
        }
    }

    apply_mask(work, best_mask);
    place_format_info(work, best_mask);

    /* ── Copy to output ────────────────────────────────────── */

    for(int y = 0; y < result->size; y++)
        for(int x = 0; x < result->size; x++)
            result->modules[y][x] = work->grid[y][x] & 1;

    free(work);
    free(trial);

    result->ok = true;
    return result;
}
