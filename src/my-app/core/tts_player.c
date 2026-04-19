/*
 * tts_player.c — 流式 TTS PCM 播放器
 *
 * 复用 audio_player.c 里验证过的 ALSA HW/SW 参数配置逻辑（不依赖 aplay）。
 * 一条 worker 线程从 chunk 队列取 PCM 写到 /dev/snd/pcmC0D0p；
 * feed/stop/end/begin 通过 mutex+cond 与 worker 同步。
 */
#include "tts_player.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sound/asound.h>

#define ALSA_PCM_DEVICE   "/dev/snd/pcmC0D0p"

typedef struct chunk {
    uint8_t     *data;
    size_t       len;
    struct chunk *next;
} chunk_t;

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t  cv;
    pthread_t       tid;
    int             running;      /* worker 线程活着 */
    int             have_thread;
    int             cancel;       /* stop() 设置：立即丢队列 + DROP */
    int             ending;       /* end() 设置：队列放完就退出 */
    int             fd;
    int             rate, channels, bits;        /* 服务端下发的 PCM 参数 */
    unsigned int    out_rate;                    /* driver 真正接受的 rate */
    /* 线性重采样器状态：保存上一 chunk 最末一帧，供下一 chunk 首帧插值 */
    int             has_prev;
    int16_t         prev_sample[2];              /* 最多 stereo */
    double          phase;                       /* 当前在源流中的小数位置 */
    chunk_t        *head, *tail;
    size_t          queued_bytes;
} tts_state_t;

static tts_state_t g = {
    .mu = PTHREAD_MUTEX_INITIALIZER,
    .cv = PTHREAD_COND_INITIALIZER,
    .fd = -1,
};

/* -------- ALSA 配置（与 audio_player.c 保持一致） -------- */
static void mask_set(struct snd_mask *m, unsigned int val) {
    memset(m, 0, sizeof(*m));
    m->bits[val >> 5] |= (1u << (val & 31));
}
static void interval_set(struct snd_interval *v, unsigned int val) {
    memset(v, 0, sizeof(*v));
    v->min = v->max = val;
    v->integer = 1;
}
static int alsa_snd_format(int bit_width) {
    switch (bit_width) {
        case 8:  return SNDRV_PCM_FORMAT_S8;
        case 16: return SNDRV_PCM_FORMAT_S16_LE;
        case 24: return SNDRV_PCM_FORMAT_S24_LE;
        case 32: return SNDRV_PCM_FORMAT_S32_LE;
        default: return SNDRV_PCM_FORMAT_S16_LE;
    }
}

/* 全开 hp：所有 mask=0xff、所有 interval=[0, ~0]，rmask=~0，info=~0。
 * HW_REFINE / HW_PARAMS 都需要这个初态，否则内核无法识别要 refine 的字段。 */
