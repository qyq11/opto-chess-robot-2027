#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "motor_control.h"
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#define DEFAULT_DURATION_MS 3000
#define DEFAULT_SPEED_MM_S 60
#define MIN_SPEED_MM_S 20
#define MAX_SPEED_MM_S 300
#define MAX_DURATION_MS 10000
#define HISTORY 16
#define DEFAULT_DISTANCE_MM 200
#define MIN_DISTANCE_MM 50
#define MAX_DISTANCE_MM 500
#define DISTANCE_TIMEOUT_MS 15000
/* 65毫米轮、13线、减速比20；暂按AB相四倍计数换算，实车须核一圈计数与尺测。 */
#define MM_PER_TICK (65.0 * G_PI / (13.0 * 20.0 * 4.0))

/* 通道顺序：左前M1、左后M2、右前M3、右后M4；适用于俯视呈X形的麦轮安装。 */
static const struct {
    const char *action;
    int wheels[4];
} motions[] = {
    {"forward",  { 1,  1,  1,  1}},
    {"backward", {-1, -1, -1, -1}},
    {"left",     {-1,  1,  1, -1}},
    {"right",    { 1, -1, -1,  1}},
    {"distance", { 1,  1,  1,  1}},
};

struct MotorControl {
    pthread_mutex_t mutex;
    pthread_t thread;
    gboolean thread_started, quit, moving, ready, fault, feedback;
    int fd;
    gint64 deadline, query_until, feedback_until, ramp_at, last_feedback;
    unsigned duration_ms, ramp_speed, target_speed;
    int direction[4], signs[4];
    char *port;
    char error[256], rx[2049], action[16], feedback_rx[1025], speeds[128];
    size_t feedback_size;
    double peak[4];
    double actual[4];
    unsigned feedback_samples;
    size_t rx_size;
    unsigned sequence, generation;
    char *events[HISTORY];
    unsigned event_count;
    gboolean distance_mode, encoder_baseline;
    unsigned target_distance, encoder_samples;
    gint64 last_encoder, last_progress;
    int64_t encoder[4], wheel_ticks[4];
    double estimated_mm, progress_mark;
    char distance_phase[32], stop_reason[32];
};

static void json_text(GString *json, const char *text) {
    g_string_append_c(json, '"');
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        if (*p == '"' || *p == '\\') g_string_append_printf(json, "\\%c", *p);
        else if (*p < 32) g_string_append_printf(json, "\\u%04x", *p);
        else g_string_append_c(json, (char)*p);
    }
    g_string_append_c(json, '"');
}

/* 调用者持锁；日志同时保留文本和十六进制，写入成功只表示内核接受字节。 */
static void record(MotorControl *motor, const char *direction, const char *bytes, size_t size) {
    GString *line = g_string_new(NULL);
    g_string_append_printf(line, "#%u %s %zu bytes TEXT=", ++motor->sequence, direction, size);
    for (size_t i = 0; i < size; ++i) {
        unsigned char c = (unsigned char)bytes[i];
        if (c == '\r') g_string_append(line, "\\r");
        else if (c == '\n') g_string_append(line, "\\n");
        else if (c == '\t') g_string_append(line, "\\t");
        else if (c >= 32 && c < 127) g_string_append_c(line, (char)c);
        else g_string_append_printf(line, "\\x%02X", c);
    }
    g_string_append(line, " HEX=");
    for (size_t i = 0; i < size; ++i) g_string_append_printf(line, "%02X ", (unsigned char)bytes[i]);
    fprintf(stdout, "motor port=%s %s\n", motor->port, line->str);
    /* 收发各保留八条，避免高频反馈把刚发送的运动/停止指令挤掉。 */
    unsigned same_direction = 0, oldest = 0;
    gboolean found = FALSE;
    for (unsigned i = 0; i < motor->event_count; ++i) {
        if (strstr(motor->events[i], direction)) {
            ++same_direction;
            if (!found) { oldest = i; found = TRUE; }
        }
    }
    if (same_direction >= HISTORY / 2 || motor->event_count == HISTORY) {
        if (!found) oldest = 0;
        g_free(motor->events[oldest]);
        memmove(motor->events + oldest, motor->events + oldest + 1,
                (motor->event_count - oldest - 1) * sizeof(char *));
        --motor->event_count;
    }
    motor->events[motor->event_count++] = g_string_free(line, FALSE);
}

