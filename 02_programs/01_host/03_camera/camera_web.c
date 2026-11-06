#define _POSIX_C_SOURCE 200809L
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <microhttpd.h>
#include <pthread.h>
#include <stdatomic.h>
#include <signal.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include "image_quality.h"
#include "image_controls.h"
#include "motor_control.h"

/* 一个采集源供全部网页使用；照片仅存内存，重启或到期后清空。 */
#define WIDTH 1280
#define HEIGHT 720
#define CAPTURE_FPS 30
#define PREVIEW_FPS 15
#define CAPTURE_LIMIT 12
#define PHOTO_BYTES_LIMIT (32U * 1024U * 1024U)
#define FRAME_BYTES_LIMIT (4U * 1024U * 1024U)
#define RETENTION_S 1800
#define FRESH_MS 2000.0
#define STREAM_LIMIT 3

typedef struct {
    unsigned width, height, preview_fps, jpeg_quality;
} Settings;

typedef struct {
    _Atomic unsigned refs;
    uint64_t id;
    unsigned width, height, settings_version;
    double received_mono;
    gint64 received_real;
    size_t size;
    unsigned char data[];
} Frame;

typedef struct {
    char id[40], sha256[65];
    Frame *frame;
    uint64_t request_frame_id;
    double created_mono;
} Photo;

typedef struct {
    pthread_mutex_t mutex;
    pthread_mutex_t settings_mutex;
    pthread_mutex_t quality_mutex;
    pthread_cond_t changed;
    Frame *latest;
    Photo photos[CAPTURE_LIMIT];
    size_t photo_bytes;
    unsigned photo_counter, streams, waiting_captures, settings_version;
    uint64_t frame_counter;
    Settings settings, pending_settings;
    gboolean reconfiguring, settings_pending, capture_stop, capture_thread_started;
    char settings_error[512];
    char session[9], error[512];
    double started;
    gboolean quit;
    const char *web_dir;
    const char *device, *config_path;
    GstElement *pipeline, *sink;
    GstBus *bus;
    pthread_t capture_thread;
    MotorControl *motor;
} App;

typedef struct { gboolean done; size_t body_size; char body[1025]; } Request;
typedef struct {
    App *app;
    Frame *frame;
    uint64_t last_id;
    size_t offset, header_size;
    char header[256];
} Stream;

static volatile sig_atomic_t stop_requested;
static void stop_signal(int sig) { (void)sig; stop_requested = 1; }

static double mono_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec * 1000.0 + now.tv_nsec / 1000000.0;
}

static struct timespec deadline_after(double ms) {
    double end = mono_ms() + ms;
    struct timespec result = { (time_t)(end / 1000.0),
                              (long)((end - (time_t)(end / 1000.0) * 1000.0) * 1000000.0) };
    return result;
}

static Frame *frame_ref(Frame *frame) {
    if (frame) atomic_fetch_add_explicit(&frame->refs, 1, memory_order_relaxed);
    return frame;
}

static void frame_unref(Frame *frame) {
    if (frame && atomic_fetch_sub_explicit(&frame->refs, 1, memory_order_acq_rel) == 1)
        free(frame);
}

static void remove_photo(App *app, Photo *photo) {
    if (!photo->frame) return;
    app->photo_bytes -= photo->frame->size;
    frame_unref(photo->frame);
    memset(photo, 0, sizeof(*photo));
}

/* 调用方持有锁；网页关闭并不等于服务会话结束，照片按30分钟到期清理。 */
static void prune_photos(App *app) {
    double now = mono_ms();
    for (unsigned i = 0; i < CAPTURE_LIMIT; ++i)
        if (app->photos[i].frame && now - app->photos[i].created_mono >= RETENTION_S * 1000.0)
            remove_photo(app, &app->photos[i]);
}

static unsigned photo_count(App *app) {
    unsigned count = 0;
    for (unsigned i = 0; i < CAPTURE_LIMIT; ++i) count += app->photos[i].frame != NULL;
    return count;
}

static gboolean frame_fresh(App *app) {
    return !app->quit && !app->reconfiguring && app->latest && mono_ms() - app->latest->received_mono < FRESH_MS;
}

static void json_string(GString *json, const char *value) {
    g_string_append_c(json, '"');
    for (const unsigned char *p = (const unsigned char *)value; *p; ++p) {
        if (*p == '"' || *p == '\\') g_string_append_c(json, '\\');
        if (*p < 0x20) g_string_append_printf(json, "\\u%04x", *p);
        else g_string_append_c(json, (char)*p);
    }
    g_string_append_c(json, '"');
}

static void photo_json(GString *json, const Photo *photo) {
    GDateTime *date = g_date_time_new_from_unix_utc(photo->frame->received_real / G_USEC_PER_SEC);
    char *stamp = g_date_time_format(date, "%Y-%m-%dT%H:%M:%S");
    g_string_append_printf(json,
        "{\"id\":\"%s\",\"frame_id\":%" PRIu64 ",\"request_frame_id\":%" PRIu64
        ",\"received_at\":\"%s.%03dZ\",\"width\":%d,\"height\":%d,\"bytes\":%zu,"
        "\"settings_version\":%u,\"sha256\":\"%s\",\"url\":\"/api/captures/%s.jpg\","
        "\"download_url\":\"/api/captures/%s.jpg?download=1\"}",
        photo->id, photo->frame->id, photo->request_frame_id, stamp,
        (int)((photo->frame->received_real % G_USEC_PER_SEC) / 1000),
        photo->frame->width, photo->frame->height, photo->frame->size,
        photo->frame->settings_version, photo->sha256, photo->id, photo->id);
    g_free(stamp);
    g_date_time_unref(date);
}

