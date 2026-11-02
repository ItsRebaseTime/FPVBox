/* FPVBox video pipeline (see video_pipe.h).
 *
 *  capture node --> [sync task, core 1] --line jobs--> [workers, cores 0/1]
 *                                                       | rows into a field slot
 *  field complete (last line decoded) --> [output task, core 0]
 *      YUYV field mode : the field buffer itself goes to USB (zero copy)
 *      YUYV frame mode : two fields woven into one frame
 *      MJPEG / H.264   : field line-interpolated to a full frame, encoded
 *
 * Field slots are reference counted: the decoder, the weave history and an
 * in-flight USB transfer each hold a reference.
 */
#include "video_pipe.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "driver/jpeg_encode.h"
#include "esp_cache.h"
#include "esp_check.h"
#include "esp_h264_alloc.h"
#include "esp_h264_enc_single.h"
#include "esp_h264_enc_single_hw.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "osd.h"
#include "sdkconfig.h"
#include "vrx_capture.h"

static const char *TAG = "video_pipe";

#define FIELD_SLOTS     6
#define JOBS            48
#define FIELD_BYTES     ((size_t)CVBS_MAX_FIELD_LINES * CVBS_OUT_STRIDE)
#define FRAME_BYTES     ((size_t)576 * CVBS_OUT_STRIDE)
#define ENC_OUT_BYTES   (512u * 1024u)
#define NO_SIGNAL_MS    200     /* no field for this long: show the NO SIGNAL screen */

typedef struct field_slot {
    uint8_t *buf;
    uint32_t seq;
    cvbs_std_t std;
    uint8_t parity;
    uint16_t lines;
    bool void_;
    int64_t t_start_us;
    atomic_int pending;         /* line jobs queued or decoding */
    atomic_bool ended;          /* the sync stage emitted the last line */
    atomic_bool done;           /* published or released (exactly once) */
    atomic_int refs;
    uint8_t idx;
} field_slot_t;

typedef struct {
    cvbs_line_job_t job;
    field_slot_t *slot;
} job_t;

enum { MSG_FIELD, MSG_STREAM, MSG_STOP, MSG_XFER_DONE };
typedef struct {
    uint8_t type;
    field_slot_t *slot;
    usb_video_stream_t stream;
} out_msg_t;

typedef enum { CONV_NONE, CONV_UYVY, CONV_YVYU } conv_t;

static field_slot_t s_slots[FIELD_SLOTS];
static QueueHandle_t s_free_slots, s_free_jobs, s_work, s_out;
static cvbs_sync_t *s_sync;
static field_slot_t *s_cur;             /* sync task only */
static video_pipe_status_t s_st;
static portMUX_TYPE s_st_lock = portMUX_INITIALIZER_UNLOCKED;
static char s_info[2][40] = {"", ""};

/* Output task state. */
static bool s_streaming;
static usb_video_stream_t s_stream;
static field_slot_t *s_top, *s_bottom;  /* weave history */
static field_slot_t *s_inflight_slot;
static bool s_usb_busy;
static uint8_t *s_frame[2];
static int s_frame_idx;
static jpeg_encoder_handle_t s_jpeg;
static uint8_t *s_enc_in;
static size_t s_enc_in_size;
static uint8_t *s_enc_out[2];
static size_t s_enc_out_size;
static esp_h264_enc_handle_t s_h264;
static int64_t s_last_field_us, s_last_nosig_us, s_fps_t0;
static uint32_t s_fps_frames;
static uint32_t s_h264_pts;

/* ---------------------------------------------------------------------- */
/* Field slots                                                              */

static void slot_put(field_slot_t *sl)
{
    if (!sl) return;
    if (atomic_fetch_sub(&sl->refs, 1) == 1) {
        const uint8_t idx = sl->idx;
        xQueueSend(s_free_slots, &idx, 0);
    }
}

static void slot_get(field_slot_t *sl) { atomic_fetch_add(&sl->refs, 1); }

/* Publish (or release) a slot once all its lines are decoded. */
static void slot_try_finish(field_slot_t *sl)
{
    if (!atomic_load(&sl->ended) || atomic_load(&sl->pending) != 0) return;
    bool expected = false;
    if (!atomic_compare_exchange_strong(&sl->done, &expected, true)) return;
    if (sl->void_) { slot_put(sl); return; }
    const out_msg_t m = { .type = MSG_FIELD, .slot = sl };
    if (xQueueSend(s_out, &m, 0) != pdTRUE) slot_put(sl);
}