static void fail(MotorControl *motor, const char *message) {
    motor->fault = TRUE;
    motor->ready = FALSE;
    g_strlcpy(motor->error, message, sizeof(motor->error));
}

static gboolean send_frame(MotorControl *motor, const char *frame) {
    size_t sent = 0, length = strlen(frame);
    gint64 until = g_get_monotonic_time() + 50000;
    while (sent < length && g_get_monotonic_time() < until) {
        ssize_t count = write(motor->fd, frame + sent, length - sent);
        if (count > 0) sent += (size_t)count;
        else if (count < 0 && errno == EINTR) continue;
        else if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd fd = {motor->fd, POLLOUT, 0};
            (void)poll(&fd, 1, 2);
        } else break;
    }
    if (sent) record(motor, "RK->DRIVER", frame, sent);
    if (sent == length) return TRUE;
    fail(motor, "串口发送不完整，停止指令可能未送达；请现场切断动力并检查串口");
    return FALSE;
}

static gboolean stop_motor(MotorControl *motor) {
    ++motor->generation;
    /* 分帧发送，便于逐条核对；零PWM解除主动输出，实际停车仍需看轮子。 */
    gboolean speed_ok = send_frame(motor, "$spd:0,0,0,0#");
    gboolean pwm_ok = send_frame(motor, "$pwm:0,0,0,0#");
    if (speed_ok && pwm_ok) {
        motor->moving = FALSE;
        if (motor->distance_mode && motor->feedback)
            motor->feedback_until = MIN(motor->feedback_until, g_get_monotonic_time() + 700000);
        g_strlcpy(motor->action, "stop", sizeof(motor->action));
        return TRUE;
    }
    return FALSE;
}

static gboolean send_speed(MotorControl *motor) {
    int speed = (int)motor->ramp_speed;
    char frame[80];
    snprintf(frame, sizeof(frame), "$spd:%d,%d,%d,%d#",
        speed * motor->direction[0] * motor->signs[0],
        speed * motor->direction[1] * motor->signs[1],
        speed * motor->direction[2] * motor->signs[2],
        speed * motor->direction[3] * motor->signs[3]);
    return send_frame(motor, frame);
}

static void distance_failure(MotorControl *motor, const char *reason, const char *message) {
    g_strlcpy(motor->stop_reason, reason, sizeof(motor->stop_reason));
    g_strlcpy(motor->error, message, sizeof(motor->error));
    motor->ready = FALSE;
    (void)stop_motor(motor);
}