static void response_headers(struct MHD_Response *response, const char *type) {
    MHD_add_response_header(response, MHD_HTTP_HEADER_CONTENT_TYPE, type);
    MHD_add_response_header(response, MHD_HTTP_HEADER_CACHE_CONTROL, "no-store, max-age=0");
    MHD_add_response_header(response, "X-Content-Type-Options", "nosniff");
    MHD_add_response_header(response, "Referrer-Policy", "same-origin");
    MHD_add_response_header(response, "Content-Security-Policy",
        "default-src 'self'; img-src 'self' blob: data:; connect-src 'self'; "
        "script-src 'self'; style-src 'self' 'unsafe-inline'; base-uri 'none'; frame-ancestors 'none'");
}

static enum MHD_Result reply_buffer(struct MHD_Connection *connection, unsigned status,
                                   const void *data, size_t size, const char *type) {
    struct MHD_Response *response = MHD_create_response_from_buffer(size, (void *)data, MHD_RESPMEM_MUST_COPY);
    if (!response) return MHD_NO;
    response_headers(response, type);
    enum MHD_Result result = MHD_queue_response(connection, status, response);
    MHD_destroy_response(response);
    return result;
}

static enum MHD_Result reply_json(struct MHD_Connection *connection, unsigned status, GString *json) {
    enum MHD_Result result = reply_buffer(connection, status, json->str, json->len, "application/json; charset=utf-8");
    g_string_free(json, TRUE);
    return result;
}

static enum MHD_Result reply_error(struct MHD_Connection *connection, unsigned status, const char *message) {
    GString *json = g_string_new("{\"error\":");
    json_string(json, message);
    g_string_append_c(json, '}');
    return reply_json(connection, status, json);
}

static void set_error(App *app, const char *message) {
    pthread_mutex_lock(&app->mutex);
    g_strlcpy(app->error, message, sizeof(app->error));
    pthread_cond_broadcast(&app->changed);
    pthread_mutex_unlock(&app->mutex);
    fprintf(stderr, "camera: %s\n", message);
}

static void *capture_frames(void *opaque) {
    App *app = opaque;
    while (!stop_requested) {
        pthread_mutex_lock(&app->mutex);
        gboolean quit = app->quit || app->capture_stop;
        Settings settings = app->settings;
        unsigned version = app->settings_version;
        pthread_mutex_unlock(&app->mutex);
        if (quit) break;
        GstSample *sample = gst_app_sink_try_pull_sample(GST_APP_SINK(app->sink), 500 * GST_MSECOND);
        if (!sample) continue;
        GstBuffer *buffer = gst_sample_get_buffer(sample);
        GstMapInfo map;
        if (!buffer || !gst_buffer_map(buffer, &map, GST_MAP_READ)) {
            gst_sample_unref(sample);
            continue;
        }
        if (map.size >= 4 && map.size <= FRAME_BYTES_LIMIT && map.data[0] == 0xff && map.data[1] == 0xd8
            && map.data[map.size - 2] == 0xff && map.data[map.size - 1] == 0xd9) {
            Frame *frame = malloc(sizeof(*frame) + map.size);
            if (frame) {
                atomic_init(&frame->refs, 1);
                frame->width = settings.width;
                frame->height = settings.height;
                frame->settings_version = version;
                frame->received_mono = mono_ms();
                frame->received_real = g_get_real_time();
                frame->size = map.size;
                memcpy(frame->data, map.data, map.size);
                pthread_mutex_lock(&app->mutex);
                Frame *previous = NULL;
                if (app->capture_stop || app->quit) {
                    previous = frame;
                } else {
                    frame->id = ++app->frame_counter;
                    previous = app->latest;
                    app->latest = frame;
                    app->error[0] = '\0';
                    pthread_cond_broadcast(&app->changed);
                }
                pthread_mutex_unlock(&app->mutex);
                frame_unref(previous);
            } else set_error(app, "无法为相机图像分配内存");
        } else set_error(app, "相机返回的JPEG图像无效或超过大小限制");
        gst_buffer_unmap(buffer, &map);
        gst_sample_unref(sample);
    }
    return NULL;
}

static ssize_t stream_reader(void *opaque, uint64_t position, char *target, size_t max) {
    (void)position;
    Stream *stream = opaque;
    App *app = stream->app;
    if (!max) return MHD_CONTENT_READER_END_WITH_ERROR;
    if (!stream->frame) {
        struct timespec deadline = deadline_after(3000);
        pthread_mutex_lock(&app->mutex);
        while (!app->quit && (!frame_fresh(app) || app->latest->id <= stream->last_id)) {
            if (pthread_cond_timedwait(&app->changed, &app->mutex, &deadline) == ETIMEDOUT) break;
        }
        if (app->quit || !frame_fresh(app) || app->latest->id <= stream->last_id) {
            pthread_mutex_unlock(&app->mutex);
            return MHD_CONTENT_READER_END_WITH_ERROR;
        }
        stream->frame = frame_ref(app->latest);
        pthread_mutex_unlock(&app->mutex);
        stream->offset = 0;
        stream->header_size = (size_t)snprintf(stream->header, sizeof(stream->header),
            "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %zu\r\nX-Frame-Id: %" PRIu64 "\r\n\r\n",
            stream->frame->size, stream->frame->id);
    }
    size_t copied = 0;
    const size_t total = stream->header_size + stream->frame->size + 2;
    while (copied < max && stream->offset < total) {
        const char *source;
        size_t remaining;
        if (stream->offset < stream->header_size) {
            source = stream->header + stream->offset;
            remaining = stream->header_size - stream->offset;
        } else if (stream->offset < stream->header_size + stream->frame->size) {
            size_t jpeg_offset = stream->offset - stream->header_size;
            source = (const char *)stream->frame->data + jpeg_offset;
            remaining = stream->frame->size - jpeg_offset;
        } else {
            size_t end_offset = stream->offset - stream->header_size - stream->frame->size;
            source = "\r\n" + end_offset;
            remaining = 2 - end_offset;
        }
        size_t amount = remaining < max - copied ? remaining : max - copied;
        memcpy(target + copied, source, amount);
        copied += amount;
        stream->offset += amount;
    }
    if (stream->offset == total) {
        stream->last_id = stream->frame->id;
        frame_unref(stream->frame);
        stream->frame = NULL;
    }
    return (ssize_t)copied;
}

