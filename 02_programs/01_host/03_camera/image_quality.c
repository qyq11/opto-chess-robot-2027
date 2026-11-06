#include "image_quality.h"

#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <gst/video/video.h>
#include <math.h>
#include <stddef.h>
#include <string.h>

#define QUALITY_MAX_JPEG_BYTES (4U * 1024U * 1024U)
#define QUALITY_MAX_PIXELS (3840U * 2160U)
#define QUALITY_TIMEOUT_US (5 * G_TIME_SPAN_SECOND)

static GQuark quality_error_domain(void) {
    return g_quark_from_static_string("opto-image-quality-error");
}

/* 解码前先限制JPEG头中声明的尺寸，避免小文件要求分配过大的图像。 */
static gboolean jpeg_dimensions(const unsigned char *jpeg, size_t size,
                                 unsigned *width, unsigned *height, GError **error) {
    if (size < 4 || jpeg[0] != 0xff || jpeg[1] != 0xd8) {
        g_set_error_literal(error, quality_error_domain(), 1, "输入不是有效的JPEG照片");
        return FALSE;
    }
    size_t position = 2;
    while (position < size) {
        if (jpeg[position++] != 0xff) break;
        while (position < size && jpeg[position] == 0xff) ++position;
        if (position >= size) break;
        unsigned marker = jpeg[position++];
        if (marker == 0xda || marker == 0xd9 || marker == 0x00) break;
        if (marker == 0xd8 || marker == 0x01 || (marker >= 0xd0 && marker <= 0xd7)) continue;
        if (size - position < 2) break;
        size_t length = ((size_t)jpeg[position] << 8) | jpeg[position + 1];
        if (length < 2 || length > size - position) break;
        gboolean frame_marker = marker >= 0xc0 && marker <= 0xcf
            && marker != 0xc4 && marker != 0xc8 && marker != 0xcc;
        if (frame_marker) {
            if (length < 8) break;
            *height = ((unsigned)jpeg[position + 3] << 8) | jpeg[position + 4];
            *width = ((unsigned)jpeg[position + 5] << 8) | jpeg[position + 6];
            if (!*width || !*height || (uint64_t)*width * *height > QUALITY_MAX_PIXELS) {
                g_set_error_literal(error, quality_error_domain(), 2, "照片像素数超过3840×2160上限或尺寸无效");
                return FALSE;
            }
            return TRUE;
        }
        position += length;
    }
    g_set_error_literal(error, quality_error_domain(), 1, "无法读取JPEG照片的图像尺寸");
    return FALSE;
}

static gboolean decoder_message(GstBus *bus, GError **error) {
    GstMessage *message = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR);
    if (!message) return FALSE;
    GError *cause = NULL;
    gchar *debug = NULL;
    gst_message_parse_error(message, &cause, &debug);
    g_set_error(error, quality_error_domain(), 3, "JPEG解码失败：%s", cause ? cause->message : "未知解码错误");
    g_clear_error(&cause);
    g_free(debug);
    gst_message_unref(message);
    return TRUE;
}

