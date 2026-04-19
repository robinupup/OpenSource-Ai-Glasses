/**
 * mic_test - 独立麦克风测试工具
 *
 * 1) 采集 N 秒 PCM，统计 RMS / peak / 非零率，用于判断拾音是否存活
 * 2) 同时落盘为 WAV 方便用 speaker_test 回放听感验证
 *
 * 用法：
 *   mic_test [seconds] [output.wav] [rate] [ch] [bits]
 *   默认 3 秒 / /tmp/mic_test.wav / 16000 / 1 / 16
 */

#include "../core/mic_capture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <math.h>
#include <time.h>

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int main(int argc, char **argv) {
    setbuf(stdout, NULL);

    int   seconds = (argc > 1) ? atoi(argv[1]) : 3;
    const char *out = (argc > 2) ? argv[2] : "/tmp/mic_test.wav";
    int   rate = (argc > 3) ? atoi(argv[3]) : 16000;
    int   ch   = (argc > 4) ? atoi(argv[4]) : 1;
    int   bits = (argc > 5) ? atoi(argv[5]) : 16;

    if (seconds <= 0) seconds = 3;

    printf("[mic_test] seconds=%d out=%s rate=%d ch=%d bits=%d\n",
           seconds, out, rate, ch, bits);

    mic_params_t p = { .sample_rate = rate, .channels = ch, .bit_width = bits };
    mic_stream_t *s = mic_capture_start(&p);
    if (!s) {
        fprintf(stderr, "[mic_test] mic_capture_start failed\n");
        return 1;
    }
    printf("[mic_test] capture started, reading %d seconds...\n", seconds);

    /* WAV 文件输出（手工写头，兼容 bits!=16 的情形） */
    FILE *fp = fopen(out, "wb");
    if (!fp) { fprintf(stderr, "[mic_test] open %s failed\n", out); mic_capture_stop(s); return 2; }
    /* 占位 44 字节 WAV 头，录完再回填 */
    uint8_t zero44[44] = {0};
    fwrite(zero44, 1, 44, fp);

    size_t target_bytes = (size_t)rate * ch * (bits / 8) * (size_t)seconds;
    size_t total = 0;
    uint64_t t0 = now_ms();

    /* 统计量（假定 S16_LE / mono；其他格式仅做字节计数） */
    double sq_sum = 0.0;
    int64_t sample_count = 0;
    int peak = 0;
    size_t nonzero_bytes = 0;

    uint8_t buf[4096];
    while (total < target_bytes) {
        size_t want = sizeof(buf);
        if (total + want > target_bytes) want = target_bytes - total;
        ssize_t n = mic_capture_read(s, buf, want);
        if (n <= 0) {
            fprintf(stderr, "[mic_test] read failed n=%zd\n", n);
            break;
        }
        fwrite(buf, 1, (size_t)n, fp);
        total += (size_t)n;

        for (ssize_t i = 0; i < n; i++) if (buf[i] != 0) nonzero_bytes++;
        if (bits == 16) {
            int16_t *s16 = (int16_t *)buf;
            ssize_t ns = n / 2;
            for (ssize_t i = 0; i < ns; i++) {
                int v = s16[i];
                sq_sum += (double)v * (double)v;
                int av = v < 0 ? -v : v;
                if (av > peak) peak = av;
            }
            sample_count += ns;
        }
    }
    uint64_t t1 = now_ms();

    /* 回填 WAV header */
    uint32_t data_size  = (uint32_t)total;
    uint32_t chunk_size = 36 + data_size;
    uint16_t audio_fmt  = 1;
    uint16_t ch16       = (uint16_t)ch;
    uint32_t rate32     = (uint32_t)rate;
    uint16_t bits16     = (uint16_t)bits;
    uint32_t byte_rate  = (uint32_t)rate * ch * (bits / 8);
    uint16_t block_align= (uint16_t)(ch * (bits / 8));
    uint32_t fmt_size   = 16;

    fseek(fp, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, fp);       fwrite(&chunk_size, 4, 1, fp);
    fwrite("WAVE", 1, 4, fp);
    fwrite("fmt ", 1, 4, fp);       fwrite(&fmt_size, 4, 1, fp);
    fwrite(&audio_fmt, 2, 1, fp);   fwrite(&ch16, 2, 1, fp);
    fwrite(&rate32, 4, 1, fp);      fwrite(&byte_rate, 4, 1, fp);
    fwrite(&block_align, 2, 1, fp); fwrite(&bits16, 2, 1, fp);
    fwrite("data", 1, 4, fp);       fwrite(&data_size, 4, 1, fp);
    fclose(fp);

    mic_capture_stop(s);

    double elapsed_s = (t1 - t0) / 1000.0;
    double kbps = (total / 1024.0) / (elapsed_s > 0 ? elapsed_s : 1);
    double expected = (double)target_bytes;
    double fill_ratio = expected > 0 ? (double)total / expected : 0.0;

    printf("[mic_test] done: %zu bytes in %.2fs (%.1f KB/s, fill=%.1f%%)\n",
           total, elapsed_s, kbps, fill_ratio * 100);
    printf("[mic_test] nonzero bytes ratio: %.2f%%\n",
           total ? (100.0 * nonzero_bytes / total) : 0.0);
    if (sample_count > 0) {
        double rms = sqrt(sq_sum / sample_count);
        /* 约换算成 dBFS（S16 满幅 32768） */
        double dbfs = rms > 0 ? 20.0 * log10(rms / 32768.0) : -120.0;
        double peak_db = peak > 0 ? 20.0 * log10((double)peak / 32768.0) : -120.0;
        printf("[mic_test] S16 stats: samples=%lld  RMS=%.1f (%.1f dBFS)  peak=%d (%.1f dBFS)\n",
               (long long)sample_count, rms, dbfs, peak, peak_db);
    }
    printf("[mic_test] wrote WAV -> %s\n", out);
    return 0;
}