static void stream_free(void *opaque) {
    Stream *stream = opaque;
    frame_unref(stream->frame);
    pthread_mutex_lock(&stream->app->mutex);
    --stream->app->streams;
    pthread_mutex_unlock(&stream->app->mutex);
    free(stream);
}

static enum MHD_Result start_stream(App *app, struct MHD_Connection *connection) {
    pthread_mutex_lock(&app->mutex);
    if (app->streams >= STREAM_LIMIT || app->quit) {
        pthread_mutex_unlock(&app->mutex);
        return reply_error(connection, 429, "预览连接已满，请关闭其他预览页面后重试");
    }
    ++app->streams;
    pthread_mutex_unlock(&app->mutex);
    Stream *stream = calloc(1, sizeof(*stream));
    if (!stream) {
        pthread_mutex_lock(&app->mutex); --app->streams; pthread_mutex_unlock(&app->mutex);
        return reply_error(connection, 503, "内存不足，无法建立预览");
    }
    stream->app = app;
    struct MHD_Response *response = MHD_create_response_from_callback(MHD_SIZE_UNKNOWN, 64 * 1024,
                                                                    stream_reader, stream, stream_free);
    if (!response) { stream_free(stream); return MHD_NO; }
    response_headers(response, "multipart/x-mixed-replace; boundary=frame");
    MHD_add_response_header(response, "X-Accel-Buffering", "no");
    enum MHD_Result result = MHD_queue_response(connection, 200, response);
    MHD_destroy_response(response);
    return result;
}

static enum MHD_Result status_json(App *app, struct MHD_Connection *connection) {
    GString *json = g_string_new(NULL);
    pthread_mutex_lock(&app->mutex);
    prune_photos(app);
    double age = app->latest ? mono_ms() - app->latest->received_mono : -1;
    gboolean fresh = frame_fresh(app);
    const char *state = fresh ? "ready" : (app->error[0] || age > FRESH_MS ? "error" : "starting");
    g_string_append_printf(json,
        "{\"state\":\"%s\",\"fresh\":%s,\"width\":%u,\"height\":%u,"
        "\"capture_fps\":%d,\"preview_fps\":%d,\"frame_id\":%" PRIu64 ",\"frame_age_ms\":",
        state, fresh ? "true" : "false", app->settings.width, app->settings.height,
        CAPTURE_FPS, app->settings.preview_fps,
        app->latest ? app->latest->id : 0);
    if (age < 0) g_string_append(json, "null"); else g_string_append_printf(json, "%.0f", age);
    g_string_append_printf(json,
        ",\"uptime_s\":%.0f,\"capture_count\":%u,\"capture_limit\":%d,\"retention_s\":%d,"
        "\"stream_count\":%u,\"reconfiguring\":%s,\"settings_version\":%u,\"jpeg_quality\":%u,\"error\":",
        (mono_ms() - app->started) / 1000.0, photo_count(app), CAPTURE_LIMIT, RETENTION_S,
        app->streams, app->reconfiguring ? "true" : "false", app->settings_version, app->settings.jpeg_quality);
    json_string(json, app->error[0] ? app->error : (!fresh && age > FRESH_MS ? "相机暂时没有新画面，请等待服务恢复" : ""));
    g_string_append_c(json, '}');
    pthread_mutex_unlock(&app->mutex);
    return reply_json(connection, 200, json);
}

static enum MHD_Result take_photo(App *app, struct MHD_Connection *connection) {
    struct timespec deadline = deadline_after(2500);
    pthread_mutex_lock(&app->mutex);
    prune_photos(app);
    if (app->waiting_captures >= 2) {
        pthread_mutex_unlock(&app->mutex);
        return reply_error(connection, 429, "正在处理其他拍照请求，请稍后再试");
    }
    if (!frame_fresh(app)) {
        pthread_mutex_unlock(&app->mutex);
        return reply_error(connection, 503, "相机没有有效新画面，暂时不能拍照");
    }
    uint64_t request_id = app->latest->id;
    ++app->waiting_captures;
    while (!app->quit && !app->reconfiguring && app->latest && app->latest->id <= request_id) {
        if (pthread_cond_timedwait(&app->changed, &app->mutex, &deadline) == ETIMEDOUT) break;
    }
    --app->waiting_captures;
    if (!frame_fresh(app) || app->latest->id <= request_id) {
        pthread_mutex_unlock(&app->mutex);
        return reply_error(connection, 503, "等待新画面超时，本次没有生成照片");
    }
    Photo *slot = NULL;
    for (unsigned i = 0; i < CAPTURE_LIMIT; ++i) if (!app->photos[i].frame) { slot = &app->photos[i]; break; }
    if (!slot || app->photo_bytes + app->latest->size > PHOTO_BYTES_LIMIT) {
        pthread_mutex_unlock(&app->mutex);
        return reply_error(connection, 409, "临时照片空间已满，请下载所需照片并删除部分照片后再拍");
    }
    slot->frame = frame_ref(app->latest);
    slot->request_frame_id = request_id;
    slot->created_mono = mono_ms();
    snprintf(slot->id, sizeof(slot->id), "%s-%06u", app->session, ++app->photo_counter);
    char *hash = g_compute_checksum_for_data(G_CHECKSUM_SHA256, slot->frame->data, slot->frame->size);
    g_strlcpy(slot->sha256, hash, sizeof(slot->sha256));
    g_free(hash);
    app->photo_bytes += slot->frame->size;
    GString *json = g_string_new(NULL);
    photo_json(json, slot);
    fprintf(stdout, "capture=%s request_frame=%" PRIu64 " frame=%" PRIu64 " bytes=%zu\n",
            slot->id, request_id, slot->frame->id, slot->frame->size);
    pthread_mutex_unlock(&app->mutex);
    return reply_json(connection, 201, json);
}

