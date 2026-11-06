#include "image_controls.h"
#include "rkaiq_client.h"

#include <errno.h>
#include <math.h>
#include <string.h>

#define AE_ATTR "/uapi/0/ae_uapi/expSwAttr"
#define AE_INFO "/uapi/0/ae_uapi/queryExpInfo"
#define WB_MODE "/uapi/0/awb_uapi/mode"
#define WB_GAIN "/uapi/0/awb_uapi/wbgain"
#define IPC_TIMEOUT_MS RKAIQ_CLIENT_DEFAULT_TIMEOUT_MS
#define VERIFY_US (1500 * 1000)

typedef struct {
    JsonNode *ae_attr, *ae_info, *wb_mode, *wb_gain;
} ControlState;

typedef struct {
    gboolean manual, converged, manual_time_enabled, manual_gain_enabled, manual_isp_enabled;
    double manual_time_ms, manual_gain, manual_isp_gain, actual_time_ms, actual_gain;
    double time_min_ms, time_max_ms, gain_min, gain_max;
} ExposureView;

typedef struct { gboolean manual; double gain[4]; } WhiteBalanceView;
typedef struct { ExposureView exposure; WhiteBalanceView white_balance; } ControlView;
typedef struct { gboolean exposure, manual; double time_ms, gain, wb_gain[4]; } Change;

static const gchar *wb_keys[4] = { "rgain", "grgain", "gbgain", "bgain" };
static guint settings_version;

GQuark image_controls_error_quark(void) {
    return g_quark_from_static_string("opto-image-controls-error");
}

static gboolean schema_error(GError **error, const gchar *field) {
    if (!error || !*error)
        g_set_error(error, IMAGE_CONTROLS_ERROR, IMAGE_CONTROLS_ERROR_UNAVAILABLE,
                    "3A参数字段%s缺失、类型不符或数值无效", field);
    return FALSE;
}

static JsonObject *as_object(JsonNode *node, const gchar *field, GError **error) {
    if (node && JSON_NODE_HOLDS_OBJECT(node)) return json_node_get_object(node);
    schema_error(error, field);
    return NULL;
}

static JsonNode *member(JsonObject *object, const gchar *name, GError **error) {
    if (object && json_object_has_member(object, name)) return json_object_get_member(object, name);
    schema_error(error, name);
    return NULL;
}

static JsonObject *object_member(JsonObject *object, const gchar *name, GError **error) {
    JsonNode *node = member(object, name, error);
    return node ? as_object(node, name, error) : NULL;
}

static gboolean number_member(JsonObject *object, const gchar *name, double *value, GError **error) {
    JsonNode *node = member(object, name, error);
    if (!node) return FALSE;
    GType type = json_node_get_value_type(node);
    if (!JSON_NODE_HOLDS_VALUE(node) || (type != G_TYPE_INT64 && type != G_TYPE_DOUBLE))
        return schema_error(error, name);
    *value = json_node_get_double(node);
    return isfinite(*value) ? TRUE : schema_error(error, name);
}

static gboolean flag_member(JsonObject *object, const gchar *name, gboolean *value, GError **error) {
    JsonNode *node = member(object, name, error);
    if (!node || !JSON_NODE_HOLDS_VALUE(node)) return schema_error(error, name);
    if (json_node_get_value_type(node) == G_TYPE_BOOLEAN) {
        *value = json_node_get_boolean(node);
        return TRUE;
    }
    if (json_node_get_value_type(node) == G_TYPE_INT64) {
        gint64 flag = json_node_get_int(node);
        if (flag == 0 || flag == 1) { *value = flag != 0; return TRUE; }
    }
    return schema_error(error, name);
}

static gboolean mode_member(JsonObject *object, const gchar *automatic, const gchar *manual,
                             const gchar *field, gboolean *is_manual, GError **error) {
    JsonNode *node = member(object, field, error);
    if (!node || !JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_STRING)
        return schema_error(error, field);
    const gchar *mode = json_node_get_string(node);
    if (g_strcmp0(mode, automatic) == 0) { *is_manual = FALSE; return TRUE; }
    if (g_strcmp0(mode, manual) == 0) { *is_manual = TRUE; return TRUE; }
    return schema_error(error, field);
}

