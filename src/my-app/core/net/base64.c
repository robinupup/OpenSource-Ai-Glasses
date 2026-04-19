#include "base64.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static const char B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t base64_encoded_len(size_t n) {
    return 4 * ((n + 2) / 3) + 1;   /* +1 for NUL */
}

size_t base64_encode(const unsigned char *src, size_t len, char *out) {
    size_t i = 0, o = 0;
    while (i + 3 <= len) {
        unsigned v = (src[i] << 16) | (src[i+1] << 8) | src[i+2];
        out[o++] = B64[(v >> 18) & 0x3F];
        out[o++] = B64[(v >> 12) & 0x3F];
        out[o++] = B64[(v >> 6)  & 0x3F];
        out[o++] = B64[v & 0x3F];
        i += 3;
    }
    size_t rem = len - i;
    if (rem == 1) {
        unsigned v = src[i] << 16;
        out[o++] = B64[(v >> 18) & 0x3F];
        out[o++] = B64[(v >> 12) & 0x3F];
        out[o++] = '=';
        out[o++] = '=';
    } else if (rem == 2) {
        unsigned v = (src[i] << 16) | (src[i+1] << 8);
        out[o++] = B64[(v >> 18) & 0x3F];
        out[o++] = B64[(v >> 12) & 0x3F];
        out[o++] = B64[(v >> 6)  & 0x3F];
        out[o++] = '=';
    }
    out[o] = '\0';
    return o;
}

char *base64_encode_file(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    struct stat st;
    if (stat(path, &st) != 0) { fclose(fp); return NULL; }
    size_t len = (size_t)st.st_size;
    unsigned char *buf = (unsigned char *)malloc(len);
    if (!buf) { fclose(fp); return NULL; }
    if (fread(buf, 1, len, fp) != len) { free(buf); fclose(fp); return NULL; }
    fclose(fp);

    size_t olen = base64_encoded_len(len);
    char *out = (char *)malloc(olen);
    if (!out) { free(buf); return NULL; }
    base64_encode(buf, len, out);
    free(buf);
    return out;
}