static enum MHD_Result list_photos(App *app, struct MHD_Connection *connection) {
    GString *json = g_string_new("{\"captures\":[");
    Photo *sorted[CAPTURE_LIMIT];
    unsigned count = 0;
    pthread_mutex_lock(&app->mutex);
    prune_photos(app);
    for (unsigned i = 0; i < CAPTURE_LIMIT; ++i) if (app->photos[i].frame) sorted[count++] = &app->photos[i];
    for (unsigned i = 0; i < count; ++i)
        for (unsigned j = i + 1; j < count; ++j)
            if (sorted[j]->created_mono > sorted[i]->created_mono) { Photo *swap = sorted[i]; sorted[i] = sorted[j]; sorted[j] = swap; }
    for (unsigned i = 0; i < count; ++i) { if (i) g_string_append_c(json, ','); photo_json(json, sorted[i]); }
    pthread_mutex_unlock(&app->mutex);
    g_string_append(json, "]}");
    return reply_json(connection, 200, json);
}

static enum MHD_Result photo_route(App *app, struct MHD_Connection *connection, const char *path, gboolean deleting) {
    char id[40];
    size_t length = strlen(path);
    if (!deleting) {
        if (length < 5 || strcmp(path + length - 4, ".jpg")) return reply_error(connection, 404, "照片不存在");
        length -= 4;
    }
    if (!length || length >= sizeof(id)) return reply_error(connection, 404, "照片不存在");
    memcpy(id, path, length); id[length] = '\0';
    pthread_mutex_lock(&app->mutex);
    prune_photos(app);
    Photo *photo = NULL;
    for (unsigned i = 0; i < CAPTURE_LIMIT; ++i)
        if (app->photos[i].frame && !strcmp(app->photos[i].id, id)) { photo = &app->photos[i]; break; }
    if (!photo) {
        pthread_mutex_unlock(&app->mutex);
        return reply_error(connection, 404, "照片已删除、到期或服务已重启，请重新拍照");
    }
    if (deleting) {
        remove_photo(app, photo);
        pthread_mutex_unlock(&app->mutex);
        return reply_buffer(connection, 200, "{\"deleted\":true}", 16, "application/json; charset=utf-8");
    }
    Frame *frame = frame_ref(photo->frame);
    pthread_mutex_unlock(&app->mutex);
    struct MHD_Response *response = MHD_create_response_from_buffer(frame->size, frame->data, MHD_RESPMEM_MUST_COPY);
    frame_unref(frame);
    if (!response) return MHD_NO;
    response_headers(response, "image/jpeg");
    if (MHD_lookup_connection_value(connection, MHD_GET_ARGUMENT_KIND, "download")) {
        char disposition[128];
        snprintf(disposition, sizeof(disposition), "attachment; filename=\"capture-%s.jpg\"", id);
        MHD_add_response_header(response, MHD_HTTP_HEADER_CONTENT_DISPOSITION, disposition);
    }
    enum MHD_Result result = MHD_queue_response(connection, 200, response);
    MHD_destroy_response(response);
    return result;
}

static gboolean same_origin(struct MHD_Connection *connection) {
    const char *origin = MHD_lookup_connection_value(connection, MHD_HEADER_KIND, "Origin");
    if (!origin) return TRUE;
    const char *host = MHD_lookup_connection_value(connection, MHD_HEADER_KIND, "Host");
    if (!host) return FALSE;
    char *expected = g_strdup_printf("http://%s", host);
    gboolean match = !strcmp(origin, expected);
    g_free(expected);
    return match;
}

static enum MHD_Result image_control_request(App *app, struct MHD_Connection *connection,
                                           const Request *request) {
    MHD_set_connection_option(connection, MHD_CONNECTION_OPTION_TIMEOUT, (unsigned int)25);
    if (pthread_mutex_trylock(&app->settings_mutex))
        return reply_error(connection, 409, "正在调整图像参数，请稍后再读取或应用曝光设置");
    pthread_mutex_lock(&app->mutex);
    int unavailable = app->reconfiguring || app->quit;
    pthread_mutex_unlock(&app->mutex);
    if (unavailable) {
        pthread_mutex_unlock(&app->settings_mutex);
        return reply_error(connection, 409, "相机正在切换或停止，请等预览恢复后再调节曝光与白平衡");
    }
    GError *error = NULL;
    gchar *json = NULL;
    if (request) {
        const char *type = MHD_lookup_connection_value(connection, MHD_HEADER_KIND, "Content-Type");
        if (!type || !g_str_has_prefix(type, "application/x-www-form-urlencoded")) {
            pthread_mutex_unlock(&app->settings_mutex);
            return reply_error(connection, 415, "请使用曝光与白平衡面板提交设置");
        }
        GHashTable *fields = g_uri_parse_params(request->body, (gssize)request->body_size,
                                               "&", G_URI_PARAMS_WWW_FORM, NULL);
        if (fields) {
            json = image_controls_apply_json(fields, &error);
            g_hash_table_unref(fields);
        } else g_set_error_literal(&error, IMAGE_CONTROLS_ERROR,
                                   IMAGE_CONTROLS_ERROR_INVALID_ARGUMENT, "无法读取提交的相机参数");
    } else json = image_controls_read_json(&error);
    pthread_mutex_unlock(&app->settings_mutex);
    enum MHD_Result result;
    if (json) {
        result = reply_buffer(connection, 200, json, strlen(json), "application/json; charset=utf-8");
        g_free(json);
    } else if (!request) {
        GString *unavailable = g_string_new("{\"connected\":false,\"error\":");
        json_string(unavailable, error ? error->message : "图像控制服务暂时不可连接");
        g_string_append_c(unavailable, '}');
        result = reply_json(connection, 200, unavailable);
    } else result = reply_error(connection,
        error && error->domain == IMAGE_CONTROLS_ERROR && error->code == IMAGE_CONTROLS_ERROR_INVALID_ARGUMENT ? 400 : 422,
        error ? error->message : "参数应用未通过读回核对");
    g_clear_error(&error);
    return result;
}