/* ---------------------------------------------------------------------- */
/* Sync task and decoder sink                                               */

static cvbs_line_job_t *sink_job_get(void *ctx)
{
    (void)ctx;
    if (!s_cur) return NULL;
    job_t *j;
    if (xQueueReceive(s_free_jobs, &j, 0) != pdTRUE) return NULL;
    return &j->job;
}

static void sink_job_put(void *ctx, cvbs_line_job_t *job)
{
    (void)ctx;
    job_t *j = (job_t *)job;    /* job is the first member */
    j->slot = s_cur;
    atomic_fetch_add(&s_cur->pending, 1);
    if (xQueueSend(s_work, &j, 0) != pdTRUE) {
        atomic_fetch_sub(&s_cur->pending, 1);
        xQueueSend(s_free_jobs, &j, 0);
    }
}

static void sink_event(void *ctx, const cvbs_field_event_t *ev)
{
    (void)ctx;
    switch (ev->type) {
    case CVBS_EV_FIELD_START: {
        if (s_cur) {    /* previous field never ended: void it */
            s_cur->void_ = true;
            atomic_store(&s_cur->ended, true);
            slot_try_finish(s_cur);
            s_cur = NULL;
        }
        uint8_t idx;
        if (xQueueReceive(s_free_slots, &idx, 0) != pdTRUE) {
            portENTER_CRITICAL(&s_st_lock);
            s_st.fields_dropped++;
            portEXIT_CRITICAL(&s_st_lock);
            return;
        }
        field_slot_t *sl = &s_slots[idx];
        sl->seq = ev->field_seq;
        sl->std = ev->std;
        sl->parity = ev->field;
        sl->lines = ev->lines;
        sl->void_ = false;
        sl->t_start_us = esp_timer_get_time();
        atomic_store(&sl->pending, 0);
        atomic_store(&sl->ended, false);
        atomic_store(&sl->done, false);
        atomic_store(&sl->refs, 1);
        s_cur = sl;
        break;
    }
    case CVBS_EV_FIELD_END:
    case CVBS_EV_LOCK_LOST:
        if (!s_cur) return;
        if (ev->type == CVBS_EV_LOCK_LOST || ev->field_seq != s_cur->seq) s_cur->void_ = true;
        atomic_store(&s_cur->ended, true);
        slot_try_finish(s_cur);
        s_cur = NULL;
        break;
    }
}

static void sync_task(void *arg)
{
    (void)arg;
    vrx_capture_stats_t cst;
    vrx_capture_get_stats(&cst);
    static uint8_t half[4096];
    int64_t t_stats = esp_timer_get_time();
    for (;;) {
        const uint8_t *data;
        size_t len;
        bool disc;
        if (!vrx_capture_read(&data, &len, &disc, pdMS_TO_TICKS(100))) {
            cvbs_sync_discontinuity(s_sync);
            continue;
        }
        if (disc) cvbs_sync_discontinuity(s_sync);
        if (cst.decimate_sw) {
            size_t n = len / 2u;
            if (n > sizeof(half)) n = sizeof(half);
            for (size_t k = 0; k < n; ++k) half[k] = data[2u * k];
            cvbs_sync_push(s_sync, half, n);
        } else {
            cvbs_sync_push(s_sync, data, len);
        }
        const int64_t now = esp_timer_get_time();
        if (now - t_stats > 250000) {
            t_stats = now;
            cvbs_sync_stats_t ss;
            vrx_capture_stats_t cs;
            cvbs_sync_get_stats(s_sync, &ss);
            vrx_capture_get_stats(&cs);
            portENTER_CRITICAL(&s_st_lock);
            s_st.locked = ss.locked;
            s_st.std = ss.std;
            s_st.color = ss.color;
            s_st.line_period_us = ss.line_period_us;
            s_st.burst_ratio = ss.burst_ratio;
            s_st.afc_ppm = ss.afc_ppm;
            s_st.lock_losses = ss.lock_losses;
            s_st.lines_dropped = ss.lines_dropped;
            s_st.capture_overruns = cs.overruns;
            s_st.capture_timeouts = cs.timeouts;
            portEXIT_CRITICAL(&s_st_lock);
        }
    }
}

