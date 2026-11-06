#ifndef OPTO_IMAGE_QUALITY_H
#define OPTO_IMAGE_QUALITY_H

#include <glib.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    unsigned width, height;
    double mean_brightness;
    double brightness_stddev;
    double dark_percent;
    double bright_percent;
    double sharpness_score;
    uint64_t histogram[256];
} ImageQuality;

/* 宿主先调用gst_init；本函数只解码传入的JPEG，不连接相机。
 * 亮度范围为0～255，比例单位为百分比。清晰度为灰度拉普拉斯方差，
 * 仅适合同场景、同分辨率比较，不代表对焦或识别准确率。
 * 失败时out清零；调用方按GLib惯例传入空的GError指针。 */
gboolean image_quality_analyze(const unsigned char *jpeg, size_t jpeg_size,
                               ImageQuality *out, GError **error);

#endif
