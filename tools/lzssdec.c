#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#define N 4096
#define F 18
#define THRESHOLD 2

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <in_file> <out_file>\n", argv[0]);
        return 1;
    }
    FILE *in = fopen(argv[1], "rb");
    if (!in) { perror("fopen in"); return 1; }
    fseek(in, 0, SEEK_END);
    size_t sz = ftell(in);
    fseek(in, 0, SEEK_SET);

    uint8_t *src_buf = malloc(sz);
    if (!src_buf) return 1;
    fread(src_buf, 1, sz, in);
    fclose(in);

    if (memcmp(src_buf, "complzss", 8) != 0) {
        fprintf(stderr, "Not complzss\n");
        return 1;
    }

    uint32_t decomp_len = (src_buf[12] << 24) | (src_buf[13] << 16) | (src_buf[14] << 8) | src_buf[15];
    uint32_t comp_len = (src_buf[16] << 24) | (src_buf[17] << 16) | (src_buf[18] << 8) | src_buf[19];
    printf("complzss: comp_len=0x%x (%u), decomp_len=0x%x (%u)\n", comp_len, comp_len, decomp_len, decomp_len);

    uint8_t *src = src_buf + 0x180;
    uint8_t *src_end = src_buf + sz;
    uint8_t *dst = malloc(decomp_len + 0x10000);
    if (!dst) return 1;
    uint8_t *dst_start = dst;

    uint8_t text_buf[N + F - 1];
    int r = N - F;
    memset(text_buf, ' ', r);
    uint16_t flags = 0;

    while (src < src_end && (dst - dst_start) < decomp_len) {
        if (((flags >>= 1) & 0x100) == 0) {
            flags = (*src++) | 0xff00;
        }
        if (flags & 1) {
            int c = *src++;
            *dst++ = c;
            text_buf[r++] = c;
            r &= (N - 1);
        } else {
            int i = *src++;
            int j = *src++;
            i |= ((j & 0xf0) << 4);
            j = (j & 0x0f) + THRESHOLD;
            for (int k = 0; k <= j; k++) {
                int c = text_buf[(i + k) & (N - 1)];
                *dst++ = c;
                text_buf[r++] = c;
                r &= (N - 1);
            }
        }
    }

    printf("Decompressed %ld bytes (expected %u)\n", (long)(dst - dst_start), decomp_len);
    FILE *out = fopen(argv[2], "wb");
    if (!out) { perror("fopen out"); return 1; }
    fwrite(dst_start, 1, dst - dst_start, out);
    fclose(out);
    printf("Saved to %s\n", argv[2]);
    return 0;
}