static void hp_init_all_open(struct snd_pcm_hw_params *hp) {
    memset(hp, 0, sizeof(*hp));
    for (int i = SNDRV_PCM_HW_PARAM_FIRST_MASK; i <= SNDRV_PCM_HW_PARAM_LAST_MASK; i++) {
        struct snd_mask *m = &hp->masks[i - SNDRV_PCM_HW_PARAM_FIRST_MASK];
        memset(m, 0xff, sizeof(*m));
    }
    for (int i = SNDRV_PCM_HW_PARAM_FIRST_INTERVAL; i <= SNDRV_PCM_HW_PARAM_LAST_INTERVAL; i++) {
        struct snd_interval *v = &hp->intervals[i - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
        v->min = 0; v->max = ~0; v->openmin = v->openmax = 0;
        v->integer = 0; v->empty = 0;
    }
    hp->rmask = ~0U; hp->cmask = 0; hp->info = ~0U;
}

static int alsa_open_playback(int rate, int channels, int bit_width,
                              unsigned int *out_rate) {
    int fd = open(ALSA_PCM_DEVICE, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "[tts] open %s: %s\n", ALSA_PCM_DEVICE, strerror(errno));
        return -1;
    }

    /* ------- Step 1: HW_REFINE 探测 driver 真正支持的 rate 范围 ------- */
    struct snd_pcm_hw_params hp;
    hp_init_all_open(&hp);
    mask_set(&hp.masks[SNDRV_PCM_HW_PARAM_ACCESS - SNDRV_PCM_HW_PARAM_FIRST_MASK],
             SNDRV_PCM_ACCESS_RW_INTERLEAVED);
    mask_set(&hp.masks[SNDRV_PCM_HW_PARAM_FORMAT - SNDRV_PCM_HW_PARAM_FIRST_MASK],
             alsa_snd_format(bit_width));
    mask_set(&hp.masks[SNDRV_PCM_HW_PARAM_SUBFORMAT - SNDRV_PCM_HW_PARAM_FIRST_MASK],
             SNDRV_PCM_SUBFORMAT_STD);
    interval_set(&hp.intervals[SNDRV_PCM_HW_PARAM_CHANNELS - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL],
                 (unsigned int)channels);
    /* rate 故意不锁，让内核告诉我们它真正支持的范围 */

    if (ioctl(fd, SNDRV_PCM_IOCTL_HW_REFINE, &hp) < 0) {
        fprintf(stderr, "[tts] HW_REFINE(probe): %s\n", strerror(errno));
        close(fd); return -1;
    }
    unsigned int rate_lo =
        hp.intervals[SNDRV_PCM_HW_PARAM_RATE - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL].min;
    unsigned int rate_hi =
        hp.intervals[SNDRV_PCM_HW_PARAM_RATE - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL].max;
    printf("[tts] driver supports rate=%u..%u at ch=%d bits=%d\n",
           rate_lo, rate_hi, channels, bit_width);

    /* ------- Step 2: 在候选列表中挑一个 driver 接受的 rate ------- */
    /* 优先尝试请求的 rate，其次 16k/48k/32k/24k/8k（按常见性排序） */
    unsigned int cand[8];
    int nc = 0;
    cand[nc++] = (unsigned int)rate;
    static const unsigned int kRates[] = {16000, 48000, 44100, 32000, 24000, 22050, 8000};
    for (size_t i = 0; i < sizeof(kRates)/sizeof(kRates[0]) && nc < 8; i++) {
        int dup = 0;
        for (int j = 0; j < nc; j++) if (cand[j] == kRates[i]) { dup = 1; break; }
        if (!dup) cand[nc++] = kRates[i];
    }

    unsigned int picked = 0;
    for (int i = 0; i < nc; i++) {
        unsigned int r = cand[i];
        if (r < rate_lo || r > rate_hi) continue;

        hp_init_all_open(&hp);
        mask_set(&hp.masks[SNDRV_PCM_HW_PARAM_ACCESS - SNDRV_PCM_HW_PARAM_FIRST_MASK],
                 SNDRV_PCM_ACCESS_RW_INTERLEAVED);
        mask_set(&hp.masks[SNDRV_PCM_HW_PARAM_FORMAT - SNDRV_PCM_HW_PARAM_FIRST_MASK],
                 alsa_snd_format(bit_width));
        mask_set(&hp.masks[SNDRV_PCM_HW_PARAM_SUBFORMAT - SNDRV_PCM_HW_PARAM_FIRST_MASK],
                 SNDRV_PCM_SUBFORMAT_STD);
        interval_set(&hp.intervals[SNDRV_PCM_HW_PARAM_CHANNELS - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL],
                     (unsigned int)channels);
        interval_set(&hp.intervals[SNDRV_PCM_HW_PARAM_RATE - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL], r);
        /* period_time 范围放宽到 [10ms, 200ms]，兼容 8kHz 下 driver 最小 64ms/period */
        struct snd_interval *pt =
            &hp.intervals[SNDRV_PCM_HW_PARAM_PERIOD_TIME - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
        pt->min = 10000; pt->max = 200000;
        pt->integer = 0; pt->openmin = pt->openmax = 0; pt->empty = 0;
        struct snd_interval *pc =
            &hp.intervals[SNDRV_PCM_HW_PARAM_PERIODS - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
        pc->min = 2; pc->max = 16; pc->integer = 1;

        if (ioctl(fd, SNDRV_PCM_IOCTL_HW_PARAMS, &hp) == 0) {
            picked = r;
            break;
        }
        /* HW_PARAMS 改变了 fd 状态，下一轮 try 前需要 HW_FREE */
        (void)ioctl(fd, SNDRV_PCM_IOCTL_HW_FREE, 0);
    }

    if (picked == 0) {
        fprintf(stderr, "[tts] HW_PARAMS: no usable rate in candidates (driver range %u..%u)\n",
                rate_lo, rate_hi);
        close(fd); return -1;
    }

    unsigned int neg_rate =
        hp.intervals[SNDRV_PCM_HW_PARAM_RATE - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL].min;
    unsigned int neg_ch =
        hp.intervals[SNDRV_PCM_HW_PARAM_CHANNELS - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL].min;
    unsigned int period_frames =
        hp.intervals[SNDRV_PCM_HW_PARAM_PERIOD_SIZE - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL].min;
    unsigned int buffer_frames =
        hp.intervals[SNDRV_PCM_HW_PARAM_BUFFER_SIZE - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL].min;
    if (buffer_frames == 0) buffer_frames = period_frames * 4;
    printf("[tts] hw picked: rate=%u ch=%u period=%u buffer=%u (server requested %d Hz)\n",
           neg_rate, neg_ch, period_frames, buffer_frames, rate);
    if (out_rate) *out_rate = neg_rate;

    struct snd_pcm_sw_params sw;
    memset(&sw, 0, sizeof(sw));
    sw.tstamp_mode       = SNDRV_PCM_TSTAMP_NONE;
    sw.period_step       = 1;
    sw.avail_min         = period_frames;
    sw.start_threshold   = 1;
    sw.stop_threshold    = buffer_frames;
    sw.silence_threshold = 0;
    sw.silence_size      = 0;
    sw.boundary = buffer_frames;
    while (sw.boundary < 0x40000000U) sw.boundary *= 2;
    if (ioctl(fd, SNDRV_PCM_IOCTL_SW_PARAMS, &sw) < 0) {
        fprintf(stderr, "[tts] SW_PARAMS: %s\n", strerror(errno));
        close(fd); return -1;
    }
    if (ioctl(fd, SNDRV_PCM_IOCTL_PREPARE, 0) < 0) {
        fprintf(stderr, "[tts] PREPARE: %s\n", strerror(errno));
        close(fd); return -1;
    }
    return fd;
}

/* 线性重采样：src_rate → g.out_rate。仅支持 s16le，channels ∈ {1,2}。
 * 返回新分配的 PCM 缓冲区和新字节数；调用者负责 free。
 * 使用 g.phase / g.prev_sample / g.has_prev 维持跨 chunk 的连续相位。 */
static int resample_s16(const uint8_t *in, size_t in_bytes,
                        int src_rate, int channels,
                        uint8_t **out, size_t *out_bytes) {
    if (channels < 1 || channels > 2) return -1;
    if (src_rate <= 0 || g.out_rate == 0) return -1;
    if ((int)g.out_rate == src_rate) {
        /* 直通：拷贝即可，保证调用者统一 free */
        uint8_t *b = (uint8_t *)malloc(in_bytes);
        if (!b) return -1;
        memcpy(b, in, in_bytes);
        *out = b; *out_bytes = in_bytes;
        return 0;
    }

    const int16_t *src = (const int16_t *)in;
    size_t src_frames = in_bytes / (sizeof(int16_t) * channels);
    if (src_frames == 0) {
        uint8_t *b = (uint8_t *)malloc(1);
        *out = b; *out_bytes = 0;
        return 0;
    }

    double ratio = (double)g.out_rate / (double)src_rate; /* >1 上采样, <1 下采样 */
    /* 估算上界：每个源帧最多产出 ceil(ratio) 帧，+2 冗余 */
    size_t max_out_frames = (size_t)((src_frames + 2) * (ratio + 1.0)) + 4;
    int16_t *dst = (int16_t *)malloc(max_out_frames * sizeof(int16_t) * channels);
    if (!dst) return -1;

    /* 坐标系：把 "prev_sample(若有)" 视为 src[-1]。
     * 输入样本流从 index 0 开始就是本 chunk 的 src[0..src_frames-1]。
     * g.phase 表示下一个要产出的输出帧对应到"本 chunk 起点"的分数位置。
     *   首 chunk：phase = 0，第一个输出 = src[0]（需要右端点 src[1] 存在）。
     *   后续 chunk：phase 可能落在 [-1, 0) —— 插值于 prev_sample 与 src[0] 之间；
     *                也可能 >=0 —— 正常在 src 内插值。 */
    double step = (double)src_rate / (double)g.out_rate;
    double pos = g.phase; /* 相对本 chunk 起点；首 chunk=0；后续 chunk 在 [-1, +∞) */
    size_t o = 0;

    while (1) {
        double i_floor = (pos >= 0) ? (double)(long)pos
                                     : (double)((long)pos - (pos != (long)pos ? 1 : 0));
        long   i0   = (long)i_floor;         /* 可能为 -1 */
        double frac = pos - i_floor;         /* [0, 1) */

        /* 需要 a=src[i0], b=src[i0+1] 都可取。i0==-1 需要 prev_sample，否则本样插值。 */
        if (i0 < -1) break;
        if (i0 == -1 && !g.has_prev) {
            /* 首 chunk + 负 phase，几乎不会发生；推进 pos 跳过 */
            pos += step;
            continue;
        }
        if ((size_t)(i0 + 1) >= src_frames) break; /* 右端点超出本 chunk，等下一 chunk */

        for (int c = 0; c < channels; c++) {
            int16_t a = (i0 == -1) ? g.prev_sample[c]
                                   : src[(size_t)i0 * channels + c];
            int16_t b = src[(size_t)(i0 + 1) * channels + c];
            double v = (double)a * (1.0 - frac) + (double)b * frac;
            if (v >  32767.0) v =  32767.0;
            if (v < -32768.0) v = -32768.0;
            dst[o * channels + c] = (int16_t)v;
        }
        o++;
        pos += step;
    }

    /* 保存本 chunk 最末一帧，作为下一 chunk 的 src[-1] */
    for (int c = 0; c < channels; c++) {
        g.prev_sample[c] = src[(src_frames - 1) * channels + c];
    }
    g.has_prev = 1;
    /* 下一 chunk 的 src[0] 是本 chunk 的 src[src_frames]（不存在），
     * 即现在 pos 相对下一 chunk 起点的偏移 = pos - src_frames。
     * 该值会落在 (-1, step) 附近。 */
    g.phase = pos - (double)src_frames;

    *out = (uint8_t *)dst;
    *out_bytes = o * sizeof(int16_t) * channels;
    return 0;
}

/* 写 N 帧到 PCM；返回 0 成功 / 1 被 cancel / -1 错误 */
static int write_frames(int fd, const uint8_t *data, size_t bytes, int frame_bytes) {
    size_t off = 0;
    while (off < bytes) {
        if (g.cancel) return 1;
        size_t remain_frames = (bytes - off) / frame_bytes;
        if (remain_frames == 0) break;
        struct snd_xferi xfer;
        xfer.buf    = (void *)(data + off);
        xfer.frames = remain_frames;
        xfer.result = 0;
        int rc = ioctl(fd, SNDRV_PCM_IOCTL_WRITEI_FRAMES, &xfer);
        if (rc < 0) {
            if (errno == EPIPE || errno == EBADFD || errno == ESTRPIPE) {
                ioctl(fd, SNDRV_PCM_IOCTL_PREPARE, 0);
                continue;
            }
            if (errno == EAGAIN || errno == EINTR) continue;
            fprintf(stderr, "[tts] write: %s\n", strerror(errno));
            return -1;
        }
        off += (size_t)xfer.result * frame_bytes;
    }
    return 0;
}

/* 释放整条 chunk 链 */
static void free_chunks_locked(void) {
    chunk_t *c = g.head;
    while (c) {
        chunk_t *n = c->next;
        free(c->data);
        free(c);
        c = n;
    }
    g.head = g.tail = NULL;
    g.queued_bytes = 0;
}

static void *worker_main(void *arg) {
    (void)arg;
    int frame_bytes = (g.bits / 8) * g.channels;
    if (frame_bytes <= 0) frame_bytes = 2;

    for (;;) {
        pthread_mutex_lock(&g.mu);
        while (!g.cancel && !g.head && !g.ending) {
            pthread_cond_wait(&g.cv, &g.mu);
        }

        if (g.cancel) {
            free_chunks_locked();
            pthread_mutex_unlock(&g.mu);
            if (g.fd >= 0) ioctl(g.fd, SNDRV_PCM_IOCTL_DROP, 0);
            break;
        }

        chunk_t *c = g.head;
        if (c) {
            g.head = c->next;
            if (!g.head) g.tail = NULL;
            g.queued_bytes -= c->len;
        }
        int ending_and_empty = (g.ending && !g.head);
        pthread_mutex_unlock(&g.mu);

        if (c) {
            if (g.fd >= 0) {
                uint8_t *rs_buf = NULL;
                size_t   rs_len = 0;
                if (g.bits == 16 && resample_s16(c->data, c->len,
                                                 g.rate, g.channels,
                                                 &rs_buf, &rs_len) == 0) {
                    if (rs_len > 0) {
                        (void)write_frames(g.fd, rs_buf, rs_len, frame_bytes);
                    }
                    free(rs_buf);
                } else {
                    /* 非 s16 或分配失败：退化为直写（可能音调异常，但不至于崩） */
                    (void)write_frames(g.fd, c->data, c->len, frame_bytes);
                }
            }
            free(c->data);
            free(c);
        }

        if (ending_and_empty) {
            if (g.fd >= 0) ioctl(g.fd, SNDRV_PCM_IOCTL_DRAIN, 0);
            break;
        }
    }

    pthread_mutex_lock(&g.mu);
    free_chunks_locked();
    if (g.fd >= 0) { close(g.fd); g.fd = -1; }
    g.running = 0;
    pthread_cond_broadcast(&g.cv);
    pthread_mutex_unlock(&g.mu);
    return NULL;
}

/* 等 worker 完全退出（在调用 begin/stop 时确保之前的线程已终结） */
static void join_worker_locked_unlocked(void) {
    if (!g.have_thread) return;
    pthread_t t = g.tid;
    g.have_thread = 0;
    pthread_mutex_unlock(&g.mu);
    pthread_join(t, NULL);
    pthread_mutex_lock(&g.mu);
}

int tts_player_init(void) { return 0; }

int tts_player_begin(int sample_rate, int channels, int bits) {
    if (sample_rate <= 0 || channels <= 0 || bits <= 0) return -1;

    pthread_mutex_lock(&g.mu);
    /* 抢占现有会话 */
    if (g.running) {
        g.cancel = 1;
        pthread_cond_broadcast(&g.cv);
        join_worker_locked_unlocked();
    }
    free_chunks_locked();
    if (g.fd >= 0) { close(g.fd); g.fd = -1; }

    g.rate = sample_rate;
    g.channels = channels;
    g.bits = bits;
    g.cancel = 0;
    g.ending = 0;

    unsigned int picked = 0;
    g.fd = alsa_open_playback(sample_rate, channels, bits, &picked);
    if (g.fd < 0) {
        pthread_mutex_unlock(&g.mu);
        return -1;
    }
    /* driver 声称接受的 rate（= HW_PARAMS 里那个 label）。 */
    unsigned int alsa_label_rate = picked ? picked : (unsigned int)sample_rate;

    /* 物理 DAC 时钟速率：可通过环境变量 MYAPP_TTS_PHYS_RATE 覆盖。
     * 这块 rv1103b-acodec + 内核 DAI 在当前 DTS 下只报 "16000" 一档，但实测
     * 按 16000 喂数据声音高一个八度 —— 说明物理速率是 driver 报值的 2×（≈32000）。
     * 默认按 "label × 2" 走，用户可以通过 env 改 16000/24000/32000/48000 调整。 */
    unsigned int phys_rate = alsa_label_rate * 2;
    const char *env = getenv("MYAPP_TTS_PHYS_RATE");
    if (env && *env) {
        long v = strtol(env, NULL, 10);
        if (v >= 8000 && v <= 192000) phys_rate = (unsigned int)v;
    }
    g.out_rate = phys_rate;  /* 重采样器的目标速率 —— 决定音调正确性 */
    printf("[tts] resampler target = %u Hz (ALSA label rate = %u Hz)\n",
           g.out_rate, alsa_label_rate);

    g.has_prev = 0;
    g.phase = 0.0;
    memset(g.prev_sample, 0, sizeof(g.prev_sample));

    g.running = 1;
    if (pthread_create(&g.tid, NULL, worker_main, NULL) != 0) {
        g.running = 0;
        close(g.fd); g.fd = -1;
        pthread_mutex_unlock(&g.mu);
        return -1;
    }
    g.have_thread = 1;
    pthread_mutex_unlock(&g.mu);
    printf("[tts] begin rate=%d ch=%d bits=%d\n", sample_rate, channels, bits);
    return 0;
}

int tts_player_feed(const void *pcm, size_t len) {
    if (!pcm || len == 0) return 0;
    chunk_t *c = (chunk_t *)calloc(1, sizeof(*c));
    if (!c) return -1;
    c->data = (uint8_t *)malloc(len);
    if (!c->data) { free(c); return -1; }
    memcpy(c->data, pcm, len);
    c->len = len;

    pthread_mutex_lock(&g.mu);
    if (!g.running || g.cancel) {
        pthread_mutex_unlock(&g.mu);
        free(c->data); free(c);
        return -1;
    }
    if (g.tail) g.tail->next = c; else g.head = c;
    g.tail = c;
    g.queued_bytes += len;
    pthread_cond_broadcast(&g.cv);
    pthread_mutex_unlock(&g.mu);
    return 0;
}

int tts_player_end(void) {
    pthread_mutex_lock(&g.mu);
    g.ending = 1;
    pthread_cond_broadcast(&g.cv);
    pthread_mutex_unlock(&g.mu);
    return 0;
}

int tts_player_stop(void) {
    pthread_mutex_lock(&g.mu);
    if (!g.running) {
        free_chunks_locked();
        if (g.fd >= 0) { close(g.fd); g.fd = -1; }
        pthread_mutex_unlock(&g.mu);
        return 0;
    }
    g.cancel = 1;
    /* 尽早让硬件停发声：DROP 清空内核环形缓冲 */
    if (g.fd >= 0) ioctl(g.fd, SNDRV_PCM_IOCTL_DROP, 0);
    pthread_cond_broadcast(&g.cv);
    /* 等 worker 自己退出，fd/chunks 由 worker 清理 */
    join_worker_locked_unlocked();
    pthread_mutex_unlock(&g.mu);
    printf("[tts] stop (flush)\n");
    return 0;
}

int tts_player_is_active(void) {
    pthread_mutex_lock(&g.mu);
    int r = g.running;
    pthread_mutex_unlock(&g.mu);
    return r;
}

void tts_player_shutdown(void) { tts_player_stop(); }
