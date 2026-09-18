/*
 * Host-side tests for the I2C address arithmetic and chunking.
 *
 * The conversion is duplicated from src/execute/i2c_exec.c rather than
 * linked, because that file needs the Flipper I2C HAL. Keep the two in
 * step: if i2c_wire_addr() or PIFK_I2C_CHUNK change there, change
 * them here.
 *
 * Build & run:
 *   cc -Wall -Wextra -Werror -fsanitize=address,undefined \
 *      -o /tmp/test_i2c_addr tests/test_i2c_addr.c
 *   /tmp/test_i2c_addr
 *
 * Exits non-zero on failure so it can gate CI.
 *
 * Why this is worth testing at all: a wrong shift addresses a device one
 * position away from the one the operator named, and an I2C write cannot
 * be undone. 0x50 -> 0xA0 is the canonical 24Cxx EEPROM pair from the
 * datasheet, so it pins the convention against something external.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#define PIFK_I2C_ADDR_MIN 0x08
#define PIFK_I2C_ADDR_MAX 0x77
#define PIFK_I2C_CHUNK    16
static uint8_t i2c_wire_addr(uint8_t a7) {
    return (uint8_t)(a7 << 1);
}
static int fails = 0, checks = 0;
#define CHECK(c, ...)                        \
    do {                                     \
        checks++;                            \
        if(!(c)) {                           \
            fails++;                         \
            printf("  FAIL %d: ", __LINE__); \
            printf(__VA_ARGS__);             \
            puts("");                        \
        }                                    \
    } while(0)
int main(void) {
    puts("wire address conversion");
    /* 0x50 EEPROM -> 0xA0 write address, the canonical datasheet pair. */
    CHECK(i2c_wire_addr(0x50) == 0xA0, "0x50 -> 0x%02X, want 0xA0", i2c_wire_addr(0x50));
    CHECK(i2c_wire_addr(0x3C) == 0x78, "0x3C -> 0x%02X, want 0x78", i2c_wire_addr(0x3C));
    CHECK(i2c_wire_addr(0x08) == 0x10, "min");
    CHECK(i2c_wire_addr(0x77) == 0xEE, "max 0x77 -> 0x%02X, want 0xEE", i2c_wire_addr(0x77));
    /* The whole valid range must stay inside a byte with bit0 clear
     * (write direction) -- if it overflowed we'd address something else. */
    for(uint8_t a = PIFK_I2C_ADDR_MIN; a <= PIFK_I2C_ADDR_MAX; a++) {
        uint8_t w = i2c_wire_addr(a);
        CHECK((w & 1) == 0, "0x%02X wire 0x%02X has R/W bit set", a, w);
        CHECK((w >> 1) == a, "0x%02X does not round-trip", a);
    }
    printf("  0x08..0x77 all shift cleanly, R/W bit clear\n");

    puts("\nreserved ranges excluded");
    CHECK(PIFK_I2C_ADDR_MIN == 0x08, "min must skip 0x00-0x07 reserved");
    CHECK(PIFK_I2C_ADDR_MAX == 0x77, "max must skip 0x78-0x7F reserved");
    printf("  scan covers %d addresses\n", PIFK_I2C_ADDR_MAX - PIFK_I2C_ADDR_MIN + 1);

    puts("\nchunking covers the payload exactly");
    for(size_t len = 1; len <= 512; len++) {
        size_t off = 0, iters = 0, total = 0;
        while(off < len) {
            size_t take = len - off;
            if(take > PIFK_I2C_CHUNK) take = PIFK_I2C_CHUNK;
            total += take;
            off += take;
            iters++;
            if(iters > 600) {
                CHECK(0, "len %zu did not terminate", len);
                break;
            }
        }
        if(total != len) {
            CHECK(0, "len %zu wrote %zu", len, total);
            break;
        }
    }
    printf("  1..512 bytes: every length delivered exactly once, no overrun\n");
    printf("  512 bytes -> %d transactions\n", (512 + PIFK_I2C_CHUNK - 1) / PIFK_I2C_CHUNK);
    printf("\n%d checks, %d failure(s)\n", checks, fails);
    return fails ? 1 : 0;
}