static enum MHD_Result check_photo_quality(App *app, struct MHD_Connection *connection, const char *path) {
    const char *suffix = "/quality";
    size_t length = strlen(path), suffix_size = strlen(suffix);
    char id[40];
    if (length <= suffix_size || strcmp(path + length - suffix_size, suffix)
        || length - suffix_size >= sizeof(id)) return reply_error(connection, 404, "照片不存在");
    memcpy(id, path, length - suffix_size); id[length - suffix_size] = '\0';
    if (pthread_mutex_trylock(&app->quality_mutex)) return reply_error(connection, 429, "正在检查另一张照片，请稍后再试");
    pthread_mutex_lock(&app->mutex);
    prune_photos(app);
    Frame *frame = NULL;
    for (unsigned i = 0; i < CAPTURE_LIMIT; ++i)
        if (app->photos[i].frame && !strcmp(app->photos[i].id, id)) { frame = frame_ref(app->photos[i].frame); break; }
    pthread_mutex_unlock(&app->mutex);
    if (!frame) {
        pthread_mutex_unlock(&app->quality_mutex);
        return reply_error(connection, 404, "照片已删除或到期，请重新拍照");
    }
    ImageQuality quality;
    GError *error = NULL;
    double began = mono_ms();
    gboolean success = image_quality_analyze(frame->data, frame->size, &quality, &error);
    enum MHD_Result result;
    if (!success) result = reply_error(connection, 422, error ? error->message : "无法检查这张照片");
    else if (quality.width != frame->width || quality.height != frame->height)
        result = reply_error(connection, 422, "照片内部尺寸与采集记录不一致");
    else {
        GString *json = g_string_new(NULL);
        g_string_append_printf(json,
            "{\"capture_id\":\"%s\",\"frame_id\":%" PRIu64 ",\"width\":%u,\"height\":%u,"
            "\"mean_brightness\":%.4f,\"brightness_stddev\":%.4f,\"dark_percent\":%.4f,"
            "\"bright_percent\":%.4f,\"sharpness_score\":%.4f,\"elapsed_ms\":%.2f,\"histogram\":[",
            id, frame->id, quality.width, quality.height, quality.mean_brightness, quality.brightness_stddev,
            quality.dark_percent, quality.bright_percent, quality.sharpness_score, mono_ms() - began);
        for (unsigned i = 0; i < 256; ++i)
            g_string_append_printf(json, "%s%" PRIu64, i ? "," : "", (uint64_t)quality.histogram[i]);
        g_string_append(json, "]}");
        result = reply_json(connection, 200, json);
    }
    g_clear_error(&error);
    frame_unref(frame);
    pthread_mutex_unlock(&app->quality_mutex);
    return result;
}

static gboolean settings_valid(Settings value) {
    gboolean resolution = (value.width == 1280 && value.height == 720)
        || (value.width == 1920 && value.height == 1080);
    gboolean fps = value.preview_fps == 5 || value.preview_fps == 10 || value.preview_fps == 15 || value.preview_fps == 30;
    return resolution && fps && value.jpeg_quality >= 50 && value.jpeg_quality <= 98;
}

static enum MHD_Result settings_json(App *app, struct MHD_Connection *connection) {
    GString *json = g_string_new(NULL);
    pthread_mutex_lock(&app->mutex);
    g_string_append_printf(json,
        "{\"width\":%u,\"height\":%u,\"capture_fps\":30,\"preview_fps\":%u,\"jpeg_quality\":%u,"
        "\"settings_version\":%u,\"reconfiguring\":%s,\"profiles\":["
        "{\"width\":1280,\"height\":720,\"label\":\"720p · 1280×720\"},"
        "{\"width\":1920,\"height\":1080,\"label\":\"1080p · 1920×1080\"}],"
        "\"preview_fps_options\":[5,10,15,30],\"error\":",
        app->settings.width, app->settings.height, app->settings.preview_fps, app->settings.jpeg_quality,
        app->settings_version, app->reconfiguring ? "true" : "false");
    json_string(json, app->settings_error);
    g_string_append_c(json, '}');
    pthread_mutex_unlock(&app->mutex);
    return reply_json(connection, 200, json);
}

static gboolean parse_number(GHashTable *fields, const char *name, unsigned *value) {
    const char *text = g_hash_table_lookup(fields, name);
    if (!text || !*text || strspn(text, "0123456789") != strlen(text)) return FALSE;
    char *end = NULL;
    unsigned long parsed = strtoul(text, &end, 10);
    if (*end || parsed > 10000) return FALSE;
    *value = (unsigned)parsed;
    return TRUE;
}

static enum MHD_Result change_settings(App *app, struct MHD_Connection *connection, const Request *request) {
    MHD_set_connection_option(connection, MHD_CONNECTION_OPTION_TIMEOUT, (unsigned int)35);
    const char *type = MHD_lookup_connection_value(connection, MHD_HEADER_KIND, "Content-Type");
    if (!type || !g_str_has_prefix(type, "application/x-www-form-urlencoded"))
        return reply_error(connection, 415, "请使用网页的参数面板提交设置");
    GHashTable *fields = g_uri_parse_params(request->body, (gssize)request->body_size, "&", G_URI_PARAMS_WWW_FORM, NULL);
    Settings requested = {0};
    gboolean valid = fields && g_hash_table_size(fields) == 4
        && parse_number(fields, "width", &requested.width) && parse_number(fields, "height", &requested.height)
        && parse_number(fields, "preview_fps", &requested.preview_fps) && parse_number(fields, "jpeg_quality", &requested.jpeg_quality)
        && settings_valid(requested);
    if (fields) g_hash_table_unref(fields);
    if (!valid) return reply_error(connection, 400, "分辨率、预览帧率或JPEG压缩质量不在支持范围内");
    if (pthread_mutex_trylock(&app->settings_mutex)) return reply_error(connection, 409, "正在调整相机，请稍后再试");
    pthread_mutex_lock(&app->mutex);
    if (app->reconfiguring || app->quit) {
        pthread_mutex_unlock(&app->mutex); pthread_mutex_unlock(&app->settings_mutex);
        return reply_error(connection, 409, "相机正在调整或停止，请稍后再试");
    }
    app->pending_settings = requested;
    app->settings_pending = TRUE;
    app->reconfiguring = TRUE;
    app->settings_error[0] = '\0';
    pthread_cond_broadcast(&app->changed);
    struct timespec deadline = deadline_after(25000);
    while (app->reconfiguring && !app->quit)
        if (pthread_cond_timedwait(&app->changed, &app->mutex, &deadline) == ETIMEDOUT) break;
    gboolean waiting = app->reconfiguring || app->quit;
    char message[512]; g_strlcpy(message, app->settings_error, sizeof(message));
    pthread_mutex_unlock(&app->mutex);
    enum MHD_Result result;
    if (waiting) result = reply_error(connection, 503, "调整尚未完成，请稍后查看实际参数");
    else if (message[0]) result = reply_error(connection, 422, message);
    else result = settings_json(app, connection);
    pthread_mutex_unlock(&app->settings_mutex);
    return result;
}