static gboolean exposure_view(JsonNode *attr_node, JsonNode *info_node,
                                ExposureView *view, GError **error) {
    JsonObject *attr = as_object(attr_node, AE_ATTR, error);
    JsonObject *info = as_object(info_node, AE_INFO, error);
    if (!attr || !info) return FALSE;
    JsonObject *comm = object_member(attr, "commCtrl", error);
    JsonObject *control = comm ? object_member(comm, "meCtrl", error) : NULL;
    JsonObject *manual = control ? object_member(control, "linMe", error) : NULL;
    JsonObject *linear = object_member(info, "linExpInfo", error);
    JsonObject *range = linear ? object_member(linear, "expRange", error) : NULL;
    JsonObject *actual = linear ? object_member(linear, "expParam", error) : NULL;
    if (!comm || !manual || !range || !actual) return FALSE;
    double manual_seconds, actual_seconds, min_seconds, max_seconds, analog, digital, isp;
    if (!mode_member(comm, "RK_AIQ_OP_MODE_AUTO", "RK_AIQ_OP_MODE_MANUAL", "sw_aeT_opt_mode", &view->manual, error)
        || !flag_member(manual, "sw_aeT_manTime_en", &view->manual_time_enabled, error)
        || !flag_member(manual, "sw_aeT_manGain_en", &view->manual_gain_enabled, error)
        || !flag_member(manual, "sw_aeT_manIspDGain_en", &view->manual_isp_enabled, error)
        || !number_member(manual, "sw_aeT_manTime_val", &manual_seconds, error)
        || !number_member(manual, "sw_aeT_manGain_val", &view->manual_gain, error)
        || !number_member(manual, "sw_aeT_manIspDGain_val", &view->manual_isp_gain, error)
        || !number_member(actual, "integration_time", &actual_seconds, error)
        || !number_member(actual, "analog_gain", &analog, error)
        || !number_member(actual, "digital_gain", &digital, error)
        || !number_member(actual, "isp_dgain", &isp, error)
        || !number_member(range, "sw_aeT_time_min", &min_seconds, error)
        || !number_member(range, "sw_aeT_time_max", &max_seconds, error)
        || !number_member(range, "sw_aeT_gain_min", &view->gain_min, error)
        || !number_member(range, "sw_aeT_gain_max", &view->gain_max, error)
        || !flag_member(info, "isConverged", &view->converged, error)) return FALSE;
    view->manual_time_ms = manual_seconds * 1000.0;
    view->actual_time_ms = actual_seconds * 1000.0;
    view->time_min_ms = min_seconds * 1000.0;
    view->time_max_ms = max_seconds * 1000.0;
    view->actual_gain = analog * digital * isp;
    if (!(view->manual_time_ms > 0 && view->manual_gain > 0 && view->manual_isp_gain > 0
          && view->actual_time_ms > 0 && analog > 0 && digital > 0 && isp > 0
          && view->time_min_ms > 0 && view->time_max_ms >= view->time_min_ms
          && view->gain_min > 0 && view->gain_max >= view->gain_min)
        || !isfinite(view->manual_time_ms) || !isfinite(view->actual_time_ms)
        || !isfinite(view->time_min_ms) || !isfinite(view->time_max_ms)
        || !isfinite(view->actual_gain)) return schema_error(error, "线性曝光和增益范围");
    return TRUE;
}

static gboolean white_balance_view(JsonNode *mode_node, JsonNode *gain_node,
                                     WhiteBalanceView *view, GError **error) {
    JsonObject *mode = as_object(mode_node, WB_MODE, error);
    JsonObject *gain = as_object(gain_node, WB_GAIN, error);
    if (!mode || !gain || !mode_member(mode, "OP_AUTO", "OP_MANUAL", "mode", &view->manual, error)) return FALSE;
    for (guint i = 0; i < 4; ++i)
        if (!number_member(gain, wb_keys[i], &view->gain[i], error)
            || view->gain[i] <= 0) return schema_error(error, wb_keys[i]);
    return TRUE;
}

static void state_clear(ControlState *state) {
    if (state->ae_attr) json_node_unref(state->ae_attr);
    if (state->ae_info) json_node_unref(state->ae_info);
    if (state->wb_mode) json_node_unref(state->wb_mode);
    if (state->wb_gain) json_node_unref(state->wb_gain);
    memset(state, 0, sizeof(*state));
}

