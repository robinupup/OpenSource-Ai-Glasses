/**
 * 麦克风采集实现（两后端，自动探测）：
 *   1) 若存在 /usr/bin/arecord，fork+exec 从 stdout 读 PCM（老路径）
 *   2) 否则直接用内核原生 ALSA API 读 /dev/snd/pcmC0D0c
 *
 * 设计与 audio_player.c 对称：HW_PARAMS/SW_PARAMS/PREPARE/START，然后
 * SNDRV_PCM_IOCTL_READI_FRAMES 阻塞读帧；遇到 EPIPE/EBADFD/ESTRPIPE
 * 自动 PREPARE 恢复（overrun）。
 */

#include "mic_capture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <time.h>
#include <sound/asound.h>

#define ARECORD_BIN      "/usr/bin/arecord"
#define ALSA_PCM_CAPTURE "/dev/snd/pcmC0D0c"

struct mic_stream {
    /* arecord 后端 */
    pid_t pid;
    int   pipe_fd;

    /* 原生 ALSA 后端 */
    int   alsa_fd;
    int   frame_bytes;

    /* 外部请求尽快返回（用于打断 pump 线程内的阻塞 ioctl/read） */
    volatile int stopping;
};

static void fill_defaults(mic_params_t *p) {
    if (p->sample_rate <= 0) p->sample_rate = 16000;
    if (p->channels    <= 0) p->channels    = 1;
    if (p->bit_width   <= 0) p->bit_width   = 16;
}

static const char *fmt_string(int bits) {
    switch (bits) {
        case 8:  return "U8";
        case 24: return "S24_LE";
        case 32: return "S32_LE";
        default: return "S16_LE";
    }
}

static int have_arecord(void) {
    return access(ARECORD_BIN, X_OK) == 0;
}

/* ========================================================================
 * 原生 ALSA capture
 * ======================================================================== */
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

