/**
 * my-app core - Silero VAD (see vad.h for overview)
 *
 * 与 english_practice/device/vad.py 对齐（Silero V5 契约）：
 *   - 每帧 512 样本（32 ms @16 kHz），但模型 input 要求 64+512=576：
 *     把上一窗口末尾 64 样本作为 context 拼到当前 512 前面。
 *     ** 不做这一步，probs 会一直接近 0，VAD 永远不会开口。 **
 *   - 双阈值迟滞：activation=0.5 / deactivation=0.35，SPEAKING 状态下 >0.35
 *     就算持续；避免单帧抖动误闭口。
 *   - ExpFilter(alpha=0.35) 平滑原始 prob，压单帧毛刺。
 *   - min_speech_ms / min_silence_ms 基于时长门限。
 *   - prefix-padding 环形缓冲：SPEECH_START 时把前 0.5 s 音频一次性转发，
 *     避免 ASR 漏首字（和 device_main.py 行为一致）。
 */
#include "vad.h"
#include "onnxruntime_c_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FRAME_SAMPLES   512       /* Silero V5 @16kHz window */
#define CONTEXT_SAMPLES 64        /* Silero V5 @16kHz context */
#define INPUT_SAMPLES   (CONTEXT_SAMPLES + FRAME_SAMPLES)  /* 576 */
#define STATE_SIZE      (2 * 1 * 128)
#define FRAME_MS        32

/* prefix padding: 0.5 s = ~16 帧 */
#define PREFIX_FRAMES   16

/* 双阈值迟滞 + 平滑（对齐 Python 端） */
#define TH_DEACT        0.35f
#define SMOOTH_ALPHA    0.35f     /* smoothed = 0.35*old + 0.65*new */

typedef enum { ST_SILENCE, ST_SPEAKING } state_t;

struct vad {
    vad_config_t      cfg;

    const OrtApi     *ort;
    OrtEnv           *env;
    OrtSessionOptions*opts;
    OrtSession       *sess;
    OrtMemoryInfo    *mem;

    /* 本帧 ORT 输入：float32[1,576] = [context(64) | current(512)] */
    float             input_buf[INPUT_SAMPLES];
    float             state_buf[STATE_SIZE];
    int64_t           sr_val;

    /* 原始 int16 PCM 碎片缓冲：累到 FRAME_SAMPLES 才跑一次推理 */
    int16_t           pend_i16[FRAME_SAMPLES];
    size_t            pend_n;

    /* 平滑 */
    float             smooth_val;
    int               smooth_has;

    /* prefix-padding 环形缓冲（原始 int16 PCM，每项 FRAME_SAMPLES 样本） */
    int16_t           prefix_buf[PREFIX_FRAMES][FRAME_SAMPLES];
    int               prefix_head;   /* 下一个写入位置 */
    int               prefix_count;  /* 当前已缓冲的帧数（<=PREFIX_FRAMES） */

    /* 状态机 */
    state_t           st;
    int               speech_frames;
    int               silence_frames;
    int               speech_frames_need;
    int               silence_frames_need;
};

/* --- helpers ------------------------------------------------------------- */

static int vad_ort_load(vad_t *v, const char *model_path) {
    v->ort = OrtGetApiBase()->GetApi(ORT_API_VERSION);
    if (!v->ort) { fprintf(stderr, "[vad] OrtGetApiBase failed\n"); return -1; }

    OrtStatus *st = v->ort->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "my-app-vad", &v->env);
    if (st) { fprintf(stderr, "[vad] CreateEnv: %s\n", v->ort->GetErrorMessage(st));
              v->ort->ReleaseStatus(st); return -1; }

    v->ort->CreateSessionOptions(&v->opts);
    v->ort->SetIntraOpNumThreads(v->opts, 1);
    v->ort->SetInterOpNumThreads(v->opts, 1);
    v->ort->SetSessionGraphOptimizationLevel(v->opts, ORT_ENABLE_ALL);

    st = v->ort->CreateSession(v->env, model_path, v->opts, &v->sess);
    if (st) {
        fprintf(stderr, "[vad] CreateSession(%s): %s\n", model_path,
                v->ort->GetErrorMessage(st));
        v->ort->ReleaseStatus(st);
        return -1;
    }

    v->ort->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &v->mem);
    v->sr_val = 16000;
    return 0;
}

/* 跑一帧推理；input_buf[0..63] 必须是 context，[64..575] 是当前 512 样本。
 * 返回 speech probability。 */
