#define _DEFAULT_SOURCE
#include "../motor_control.h"
#include <json-glib/json-glib.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <pty.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* 只能在RK/Linux虚拟串口运行；输入均为模拟数据，不证明真实轮子或地面精度。 */
static int peer;
static MotorControl *motor;
static atomic_bool feeding, with_encoder;
static atomic_int_fast64_t counts[4];
static pthread_t feeder;
static pthread_mutex_t counts_mutex = PTHREAD_MUTEX_INITIALIZER;

static void require(gboolean condition, const char *message) {
    if (!condition) { fprintf(stderr, "CHECK FAILED: %s\n", message); exit(1); }
}

static JsonObject *state(void) {
    char *text = motor_control_json(motor);
    JsonParser *parser = json_parser_new();
    require(json_parser_load_from_data(parser, text, -1, NULL), "valid JSON");
    JsonObject *object = json_object_ref(json_node_get_object(json_parser_get_root(parser)));
    g_object_unref(parser); g_free(text);
    return object;
}

static void expect_reason(const char *reason) {
    JsonObject *object = state();
    printf("OBSERVED stopped=%s reason=%s estimated_mm=", json_object_get_boolean_member(object, "moving") ? "false" : "true",
        json_object_get_string_member(object, "stop_reason"));
    JsonNode *estimate = json_object_get_member(object, "estimated_mm");
    if (JSON_NODE_HOLDS_NULL(estimate)) puts("null");
    else printf("%.3f\n", json_node_get_double(estimate));
    require(!json_object_get_boolean_member(object, "moving"), "operation stopped");
    require(!strcmp(json_object_get_string_member(object, "stop_reason"), reason), reason);
    json_object_unref(object);
}

static void send_input(const char *text) {
    printf("SIMULATED DRIVER->RK TEXT=%s\n", text);
    require(write(peer, text, strlen(text)) == (ssize_t)strlen(text), "write virtual feedback");
}

static void *feedback_loop(void *unused) {
    (void)unused;
    while (atomic_load(&feeding)) {
        char frame[256];
        pthread_mutex_lock(&counts_mutex);
        if (atomic_load(&with_encoder)) {
            snprintf(frame, sizeof(frame), "$MAll:%lld,%lld,%lld,%lld#$MSPD:0,0,0,0#",
                (long long)atomic_load(&counts[0]), (long long)atomic_load(&counts[1]),
                (long long)atomic_load(&counts[2]), (long long)atomic_load(&counts[3]));
        } else g_strlcpy(frame, "$MSPD:0,0,0,0#", sizeof(frame));
        pthread_mutex_unlock(&counts_mutex);
        if (write(peer, frame, strlen(frame)) != (ssize_t)strlen(frame)) break;
        g_usleep(40000);
    }
    return NULL;
}

static void set_counts(int64_t value, gboolean alternate) {
    printf("SIMULATED DRIVER cumulative counts: %lld,%lld,%lld,%lld\n", (long long)value,
        (long long)(alternate ? -value : value), (long long)value, (long long)(alternate ? -value : value));
    pthread_mutex_lock(&counts_mutex);
    for (unsigned i = 0; i < 4; ++i) atomic_store(&counts[i], alternate && i % 2 ? -value : value);
    pthread_mutex_unlock(&counts_mutex);
}

static void receive_exact(const char *expected, int timeout_ms) {
    char text[2048] = {0}; size_t used = 0;
    gint64 until = g_get_monotonic_time() + timeout_ms * 1000;
    while (used < strlen(expected) && used < sizeof(text) - 1 && g_get_monotonic_time() < until) {
        ssize_t length = read(peer, text + used, sizeof(text) - 1 - used);
        if (length > 0) used += (size_t)length;
        else { struct pollfd fd = {peer, POLLIN, 0}; (void)poll(&fd, 1, 5); }
    }
    printf("OBSERVED RK->VIRTUAL_DRIVER TEXT=%s\n", text);
    require(!strcmp(text, expected), expected);
}

static void request(const char *action, const char *distance, unsigned expected, gboolean alternate, const char *extra) {
    JsonObject *object = state();
    char generation[32];
    snprintf(generation, sizeof(generation), "%lld", (long long)json_object_get_int_member(object, "generation"));
    json_object_unref(object);
    GHashTable *fields = g_hash_table_new(g_str_hash, g_str_equal);
    g_hash_table_insert(fields, "action", (char *)action);
    g_hash_table_insert(fields, "generation", generation);
    if (distance) g_hash_table_insert(fields, "distance_mm", (char *)distance);
    if (alternate) { g_hash_table_insert(fields, "m2", "-1"); g_hash_table_insert(fields, "m4", "-1"); }
    if (extra) g_hash_table_insert(fields, (char *)extra, "60");
    unsigned status;
    char *reply = motor_control_command(motor, fields, &status);
    printf("INPUT action=%s distance_mm=%s extra=%s -> HTTP %u TEXT=%s\n", action,
        distance ? distance : "absent", extra ? extra : "none", status, reply);
    require(status == expected, reply);
    g_free(reply); g_hash_table_unref(fields);
}