/* 累计计数只用于估计轮子走过的距离；不能证明车身没有打滑或跑偏。 */
static gboolean encoder_frame(MotorControl *motor, const char *frame) {
    if (strncmp(frame, "$MAll:", 6)) return FALSE;
    int64_t counts[4];
    const char *cursor = frame + 6;
    for (unsigned i = 0; i < 4; ++i) {
        char *end = NULL;
        if (!g_ascii_isdigit(*cursor) && !(*cursor == '-' && g_ascii_isdigit(cursor[1]))) return FALSE;
        errno = 0;
        gint64 value = g_ascii_strtoll(cursor, &end, 10);
        if (errno || end == cursor || value < INT32_MIN || value > INT32_MAX ||
            *end != (i == 3 ? '#' : ',')) return FALSE;
        counts[i] = value;
        cursor = end + 1;
    }
    if (*cursor) return FALSE;
    if (!motor->distance_mode || !motor->feedback) return TRUE;
    gint64 now = g_get_monotonic_time();
    ++motor->encoder_samples;
    if (!motor->encoder_baseline) {
        if (!motor->moving) return TRUE;
        memcpy(motor->encoder, counts, sizeof(counts));
        motor->encoder_baseline = TRUE;
        motor->last_encoder = motor->last_progress = now;
        motor->ramp_speed = 20;
        motor->ramp_at = now + 200000;
        g_strlcpy(motor->distance_phase, "accelerating", sizeof(motor->distance_phase));
        if (!send_speed(motor)) (void)stop_motor(motor);
        return TRUE;
    }
    int64_t deltas[4];
    double interval = (now - motor->last_encoder) / 1000000.0;
    for (unsigned i = 0; i < 4; ++i) {
        int64_t delta = counts[i] - motor->encoder[i];
        /* 处理有符号32位计数自然回绕；远超物理速度的变化按复位或坏帧处理。 */
        if (delta > INT32_MAX) delta -= INT64_C(4294967296);
        if (delta < INT32_MIN) delta += INT64_C(4294967296);
        if (fabs(delta * MM_PER_TICK) > 1500.0 * interval + 3.0) {
            if (motor->moving) {
                distance_failure(motor, "encoder_jump", "累计计数突变，已发送停止；请检查驱动复位、供电与回包后重新读取驱动");
            }
            /* 停车后也保留最后有效估计，避免驱动复位把结果改写成大幅位移。 */
            motor->feedback_until = now;
            return TRUE;
        }
        deltas[i] = delta * motor->signs[i];
    }
    memcpy(motor->encoder, counts, sizeof(counts));
    motor->last_encoder = now;
    double total = 0;
    for (unsigned i = 0; i < 4; ++i) {
        motor->wheel_ticks[i] += deltas[i];
        total += motor->wheel_ticks[i] * MM_PER_TICK;
        if (motor->moving && motor->wheel_ticks[i] * MM_PER_TICK < -5.0) {
            distance_failure(motor, "wrong_direction", "轮子累计计数方向与前进不符，已发送停止；请核对轮序及方向");
            return TRUE;
        }
    }
    motor->estimated_mm = total / 4.0;
    if (motor->estimated_mm >= motor->progress_mark + 1.0) {
        motor->progress_mark = motor->estimated_mm;
        motor->last_progress = now;
    }
    if (motor->moving && motor->estimated_mm >= motor->target_distance) {
        g_strlcpy(motor->stop_reason, "target_reached", sizeof(motor->stop_reason));
        (void)stop_motor(motor);
    }
    return TRUE;
}