static int alsa_open_capture(int rate, int channels, int bit_width) {
    int fd = open(ALSA_PCM_CAPTURE, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "[mic] open %s: %s\n", ALSA_PCM_CAPTURE, strerror(errno));
        return -1;
    }

    struct snd_pcm_hw_params hp;
    memset(&hp, 0, sizeof(hp));
    for (int i = SNDRV_PCM_HW_PARAM_FIRST_MASK; i <= SNDRV_PCM_HW_PARAM_LAST_MASK; i++) {
        struct snd_mask *m = &hp.masks[i - SNDRV_PCM_HW_PARAM_FIRST_MASK];
        memset(m, 0xff, sizeof(*m));
    }
    for (int i = SNDRV_PCM_HW_PARAM_FIRST_INTERVAL; i <= SNDRV_PCM_HW_PARAM_LAST_INTERVAL; i++) {
        struct snd_interval *v = &hp.intervals[i - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
        v->min = 0; v->max = ~0;  v->openmin = v->openmax = 0;
        v->integer = 0; v->empty = 0;
    }
    hp.rmask = ~0U;
    hp.cmask = 0;
    hp.info  = ~0U;

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

    struct snd_interval *pt =
        &hp.intervals[SNDRV_PCM_HW_PARAM_PERIOD_TIME - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
    pt->min = 10000; pt->max = 50000;
    pt->integer = 0; pt->openmin = pt->openmax = 0; pt->empty = 0;
    struct snd_interval *pc =
        &hp.intervals[SNDRV_PCM_HW_PARAM_PERIODS - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
    pc->min = 2; pc->max = 8; pc->integer = 1;

    if (ioctl(fd, SNDRV_PCM_IOCTL_HW_PARAMS, &hp) < 0) {
        fprintf(stderr, "[mic] HW_PARAMS: %s\n", strerror(errno));
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
    sw.start_threshold   = 1;
    sw.stop_threshold    = buffer_frames;
    sw.silence_threshold = 0;
    sw.silence_size      = 0;
    sw.boundary = buffer_frames;
    while (sw.boundary < 0x40000000U) sw.boundary *= 2;

    if (ioctl(fd, SNDRV_PCM_IOCTL_SW_PARAMS, &sw) < 0) {
        fprintf(stderr, "[mic] SW_PARAMS: %s\n", strerror(errno));
        close(fd); return -1;
    }
    if (ioctl(fd, SNDRV_PCM_IOCTL_PREPARE, 0) < 0) {
        fprintf(stderr, "[mic] PREPARE: %s\n", strerror(errno));
        close(fd); return -1;
    }
    /* capture 需要显式 START */
    if (ioctl(fd, SNDRV_PCM_IOCTL_START, 0) < 0) {
        fprintf(stderr, "[mic] START: %s\n", strerror(errno));
        close(fd); return -1;
    }

    return fd;
}

static ssize_t alsa_read_once(int fd, void *buf, size_t size, int frame_bytes) {
    if (size < (size_t)frame_bytes) return 0;
    struct snd_xferi xfer;
    xfer.buf    = buf;
    xfer.frames = size / frame_bytes;
    xfer.result = 0;
    int rc = ioctl(fd, SNDRV_PCM_IOCTL_READI_FRAMES, &xfer);
    if (rc < 0) {
        if (errno == EPIPE || errno == EBADFD || errno == ESTRPIPE) {
            /* overrun 恢复 */
            if (ioctl(fd, SNDRV_PCM_IOCTL_PREPARE, 0) < 0) return -1;
            if (ioctl(fd, SNDRV_PCM_IOCTL_START, 0)   < 0) return -1;
            return 0;
        }
        if (errno == EAGAIN || errno == EINTR) return 0;
        fprintf(stderr, "[mic] read: %s\n", strerror(errno));
        return -1;
    }
    return (ssize_t)xfer.result * frame_bytes;
}

/* ========================================================================
 * Public API
 * ======================================================================== */
mic_stream_t *mic_capture_start(const mic_params_t *params) {
    mic_params_t p = {0};
    if (params) p = *params;
    fill_defaults(&p);

    mic_stream_t *s = (mic_stream_t *)calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->pid = -1; s->pipe_fd = -1; s->alsa_fd = -1;

    if (have_arecord()) {
        int pipefd[2];
        if (pipe(pipefd) != 0) { free(s); return NULL; }
        pid_t pid = fork();
        if (pid < 0) { close(pipefd[0]); close(pipefd[1]); free(s); return NULL; }
        if (pid == 0) {
            dup2(pipefd[1], STDOUT_FILENO);
            close(pipefd[0]); close(pipefd[1]);
            char sr[16], ch[16];
            snprintf(sr, sizeof(sr), "%d", p.sample_rate);
            snprintf(ch, sizeof(ch), "%d", p.channels);
            execl(ARECORD_BIN, "arecord", "-q", "-t", "raw",
                  "-r", sr, "-c", ch, "-f", fmt_string(p.bit_width),
                  (char *)NULL);
            _exit(127);
        }
        close(pipefd[1]);
        s->pid = pid;
        s->pipe_fd = pipefd[0];
        return s;
    }

    /* 原生 ALSA */
    int fd = alsa_open_capture(p.sample_rate, p.channels, p.bit_width);
    if (fd < 0) { free(s); return NULL; }
    s->alsa_fd     = fd;
    s->frame_bytes = (p.bit_width / 8) * p.channels;
    return s;
}

ssize_t mic_capture_read(mic_stream_t *stream, void *buf, size_t size) {
    if (!stream) return -1;

    if (stream->pipe_fd >= 0) {
        ssize_t n;
        do { n = read(stream->pipe_fd, buf, size); }
        while (n < 0 && errno == EINTR);
        return n;
    }

    if (stream->alsa_fd >= 0) {
        /* 循环读至少一帧；但检测 stopping 时立即返回 -1，让 pump 线程退出。*/
        for (;;) {
            if (stream->stopping) return -1;
            ssize_t n = alsa_read_once(stream->alsa_fd, buf, size, stream->frame_bytes);
            if (n != 0) return n;
            if (stream->stopping) return -1;
        }
    }

    return -1;
}

void mic_capture_stop(mic_stream_t *stream) {
    if (!stream) return;
    if (stream->pid > 0) {
        kill(stream->pid, SIGTERM);
        waitpid(stream->pid, NULL, 0);
    }
    if (stream->pipe_fd >= 0) close(stream->pipe_fd);
    if (stream->alsa_fd  >= 0) {
        ioctl(stream->alsa_fd, SNDRV_PCM_IOCTL_DROP, 0);
        close(stream->alsa_fd);
    }
    free(stream);
}

/* ========================================================================
 * 便捷：录制 N 秒到 WAV
 * ======================================================================== */
static void wav_write_header(FILE *fp, int rate, int ch, int bits, uint32_t data_size) {
    uint32_t chunk_size = 36 + data_size;
    uint16_t audio_format = 1; /* PCM */
    uint32_t byte_rate   = rate * ch * (bits / 8);
    uint16_t block_align = ch * (bits / 8);
    uint32_t fmt_size    = 16;

    fwrite("RIFF", 1, 4, fp);
    fwrite(&chunk_size, 4, 1, fp);
    fwrite("WAVE", 1, 4, fp);
    fwrite("fmt ", 1, 4, fp);
    fwrite(&fmt_size, 4, 1, fp);
    fwrite(&audio_format, 2, 1, fp);
    uint16_t channels16 = (uint16_t)ch;
    fwrite(&channels16, 2, 1, fp);
    uint32_t rate32 = (uint32_t)rate;
    fwrite(&rate32, 4, 1, fp);
    fwrite(&byte_rate, 4, 1, fp);
    fwrite(&block_align, 2, 1, fp);
    uint16_t bits16 = (uint16_t)bits;
    fwrite(&bits16, 2, 1, fp);
    fwrite("data", 1, 4, fp);
    fwrite(&data_size, 4, 1, fp);
}

int mic_capture_to_wav(const char *output_path, int duration_sec,
                       const mic_params_t *params) {
    if (!output_path || duration_sec <= 0) return -1;

    mic_params_t p = {0};
    if (params) p = *params;
    fill_defaults(&p);

    /* 老路径：直接让 arecord 落盘 */
    if (have_arecord()) {
        pid_t pid = fork();
        if (pid < 0) return -1;
        if (pid == 0) {
            char sr[16], ch[16], dur[16];
            snprintf(sr, sizeof(sr), "%d", p.sample_rate);
            snprintf(ch, sizeof(ch), "%d", p.channels);
            snprintf(dur, sizeof(dur), "%d", duration_sec);
            execl(ARECORD_BIN, "arecord", "-q", "-t", "wav",
                  "-r", sr, "-c", ch, "-f", fmt_string(p.bit_width),
                  "-d", dur, output_path, (char *)NULL);
            _exit(127);
        }
        int status = 0;
        waitpid(pid, &status, 0);
        return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
    }

    /* 原生 ALSA：读 duration_sec 秒 -> WAV */
    mic_stream_t *s = mic_capture_start(&p);
    if (!s) return -1;

    FILE *fp = fopen(output_path, "wb");
    if (!fp) { mic_capture_stop(s); return -1; }

    wav_write_header(fp, p.sample_rate, p.channels, p.bit_width, 0);

    size_t total_target =
        (size_t)p.sample_rate * p.channels * (p.bit_width / 8) * (size_t)duration_sec;
    uint8_t buf[4096];
    size_t captured = 0;
    while (captured < total_target) {
        size_t want = sizeof(buf);
        if (captured + want > total_target) want = total_target - captured;
        ssize_t n = mic_capture_read(s, buf, want);
        if (n <= 0) break;
        fwrite(buf, 1, (size_t)n, fp);
        captured += (size_t)n;
    }

    /* 回填实际 data_size */
    fflush(fp);
    fseek(fp, 0, SEEK_SET);
    wav_write_header(fp, p.sample_rate, p.channels, p.bit_width, (uint32_t)captured);
    fclose(fp);

    mic_capture_stop(s);
    return captured > 0 ? 0 : -1;
}

/* ========================================================================
 * Pump 模式：后台线程 + 回调
 * ======================================================================== */
struct mic_pump {
    pthread_t        thread;
    volatile int     running;
    mic_stream_t    *stream;
    mic_pcm_cb_t     cb;
    void            *user_data;
    size_t           chunk_bytes;
};

/* 供 mic_pump_stop 强行打断 pump 线程里阻塞的 READI_FRAMES ioctl。
 * RK 内核上 SNDRV_PCM_IOCTL_DROP 并不能可靠地唤醒另一个线程正在等待的
 * __snd_pcm_lib_xfer——必须靠给那个线程发一个不带 SA_RESTART 的信号，
 * 让 ioctl 以 -EINTR 返回。 */
static void mic_pump_sig_noop(int sig) { (void)sig; }

static void pump_thread_install_intr_sig(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = mic_pump_sig_noop;
    sa.sa_flags   = 0;          /* 关键：不带 SA_RESTART，ioctl 必须返回 -EINTR */
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, NULL);
}

static void *pump_thread_main(void *arg) {
    mic_pump_t *pump = (mic_pump_t *)arg;

    /* 解除 SIGUSR1 屏蔽，让 pthread_kill(thread, SIGUSR1) 能打断 ioctl */
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    pthread_sigmask(SIG_UNBLOCK, &set, NULL);

    size_t chunk = pump->chunk_bytes > 0 ? pump->chunk_bytes : 4096;
    uint8_t *buf = (uint8_t *)malloc(chunk);
    if (!buf) { pump->running = 0; return NULL; }

    while (pump->running) {
        ssize_t n = mic_capture_read(pump->stream, buf, chunk);
        if (n < 0) break;
        if (n == 0) continue;
        if (pump->cb) pump->cb(buf, (size_t)n, pump->user_data);
    }
    free(buf);
    return NULL;
}

mic_pump_t *mic_pump_start(const mic_params_t *params, size_t chunk_bytes,
                            mic_pcm_cb_t cb, void *user_data) {
    if (!cb) return NULL;
    /* 全进程安装一次 SIGUSR1 的 no-op handler，用于停机时打断 ioctl */
    pump_thread_install_intr_sig();
    mic_stream_t *s = mic_capture_start(params);
    if (!s) return NULL;
    mic_pump_t *pump = (mic_pump_t *)calloc(1, sizeof(*pump));
    if (!pump) { mic_capture_stop(s); return NULL; }
    pump->stream       = s;
    pump->cb           = cb;
    pump->user_data    = user_data;
    pump->chunk_bytes  = chunk_bytes;
    pump->running      = 1;
    if (pthread_create(&pump->thread, NULL, pump_thread_main, pump) != 0) {
        mic_capture_stop(s);
        free(pump);
        return NULL;
    }
    return pump;
}

void mic_pump_stop(mic_pump_t *pump) {
    if (!pump) return;
    pump->running = 0;

    /* 1) 标记 stream 正在退出 —— mic_capture_read 的外层循环每圈会检查 */
    if (pump->stream) {
        pump->stream->stopping = 1;
        if (pump->stream->alsa_fd >= 0) {
            ioctl(pump->stream->alsa_fd, SNDRV_PCM_IOCTL_DROP, 0);
        }
        if (pump->stream->pid > 0) {
            kill(pump->stream->pid, SIGTERM);
        }
    }

    /* 2) 强行打断 pump 线程里可能阻塞的 READI_FRAMES ioctl：
     *    RK 内核实测 SNDRV_PCM_IOCTL_DROP 并不总能唤醒同一个 fd 上另一线程
     *    的 __snd_pcm_lib_xfer 等待。靠给那个线程发 SIGUSR1（无 SA_RESTART），
     *    ioctl 必被打断返回 -EINTR；外层循环检测到 stopping 就退出。
     *    发若干次覆盖 signal-before-ioctl 的时序 race。*/
    for (int i = 0; i < 10; i++) {
        if (pthread_kill(pump->thread, 0) != 0) break;  /* 线程已退出 */
        pthread_kill(pump->thread, SIGUSR1);
        struct timespec ts = { 0, 10 * 1000 * 1000 };   /* 10ms */
        nanosleep(&ts, NULL);
    }

    pthread_join(pump->thread, NULL);
    mic_capture_stop(pump->stream);
    free(pump);
}