static void gray_statistics(const GstVideoFrame *frame, ImageQuality *result) {
    const unsigned width = result->width, height = result->height;
    const guint8 *pixels = GST_VIDEO_FRAME_PLANE_DATA(frame, 0);
    const ptrdiff_t stride = GST_VIDEO_FRAME_PLANE_STRIDE(frame, 0);
    uint64_t sum = 0, sum_squared = 0;
    for (unsigned y = 0; y < height; ++y) {
        const guint8 *row = pixels + (ptrdiff_t)y * stride;
        for (unsigned x = 0; x < width; ++x) {
            unsigned value = row[x];
            ++result->histogram[value];
            sum += value;
            sum_squared += (uint64_t)value * value;
        }
    }
    const double count = (double)width * height;
    result->mean_brightness = sum / count;
    double variance = sum_squared / count - result->mean_brightness * result->mean_brightness;
    result->brightness_stddev = sqrt(fmax(0.0, variance));
    uint64_t dark = 0, bright = 0;
    for (unsigned value = 0; value <= 16; ++value) dark += result->histogram[value];
    for (unsigned value = 245; value < 256; ++value) bright += result->histogram[value];
    result->dark_percent = 100.0 * dark / count;
    result->bright_percent = 100.0 * bright / count;

    if (width < 3 || height < 3) return;
    int64_t laplacian_sum = 0;
    uint64_t laplacian_squared = 0;
    /* 只统计具有四个邻居的内部像素，不将行末填充字节当作图像。 */
    for (unsigned y = 1; y + 1 < height; ++y) {
        const guint8 *row = pixels + (ptrdiff_t)y * stride;
        const guint8 *above = row - stride;
        const guint8 *below = row + stride;
        for (unsigned x = 1; x + 1 < width; ++x) {
            int value = (int)row[x - 1] + row[x + 1] + above[x] + below[x] - 4 * (int)row[x];
            laplacian_sum += value;
            laplacian_squared += (uint64_t)((int64_t)value * value);
        }
    }
    const double interior_count = (double)(width - 2) * (height - 2);
    const double mean = laplacian_sum / interior_count;
    result->sharpness_score = fmax(0.0, laplacian_squared / interior_count - mean * mean);
}