static enum MHD_Result motion_request(App *app, struct MHD_Connection *connection, const Request *request) {
    unsigned status = 200;
    char *json;
    if (request) {
        const char *type = MHD_lookup_connection_value(connection, MHD_HEADER_KIND, "Content-Type");
        const char *control = MHD_lookup_connection_value(connection, MHD_HEADER_KIND, "X-Motion-Control");
        if (!control || strcmp(control, "1")) return reply_error(connection, 403, "请使用本页小车试转按钮");
        if (!type || !g_str_has_prefix(type, "application/x-www-form-urlencoded"))
            return reply_error(connection, 415, "请使用本页小车试转面板提交操作");
        GHashTable *fields = g_uri_parse_params(request->body, (gssize)request->body_size,
                                               "&", G_URI_PARAMS_WWW_FORM, NULL);
        json = motor_control_command(app->motor, fields, &status);
        if (fields) g_hash_table_unref(fields);
    } else json = motor_control_json(app->motor);
    enum MHD_Result result = reply_buffer(connection, status, json, strlen(json), "application/json; charset=utf-8");
    g_free(json);
    return result;
}

static enum MHD_Result serve_static(App *app, struct MHD_Connection *connection, const char *url) {
    const char *file = NULL, *type = NULL;
    if (!strcmp(url, "/")) { file = "index.html"; type = "text/html; charset=utf-8"; }
    else if (!strcmp(url, "/app.css")) { file = "app.css"; type = "text/css; charset=utf-8"; }
    else if (!strcmp(url, "/app.js")) { file = "app.js"; type = "text/javascript; charset=utf-8"; }
    else if (!strcmp(url, "/motion.js")) { file = "motion.js"; type = "text/javascript; charset=utf-8"; }
    else if (!strcmp(url, "/motion.css")) { file = "motion.css"; type = "text/css; charset=utf-8"; }
    else return reply_error(connection, 404, "页面不存在");
    char *path = g_build_filename(app->web_dir, file, NULL);
    char *content = NULL; gsize size = 0;
    gboolean read_ok = g_file_get_contents(path, &content, &size, NULL);
    g_free(path);
    if (!read_ok || size > 2 * 1024 * 1024) { g_free(content); return reply_error(connection, 503, "网页文件不可用，请检查部署"); }
    enum MHD_Result result = reply_buffer(connection, 200, content, size, type);
    g_free(content);
    return result;
}

static enum MHD_Result handle_request(void *opaque, struct MHD_Connection *connection,
        const char *url, const char *method, const char *version,
        const char *upload_data, size_t *upload_size, void **context) {
    (void)version; (void)upload_data;
    App *app = opaque;
    if (!*context) { *context = calloc(1, sizeof(Request)); return *context ? MHD_YES : MHD_NO; }
    Request *request = *context;
    if (request->done) return MHD_YES;
    if (*upload_size) {
        size_t incoming = *upload_size;
        if (request->body_size + incoming < sizeof(request->body)) {
            memcpy(request->body + request->body_size, upload_data, incoming);
            request->body[request->body_size + incoming] = '\0';
        }
        request->body_size += *upload_size;
        *upload_size = 0;
        if (request->body_size >= sizeof(request->body)) { request->done = TRUE; return reply_error(connection, 413, "本接口不接受上传文件"); }
        return MHD_YES;
    }
    request->done = TRUE;
    if (!strcmp(method, "GET")) {
        if (!strcmp(url, "/api/status")) return status_json(app, connection);
        if (!strcmp(url, "/api/settings")) return settings_json(app, connection);
        if (!strcmp(url, "/api/image-controls")) return image_control_request(app, connection, NULL);
        if (!strcmp(url, "/api/motion")) return motion_request(app, connection, NULL);
        if (!strcmp(url, "/api/captures")) return list_photos(app, connection);
        if (!strcmp(url, "/stream.mjpg")) return start_stream(app, connection);
        if (g_str_has_prefix(url, "/api/captures/")) return photo_route(app, connection, url + strlen("/api/captures/"), FALSE);
        if (!strcmp(url, "/favicon.ico")) return reply_buffer(connection, 204, "", 0, "image/x-icon");
        return serve_static(app, connection, url);
    }
    if (!same_origin(connection)) return reply_error(connection, 403, "请从相机网页执行此操作");
    if (!strcmp(method, "POST") && !strcmp(url, "/api/settings")) return change_settings(app, connection, request);
    if (!strcmp(method, "POST") && !strcmp(url, "/api/image-controls")) return image_control_request(app, connection, request);
    if (!strcmp(method, "POST") && !strcmp(url, "/api/motion")) return motion_request(app, connection, request);
    if (request->body_size) return reply_error(connection, 400, "拍照和删除请求无需附带数据");
    if (!strcmp(method, "POST") && !strcmp(url, "/api/captures")) return take_photo(app, connection);
    if (!strcmp(method, "POST") && g_str_has_prefix(url, "/api/captures/"))
        return check_photo_quality(app, connection, url + strlen("/api/captures/"));
    if (!strcmp(method, "DELETE") && g_str_has_prefix(url, "/api/captures/"))
        return photo_route(app, connection, url + strlen("/api/captures/"), TRUE);
    return reply_error(connection, 405, "不支持此操作");
}