static void worker_task(void *arg)
{
    (void)arg;
    for (;;) {
        job_t *j;
        xQueueReceive(s_work, &j, portMAX_DELAY);
        field_slot_t *sl = j->slot;
        if (j->job.line < sl->lines)
            cvbs_line_decode(&j->job, sl->buf + (size_t)j->job.line * CVBS_OUT_STRIDE);
        xQueueSend(s_free_jobs, &j, 0);
        if (atomic_fetch_sub(&sl->pending, 1) == 1) slot_try_finish(sl);
    }
}

/* ---------------------------------------------------------------------- */
/* Frame composition                                                        */

static inline uint32_t conv_word(uint32_t x, conv_t c)
{
    switch (c) {
    case CONV_UYVY: return ((x & 0x00ff00ffu) << 8) | ((x >> 8) & 0x00ff00ffu);
    case CONV_YVYU: return (x & 0x00ff00ffu) | ((x & 0x0000ff00u) << 16) | ((x & 0xff000000u) >> 16);
    default: return x;
    }
}

/* dst = conv(a) or conv(mean(a, b)). */
static void row_out(uint8_t *dst, const uint8_t *a, const uint8_t *b, conv_t c)
{
    const uint32_t *pa = (const uint32_t *)a, *pb = (const uint32_t *)b;
    uint32_t *pd = (uint32_t *)dst;
    const int n = CVBS_OUT_STRIDE / 4;
    if (!pb && c == CONV_NONE) { memcpy(dst, a, CVBS_OUT_STRIDE); return; }
    for (int i = 0; i < n; ++i) {
        uint32_t x = pa[i];
        if (pb) {
            const uint32_t y = pb[i];
            x = (x | y) - (((x ^ y) & 0xfefefefeu) >> 1);   /* bytewise rounding-up mean */
        }
        pd[i] = conv_word(x, c);
    }
}

static inline const uint8_t *fline(const field_slot_t *f, int i)
{
    if (i < 0) i = 0;
    if (i >= f->lines) i = f->lines - 1;
    return f->buf + (size_t)i * CVBS_OUT_STRIDE;
}

static inline bool is_top(const field_slot_t *f) { return f->parity == CONFIG_FPV_TOP_FIELD; }

/* One field to an h-line progressive frame (line interpolation; the field's
 * half-line offset is kept, so the picture does not bob). */
static void compose_bob(uint8_t *dst, int h, const field_slot_t *f, conv_t c)
{
    const int l2 = 2 * f->lines;
    const bool top = is_top(f);
    for (int r = 0; r < h; ++r) {
        const int v = (int)((int64_t)r * l2 / h);
        uint8_t *d = dst + (size_t)r * CVBS_OUT_STRIDE;
        if (top) {
            if (!(v & 1)) row_out(d, fline(f, v / 2), NULL, c);
            else row_out(d, fline(f, (v - 1) / 2), fline(f, (v + 1) / 2), c);
        } else {
            if (v & 1) row_out(d, fline(f, (v - 1) / 2), NULL, c);
            else row_out(d, fline(f, v / 2 - 1), fline(f, v / 2), c);
        }
    }
}

static void compose_weave(uint8_t *dst, int h, const field_slot_t *top, const field_slot_t *bot, conv_t c)
{
    const int l2 = 2 * top->lines;
    for (int r = 0; r < h; ++r) {
        const int v = (int)((int64_t)r * l2 / h);
        row_out(dst + (size_t)r * CVBS_OUT_STRIDE, (v & 1) ? fline(bot, v / 2) : fline(top, v / 2), NULL, c);
    }
}

static void compose_field(uint8_t *dst, int h, const field_slot_t *f, conv_t c)
{
    for (int r = 0; r < h; ++r)
        row_out(dst + (size_t)r * CVBS_OUT_STRIDE, fline(f, (int)((int64_t)r * f->lines / h)), NULL, c);
}

static void convert_in_place(uint8_t *buf, size_t bytes, conv_t c)
{
    if (c == CONV_NONE) return;
    uint32_t *p = (uint32_t *)buf;
    for (size_t i = 0; i < bytes / 4u; ++i) p[i] = conv_word(p[i], c);
}