gboolean image_quality_analyze(const unsigned char *jpeg, size_t jpeg_size,
                               ImageQuality *out, GError **error) {
    if (out) memset(out, 0, sizeof(*out));
    if (!out || !jpeg || !jpeg_size || jpeg_size > QUALITY_MAX_JPEG_BYTES) {
        g_set_error_literal(error, quality_error_domain(), 1, "照片数据为空或超过4MiB上限");
        return FALSE;
    }
    if (!gst_is_initialized()) {
        g_set_error_literal(error, quality_error_domain(), 3, "图像解码环境尚未初始化");
        return FALSE;
    }
    unsigned jpeg_width = 0, jpeg_height = 0;
    if (!jpeg_dimensions(jpeg, jpeg_size, &jpeg_width, &jpeg_height, error)) return FALSE;
    const gint64 deadline = g_get_monotonic_time() + QUALITY_TIMEOUT_US;
    gboolean success = FALSE, mapped = FALSE;
    GstElement *pipeline = NULL, *source = NULL, *sink = NULL;
    GstBus *bus = NULL;
    GstSample *sample = NULL;
    GstBuffer *input = NULL;
    GstVideoFrame frame;
    memset(&frame, 0, sizeof(frame));
    GError *cause = NULL;
    pipeline = gst_parse_launch(
        "appsrc name=jpeg_source format=bytes block=false ! jpegdec ! videoconvert "
        "! video/x-raw,format=GRAY8 ! appsink name=gray_frames max-buffers=1 drop=true "
        "sync=false wait-on-eos=false", &cause);
    if (!pipeline || cause) {
        g_set_error(error, quality_error_domain(), 3, "无法建立照片解码链：%s", cause ? cause->message : "缺少解码组件");
        g_clear_error(&cause);
        goto cleanup;
    }
    source = gst_bin_get_by_name(GST_BIN(pipeline), "jpeg_source");
    sink = gst_bin_get_by_name(GST_BIN(pipeline), "gray_frames");
    bus = gst_element_get_bus(pipeline);
    if (!source || !sink || !bus) {
        g_set_error_literal(error, quality_error_domain(), 3, "照片解码链缺少输入或输出组件");
        goto cleanup;
    }
    GstCaps *caps = gst_caps_new_simple("image/jpeg", "width", G_TYPE_INT, (gint)jpeg_width,
        "height", G_TYPE_INT, (gint)jpeg_height, "framerate", GST_TYPE_FRACTION, 0, 1, NULL);
    if (!caps) {
        g_set_error_literal(error, quality_error_domain(), 3, "无法分配JPEG格式说明");
        goto cleanup;
    }
    gst_app_src_set_caps(GST_APP_SRC(source), caps);
    gst_caps_unref(caps);
    if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        if (!decoder_message(bus, error)) g_set_error_literal(error, quality_error_domain(), 3, "无法启动照片解码");
        goto cleanup;
    }
    input = gst_buffer_new_allocate(NULL, jpeg_size, NULL);
    if (!input || gst_buffer_fill(input, 0, jpeg, jpeg_size) != jpeg_size) {
        g_set_error_literal(error, quality_error_domain(), 3, "无法分配JPEG解码输入");
        goto cleanup;
    }
    GstFlowReturn flow = gst_app_src_push_buffer(GST_APP_SRC(source), input);
    input = NULL; /* appsrc接管缓冲区，包括推送失败的情况。 */
    if (flow != GST_FLOW_OK || gst_app_src_end_of_stream(GST_APP_SRC(source)) != GST_FLOW_OK) {
        if (!decoder_message(bus, error)) g_set_error_literal(error, quality_error_domain(), 3, "无法提交JPEG照片进行解码");
        goto cleanup;
    }
    while (g_get_monotonic_time() < deadline) {
        gint64 remaining = deadline - g_get_monotonic_time();
        if (remaining <= 0) break;
        GstClockTime wait = (GstClockTime)MIN(remaining, (gint64)100000) * GST_USECOND;
        sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), wait);
        if (sample) break;
        if (decoder_message(bus, error)) goto cleanup;
        if (gst_app_sink_is_eos(GST_APP_SINK(sink))) {
            g_set_error_literal(error, quality_error_domain(), 3, "JPEG解码结束但没有取得图像");
            goto cleanup;
        }
    }
    if (!sample) {
        g_set_error_literal(error, quality_error_domain(), 4, "JPEG解码5秒内未返回图像");
        goto cleanup;
    }
    GstVideoInfo info;
    gst_video_info_init(&info);
    GstCaps *decoded_caps = gst_sample_get_caps(sample);
    GstBuffer *decoded = gst_sample_get_buffer(sample);
    if (!decoded_caps || !decoded || !gst_video_info_from_caps(&info, decoded_caps)
        || GST_VIDEO_INFO_FORMAT(&info) != GST_VIDEO_FORMAT_GRAY8) {
        g_set_error_literal(error, quality_error_domain(), 3, "解码结果不是可读取的GRAY8灰度图像");
        goto cleanup;
    }
    unsigned width = GST_VIDEO_INFO_WIDTH(&info), height = GST_VIDEO_INFO_HEIGHT(&info);
    if (!width || !height || (uint64_t)width * height > QUALITY_MAX_PIXELS
        || width != jpeg_width || height != jpeg_height) {
        g_set_error_literal(error, quality_error_domain(), 2, "解码图像尺寸无效、超限或与JPEG记录不一致");
        goto cleanup;
    }
    if (!gst_video_frame_map(&frame, &info, decoded, GST_MAP_READ)) {
        g_set_error_literal(error, quality_error_domain(), 3, "无法读取解码后的灰度像素");
        goto cleanup;
    }
    mapped = TRUE;
    int64_t stride = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0);
    if (!GST_VIDEO_FRAME_PLANE_DATA(&frame, 0) || (stride >= 0 ? stride : -stride) < width) {
        g_set_error_literal(error, quality_error_domain(), 3, "灰度图像的行跨度无效");
        goto cleanup;
    }
    ImageQuality result = { .width = width, .height = height };
    gray_statistics(&frame, &result);
    *out = result;
    success = TRUE;

cleanup:
    if (mapped) gst_video_frame_unmap(&frame);
    if (sample) gst_sample_unref(sample);
    if (input) gst_buffer_unref(input);
    if (pipeline) gst_element_set_state(pipeline, GST_STATE_NULL);
    if (bus) gst_object_unref(bus);
    if (sink) gst_object_unref(sink);
    if (source) gst_object_unref(source);
    if (pipeline) gst_object_unref(pipeline);
    return success;
}