static void request_done(void *opaque, struct MHD_Connection *connection, void **context,
                         enum MHD_RequestTerminationCode reason) {
    (void)opaque; (void)connection; (void)reason;
    free(*context); *context = NULL;
}

static void stop_pipeline(App *app) {
    pthread_mutex_lock(&app->mutex);
    app->capture_stop = TRUE;
    Frame *last = app->latest; app->latest = NULL;
    pthread_cond_broadcast(&app->changed);
    pthread_mutex_unlock(&app->mutex);
    if (app->pipeline) gst_element_set_state(app->pipeline, GST_STATE_NULL);
    if (app->capture_thread_started) pthread_join(app->capture_thread, NULL);
    app->capture_thread_started = FALSE;
    if (app->bus) gst_object_unref(app->bus);
    if (app->sink) gst_object_unref(app->sink);
    if (app->pipeline) gst_object_unref(app->pipeline);
    app->bus = NULL; app->sink = NULL; app->pipeline = NULL;
    frame_unref(last);
}

/* 只由主线程重建采集链；HTTP连接、帧号和已拍照片继续保留。 */
static gboolean start_pipeline(App *app, Settings settings, unsigned version) {
    char *description = g_strdup_printf(
        "v4l2src name=source io-mode=mmap ! video/x-raw,format=NV12,width=%u,height=%u,framerate=30/1 "
        "! videorate drop-only=true ! video/x-raw,framerate=%u/1 ! jpegenc quality=%u "
        "! appsink name=frames max-buffers=1 drop=true sync=false wait-on-eos=false",
        settings.width, settings.height, settings.preview_fps, settings.jpeg_quality);
    GError *error = NULL;
    app->pipeline = gst_parse_launch(description, &error);
    g_free(description);
    if (!app->pipeline || error) {
        set_error(app, error ? error->message : "无法建立相机采集链");
        g_clear_error(&error); stop_pipeline(app); return FALSE;
    }
    GstElement *source = gst_bin_get_by_name(GST_BIN(app->pipeline), "source");
    g_object_set(source, "device", app->device, NULL); gst_object_unref(source);
    app->sink = gst_bin_get_by_name(GST_BIN(app->pipeline), "frames");
    app->bus = gst_element_get_bus(app->pipeline);
    pthread_mutex_lock(&app->mutex);
    app->settings = settings; app->settings_version = version; app->capture_stop = FALSE;
    pthread_mutex_unlock(&app->mutex);
    if (gst_element_set_state(app->pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        set_error(app, "无法以所选参数启动相机"); stop_pipeline(app); return FALSE;
    }
    if (pthread_create(&app->capture_thread, NULL, capture_frames, app)) {
        set_error(app, "无法启动相机采集线程"); stop_pipeline(app); return FALSE;
    }
    app->capture_thread_started = TRUE;
    double deadline = mono_ms() + 8000;
    while (!stop_requested && mono_ms() < deadline) {
        pthread_mutex_lock(&app->mutex);
        gboolean received = app->latest && app->latest->settings_version == version;
        if (!received) {
            struct timespec wait = deadline_after(200);
            pthread_cond_timedwait(&app->changed, &app->mutex, &wait);
            received = app->latest && app->latest->settings_version == version;
        }
        pthread_mutex_unlock(&app->mutex);
        if (received) return TRUE;
        GstMessage *message = gst_bus_pop_filtered(app->bus, GST_MESSAGE_ERROR | GST_MESSAGE_EOS);
        if (message) {
            if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
                GError *failure = NULL; char *debug = NULL;
                gst_message_parse_error(message, &failure, &debug);
                set_error(app, failure->message); g_error_free(failure); g_free(debug);
            } else set_error(app, "所选参数未能持续取得图像");
            gst_message_unref(message); stop_pipeline(app); return FALSE;
        }
    }
    set_error(app, "切换参数后8秒内未收到新图像"); stop_pipeline(app); return FALSE;
}

static gboolean save_settings(App *app, Settings settings) {
    if (!app->config_path) return TRUE;
    GKeyFile *file = g_key_file_new();
    g_key_file_set_integer(file, "camera", "width", (int)settings.width);
    g_key_file_set_integer(file, "camera", "height", (int)settings.height);
    g_key_file_set_integer(file, "camera", "preview_fps", (int)settings.preview_fps);
    g_key_file_set_integer(file, "camera", "jpeg_quality", (int)settings.jpeg_quality);
    gsize size; char *text = g_key_file_to_data(file, &size, NULL);
    GError *error = NULL;
    gboolean saved = g_file_set_contents(app->config_path, text, (gssize)size, &error);
    if (error) { set_error(app, error->message); g_error_free(error); }
    g_free(text); g_key_file_unref(file);
    return saved;
}

static gboolean apply_pending_settings(App *app) {
    pthread_mutex_lock(&app->mutex);
    Settings previous = app->settings, requested = app->pending_settings;
    unsigned previous_version = app->settings_version;
    app->settings_pending = FALSE;
    pthread_mutex_unlock(&app->mutex);
    stop_pipeline(app);
    gboolean success = start_pipeline(app, requested, previous_version + 1);
    if (success) success = save_settings(app, requested);
    gboolean healthy = success;
    char failure[512] = "";
    if (!success) {
        pthread_mutex_lock(&app->mutex); g_strlcpy(failure, app->error, sizeof(failure)); pthread_mutex_unlock(&app->mutex);
        stop_pipeline(app);
        healthy = start_pipeline(app, previous, previous_version);
    }
    pthread_mutex_lock(&app->mutex);
    if (!success) g_snprintf(app->settings_error, sizeof(app->settings_error), "参数应用失败，%s：%.350s",
                            healthy ? "已恢复原设置" : "恢复失败，服务将重新启动", failure);
    app->reconfiguring = FALSE;
    pthread_cond_broadcast(&app->changed);
    pthread_mutex_unlock(&app->mutex);
    if (success) fprintf(stdout, "settings=%u resolution=%ux%u preview_fps=%u jpeg_quality=%u\n",
                          previous_version + 1, requested.width, requested.height, requested.preview_fps, requested.jpeg_quality);
    return healthy;
}