/* ---------------------------------------------------------------------- */
/* Encoders and USB                                                         */

static void encoders_free(void)
{
    if (s_jpeg) { jpeg_del_encoder_engine(s_jpeg); s_jpeg = NULL; }
    if (s_h264) { esp_h264_enc_close(s_h264); esp_h264_enc_del(s_h264); s_h264 = NULL; }
    free(s_enc_in); s_enc_in = NULL;
    free(s_enc_out[0]); s_enc_out[0] = NULL;
    free(s_enc_out[1]); s_enc_out[1] = NULL;
}

static esp_err_t encoders_setup(const usb_video_stream_t *st)
{
    encoders_free();
    s_enc_in_size = (size_t)st->width * st->height * 2u;
    if (st->format == USB_VIDEO_MJPEG) {
        const jpeg_encode_engine_cfg_t ecfg = { .timeout_ms = 100 };
        ESP_RETURN_ON_ERROR(jpeg_new_encoder_engine(&ecfg, &s_jpeg), TAG, "jpeg engine");
        const jpeg_encode_memory_alloc_cfg_t in_cfg = { .buffer_direction = JPEG_ENC_ALLOC_INPUT_BUFFER };
        const jpeg_encode_memory_alloc_cfg_t out_cfg = { .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER };
        size_t got;
        s_enc_in = jpeg_alloc_encoder_mem(s_enc_in_size, &in_cfg, &got);
        s_enc_out[0] = jpeg_alloc_encoder_mem(ENC_OUT_BYTES, &out_cfg, &s_enc_out_size);
        s_enc_out[1] = jpeg_alloc_encoder_mem(ENC_OUT_BYTES, &out_cfg, &got);
    } else if (st->format == USB_VIDEO_H264) {
        esp_h264_enc_cfg_hw_t cfg = {
            .pic_type = ESP_H264_RAW_FMT_UYVY,
            .gop = CONFIG_FPV_H264_GOP,
            .fps = (uint8_t)(10000000u / (st->interval_100ns ? st->interval_100ns : 166833u)),
            .res = { .width = st->width, .height = st->height },
            .rc = { .bitrate = CONFIG_FPV_H264_KBPS * 1000u, .qp_min = 20, .qp_max = 40 },
        };
        ESP_RETURN_ON_FALSE(esp_h264_enc_hw_new(&cfg, &s_h264) == ESP_H264_ERR_OK, ESP_FAIL, TAG, "h264 new");
        ESP_RETURN_ON_FALSE(esp_h264_enc_open(s_h264) == ESP_H264_ERR_OK, ESP_FAIL, TAG, "h264 open");
        uint32_t got;
        s_enc_in = esp_h264_aligned_calloc(128, 1, (uint32_t)s_enc_in_size, &got, ESP_H264_MEM_SPIRAM);
        s_enc_out[0] = esp_h264_aligned_calloc(128, 1, ENC_OUT_BYTES, &got, ESP_H264_MEM_SPIRAM);
        s_enc_out[1] = esp_h264_aligned_calloc(128, 1, ENC_OUT_BYTES, &got, ESP_H264_MEM_SPIRAM);
        s_enc_out_size = ENC_OUT_BYTES;
        s_h264_pts = 0;
    } else {
        return ESP_OK;
    }
    ESP_RETURN_ON_FALSE(s_enc_in && s_enc_out[0] && s_enc_out[1], ESP_ERR_NO_MEM, TAG, "encoder buffers");
    return ESP_OK;
}

static void count_frame(bool sent)
{
    portENTER_CRITICAL(&s_st_lock);
    if (sent) s_st.frames_sent++; else s_st.frames_dropped++;
    portEXIT_CRITICAL(&s_st_lock);
    if (sent) s_fps_frames++;
}

/* Hand a buffer to USB; slot (optional) stays referenced until done. */
static bool usb_out(const void *buf, size_t len, field_slot_t *slot)
{
    if (s_usb_busy) { count_frame(false); return false; }
    if (slot) slot_get(slot);
    s_usb_busy = true;
    s_inflight_slot = slot;
    if (!usb_video_send(buf, len)) {
        s_usb_busy = false;
        s_inflight_slot = NULL;
        if (slot) slot_put(slot);
        count_frame(false);
        return false;
    }
    count_frame(true);
    return true;
}

