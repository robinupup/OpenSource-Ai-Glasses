/**
 * 音频播放实现：两条路径，自动探测
 *   1) 若存在 /usr/bin/aplay（alsa-utils），fork+exec 调用（老路径）
 *   2) 否则直接用内核原生 ALSA API 写 /dev/snd/pcmC0D0p
 *      支持 S8 / S16_LE / S24_LE / S32_LE 的 WAV 与 raw PCM
 */

#include "audio_player.h"
#include "event_bus.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <pthread.h>
#include <sound/asound.h>

#define APLAY_BIN         "/usr/bin/aplay"
#define AMIXER_BIN        "/usr/bin/amixer"
#define ALSA_PCM_DEVICE   "/dev/snd/pcmC0D0p"

static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
static pid_t           g_aplay_pid = -1;   /* 用 aplay 时使用 */
static pthread_t       g_worker    = 0;    /* 直连 ALSA 时的异步播放线程 */
static volatile int    g_worker_running = 0;
static volatile int    g_worker_cancel  = 0;
static int             g_have_aplay     = 0;

int audio_player_init(void) {
    g_have_aplay = (access(APLAY_BIN, X_OK) == 0);
    if (!g_have_aplay) {
        if (access(ALSA_PCM_DEVICE, R_OK | W_OK) != 0) {
            fprintf(stderr, "[audio] neither aplay nor %s available\n", ALSA_PCM_DEVICE);
            return -1;
        }
        printf("[audio] using native ALSA (%s)\n", ALSA_PCM_DEVICE);
    } else {
        printf("[audio] using aplay\n");
    }
    return 0;
}

/* =========================================================================
 * WAV 头解析
 * ========================================================================= */
typedef struct {
    int   channels;
    int   sample_rate;
    int   bit_width;
    long  data_offset;
    long  data_size;
} wav_info_t;

static int parse_wav_header(FILE *fp, wav_info_t *w) {
    uint8_t hdr[12];
    if (fread(hdr, 1, 12, fp) != 12) return -1;
    if (memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) return -1;

    uint8_t chunk[8];
    while (fread(chunk, 1, 8, fp) == 8) {
        uint32_t sz = (uint32_t)chunk[4] | ((uint32_t)chunk[5] << 8)
                    | ((uint32_t)chunk[6] << 16) | ((uint32_t)chunk[7] << 24);
        if (memcmp(chunk, "fmt ", 4) == 0) {
            uint8_t fmt[16];
            if (fread(fmt, 1, 16, fp) != 16) return -1;
            w->channels    = fmt[2] | (fmt[3] << 8);
            w->sample_rate = fmt[4] | (fmt[5] << 8) | (fmt[6] << 16) | (fmt[7] << 24);
            w->bit_width   = fmt[14] | (fmt[15] << 8);
            if (sz > 16) fseek(fp, sz - 16, SEEK_CUR);
        } else if (memcmp(chunk, "data", 4) == 0) {
            w->data_offset = ftell(fp);
            w->data_size   = sz;
            return 0;
        } else {
            fseek(fp, sz, SEEK_CUR);
        }
    }
    return -1;
}

/* =========================================================================
 * 原生 ALSA：直接操作 /dev/snd/pcmC0D0p
 * ========================================================================= */
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

/**
 * 以给定参数打开并配置 PCM 设备（playback）。成功返回 fd。
 * 备注：这里采用"让驱动自行决定周期大小"，仅约束格式/通道/采样率。
 */
