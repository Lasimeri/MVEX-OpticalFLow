/* pwc.c: see pwc.h. */
#define _GNU_SOURCE
#include "pwc.h"

#include <pipewire/pipewire.h>
#include <spa/buffer/meta.h>
#include <spa/utils/result.h>
#include <spa/debug/types.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/video/type-info.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct pwc {
    struct pw_thread_loop *loop;
    struct pw_context *ctx;
    struct pw_core *core;
    struct pw_stream *stream;
    struct spa_hook hook;
    struct spa_video_info_raw raw;
    pwc_frame_fn fn;
    void *user;
    char format[96];
};

static uint64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static void on_param(void *u, uint32_t id, const struct spa_pod *param)
{
    struct pwc *c = u;
    if (!param || id != SPA_PARAM_Format)
        return;
    uint32_t mt, mst;
    if (spa_format_parse(param, &mt, &mst) < 0 || mt != SPA_MEDIA_TYPE_video || mst != SPA_MEDIA_SUBTYPE_raw)
        return;
    spa_format_video_raw_parse(param, &c->raw);
    snprintf(c->format, sizeof c->format, "%s %ux%u, max %u/%u", spa_debug_type_find_short_name(spa_type_video_format, c->raw.format), c->raw.size.width, c->raw.size.height, c->raw.max_framerate.num, c->raw.max_framerate.denom);
    uint8_t mem[512];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(mem, sizeof mem);
    const struct spa_pod *out[2];
    out[0] = spa_pod_builder_add_object(&b, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers, SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(4, 2, 16), SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1), SPA_PARAM_BUFFERS_dataType,
                                        SPA_POD_CHOICE_FLAGS_Int((1 << SPA_DATA_MemFd) | (1 << SPA_DATA_MemPtr)));
    out[1] = spa_pod_builder_add_object(&b, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta, SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header), SPA_PARAM_META_size, SPA_POD_Int(sizeof(struct spa_meta_header)));
    pw_stream_update_params(c->stream, out, 2);
}

static void on_process(void *u)
{
    struct pwc *c = u;
    struct pw_buffer *b = pw_stream_dequeue_buffer(c->stream);
    if (!b)
        return;
    const struct spa_data *d = &b->buffer->datas[0];
    if (d->data && d->chunk && d->chunk->size && !(d->chunk->flags & SPA_CHUNK_FLAG_CORRUPTED)) {
        const struct spa_meta_header *h = spa_buffer_find_meta_data(b->buffer, SPA_META_Header, sizeof *h);
        const uint64_t pts = h && h->pts > 0 ? (uint64_t)h->pts : 0;
        c->fn(c->user, (const uint8_t *)d->data + d->chunk->offset, d->chunk->stride ? d->chunk->stride : (int)c->raw.size.width * 4, (int)c->raw.size.width, (int)c->raw.size.height, pts, now_ns());
    }
    pw_stream_queue_buffer(c->stream, b);
}

static void on_state(void *u, enum pw_stream_state old, enum pw_stream_state state, const char *err)
{
    (void)u, (void)old;
    if (state == PW_STREAM_STATE_ERROR)
        fprintf(stderr, "pwc: stream error: %s\n", err ? err : "");
}

static const struct pw_stream_events ev = {.version = PW_VERSION_STREAM_EVENTS, .param_changed = on_param, .process = on_process, .state_changed = on_state};

struct pwc *pwc_open(uint32_t node, int w, int h, uint32_t num, uint32_t den, pwc_frame_fn fn, void *user)
{
    pw_init(NULL, NULL);
    struct pwc *c = calloc(1, sizeof *c);
    c->fn = fn;
    c->user = user;
    c->loop = pw_thread_loop_new("mvexflow-cast", NULL);
    c->ctx = pw_context_new(pw_thread_loop_get_loop(c->loop), NULL, 0);
    if (pw_thread_loop_start(c->loop)) {
        fprintf(stderr, "pwc: no thread\n");
        return NULL;
    }
    pw_thread_loop_lock(c->loop);
    c->core = pw_context_connect(c->ctx, NULL, 0);
    if (!c->core) {
        pw_thread_loop_unlock(c->loop);
        fprintf(stderr, "pwc: no PipeWire session\n");
        return NULL;
    }
    c->stream = pw_stream_new(c->core, "mvexflow", pw_properties_new(PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_MEDIA_ROLE, "Screen", NULL));
    pw_stream_add_listener(c->stream, &c->hook, &ev, c);
    uint8_t mem[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(mem, sizeof mem);
    const struct spa_rectangle size = SPA_RECTANGLE((uint32_t)w, (uint32_t)h);
    const struct spa_fraction zero = SPA_FRACTION(0, 1), want = SPA_FRACTION(num, den), lo = SPA_FRACTION(1, 1);
    const struct spa_pod *params[1];
    params[0] = spa_pod_builder_add_object(&b, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat, SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video), SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw), SPA_FORMAT_VIDEO_format,
                                           SPA_POD_CHOICE_ENUM_Id(3, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRA), SPA_FORMAT_VIDEO_size, SPA_POD_Rectangle(&size), SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction(&zero),
                                           SPA_FORMAT_VIDEO_maxFramerate, SPA_POD_CHOICE_RANGE_Fraction(&want, &lo, &want));
    const int rc = pw_stream_connect(c->stream, PW_DIRECTION_INPUT, node, PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS, params, 1);
    pw_thread_loop_unlock(c->loop);
    if (rc < 0) {
        fprintf(stderr, "pwc: connect to node %u: %s\n", node, spa_strerror(rc));
        return NULL;
    }
    return c;
}

void pwc_close(struct pwc *c)
{
    if (!c)
        return;
    pw_thread_loop_lock(c->loop);
    pw_stream_disconnect(c->stream);
    pw_stream_destroy(c->stream);
    pw_core_disconnect(c->core);
    pw_thread_loop_unlock(c->loop);
    pw_thread_loop_stop(c->loop);
    pw_context_destroy(c->ctx);
    pw_thread_loop_destroy(c->loop);
    free(c);
}

const char *pwc_format(const struct pwc *c) { return c->format; }