static gboolean get_node(const gchar *path, guint timeout_ms, JsonNode **node, GError **error) {
    GError *inner = NULL;
    if (rkaiq_client_get(RKAIQ_CLIENT_DEFAULT_SOCKET, path, timeout_ms, node, &inner)) return TRUE;
    g_set_error(error, IMAGE_CONTROLS_ERROR, IMAGE_CONTROLS_ERROR_UNAVAILABLE,
                "读取%s失败：%s", path, inner ? inner->message : "未收到有效回复");
    g_clear_error(&inner);
    return FALSE;
}

static gboolean put_node(const gchar *path, JsonNode *node, GError **error) {
    GError *inner = NULL;
    if (rkaiq_client_set(RKAIQ_CLIENT_DEFAULT_SOCKET, path, node, IPC_TIMEOUT_MS, &inner)) return TRUE;
    g_set_error(error, IMAGE_CONTROLS_ERROR, IMAGE_CONTROLS_ERROR_APPLY_FAILED,
                "设置%s失败：%s", path, inner ? inner->message : "写入报文未完整提交");
    g_clear_error(&inner);
    return FALSE;
}

static gboolean state_load(ControlState *state, ControlView *view, GError **error) {
    if (!get_node(AE_ATTR, IPC_TIMEOUT_MS, &state->ae_attr, error)
        || !get_node(AE_INFO, IPC_TIMEOUT_MS, &state->ae_info, error)
        || !get_node(WB_MODE, IPC_TIMEOUT_MS, &state->wb_mode, error)
        || !get_node(WB_GAIN, IPC_TIMEOUT_MS, &state->wb_gain, error)) return FALSE;
    return exposure_view(state->ae_attr, state->ae_info, &view->exposure, error)
        && white_balance_view(state->wb_mode, state->wb_gain, &view->white_balance, error);
}

static void add_number(JsonBuilder *builder, const gchar *key, double value) {
    json_builder_set_member_name(builder, key);
    json_builder_add_double_value(builder, value);
}

static void add_mode(JsonBuilder *builder, gboolean manual) {
    json_builder_set_member_name(builder, "mode");
    json_builder_add_string_value(builder, manual ? "manual" : "auto");
}

static gchar *view_json(const ControlView *view, guint version) {
    JsonBuilder *builder = json_builder_new();
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "connected"); json_builder_add_boolean_value(builder, TRUE);
    json_builder_set_member_name(builder, "error"); json_builder_add_string_value(builder, "");
    json_builder_set_member_name(builder, "version"); json_builder_add_int_value(builder, version);
    json_builder_set_member_name(builder, "exposure"); json_builder_begin_object(builder);
    const ExposureView *e = &view->exposure;
    add_mode(builder, e->manual);
    add_number(builder, "manual_time_ms", e->manual_time_ms);
    add_number(builder, "manual_gain", e->manual_gain);
    add_number(builder, "actual_time_ms", e->actual_time_ms);
    add_number(builder, "actual_gain", e->actual_gain);
    add_number(builder, "time_min_ms", e->time_min_ms);
    add_number(builder, "time_max_ms", e->time_max_ms);
    add_number(builder, "gain_min", e->gain_min); add_number(builder, "gain_max", e->gain_max);
    json_builder_set_member_name(builder, "converged"); json_builder_add_boolean_value(builder, e->converged);
    json_builder_end_object(builder);
    json_builder_set_member_name(builder, "white_balance"); json_builder_begin_object(builder);
    add_mode(builder, view->white_balance.manual);
    for (guint i = 0; i < 4; ++i) add_number(builder, wb_keys[i], view->white_balance.gain[i]);
    add_number(builder, "gain_min", 1); add_number(builder, "gain_max", 8);
    json_builder_end_object(builder); json_builder_end_object(builder);
    JsonNode *root = json_builder_get_root(builder);
    JsonGenerator *generator = json_generator_new();
    json_generator_set_root(generator, root);
    gchar *result = json_generator_to_data(generator, NULL);
    g_object_unref(generator); json_node_unref(root); g_object_unref(builder);
    return result;
}

gchar *image_controls_read_json(GError **error) {
    if (error && *error) return NULL;
    ControlState state = { 0 }; ControlView view = { 0 };
    gchar *result = NULL;
    if (state_load(&state, &view, error)) result = view_json(&view, settings_version);
    state_clear(&state);
    return result;
}

