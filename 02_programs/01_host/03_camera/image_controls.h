#ifndef OPTO_IMAGE_CONTROLS_H
#define OPTO_IMAGE_CONTROLS_H

#include <glib.h>

G_BEGIN_DECLS

#define IMAGE_CONTROLS_ERROR (image_controls_error_quark())

typedef enum {
    IMAGE_CONTROLS_ERROR_INVALID_ARGUMENT = 1,
    IMAGE_CONTROLS_ERROR_UNAVAILABLE = 2,
    IMAGE_CONTROLS_ERROR_APPLY_FAILED = 3
} ImageControlsError;

GQuark image_controls_error_quark(void);

/*
 * 调用方用settings_mutex串行处理GET和POST；不要持有相机图像锁。
 * 成功返回JSON字符串，由调用方g_free；失败返回NULL并设置error。
 * 设置仅作用于当前3A运行期，不写持久文件，也不重启采集或3A服务。
 * fields为表单的字符串键/字符串值；未知字段、非有限数字和越界值均拒绝。
 */
gchar *image_controls_read_json(GError **error);
gchar *image_controls_apply_json(GHashTable *fields, GError **error);

G_END_DECLS

#endif