static float vad_ort_run(vad_t *v) {
    int64_t in_dims[2] = {1, INPUT_SAMPLES};
    int64_t st_dims[3] = {2, 1, 128};
    int64_t sr_dims[1] = {0};

    OrtValue *in_t = NULL, *st_t = NULL, *sr_t = NULL;
    v->ort->CreateTensorWithDataAsOrtValue(v->mem,
        v->input_buf, sizeof(v->input_buf), in_dims, 2,
        ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &in_t);
    v->ort->CreateTensorWithDataAsOrtValue(v->mem,
        v->state_buf, sizeof(v->state_buf), st_dims, 3,
        ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &st_t);
    v->ort->CreateTensorWithDataAsOrtValue(v->mem,
        &v->sr_val, sizeof(int64_t), sr_dims, 0,
        ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, &sr_t);

    const char *in_names[3]  = {"input", "state", "sr"};
    const char *out_names[2] = {"output", "stateN"};
    const OrtValue *ins[3]   = {in_t, st_t, sr_t};
    OrtValue *outs[2]        = {NULL, NULL};

    float prob = 0.0f;
    OrtStatus *st = v->ort->Run(v->sess, NULL, in_names, ins, 3,
                                 out_names, 2, outs);
    if (st) {
        fprintf(stderr, "[vad] Run: %s\n", v->ort->GetErrorMessage(st));
        v->ort->ReleaseStatus(st);
    } else {
        float *p = NULL;
        v->ort->GetTensorMutableData(outs[0], (void **)&p);
        if (p) prob = p[0];

        float *ns = NULL;
        v->ort->GetTensorMutableData(outs[1], (void **)&ns);
        if (ns) memcpy(v->state_buf, ns, sizeof(v->state_buf));
    }
    if (outs[0]) v->ort->ReleaseValue(outs[0]);
    if (outs[1]) v->ort->ReleaseValue(outs[1]);
    v->ort->ReleaseValue(in_t);
    v->ort->ReleaseValue(st_t);
    v->ort->ReleaseValue(sr_t);
    return prob;
}

static inline float smooth_apply(vad_t *v, float x) {
    if (!v->smooth_has) { v->smooth_val = x; v->smooth_has = 1; return x; }
    v->smooth_val = SMOOTH_ALPHA * v->smooth_val + (1.0f - SMOOTH_ALPHA) * x;
    return v->smooth_val;
}

/* 把一帧原始 PCM（刚跑完推理的 512 样本）推入 prefix 环形缓冲 */
static inline void prefix_push(vad_t *v, const int16_t *frame) {
    memcpy(v->prefix_buf[v->prefix_head], frame, FRAME_SAMPLES * sizeof(int16_t));
    v->prefix_head = (v->prefix_head + 1) % PREFIX_FRAMES;
    if (v->prefix_count < PREFIX_FRAMES) v->prefix_count++;
}

/* SPEECH_START 时依次吐出环形缓冲里的所有帧（按时序老→新） */
static void prefix_flush_as_chunks(vad_t *v) {
    if (v->prefix_count <= 0) return;
    int start = (v->prefix_head - v->prefix_count + PREFIX_FRAMES) % PREFIX_FRAMES;
    for (int i = 0; i < v->prefix_count; i++) {
        int idx = (start + i) % PREFIX_FRAMES;
        v->cfg.on_event(VAD_EV_SPEECH_CHUNK,
                        v->prefix_buf[idx], FRAME_SAMPLES,
                        0.f, v->cfg.user_data);
    }
    v->prefix_count = 0;
    v->prefix_head  = 0;
}

/* --- public -------------------------------------------------------------- */

vad_t *vad_create(const vad_config_t *cfg) {
    if (!cfg || !cfg->model_path || !cfg->on_event) return NULL;

    vad_t *v = calloc(1, sizeof(*v));
    if (!v) return NULL;
    v->cfg = *cfg;
    if (v->cfg.speech_threshold <= 0.f) v->cfg.speech_threshold = 0.5f;
    if (v->cfg.min_speech_ms    <= 0)   v->cfg.min_speech_ms    = 250;
    if (v->cfg.min_silence_ms   <= 0)   v->cfg.min_silence_ms   = 600;
    v->speech_frames_need  = (v->cfg.min_speech_ms  + FRAME_MS - 1) / FRAME_MS;
    v->silence_frames_need = (v->cfg.min_silence_ms + FRAME_MS - 1) / FRAME_MS;

    if (vad_ort_load(v, v->cfg.model_path) != 0) {
        vad_destroy(v);
        return NULL;
    }
    printf("[vad] ready: model=%s th_act=%.2f th_deact=%.2f speech>=%dms silence>=%dms "
           "(frames need speech>=%d silence>=%d, input=[1,%d])\n",
           v->cfg.model_path, v->cfg.speech_threshold, TH_DEACT,
           v->cfg.min_speech_ms, v->cfg.min_silence_ms,
           v->speech_frames_need, v->silence_frames_need, INPUT_SAMPLES);
    return v;
}