static gboolean invalid(GError **error, const gchar *message) {
    g_set_error_literal(error, IMAGE_CONTROLS_ERROR, IMAGE_CONTROLS_ERROR_INVALID_ARGUMENT, message);
    return FALSE;
}

static gboolean parse_number(GHashTable *fields, const gchar *key, double *value, GError **error) {
    const gchar *text = g_hash_table_lookup(fields, key);
    gchar *end = NULL;
    if (!text || !*text) return invalid(error, "数值字段不能为空");
    errno = 0;
    *value = g_ascii_strtod(text, &end);
    if (errno == ERANGE || end == text || *end || !isfinite(*value))
        return invalid(error, "参数必须是完整的有限数字，不能包含NaN或Infinity");
    return TRUE;
}

static gboolean parse_change(GHashTable *fields, Change *change, GError **error) {
    if (!fields) return invalid(error, "缺少参数表单");
    const gchar *group = g_hash_table_lookup(fields, "group");
    const gchar *mode = g_hash_table_lookup(fields, "mode");
    if (!group || (!g_str_equal(group, "exposure") && !g_str_equal(group, "white_balance")))
        return invalid(error, "group须为exposure或white_balance");
    if (!mode || (!g_str_equal(mode, "auto") && !g_str_equal(mode, "manual")))
        return invalid(error, "mode须为auto或manual");
    change->exposure = g_str_equal(group, "exposure");
    change->manual = g_str_equal(mode, "manual");
    const gchar *allowed[6] = { "group", "mode", NULL, NULL, NULL, NULL };
    guint expected = 2;
    if (change->manual && change->exposure) { allowed[2] = "time_ms"; allowed[3] = "gain"; expected = 4; }
    else if (change->manual) { for (guint i = 0; i < 4; ++i) allowed[i + 2] = wb_keys[i]; expected = 6; }
    if (g_hash_table_size(fields) != expected) return invalid(error, "参数字段数量不符；自动模式只接受group和mode");
    GHashTableIter iter; gpointer key, value;
    g_hash_table_iter_init(&iter, fields);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        gboolean known = FALSE;
        for (guint i = 0; i < expected; ++i) if (key && g_str_equal(key, allowed[i])) { known = TRUE; break; }
        if (!known || !value) return invalid(error, "包含未知字段或空参数");
    }
    if (change->manual && change->exposure)
        return parse_number(fields, "time_ms", &change->time_ms, error)
            && parse_number(fields, "gain", &change->gain, error);
    if (change->manual) {
        for (guint i = 0; i < 4; ++i)
            if (!parse_number(fields, wb_keys[i], &change->wb_gain[i], error)) return FALSE;
        for (guint i = 0; i < 4; ++i)
            if (change->wb_gain[i] < 1 || change->wb_gain[i] > 8)
                return invalid(error, "本轮白平衡手动增益范围为1～8倍");
    }
    return TRUE;
}

static JsonNode *clone_node(JsonNode *source, GError **error) {
    JsonGenerator *generator = json_generator_new(); json_generator_set_root(generator, source);
    gsize length = 0; gchar *text = json_generator_to_data(generator, &length);
    g_object_unref(generator);
    if (!text) { invalid(error, "无法复制原3A配置"); return NULL; }
    JsonParser *parser = json_parser_new(); GError *inner = NULL; JsonNode *result = NULL;
    if (json_parser_load_from_data(parser, text, (gssize)length, &inner))
        result = json_node_copy(json_parser_get_root(parser));
    else {
        g_set_error(error, IMAGE_CONTROLS_ERROR, IMAGE_CONTROLS_ERROR_UNAVAILABLE,
                    "无法复制原3A配置：%s", inner->message);
        g_clear_error(&inner);
    }
    g_object_unref(parser); g_free(text);
    return result;
}

static gboolean close_number(double actual, double expected, double absolute, double relative) {
    return fabs(actual - expected) <= MAX(absolute, fabs(expected) * relative);
}

static guint remaining_timeout(gint64 deadline) {
    gint64 remaining = deadline - g_get_monotonic_time();
    return remaining <= 0 ? 0 : (guint)MIN((remaining + 999) / 1000, (gint64)IPC_TIMEOUT_MS);
}