int main(int argc, char **argv) {
    const char *device = "/dev/video-camera0", *web_dir = "web", *config_path = NULL, *motor_port = NULL;
    unsigned port = 8080;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--web-dir") && i + 1 < argc) web_dir = argv[++i];
        else if (!strcmp(argv[i], "--device") && i + 1 < argc) device = argv[++i];
        else if (!strcmp(argv[i], "--config") && i + 1 < argc) config_path = argv[++i];
        else if (!strcmp(argv[i], "--motor-port") && i + 1 < argc) motor_port = argv[++i];
        else if (!strcmp(argv[i], "--port") && i + 1 < argc) {
            char *end = NULL; unsigned long value = strtoul(argv[++i], &end, 10);
            if (!value || value > 65535 || *end) { fprintf(stderr, "Invalid port\n"); return 2; }
            port = (unsigned)value;
        } else { fprintf(stderr, "Usage: %s [--port 8080] [--device /dev/video-camera0] [--web-dir web] [--config camera.ini] [--motor-port /dev/ttyS4]\n", argv[0]); return 2; }
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGINT, stop_signal); signal(SIGTERM, stop_signal); signal(SIGPIPE, SIG_IGN);
    gst_init(NULL, NULL);
    App app = { .started = mono_ms(), .web_dir = web_dir, .device = device, .config_path = config_path };
    pthread_mutex_init(&app.mutex, NULL);
    pthread_mutex_init(&app.settings_mutex, NULL);
    pthread_mutex_init(&app.quality_mutex, NULL);
    pthread_condattr_t cond_attr;
    pthread_condattr_init(&cond_attr);
    pthread_condattr_setclock(&cond_attr, CLOCK_MONOTONIC);
    pthread_cond_init(&app.changed, &cond_attr);
    pthread_condattr_destroy(&cond_attr);
    char *uuid = g_uuid_string_random(); memcpy(app.session, uuid, 8); g_free(uuid);
    Settings initial = {1280, 720, 10, 85};
    if (config_path) {
        GKeyFile *file = g_key_file_new();
        if (g_key_file_load_from_file(file, config_path, G_KEY_FILE_NONE, NULL)) {
            Settings stored = {(unsigned)g_key_file_get_integer(file, "camera", "width", NULL),
                (unsigned)g_key_file_get_integer(file, "camera", "height", NULL),
                (unsigned)g_key_file_get_integer(file, "camera", "preview_fps", NULL),
                (unsigned)g_key_file_get_integer(file, "camera", "jpeg_quality", NULL)};
            if (settings_valid(stored)) initial = stored;
            else fprintf(stderr, "Invalid saved settings, using 720p defaults\n");
        }
        g_key_file_unref(file);
    }
    if (!start_pipeline(&app, initial, 1)) return 1;
    app.motor = motor_control_new(motor_port);
    struct MHD_Daemon *http = MHD_start_daemon(MHD_USE_INTERNAL_POLLING_THREAD | MHD_USE_THREAD_PER_CONNECTION | MHD_USE_ERROR_LOG,
        (uint16_t)port, NULL, NULL, handle_request, &app,
        MHD_OPTION_CONNECTION_LIMIT, (unsigned int)16,
        MHD_OPTION_PER_IP_CONNECTION_LIMIT, (unsigned int)12,
        MHD_OPTION_CONNECTION_TIMEOUT, (unsigned int)10,
        MHD_OPTION_CONNECTION_MEMORY_LIMIT, (size_t)(32 * 1024),
        MHD_OPTION_NOTIFY_COMPLETED, request_done, NULL, MHD_OPTION_END);
    int result = http ? 0 : 1;
    fprintf(stdout, "opto-camera port=%u device=%s source=%ux%u@30 preview=%u quality=%u session=%s\n",
            port, device, initial.width, initial.height, initial.preview_fps, initial.jpeg_quality, app.session);
    while (http && !stop_requested) {
        pthread_mutex_lock(&app.mutex);
        gboolean pending = app.settings_pending;
        pthread_mutex_unlock(&app.mutex);
        if (pending && !apply_pending_settings(&app)) { result = 1; break; }
        GstMessage *message = gst_bus_timed_pop_filtered(app.bus, 250 * GST_MSECOND, GST_MESSAGE_ERROR | GST_MESSAGE_EOS);
        if (message) {
            if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
                GError *gst_error = NULL; char *debug = NULL;
                gst_message_parse_error(message, &gst_error, &debug);
                set_error(&app, gst_error->message); g_error_free(gst_error); g_free(debug);
            } else set_error(&app, "相机采集已结束");
            gst_message_unref(message); result = 1; break;
        }
        pthread_mutex_lock(&app.mutex);
        prune_photos(&app);
        gboolean stalled = mono_ms() - (app.latest ? app.latest->received_mono : app.started) > 10000;
        pthread_mutex_unlock(&app.mutex);
        if (stalled) { set_error(&app, "相机超过10秒没有新帧，服务将重新启动"); result = 1; break; }
    }
    pthread_mutex_lock(&app.mutex); app.quit = TRUE; pthread_cond_broadcast(&app.changed); pthread_mutex_unlock(&app.mutex);
    if (http) MHD_stop_daemon(http);
    motor_control_free(app.motor);
    stop_pipeline(&app);
    for (unsigned i = 0; i < CAPTURE_LIMIT; ++i) remove_photo(&app, &app.photos[i]);
    pthread_cond_destroy(&app.changed); pthread_mutex_destroy(&app.quality_mutex);
    pthread_mutex_destroy(&app.settings_mutex); pthread_mutex_destroy(&app.mutex);
    gst_deinit();
    return result;
}