static void *motor_loop(void *opaque) {
    MotorControl *motor = opaque;
    for (;;) {
        pthread_mutex_lock(&motor->mutex);
        if (motor->quit) { pthread_mutex_unlock(&motor->mutex); break; }
        if (motor->moving && (motor->fault || g_get_monotonic_time() >= motor->deadline)) {
            g_strlcpy(motor->stop_reason, motor->fault ? "serial_error" : motor->distance_mode ? "time_limit" : "timer", sizeof(motor->stop_reason));
            if (motor->distance_mode && !motor->fault) {
                motor->ready = FALSE;
                g_strlcpy(motor->error, "定距离动作达到15秒上限，已发送停止；未确认到达目标，请核对计数与实测位移", sizeof(motor->error));
            }
            (void)stop_motor(motor);
        }
        if (motor->feedback && g_get_monotonic_time() >= motor->feedback_until) {
            (void)send_frame(motor, "$upload:0,0,0#");
            motor->feedback = FALSE;
        }
        char buffer[256];
        ssize_t count = read(motor->fd, buffer, sizeof(buffer));
        if (count > 0) {
            record(motor, "DRIVER->RK", buffer, (size_t)count);
            for (ssize_t i = 0; i < count; ++i) {
                if (buffer[i] == '$') motor->feedback_size = 0;
                if (motor->feedback_size < sizeof(motor->feedback_rx) - 1)
                    motor->feedback_rx[motor->feedback_size++] = buffer[i];
                if (buffer[i] == '#') {
                    motor->feedback_rx[motor->feedback_size] = '\0';
                    (void)encoder_frame(motor, motor->feedback_rx);
                    double a, b, c, d; int end = 0;
                    if (sscanf(motor->feedback_rx, "$MSPD:%lf,%lf,%lf,%lf#%n", &a, &b, &c, &d, &end) == 4 &&
                        end > 0 && motor->feedback_rx[end] == '\0' &&
                        isfinite(a) && isfinite(b) && isfinite(c) && isfinite(d))
                    {
                        snprintf(motor->speeds, sizeof(motor->speeds), "M1 %.1f / M2 %.1f / M3 %.1f / M4 %.1f mm/s", a, b, c, d);
                        double values[4] = {a, b, c, d};
                        memcpy(motor->actual, values, sizeof(values));
                        for (unsigned j = 0; j < 4; ++j) {
                            double magnitude = values[j] < 0 ? -values[j] : values[j];
                            if (magnitude > motor->peak[j]) motor->peak[j] = magnitude;
                        }
                        ++motor->feedback_samples;
                        motor->last_feedback = g_get_monotonic_time();
                    }
                    motor->feedback_size = 0;
                }
            }
            if (motor->query_until && g_get_monotonic_time() <= motor->query_until) {
                size_t copy = MIN((size_t)count, sizeof(motor->rx) - 1 - motor->rx_size);
                memcpy(motor->rx + motor->rx_size, buffer, copy);
                motor->rx_size += copy;
                motor->rx[motor->rx_size] = '\0';
                /* 核对本车已有参数，既不写参数，也不将发送回显当作回复。 */
                if (strstr(motor->rx, "read_flash:OK!\r\n") &&
                    strstr(motor->rx, "Motor_type:2\r\n") && strstr(motor->rx, "Pulse_Line:13\r\n") &&
                    strstr(motor->rx, "Pulse_Phase:20\r\n") && strstr(motor->rx, "wheel_diameter:65.000\r\n") &&
                    strstr(motor->rx, "P:") && strstr(motor->rx, "D:") &&
                    motor->rx_size >= 2 && !strcmp(motor->rx + motor->rx_size - 2, "\r\n")) {
                    motor->ready = TRUE;
                    motor->query_until = 0;
                    motor->error[0] = '\0';
                }
            }
        } else if (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
            fail(motor, "串口读取失败，请检查连接；本服务拒绝继续启动电机");
        if (motor->query_until && g_get_monotonic_time() > motor->query_until) {
            motor->query_until = 0;
            g_strlcpy(motor->error, "未收到本车完整配置，请检查驱动供电、接线与回包", sizeof(motor->error));
        }
        if (motor->moving) {
            gint64 now = g_get_monotonic_time();
            if (motor->distance_mode && !motor->encoder_baseline && now - motor->last_encoder > 500000) {
                distance_failure(motor, "encoder_timeout", "未收到累计计数基准，本次未起步；已发送停止，请重新读取驱动");
            } else if (motor->distance_mode && motor->encoder_baseline && now - motor->last_encoder > 250000) {
                distance_failure(motor, "encoder_timeout", "累计计数反馈中断，已发送停止；不能只靠轮速继续定距离动作");
            } else if (motor->distance_mode && motor->encoder_baseline && now - motor->last_progress > 1500000) {
                distance_failure(motor, "no_progress", "累计计数持续没有前进，已发送停止；请检查轮子与反馈后重新读取驱动");
            } else if (now - motor->last_feedback > 250000 && (!motor->distance_mode || motor->encoder_baseline)) {
                g_strlcpy(motor->stop_reason, "speed_feedback_timeout", sizeof(motor->stop_reason));
                (void)stop_motor(motor);
                motor->ready = FALSE;
                g_strlcpy(motor->error, "启动后轮速反馈中断，已发送归零；请检查驱动供电与串口，排查后重新读取驱动", sizeof(motor->error));
            } else if (!motor->distance_mode || motor->encoder_baseline) {
                unsigned desired = motor->target_speed;
                if (motor->distance_mode) {
                    if (motor->target_distance - motor->estimated_mm <= 40.0) desired = 20;
                    g_strlcpy(motor->distance_phase, desired == 20 ? "decelerating" : motor->ramp_speed < desired ? "accelerating" : "cruising", sizeof(motor->distance_phase));
                }
                if (motor->ramp_speed != desired && now >= motor->ramp_at) {
                    if (motor->ramp_speed < desired) motor->ramp_speed = MIN(motor->ramp_speed + 20, desired);
                    else motor->ramp_speed = MAX((int)motor->ramp_speed - 20, (int)desired);
                    motor->ramp_at = now + 200000;
                    if (!send_speed(motor)) (void)stop_motor(motor);
                }
            }
        }
        pthread_mutex_unlock(&motor->mutex);
        g_usleep(10000);
    }
    return NULL;
}

MotorControl *motor_control_new(const char *port) {
    MotorControl *motor = g_new0(MotorControl, 1);
    motor->fd = -1;
    motor->duration_ms = DEFAULT_DURATION_MS;
    motor->target_speed = DEFAULT_SPEED_MM_S;
    motor->target_distance = DEFAULT_DISTANCE_MM;
    g_strlcpy(motor->stop_reason, "not_started", sizeof(motor->stop_reason));
    motor->port = g_strdup(port ? port : "");
    pthread_mutex_init(&motor->mutex, NULL);
    g_strlcpy(motor->action, "stop", sizeof(motor->action));
    if (!port) { g_strlcpy(motor->error, "服务尚未启用电机串口", sizeof(motor->error)); return motor; }
    motor->fd = open(port, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    struct termios settings;
    if (motor->fd < 0 || flock(motor->fd, LOCK_EX | LOCK_NB) || ioctl(motor->fd, TIOCEXCL) ||
        tcgetattr(motor->fd, &settings)) goto unavailable;
    cfmakeraw(&settings);
    settings.c_iflag &= ~(IXON | IXOFF | IXANY);
    settings.c_cflag &= ~(CSIZE | PARENB | CSTOPB | CRTSCTS);
    settings.c_cflag |= CS8 | CLOCAL | CREAD;
    settings.c_cc[VMIN] = 0; settings.c_cc[VTIME] = 0;
    if (cfsetispeed(&settings, B115200) || cfsetospeed(&settings, B115200) ||
        tcsetattr(motor->fd, TCSANOW, &settings) || tcflush(motor->fd, TCIOFLUSH)) goto unavailable;
    if (pthread_create(&motor->thread, NULL, motor_loop, motor)) goto unavailable;
    motor->thread_started = TRUE;
    return motor;
unavailable:
    fail(motor, "无法独占打开串口或设置115200波特，请检查节点和占用");
    if (motor->fd >= 0) { (void)ioctl(motor->fd, TIOCNXCL); close(motor->fd); motor->fd = -1; }
    return motor;
}

static char *snapshot(MotorControl *motor) {
    GString *json = g_string_new("{\"port\":");
    json_text(json, motor->port);
    g_string_append_printf(json, ",\"opened\":%s,\"ready\":%s,\"moving\":%s,\"querying\":%s,\"fault\":%s,\"speed_mm_s\":%d,\"duration_ms\":%u,\"generation\":%u",
        motor->fd >= 0 ? "true" : "false", motor->ready ? "true" : "false", motor->moving ? "true" : "false",
        motor->query_until ? "true" : "false", motor->fault ? "true" : "false", (int)motor->target_speed, motor->duration_ms, motor->generation);
    g_string_append_printf(json, ",\"active_speed_mm_s\":%u,\"remaining_ms\":%lld,\"limits\":{"
        "\"speed_mm_s\":{\"min\":%d,\"max\":%d,\"step\":10,\"default\":%d},"
        "\"duration_ms\":{\"min\":1000,\"max\":%d,\"step\":1000,\"default\":%d},"
        "\"distance_mm\":{\"min\":%d,\"max\":%d,\"step\":10,\"default\":%d}}",
        motor->moving ? motor->ramp_speed : 0,
        (long long)(motor->moving ? MAX((gint64)0, (motor->deadline - g_get_monotonic_time()) / 1000) : 0),
        MIN_SPEED_MM_S, MAX_SPEED_MM_S, DEFAULT_SPEED_MM_S, MAX_DURATION_MS, DEFAULT_DURATION_MS,
        MIN_DISTANCE_MM, MAX_DISTANCE_MM, DEFAULT_DISTANCE_MM);
    g_string_append_printf(json, ",\"mode\":\"%s\",\"distance_mm\":%u,\"encoder_samples\":%u,\"mm_per_tick\":%.9f,\"encoder_counts\":[%lld,%lld,%lld,%lld]",
        motor->distance_mode ? "distance" : "time", motor->target_distance, motor->encoder_samples, MM_PER_TICK,
        (long long)motor->encoder[0], (long long)motor->encoder[1], (long long)motor->encoder[2], (long long)motor->encoder[3]);
    g_string_append(json, ",\"stop_reason\":"); json_text(json, motor->stop_reason);
    g_string_append(json, ",\"distance_phase\":");
    json_text(json, motor->distance_mode && motor->moving ? motor->distance_phase : "stopped");
    if (motor->encoder_baseline) {
        g_string_append_printf(json, ",\"estimated_mm\":%.3f,\"remaining_mm\":%.3f,\"wheel_distance_mm\":[%.3f,%.3f,%.3f,%.3f]",
            motor->estimated_mm, MAX(0.0, motor->target_distance - motor->estimated_mm),
            motor->wheel_ticks[0] * MM_PER_TICK, motor->wheel_ticks[1] * MM_PER_TICK,
            motor->wheel_ticks[2] * MM_PER_TICK, motor->wheel_ticks[3] * MM_PER_TICK);
    } else g_string_append(json, ",\"estimated_mm\":null,\"remaining_mm\":null,\"wheel_distance_mm\":null");
    g_string_append(json, ",\"action\":"); json_text(json, motor->action);
    g_string_append(json, ",\"error\":"); json_text(json, motor->error);
    g_string_append(json, ",\"wheel_speeds\":"); json_text(json, motor->speeds);
    g_string_append_printf(json, ",\"feedback_samples\":%u,\"peak_mm_s\":[%.1f,%.1f,%.1f,%.1f]",
        motor->feedback_samples, motor->peak[0], motor->peak[1], motor->peak[2], motor->peak[3]);
    if (motor->feedback_samples)
        g_string_append_printf(json, ",\"actual_mm_s\":[%.2f,%.2f,%.2f,%.2f],\"feedback_age_ms\":%lld",
            motor->actual[0], motor->actual[1], motor->actual[2], motor->actual[3],
            (long long)MAX((gint64)0, (g_get_monotonic_time() - motor->last_feedback) / 1000));
    else g_string_append(json, ",\"actual_mm_s\":null,\"feedback_age_ms\":null");
    g_string_append(json, ",\"events\":[");
    for (unsigned i = 0; i < motor->event_count; ++i) {
        if (i) g_string_append_c(json, ',');
        json_text(json, motor->events[i]);
    }
    g_string_append(json, "]}");
    return g_string_free(json, FALSE);
}

char *motor_control_json(MotorControl *motor) {
    pthread_mutex_lock(&motor->mutex);
    char *json = snapshot(motor);
    pthread_mutex_unlock(&motor->mutex);
    return json;
}

/* 浏览器的滑杆不作为校验依据，RK独立检查完整十进制、上下限和步长。 */
static gboolean parameter(GHashTable *fields, const char *name, unsigned fallback,
                          unsigned min, unsigned max, unsigned step, unsigned *result) {
    const char *value = g_hash_table_lookup(fields, name);
    if (!value) { *result = fallback; return TRUE; }
    if (!*value) return FALSE;
    for (const char *p = value; *p; ++p) if (!g_ascii_isdigit(*p)) return FALSE;
    guint64 number = g_ascii_strtoull(value, NULL, 10);
    if (number < min || number > max || (number - min) % step) return FALSE;
    *result = (unsigned)number;
    return TRUE;
}

char *motor_control_command(MotorControl *motor, GHashTable *fields, unsigned *status) {
    const char *action = fields ? g_hash_table_lookup(fields, "action") : NULL;
    const int *direction = NULL;
    for (unsigned i = 0; action && i < G_N_ELEMENTS(motions); ++i)
        if (!strcmp(action, motions[i].action)) { direction = motions[i].wheels; break; }
    *status = 200;
    pthread_mutex_lock(&motor->mutex);
    const char *error = NULL;
    if (!action || (!direction && strcmp(action, "stop") && strcmp(action, "query") && strcmp(action, "speed"))) {
        *status = 400; error = "请选择前进、后退、左移、右移、前进指定距离、停止、读取驱动或调整速度";
    } else if (motor->fd < 0) { *status = 503; error = motor->error; }
    else if (!strcmp(action, "stop")) {
        if (motor->moving) g_strlcpy(motor->stop_reason, "manual", sizeof(motor->stop_reason));
        if (!stop_motor(motor)) { *status = 503; error = motor->error; }
    } else if (motor->fault) { *status = 503; error = motor->error; }
    else if (motor->query_until || (motor->moving && strcmp(action, "speed"))) { *status = 409; error = "正在执行上一条操作，请先停止或等待结束"; }
    else if (!strcmp(action, "query")) {
        if (motor->feedback) {
            (void)send_frame(motor, "$upload:0,0,0#");
            motor->feedback = FALSE;
            g_usleep(20000);
        }
        motor->ready = FALSE; motor->rx_size = 0; motor->rx[0] = '\0';
        if (stop_motor(motor) && tcflush(motor->fd, TCIFLUSH) == 0 && send_frame(motor, "$read_flash#")) {
            motor->query_until = g_get_monotonic_time() + 2000000;
            motor->error[0] = '\0';
        } else { *status = 503; error = "无法发送查询，请检查串口"; }
    } else if (!motor->ready) { *status = 409; error = "请先点击读取驱动，取得本车完整配置后再试转"; }
    else {
        const char *generation = g_hash_table_lookup(fields, "generation");
        char *end = NULL;
        guint64 expected = generation ? g_ascii_strtoull(generation, &end, 10) : 0;
        if (!generation || !*generation || !end || *end || expected != motor->generation) {
            *status = 409; error = "操作状态已变化，请等待页面刷新后重新点击";
        }
        int signs[4] = {1, 1, 1, 1};
        gboolean distance_mode = !strcmp(action, "distance");
        unsigned duration_ms = DEFAULT_DURATION_MS, target_speed = DEFAULT_SPEED_MM_S, target_distance = DEFAULT_DISTANCE_MM;
        if (distance_mode && (!g_hash_table_contains(fields, "distance_mm") ||
            !parameter(fields, "distance_mm", DEFAULT_DISTANCE_MM, MIN_DISTANCE_MM, MAX_DISTANCE_MM, 10, &target_distance) ||
            g_hash_table_contains(fields, "speed_mm_s") || g_hash_table_contains(fields, "duration_ms"))) {
            *status = 400; error = "首轮定距离只接受前进5到50厘米的整数，固定轮速6厘米/秒，请勿附加速度或时间";
        }
        if (!distance_mode && g_hash_table_contains(fields, "distance_mm")) {
            *status = 400; error = "定时移动与调速不能附加距离，请使用前进指定距离";
        }
        if (!parameter(fields, "duration_ms", DEFAULT_DURATION_MS, 1000, MAX_DURATION_MS, 1000, &duration_ms)) {
            *status = 400; error = "每次时间请选择1到10秒的整数";
        }
        if (!parameter(fields, "speed_mm_s", DEFAULT_SPEED_MM_S, MIN_SPEED_MM_S, MAX_SPEED_MM_S, 10, &target_speed)) {
            *status = 400; error = "目标轮速请选择每秒2到30厘米的整数档位";
        }
        if (!strcmp(action, "speed") && (!motor->moving || g_get_monotonic_time() >= motor->deadline)) {
            *status = 409; error = "本次动作已经结束，请为下一次选择速度";
        }
        if (!strcmp(action, "speed") && motor->distance_mode) {
            *status = 409; error = "本次为定距离前进，轮速由RK控制，请先停止再更改操作";
        }
        if (!strcmp(action, "speed") && (g_hash_table_size(fields) != 3 ||
            !g_hash_table_contains(fields, "speed_mm_s"))) {
            *status = 400; error = "运行中只可提交目标轮速，停止时间保持不变";
        }
        for (unsigned i = 0; i < 4; ++i) {
            char name[8]; snprintf(name, sizeof(name), "m%u", i + 1);
            const char *value = g_hash_table_lookup(fields, name);
            if (value && strcmp(value, "1") && strcmp(value, "-1")) { *status = 400; error = "各轮方向只能选择正向或反向"; break; }
            signs[i] = value && !strcmp(value, "-1") ? -1 : 1;
        }
        if (!error && !strcmp(action, "speed")) {
            motor->target_speed = target_speed;
            ++motor->generation;
            /* 不重设deadline或反馈窗口；调速不续时，迟到请求不能重启动作。 */
        } else if (!error) {
            /* 即使部分发送也安排停车；重复点击不会续时。 */
            motor->speeds[0] = '\0';
            memset(motor->peak, 0, sizeof(motor->peak));
            motor->feedback_samples = 0;
            motor->distance_mode = distance_mode;
            motor->encoder_baseline = FALSE;
            motor->encoder_samples = 0;
            motor->estimated_mm = motor->progress_mark = 0;
            memset(motor->wheel_ticks, 0, sizeof(motor->wheel_ticks));
            memset(motor->encoder, 0, sizeof(motor->encoder));
            if (distance_mode) {
                duration_ms = DISTANCE_TIMEOUT_MS;
                /* 先清掉旧帧，取得静止累计计数基准后才发送非零轮速。 */
                motor->feedback_size = 0;
                if (tcflush(motor->fd, TCIFLUSH)) { *status = 503; error = "无法清理旧计数，本次不启动"; }
            }
            motor->target_distance = target_distance;
            motor->error[0] = '\0';
            g_strlcpy(motor->stop_reason, "running", sizeof(motor->stop_reason));
            g_strlcpy(motor->distance_phase, "waiting_encoder", sizeof(motor->distance_phase));
            if (!error && !send_frame(motor, "$upload:1,0,1#")) {
                *status = 503; error = motor->error;
            }
            if (error) g_strlcpy(motor->stop_reason, "serial_error", sizeof(motor->stop_reason));
            motor->feedback = TRUE;
            motor->feedback_until = g_get_monotonic_time() + (duration_ms + 700) * 1000;
            g_usleep(20000);
            motor->moving = error == NULL;
            ++motor->generation;
            motor->duration_ms = duration_ms;
            motor->target_speed = target_speed;
            motor->deadline = g_get_monotonic_time() + duration_ms * 1000;
            motor->last_feedback = g_get_monotonic_time();
            motor->last_encoder = g_get_monotonic_time();
            motor->ramp_speed = distance_mode ? 0 : 20;
            motor->ramp_at = g_get_monotonic_time() + 200000;
            memcpy(motor->direction, direction, sizeof(motor->direction));
            memcpy(motor->signs, signs, sizeof(signs));
            g_strlcpy(motor->action, action, sizeof(motor->action));
            if (!error && !distance_mode && !send_speed(motor)) { (void)stop_motor(motor); *status = 503; error = motor->error; }
        }
    }
    char *json;
    if (error) {
        GString *reply = g_string_new("{\"error\":"); json_text(reply, error); g_string_append_c(reply, '}');
        json = g_string_free(reply, FALSE);
    } else json = snapshot(motor);
    pthread_mutex_unlock(&motor->mutex);
    return json;
}

void motor_control_free(MotorControl *motor) {
    if (!motor) return;
    pthread_mutex_lock(&motor->mutex);
    motor->quit = TRUE;
    /* 未执行过动作也不在启动/退出时意外写入；查询和停止由用户明确触发。 */
    if (motor->moving) (void)stop_motor(motor);
    if (motor->feedback) (void)send_frame(motor, "$upload:0,0,0#");
    pthread_mutex_unlock(&motor->mutex);
    if (motor->thread_started) pthread_join(motor->thread, NULL);
    if (motor->fd >= 0) { (void)ioctl(motor->fd, TIOCNXCL); close(motor->fd); }
    for (unsigned i = 0; i < motor->event_count; ++i) g_free(motor->events[i]);
    pthread_mutex_destroy(&motor->mutex);
    g_free(motor->port); g_free(motor);
}