static void pause_verification(gint64 deadline) {
    gint64 remaining = deadline - g_get_monotonic_time();
    if (remaining > 0) g_usleep((gulong)MIN(remaining, (gint64)100000));
}

/* 只核对线性实际曝光；未使用的HDR分支不能当成当前相机状态。 */
static gboolean verify_exposure(const Change *change, GError **error) {
    gint64 deadline = g_get_monotonic_time() + VERIFY_US;
    ExposureView last = { 0 };
    do {
        JsonNode *attr = NULL, *info = NULL;
        guint timeout = remaining_timeout(deadline);
        if (!timeout) break;
        if (!get_node(AE_ATTR, timeout, &attr, error)) return FALSE;
        timeout = remaining_timeout(deadline);
        if (!timeout) { json_node_unref(attr); break; }
        gboolean loaded = get_node(AE_INFO, timeout, &info, error);
        gboolean valid = loaded && exposure_view(attr, info, &last, error);
        json_node_unref(attr); if (info) json_node_unref(info);
        if (!valid) return FALSE;
        gboolean configured = last.manual == change->manual;
        if (change->manual)
            configured = configured && last.manual_time_enabled && last.manual_gain_enabled && last.manual_isp_enabled
                && close_number(last.manual_time_ms, change->time_ms, 0.001, 0.001)
                && close_number(last.manual_gain, change->gain, 0.001, 0.001)
                && close_number(last.manual_isp_gain, 1, 0.001, 0.001);
        if (configured && (!change->manual
            || (close_number(last.actual_time_ms, change->time_ms, 0.1, 0.02)
                && close_number(last.actual_gain, change->gain, 0, 0.03)))) return TRUE;
        pause_verification(deadline);
    } while (g_get_monotonic_time() < deadline);
    g_set_error(error, IMAGE_CONTROLS_ERROR, IMAGE_CONTROLS_ERROR_APPLY_FAILED,
                "曝光在1.5秒内未通过配置和实际值核对；最后实际时间%.6g毫秒、增益%.6g倍",
                last.actual_time_ms, last.actual_gain);
    return FALSE;
}

static gboolean verify_white_balance(const Change *change, GError **error) {
    gint64 deadline = g_get_monotonic_time() + VERIFY_US;
    do {
        JsonNode *mode = NULL, *gain = NULL; WhiteBalanceView current = { 0 };
        guint timeout = remaining_timeout(deadline);
        if (!timeout) break;
        if (!get_node(WB_MODE, timeout, &mode, error)) return FALSE;
        timeout = remaining_timeout(deadline);
        if (!timeout) { json_node_unref(mode); break; }
        gboolean loaded = get_node(WB_GAIN, timeout, &gain, error);
        gboolean valid = loaded && white_balance_view(mode, gain, &current, error);
        json_node_unref(mode); if (gain) json_node_unref(gain);
        if (!valid) return FALSE;
        gboolean matched = current.manual == change->manual;
        if (change->manual)
            for (guint i = 0; i < 4; ++i)
                matched = matched && close_number(current.gain[i], change->wb_gain[i], 0.01, 0.01);
        if (matched) return TRUE;
        pause_verification(deadline);
    } while (g_get_monotonic_time() < deadline);
    g_set_error_literal(error, IMAGE_CONTROLS_ERROR, IMAGE_CONTROLS_ERROR_APPLY_FAILED,
                        "白平衡在1.5秒内未通过模式和增益读回核对");
    return FALSE;
}

/* 失败时恢复先前整对象；两个白平衡恢复动作各自尝试，不因第一个失败跳过第二个。 */
static void report_apply_failure(const Change *change, const ControlState *original,
                                  GError *failure, GError **error) {
    GError *first = NULL, *second = NULL;
    gboolean restored;
    if (change->exposure) restored = put_node(AE_ATTR, original->ae_attr, &first);
    else {
        gboolean gains = put_node(WB_GAIN, original->wb_gain, &first);
        gboolean mode = put_node(WB_MODE, original->wb_mode, &second);
        restored = gains && mode;
    }
    if (restored)
        g_set_error(error, IMAGE_CONTROLS_ERROR, IMAGE_CONTROLS_ERROR_APPLY_FAILED,
                    "%s；已提交恢复原配置请求，请重新读取核对是否生效", failure ? failure->message : "参数验证失败");
    else
        g_set_error(error, IMAGE_CONTROLS_ERROR, IMAGE_CONTROLS_ERROR_APPLY_FAILED,
                    "%s；恢复原配置也失败：%s%s%s，请人工核对当前参数",
                    failure ? failure->message : "参数验证失败", first ? first->message : "",
                    first && second ? "；" : "", second ? second->message : "");
    g_clear_error(&first); g_clear_error(&second); g_clear_error(&failure);
}