void vad_reset(vad_t *v) {
    if (!v) return;
    memset(v->state_buf, 0, sizeof(v->state_buf));
    memset(v->input_buf, 0, sizeof(v->input_buf));
    v->pend_n = 0;
    v->st = ST_SILENCE;
    v->speech_frames = 0;
    v->silence_frames = 0;
    v->smooth_has = 0;
    v->smooth_val = 0.f;
    v->prefix_head = 0;
    v->prefix_count = 0;
}

void vad_destroy(vad_t *v) {
    if (!v) return;
    if (v->mem)  v->ort->ReleaseMemoryInfo(v->mem);
    if (v->sess) v->ort->ReleaseSession(v->sess);
    if (v->opts) v->ort->ReleaseSessionOptions(v->opts);
    if (v->env)  v->ort->ReleaseEnv(v->env);
    free(v);
}

void vad_force_end(vad_t *v) {
    if (!v) return;
    if (v->st == ST_SPEAKING) {
        v->st = ST_SILENCE;
        v->speech_frames = 0;
        v->silence_frames = 0;
        v->cfg.on_event(VAD_EV_SPEECH_END, NULL, 0, 0.f, v->cfg.user_data);
    }
}

/* 受输入 pcm → 按 512 样本对齐切帧 → 每帧拼 64 context → Silero → 状态机 */
void vad_feed(vad_t *v, const int16_t *pcm, size_t samples) {
    if (!v || !pcm || samples == 0) return;

    size_t idx = 0;
    while (idx < samples) {
        size_t need = FRAME_SAMPLES - v->pend_n;
        size_t take = (samples - idx < need) ? (samples - idx) : need;
        memcpy(&v->pend_i16[v->pend_n], pcm + idx, take * sizeof(int16_t));
        v->pend_n += take;
        idx += take;
        if (v->pend_n < FRAME_SAMPLES) break;

        /* 拼接输入: input_buf[0..63] 保持上一帧的 context（已就绪），
         * input_buf[64..575] 为当前 512 样本 float32。 */
        float *cur = &v->input_buf[CONTEXT_SAMPLES];
        for (int i = 0; i < FRAME_SAMPLES; i++) {
            cur[i] = (float)v->pend_i16[i] / 32768.0f;
        }

        float raw_prob = vad_ort_run(v);
        float prob = smooth_apply(v, raw_prob);

        /* 更新 context = 本帧末尾 64 样本（留给下一帧拼接用） */
        memcpy(v->input_buf,
               &v->input_buf[INPUT_SAMPLES - CONTEXT_SAMPLES],
               CONTEXT_SAMPLES * sizeof(float));

        /* 双阈值迟滞：SPEAKING 状态下只要 > deact 就继续算"语音帧" */
        int speaking_now  = (v->st == ST_SPEAKING);
        int is_speech_frame =
            (prob >= v->cfg.speech_threshold) ||
            (speaking_now && prob > TH_DEACT);

        /* ---- 状态机 ---- */
        if (v->st == ST_SILENCE) {
            /* 静音时把原始 PCM 存入 prefix 环形缓冲 */
            prefix_push(v, v->pend_i16);

            if (is_speech_frame) {
                v->silence_frames = 0;
                v->speech_frames++;
                if (v->speech_frames >= v->speech_frames_need) {
                    v->st = ST_SPEAKING;
                    v->cfg.on_event(VAD_EV_SPEECH_START, NULL, 0, prob,
                                    v->cfg.user_data);
                    /* 把前 0.5s 的 PCM 一次性推给上层，避免 ASR 漏首字 */
                    prefix_flush_as_chunks(v);
                }
            } else {
                v->speech_frames = 0;
            }
        } else {   /* ST_SPEAKING */
            /* 持续把当前帧 512 样本推给上层（作为流式上行） */
            v->cfg.on_event(VAD_EV_SPEECH_CHUNK,
                            v->pend_i16, FRAME_SAMPLES,
                            prob, v->cfg.user_data);

            if (is_speech_frame) {
                v->silence_frames = 0;
                v->speech_frames++;
            } else {
                v->speech_frames = 0;
                v->silence_frames++;
                if (v->silence_frames >= v->silence_frames_need) {
                    v->st = ST_SILENCE;
                    v->silence_frames = 0;
                    v->cfg.on_event(VAD_EV_SPEECH_END, NULL, 0, prob,
                                    v->cfg.user_data);
                }
            }
        }

        v->pend_n = 0;
    }
}