static void open_motor(gboolean encoders) {
    int slave; char path[128];
    require(openpty(&peer, &slave, path, NULL, NULL) == 0, "openpty");
    close(slave);
    require(fcntl(peer, F_SETFL, O_NONBLOCK) == 0, "nonblocking peer");
    printf("VIRTUAL UART ONLY %s; no physical UART opened\n", path);
    motor = motor_control_new(path);
    request("query", NULL, 200, FALSE, NULL);
    receive_exact("$spd:0,0,0,0#$pwm:0,0,0,0#$read_flash#", 250);
    send_input("read_flash:OK!\r\nMotor_type:2\r\nPulse_Line:13\r\nPulse_Phase:20\r\nwheel_diameter:65.000\r\nP:0.800\tD:0.500\r\n");
    g_usleep(50000);
    JsonObject *object = state();
    require(json_object_get_boolean_member(object, "ready"), "configuration accepted");
    json_object_unref(object);
    set_counts(10000, FALSE);
    atomic_store(&with_encoder, encoders);
    atomic_store(&feeding, TRUE);
    require(!pthread_create(&feeder, NULL, feedback_loop, NULL), "create virtual driver");
}

static void close_motor(void) {
    motor_control_free(motor);
    atomic_store(&feeding, FALSE);
    pthread_join(feeder, NULL);
    close(peer);
}

static void begin_distance(gboolean alternate) {
    request("distance", "200", 200, alternate, NULL);
    receive_exact(alternate ? "$upload:1,0,1#$spd:20,-20,20,-20#" : "$upload:1,0,1#$spd:20,20,20,20#", 200);
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    open_motor(FALSE);
    const char *invalid[] = {NULL, "", "0", "40", "510", "55", "200.0", "-200", "9999999999999999999999"};
    for (unsigned i = 0; i < G_N_ELEMENTS(invalid); ++i) request("distance", invalid[i], 400, FALSE, NULL);
    request("distance", "200", 400, FALSE, "speed_mm_s");
    request("distance", "200", 400, FALSE, "duration_ms");
    request("forward", "200", 400, FALSE, NULL);
    request("distance", "200", 200, FALSE, NULL);
    receive_exact("$upload:1,0,1#", 100);
    request("distance", "200", 409, FALSE, NULL);
    /* 只有轮速、没有累计计数时，不得发任何非零速度。 */
    receive_exact("$spd:0,0,0,0#$pwm:0,0,0,0#", 700);
    expect_reason("encoder_timeout");
    close_motor();

    open_motor(TRUE);
    begin_distance(FALSE);
    request("speed", NULL, 409, FALSE, "speed_mm_s");
    receive_exact("$spd:40,40,40,40#$spd:60,60,60,60#", 500);
    for (int tick = 40; tick <= 800; tick += 40) { set_counts(10000 + tick, FALSE); g_usleep(50000); }
    set_counts(10850, FALSE);
    receive_exact("$spd:40,40,40,40#$spd:20,20,20,20#", 600);
    for (int tick = 860; tick <= 1020; tick += 10) { set_counts(10000 + tick, FALSE); g_usleep(50000); }
    receive_exact("$spd:0,0,0,0#$pwm:0,0,0,0#", 150);
    expect_reason("target_reached");
    JsonObject *object = state();
    require(json_object_get_double_member(object, "estimated_mm") >= 200.0 &&
        json_object_get_double_member(object, "estimated_mm") < 202.0, "200mm converted from four-wheel count changes");
    json_object_unref(object);
    set_counts(11025, FALSE); g_usleep(70000);
    object = state();
    require(json_object_get_double_member(object, "estimated_mm") > 201.0, "post-stop rolling is retained in estimate");
    json_object_unref(object);
    close_motor();

    open_motor(TRUE);
    set_counts(10000, TRUE);
    begin_distance(TRUE);
    set_counts(10020, TRUE); g_usleep(70000);
    object = state();
    require(json_object_get_double_member(object, "estimated_mm") > 3.0, "per-wheel direction signs applied to counts");
    json_object_unref(object);
    request("stop", NULL, 200, FALSE, NULL);
    receive_exact("$spd:0,0,0,0#$pwm:0,0,0,0#", 100);
    expect_reason("manual");
    close_motor();

    open_motor(TRUE);
    begin_distance(FALSE);
    atomic_store(&with_encoder, FALSE);
    receive_exact("$spd:40,40,40,40#$spd:0,0,0,0#$pwm:0,0,0,0#", 500);
    expect_reason("encoder_timeout");
    close_motor();

    open_motor(TRUE);
    begin_distance(FALSE);
    set_counts(1000000, FALSE);
    receive_exact("$spd:0,0,0,0#$pwm:0,0,0,0#", 150);
    expect_reason("encoder_jump");
    close_motor();

    open_motor(TRUE);
    begin_distance(FALSE);
    set_counts(9970, FALSE);
    receive_exact("$spd:0,0,0,0#$pwm:0,0,0,0#", 150);
    expect_reason("wrong_direction");
    close_motor();

    open_motor(TRUE);
    begin_distance(FALSE);
    receive_exact("$spd:40,40,40,40#$spd:60,60,60,60#$spd:0,0,0,0#$pwm:0,0,0,0#", 1800);
    expect_reason("no_progress");
    close_motor();

    open_motor(TRUE);
    set_counts(INT32_MAX - 7, FALSE);
    begin_distance(FALSE);
    set_counts(INT32_MIN + 8, FALSE); g_usleep(70000);
    object = state();
    double estimate = json_object_get_double_member(object, "estimated_mm");
    require(estimate > 3.0 && estimate < 3.3, "natural 32-bit counter wrap retains small displacement");
    json_object_unref(object);
    request("stop", NULL, 200, FALSE, NULL);
    receive_exact("$spd:0,0,0,0#$pwm:0,0,0,0#", 100);
    close_motor();
    puts("Virtual-only checks complete: distance input; no motion without baseline; actual count differences; ramp/deceleration/target stop; post-stop counts; wheel signs; count loss/jump/reverse/stall; counter wrap. Real wheel counts, ruler distance and physical stop remain untested.");
    return 0;
}