gchar *image_controls_apply_json(GHashTable *fields, GError **error) {
    if (error && *error) return NULL;
    Change change = { 0 };
    if (!parse_change(fields, &change, error)) return NULL;
    ControlState original = { 0 }, after = { 0 };
    ControlView before = { 0 }, current = { 0 };
    GError *failure = NULL;
    JsonNode *new_attr = NULL, *new_mode = NULL, *new_gain = NULL;
    gboolean attempted_write = FALSE;
    gchar *result = NULL;
    if (!state_load(&original, &before, &failure)) goto done;
    if (change.exposure && change.manual
        && (change.time_ms < before.exposure.time_min_ms || change.time_ms > before.exposure.time_max_ms
            || change.gain < before.exposure.gain_min || change.gain > before.exposure.gain_max)) {
        invalid(&failure, "手动曝光时间或增益超出当前相机模式给出的范围");
        goto done;
    }
    if (change.exposure) {
        new_attr = clone_node(original.ae_attr, &failure);
        if (!new_attr) goto done;
        JsonObject *attr = json_node_get_object(new_attr);
        JsonObject *comm = object_member(attr, "commCtrl", &failure);
        JsonObject *control = comm ? object_member(comm, "meCtrl", &failure) : NULL;
        JsonObject *linear = control ? object_member(control, "linMe", &failure) : NULL;
        if (!comm || !linear) goto done;
        json_object_set_string_member(comm, "sw_aeT_opt_mode", change.manual ? "RK_AIQ_OP_MODE_MANUAL" : "RK_AIQ_OP_MODE_AUTO");
        if (change.manual) {
            json_object_set_int_member(linear, "sw_aeT_manTime_en", 1);
            json_object_set_int_member(linear, "sw_aeT_manGain_en", 1);
            json_object_set_int_member(linear, "sw_aeT_manIspDGain_en", 1);
            json_object_set_double_member(linear, "sw_aeT_manTime_val", change.time_ms / 1000.0);
            json_object_set_double_member(linear, "sw_aeT_manGain_val", change.gain);
            json_object_set_double_member(linear, "sw_aeT_manIspDGain_val", 1);
        }
        attempted_write = TRUE;
        if (!put_node(AE_ATTR, new_attr, &failure) || !verify_exposure(&change, &failure)) goto done;
    } else {
        new_mode = clone_node(original.wb_mode, &failure);
        if (!new_mode) goto done;
        json_object_set_string_member(json_node_get_object(new_mode), "mode", change.manual ? "OP_MANUAL" : "OP_AUTO");
        if (change.manual) {
            new_gain = clone_node(original.wb_gain, &failure);
            if (!new_gain) goto done;
            JsonObject *gains = json_node_get_object(new_gain);
            for (guint i = 0; i < 4; ++i) json_object_set_double_member(gains, wb_keys[i], change.wb_gain[i]);
            attempted_write = TRUE;
            if (!put_node(WB_GAIN, new_gain, &failure)) goto done;
        }
        attempted_write = TRUE;
        if (!put_node(WB_MODE, new_mode, &failure) || !verify_white_balance(&change, &failure)) goto done;
    }
    if (!state_load(&after, &current, &failure)) goto done;
    result = view_json(&current, settings_version + 1);
    if (!result) {
        g_set_error_literal(&failure, IMAGE_CONTROLS_ERROR, IMAGE_CONTROLS_ERROR_APPLY_FAILED,
                            "参数已写入，但无法生成经过核对的网页返回");
        goto done;
    }
    ++settings_version;
done:
    if (!result && attempted_write) report_apply_failure(&change, &original, failure, error);
    else if (failure) g_propagate_error(error, failure);
    if (new_attr) json_node_unref(new_attr);
    if (new_mode) json_node_unref(new_mode);
    if (new_gain) json_node_unref(new_gain);
    state_clear(&original); state_clear(&after);
    return result;
}