static int alsa_open_playback(int rate, int channels, int bit_width,
                              unsigned int *period_frames_out) {
    int fd = open(ALSA_PCM_DEVICE, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "[audio] open %s: %s\n", ALSA_PCM_DEVICE, strerror(errno));
        return -1;
    }

    struct snd_pcm_hw_params hp;
    memset(&hp, 0, sizeof(hp));
    /* 所有参数先 "不限制" */
    for (int i = SNDRV_PCM_HW_PARAM_FIRST_MASK; i <= SNDRV_PCM_HW_PARAM_LAST_MASK; i++) {
        struct snd_mask *m = &hp.masks[i - SNDRV_PCM_HW_PARAM_FIRST_MASK];
        memset(m, 0xff, sizeof(*m));
    }
    for (int i = SNDRV_PCM_HW_PARAM_FIRST_INTERVAL; i <= SNDRV_PCM_HW_PARAM_LAST_INTERVAL; i++) {
        struct snd_interval *v = &hp.intervals[i - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
        v->min = 0;  v->max = ~0;  v->openmin = v->openmax = 0;
        v->integer = 0; v->empty = 0;
    }
    hp.rmask = ~0U;
    hp.cmask = 0;
    hp.info  = ~0U;

    /* 绑定关键参数 */
    mask_set(&hp.masks[SNDRV_PCM_HW_PARAM_ACCESS - SNDRV_PCM_HW_PARAM_FIRST_MASK],
             SNDRV_PCM_ACCESS_RW_INTERLEAVED);
    mask_set(&hp.masks[SNDRV_PCM_HW_PARAM_FORMAT - SNDRV_PCM_HW_PARAM_FIRST_MASK],
             alsa_snd_format(bit_width));
    mask_set(&hp.masks[SNDRV_PCM_HW_PARAM_SUBFORMAT - SNDRV_PCM_HW_PARAM_FIRST_MASK],
             SNDRV_PCM_SUBFORMAT_STD);
    interval_set(&hp.intervals[SNDRV_PCM_HW_PARAM_CHANNELS - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL],
                 (unsigned int)channels);
    interval_set(&hp.intervals[SNDRV_PCM_HW_PARAM_RATE - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL],
                 (unsigned int)rate);
    /* 让驱动自选 period/buffer 大小 - 给个合理范围 */
    struct snd_interval *pt =
        &hp.intervals[SNDRV_PCM_HW_PARAM_PERIOD_TIME - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
    pt->min = 10000; pt->max = 50000; /* us */
    pt->integer = 0; pt->openmin = pt->openmax = 0; pt->empty = 0;
    struct snd_interval *pc =
        &hp.intervals[SNDRV_PCM_HW_PARAM_PERIODS - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
    pc->min = 2; pc->max = 8; pc->integer = 1;

    if (ioctl(fd, SNDRV_PCM_IOCTL_HW_PARAMS, &hp) < 0) {
        fprintf(stderr, "[audio] HW_PARAMS: %s\n", strerror(errno));
        close(fd); return -1;
    }

    unsigned int period_frames =
        hp.intervals[SNDRV_PCM_HW_PARAM_PERIOD_SIZE - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL].min;
    unsigned int buffer_frames =
        hp.intervals[SNDRV_PCM_HW_PARAM_BUFFER_SIZE - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL].min;

    if (buffer_frames == 0) buffer_frames = period_frames * 4;
    struct snd_pcm_sw_params sw;
    memset(&sw, 0, sizeof(sw));
    sw.tstamp_mode       = SNDRV_PCM_TSTAMP_NONE;
    sw.period_step       = 1;
    sw.avail_min         = period_frames;
    sw.start_threshold   = 1;                 /* 收到第一帧即开始播放 */
    sw.stop_threshold    = buffer_frames;
    sw.silence_threshold = 0;
    sw.silence_size      = 0;
    /* boundary 必须是 buffer_size 的整数倍且为 2 的幂 */
    sw.boundary = buffer_frames;
    while (sw.boundary < 0x40000000U) sw.boundary *= 2;
    if (ioctl(fd, SNDRV_PCM_IOCTL_SW_PARAMS, &sw) < 0) {
        fprintf(stderr, "[audio] SW_PARAMS: %s\n", strerror(errno));
        close(fd); return -1;
    }
    if (ioctl(fd, SNDRV_PCM_IOCTL_PREPARE, 0) < 0) {
        fprintf(stderr, "[audio] PREPARE: %s\n", strerror(errno));
        close(fd); return -1;
    }

    if (period_frames_out) *period_frames_out = period_frames;
    return fd;
}

/**
 * 写 N 帧到 PCM。遇到 underrun 自动恢复。
 * @return 成功返回 0；被 cancel 返回 1；失败 -1
 */
static int alsa_write_loop(int fd, const uint8_t *data, size_t bytes,
                            int frame_bytes) {
    size_t off = 0;
    while (off < bytes) {
        if (g_worker_cancel) return 1;
        size_t remain_frames = (bytes - off) / frame_bytes;
        if (remain_frames == 0) break;

        struct snd_xferi xfer;
        xfer.buf    = (void *)(data + off);
        xfer.frames = remain_frames;
        xfer.result = 0;
        int rc = ioctl(fd, SNDRV_PCM_IOCTL_WRITEI_FRAMES, &xfer);
        if (rc < 0) {
            /* EPIPE = XRUN, EBADFD = SETUP/PREPARED 状态不符, ESTRPIPE = 挂起 */
            if (errno == EPIPE || errno == EBADFD || errno == ESTRPIPE) {
                if (ioctl(fd, SNDRV_PCM_IOCTL_PREPARE, 0) < 0) {
                    fprintf(stderr, "[audio] recover PREPARE: %s\n", strerror(errno));
                    return -1;
                }
                continue;
            }
            if (errno == EAGAIN || errno == EINTR) continue;
            fprintf(stderr, "[audio] write: %s\n", strerror(errno));
            return -1;
        }
        off += (size_t)xfer.result * frame_bytes;
    }
    ioctl(fd, SNDRV_PCM_IOCTL_DRAIN, 0);
    return 0;
}

/* ========================================================================= */

typedef struct {
    audio_play_params_t p;
    char path[256];
} play_job_t;

static int do_play_native(const audio_play_params_t *p) {
    FILE *fp = fopen(p->file_path, "rb");
    if (!fp) return -1;

    int rate = p->sample_rate > 0 ? p->sample_rate : 16000;
    int ch   = p->channels    > 0 ? p->channels    : 1;
    int bw   = p->bit_width   > 0 ? p->bit_width   : 16;
    long data_offset = 0;
    long data_size   = -1;

    if (!p->is_raw) {
        wav_info_t w;
        if (parse_wav_header(fp, &w) != 0) {
            fprintf(stderr, "[audio] invalid wav: %s\n", p->file_path);
            fclose(fp);
            return -1;
        }
        rate = w.sample_rate;
        ch   = w.channels;
        bw   = w.bit_width;
        data_offset = w.data_offset;
        data_size   = w.data_size;
        fseek(fp, data_offset, SEEK_SET);
    }

    int fd = alsa_open_playback(rate, ch, bw, NULL);
    if (fd < 0) { fclose(fp); return -1; }

    int frame_bytes = (bw / 8) * ch;
    uint8_t buf[4096];
    long fed = 0;
    int  rc  = 0;
    while (!g_worker_cancel) {
        size_t want = sizeof(buf);
        if (data_size > 0 && fed + (long)want > data_size)
            want = data_size - fed;
        if (want == 0) break;
        size_t n = fread(buf, 1, want, fp);
        if (n == 0) break;
        fed += (long)n;
        size_t frames = n / frame_bytes;
        if (frames == 0) break;
        rc = alsa_write_loop(fd, buf, frames * frame_bytes, frame_bytes);
        if (rc < 0) break;
    }
    ioctl(fd, SNDRV_PCM_IOCTL_DRAIN, 0);
    close(fd);
    fclose(fp);
    return (rc < 0) ? -1 : 0;
}

/* =========================================================================
 * aplay 后端（老路径）
 * ========================================================================= */
static int spawn_aplay(const audio_play_params_t *p) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        char sr[16], ch[16], fmt[16];
        if (p->is_raw) {
            snprintf(sr, sizeof(sr), "%d", p->sample_rate > 0 ? p->sample_rate : 16000);
            snprintf(ch, sizeof(ch), "%d", p->channels > 0 ? p->channels : 1);
            const char *fmtstr = "S16_LE";
            if (p->bit_width == 8)       fmtstr = "U8";
            else if (p->bit_width == 24) fmtstr = "S24_LE";
            else if (p->bit_width == 32) fmtstr = "S32_LE";
            snprintf(fmt, sizeof(fmt), "%s", fmtstr);
            execl(APLAY_BIN, "aplay", "-q",
                  "-t", "raw", "-r", sr, "-c", ch, "-f", fmt,
                  p->file_path, (char *)NULL);
        } else {
            execl(APLAY_BIN, "aplay", "-q", p->file_path, (char *)NULL);
        }
        _exit(127);
    }
    return (int)pid;
}