/* Encode the composed s_enc_in (already in the encoder byte order). */
static void encode_and_send(void)
{
    uint8_t *out = s_enc_out[s_frame_idx];
    s_frame_idx ^= 1;
    esp_cache_msync(s_enc_in, s_enc_in_size, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    uint32_t len = 0;
    if (s_stream.format == USB_VIDEO_MJPEG) {
        const jpeg_encode_cfg_t cfg = {
            .width = s_stream.width, .height = s_stream.height,
            .src_type = JPEG_ENCODE_IN_FORMAT_YUV422,
            .sub_sample = JPEG_DOWN_SAMPLING_YUV422,
            .image_quality = CONFIG_FPV_JPEG_QUALITY,
        };
        if (jpeg_encoder_process(s_jpeg, &cfg, s_enc_in, s_enc_in_size, out, s_enc_out_size, &len) != ESP_OK) {
            count_frame(false);
            return;
        }
    } else {
        esp_h264_enc_in_frame_t in = { .raw_data = { .buffer = s_enc_in, .len = s_enc_in_size }, .pts = s_h264_pts };
        esp_h264_enc_out_frame_t o = { .raw_data = { .buffer = out, .len = s_enc_out_size } };
        s_h264_pts += 1;
        if (esp_h264_enc_process(s_h264, &in, &o) != ESP_H264_ERR_OK) { count_frame(false); return; }
        len = o.length;
    }
    portENTER_CRITICAL(&s_st_lock);
    s_st.last_encoded_bytes = len;
    portEXIT_CRITICAL(&s_st_lock);
    usb_out(out, len, NULL);
}

static conv_t enc_conv(void)
{
    if (s_stream.format == USB_VIDEO_H264) return CONV_UYVY;
#if CONFIG_FPV_JPEG_INPUT_YVYU
    if (s_stream.format == USB_VIDEO_MJPEG) return CONV_YVYU;
#endif
    return CONV_NONE;
}

static void handle_field(field_slot_t *f)
{
    portENTER_CRITICAL(&s_st_lock);
    s_st.fields_decoded++;
    s_st.decode_ms = (float)(esp_timer_get_time() - f->t_start_us) / 1000.0f;
    portEXIT_CRITICAL(&s_st_lock);
    s_last_field_us = esp_timer_get_time();

    /* Weave history. */
    field_slot_t **keep = is_top(f) ? &s_top : &s_bottom;
    slot_put(*keep);
    slot_get(f);
    *keep = f;

    if (!s_streaming) { slot_put(f); return; }
    const int h = s_stream.height;
    if (s_stream.format == USB_VIDEO_YUYV) {
        if (h <= CVBS_MAX_FIELD_LINES) {
            /* Field mode: zero copy when the standard matches. */
            if (h == f->lines) usb_out(f->buf, (size_t)h * CVBS_OUT_STRIDE, f);
            else if (!s_usb_busy) {
                uint8_t *fr = s_frame[s_frame_idx];
                s_frame_idx ^= 1;
                compose_field(fr, h, f, CONV_NONE);
                usb_out(fr, (size_t)h * CVBS_OUT_STRIDE, NULL);
            } else count_frame(false);
        } else if (f == s_bottom && s_top && s_top->std == f->std && f->seq == s_top->seq + 1u) {
            /* Frame mode: a top field followed by its bottom field. */
            if (!s_usb_busy) {
                uint8_t *fr = s_frame[s_frame_idx];
                s_frame_idx ^= 1;
                compose_weave(fr, h, s_top, s_bottom, CONV_NONE);
                usb_out(fr, (size_t)h * CVBS_OUT_STRIDE, NULL);
            } else count_frame(false);
        }
    } else if (s_enc_in) {
        /* Encode only when USB can take the result: an H.264 frame that is
         * encoded but not sent would break the reference chain. */
        if (s_usb_busy) count_frame(false);
        else { compose_bob(s_enc_in, h, f, enc_conv()); encode_and_send(); }
    }
    slot_put(f);
}

static void send_no_signal(void)
{
    const int64_t now = esp_timer_get_time();
    if (now - s_last_nosig_us < 100000) return;     /* 10 fps */
    if (s_usb_busy) return;
    s_last_nosig_us = now;
    const int w = s_stream.width, h = s_stream.height;
    const bool enc = s_stream.format != USB_VIDEO_YUYV;
    uint8_t *buf = enc ? s_enc_in : s_frame[s_frame_idx];
    if (!buf) return;
    if (!enc) s_frame_idx ^= 1;
    char l1[40], l2[40];
    portENTER_CRITICAL(&s_st_lock);
    memcpy(l1, s_info[0], sizeof(l1));
    memcpy(l2, s_info[1], sizeof(l2));
    portEXIT_CRITICAL(&s_st_lock);
    osd_fill(buf, w, h, 41, 240, 110);  /* BT.601 blue */
    const int sy = h >= 400 ? 4 : 2;
    osd_text_centered(buf, w, h, h / 2 - 12 * sy, "NO SIGNAL", 6, sy + sy / 2);
    if (l1[0]) osd_text_centered(buf, w, h, h / 2 + 4 * sy, l1, 3, sy);
    if (l2[0]) osd_text_centered(buf, w, h, h / 2 + 14 * sy, l2, 3, sy);
    if (enc) {
        convert_in_place(buf, (size_t)w * h * 2u, enc_conv());
        encode_and_send();
    } else {
        usb_out(buf, (size_t)w * h * 2u, NULL);
    }
}

static void stream_start(const usb_video_stream_t *st)
{
    s_stream = *st;
    s_streaming = encoders_setup(st) == ESP_OK;
    s_fps_t0 = esp_timer_get_time();
    s_fps_frames = 0;
    s_last_nosig_us = 0;
    ESP_LOGI(TAG, "stream %s %ux%u interval %lu%s",
             st->format == USB_VIDEO_YUYV ? "YUYV" : st->format == USB_VIDEO_MJPEG ? "MJPEG" : "H264",
             st->width, st->height, (unsigned long)st->interval_100ns, s_streaming ? "" : " FAILED");
    portENTER_CRITICAL(&s_st_lock);
    s_st.streaming = s_streaming;
    s_st.stream = *st;
    portEXIT_CRITICAL(&s_st_lock);
}

static void stream_stop(void)
{
    s_streaming = false;
    encoders_free();
    portENTER_CRITICAL(&s_st_lock);
    s_st.streaming = false;
    portEXIT_CRITICAL(&s_st_lock);
    ESP_LOGI(TAG, "stream stopped");
}

static void out_task(void *arg)
{
    (void)arg;
    for (;;) {
        out_msg_t m;
        if (xQueueReceive(s_out, &m, pdMS_TO_TICKS(20)) == pdTRUE) {
            switch (m.type) {
            case MSG_FIELD: handle_field(m.slot); break;
            case MSG_STREAM: stream_start(&m.stream); break;
            case MSG_STOP: stream_stop(); break;
            case MSG_XFER_DONE:
                s_usb_busy = false;
                slot_put(s_inflight_slot);
                s_inflight_slot = NULL;
                break;
            }
        }
        const int64_t now = esp_timer_get_time();
        if (s_streaming && now - s_last_field_us > NO_SIGNAL_MS * 1000) send_no_signal();
        if (now - s_fps_t0 >= 1000000) {
            portENTER_CRITICAL(&s_st_lock);
            s_st.out_fps = (float)s_fps_frames * 1e6f / (float)(now - s_fps_t0);
            portEXIT_CRITICAL(&s_st_lock);
            s_fps_t0 = now;
            s_fps_frames = 0;
        }
    }
}

/* ---------------------------------------------------------------------- */
/* USB callbacks (TinyUSB task)                                             */

static void cb_start(const usb_video_stream_t *st, void *ctx)
{
    (void)ctx;
    out_msg_t m = { .type = MSG_STREAM, .stream = *st };
    xQueueSend(s_out, &m, 0);
}

static void cb_stop(void *ctx)
{
    (void)ctx;
    const out_msg_t m = { .type = MSG_STOP };
    xQueueSend(s_out, &m, 0);
}

static void cb_done(void *ctx)
{
    (void)ctx;
    const out_msg_t m = { .type = MSG_XFER_DONE };
    xQueueSendToFront(s_out, &m, 0);
}

static const usb_video_callbacks_t s_usb_cbs = { .start = cb_start, .stop = cb_stop, .xfer_done = cb_done };

const usb_video_callbacks_t *video_pipe_usb_callbacks(void) { return &s_usb_cbs; }

/* ---------------------------------------------------------------------- */

esp_err_t video_pipe_start(void)
{
    s_free_slots = xQueueCreate(FIELD_SLOTS, sizeof(uint8_t));
    s_free_jobs = xQueueCreate(JOBS, sizeof(job_t *));
    s_work = xQueueCreate(JOBS, sizeof(job_t *));
    s_out = xQueueCreate(FIELD_SLOTS + 8, sizeof(out_msg_t));
    ESP_RETURN_ON_FALSE(s_free_slots && s_free_jobs && s_work && s_out, ESP_ERR_NO_MEM, TAG, "queues");

    for (uint8_t i = 0; i < FIELD_SLOTS; ++i) {
        s_slots[i].idx = i;
        s_slots[i].buf = heap_caps_aligned_calloc(128, 1, FIELD_BYTES, MALLOC_CAP_SPIRAM);
        ESP_RETURN_ON_FALSE(s_slots[i].buf, ESP_ERR_NO_MEM, TAG, "field buffer");
        xQueueSend(s_free_slots, &i, 0);
    }
    for (int i = 0; i < 2; ++i) {
        s_frame[i] = heap_caps_aligned_calloc(128, 1, FRAME_BYTES, MALLOC_CAP_SPIRAM);
        ESP_RETURN_ON_FALSE(s_frame[i], ESP_ERR_NO_MEM, TAG, "frame buffer");
    }
    /* Line jobs are read heavily by the workers: internal RAM. */
    job_t *jobs = heap_caps_calloc(JOBS, sizeof(job_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(jobs, ESP_ERR_NO_MEM, TAG, "jobs");
    for (int i = 0; i < JOBS; ++i) {
        job_t *j = &jobs[i];
        xQueueSend(s_free_jobs, &j, 0);
    }

    vrx_capture_stats_t cst;
    ESP_RETURN_ON_ERROR(vrx_capture_start(), TAG, "capture");
    vrx_capture_get_stats(&cst);
    const cvbs_sync_config_t scfg = {
        .sample_hz = (float)cst.sample_hz,
        .color = true,
        .saturation = 1.0f,
#if CONFIG_FPV_STD_NTSC
        .force_std = CVBS_STD_NTSC,
#elif CONFIG_FPV_STD_PAL
        .force_std = CVBS_STD_PAL,
#else
        .force_std = CVBS_STD_NONE,
#endif
    };
    const cvbs_sync_sink_t sink = { .job_get = sink_job_get, .job_put = sink_job_put, .event = sink_event };
    s_sync = cvbs_sync_new(&scfg, &sink);
    ESP_RETURN_ON_FALSE(s_sync, ESP_ERR_NO_MEM, TAG, "sync");

    xTaskCreatePinnedToCore(out_task, "vp_out", 6144, NULL, 19, NULL, 0);
    for (int i = 0; i < CONFIG_FPV_DECODE_WORKERS; ++i) {
        char name[12];
        snprintf(name, sizeof(name), "vp_dec%d", i);
        /* Line buffers live on the stack (~18 KiB). */
        xTaskCreatePinnedToCore(worker_task, name, 24576, NULL, 20, NULL, i & 1);
    }
    xTaskCreatePinnedToCore(sync_task, "vp_sync", 8192, NULL, 22, NULL, 1);
    ESP_LOGI(TAG, "started: %d decode workers, %d field slots", CONFIG_FPV_DECODE_WORKERS, FIELD_SLOTS);
    return ESP_OK;
}

void video_pipe_get_status(video_pipe_status_t *st)
{
    portENTER_CRITICAL(&s_st_lock);
    *st = s_st;
    portEXIT_CRITICAL(&s_st_lock);
}

void video_pipe_set_color(bool on, float saturation)
{
    if (s_sync) cvbs_sync_set_color(s_sync, on, saturation);
}

void video_pipe_set_info(const char *line1, const char *line2)
{
    portENTER_CRITICAL(&s_st_lock);
    snprintf(s_info[0], sizeof(s_info[0]), "%s", line1 ? line1 : "");
    snprintf(s_info[1], sizeof(s_info[1]), "%s", line2 ? line2 : "");
    portEXIT_CRITICAL(&s_st_lock);
}