/* =========================================================================
 * 音量
 * ========================================================================= */
int audio_player_set_volume(int volume) {
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    if (access(AMIXER_BIN, X_OK) != 0) return -1;
    char cmd[128];
    const char *controls[] = {"Master", "Speaker", "Playback", "PCM", NULL};
    for (int i = 0; controls[i]; i++) {
        snprintf(cmd, sizeof(cmd), "%s sset '%s' %d%% >/dev/null 2>&1",
                 AMIXER_BIN, controls[i], volume);
        if (system(cmd) == 0) return 0;
    }
    return -1;
}

/* =========================================================================
 * 公共 play 函数
 * ========================================================================= */
static void *native_worker_thread(void *arg) {
    play_job_t *job = (play_job_t *)arg;
    g_worker_running = 1;
    g_worker_cancel  = 0;
    event_bus_publish("audio/play_start", job->path, strlen(job->path) + 1);
    do_play_native(&job->p);
    event_bus_publish("audio/play_finish", job->path, strlen(job->path) + 1);
    g_worker_running = 0;
    free(job);
    return NULL;
}

int audio_player_play(const audio_play_params_t *p) {
    if (!p || !p->file_path) return -1;
    if (access(p->file_path, R_OK) != 0) {
        fprintf(stderr, "[audio] file not readable: %s\n", p->file_path);
        return -1;
    }
    if (p->volume >= 0) audio_player_set_volume(p->volume);

    audio_player_stop(); /* 抢占当前播放 */

    if (g_have_aplay) {
        pthread_mutex_lock(&g_mutex);
        int pid = spawn_aplay(p);
        if (pid < 0) { pthread_mutex_unlock(&g_mutex); return -1; }
        g_aplay_pid = (pid_t)pid;
        pthread_mutex_unlock(&g_mutex);
        event_bus_publish("audio/play_start", p->file_path, strlen(p->file_path) + 1);
        if (!p->async) {
            waitpid((pid_t)pid, NULL, 0);
            pthread_mutex_lock(&g_mutex);
            if (g_aplay_pid == (pid_t)pid) g_aplay_pid = -1;
            pthread_mutex_unlock(&g_mutex);
            event_bus_publish("audio/play_finish", p->file_path, strlen(p->file_path) + 1);
        }
        return 0;
    }

    /* 原生 ALSA 路径 */
    if (p->async) {
        play_job_t *job = (play_job_t *)calloc(1, sizeof(*job));
        if (!job) return -1;
        job->p = *p;
        strncpy(job->path, p->file_path, sizeof(job->path) - 1);
        job->p.file_path = job->path; /* 保证指针在生命期内有效 */
        pthread_mutex_lock(&g_mutex);
        if (pthread_create(&g_worker, NULL, native_worker_thread, job) != 0) {
            pthread_mutex_unlock(&g_mutex);
            free(job);
            return -1;
        }
        pthread_detach(g_worker);
        pthread_mutex_unlock(&g_mutex);
        return 0;
    } else {
        event_bus_publish("audio/play_start", p->file_path, strlen(p->file_path) + 1);
        int rc = do_play_native(p);
        event_bus_publish("audio/play_finish", p->file_path, strlen(p->file_path) + 1);
        return rc;
    }
}

int audio_player_play_wav(const char *file_path) {
    audio_play_params_t p = {0};
    p.file_path = file_path; p.volume = -1; p.is_raw = 0; p.async = 0;
    return audio_player_play(&p);
}

int audio_player_play_wav_async(const char *file_path) {
    audio_play_params_t p = {0};
    p.file_path = file_path; p.volume = -1; p.is_raw = 0; p.async = 1;
    return audio_player_play(&p);
}

int audio_player_stop(void) {
    pthread_mutex_lock(&g_mutex);
    if (g_aplay_pid > 0) {
        kill(g_aplay_pid, SIGTERM);
        waitpid(g_aplay_pid, NULL, 0);
        g_aplay_pid = -1;
    }
    if (g_worker_running) {
        g_worker_cancel = 1;
        /* 异步线程自行退出；这里简单 busy wait */
        for (int i = 0; i < 50 && g_worker_running; i++) usleep(20000);
    }
    pthread_mutex_unlock(&g_mutex);
    return 0;
}

void audio_player_shutdown(void) { audio_player_stop(); }
