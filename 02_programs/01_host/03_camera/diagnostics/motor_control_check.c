#define _DEFAULT_SOURCE
#include "../motor_control.h"
#include <json-glib/json-glib.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <pty.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* 只连接Linux虚拟串口，不接真实电机。核对具体字节与计时，不代替实车验收。 */
static int peer;
static unsigned generation;
static atomic_bool feeding;
static pthread_t feeder;
static void *feedback_loop(void *unused) {
    (void)unused;
    const char *frame = "$MSPD:0,0,0,0#";
    while (atomic_load(&feeding)) {
        ssize_t sent = write(peer, frame, strlen(frame));
        if (sent != (ssize_t)strlen(frame)) return NULL;
        g_usleep(40000);
    }
    return NULL;
}

static void start_feedback(void) {
    puts("SIMULATED DRIVER->RK heartbeat every 40ms: $MSPD:0,0,0,0#");
    atomic_store(&feeding, TRUE);
    if (pthread_create(&feeder, NULL, feedback_loop, NULL)) exit(1);
}

static void stop_feedback(void) { atomic_store(&feeding, FALSE); pthread_join(feeder, NULL); }
static void require(gboolean condition, const char *message) {
    if (!condition) { fprintf(stderr, "CHECK FAILED: %s\n", message); exit(1); }
}

static char *state(MotorControl *motor) {
    char *json = motor_control_json(motor);
    JsonParser *parser = json_parser_new();
    require(json_parser_load_from_data(parser, json, -1, NULL), "valid status JSON");
    JsonObject *object = json_node_get_object(json_parser_get_root(parser));
    require(object && json_object_has_member(object, "generation"), "generation field");
    generation = (unsigned)json_object_get_int_member(object, "generation");
    g_object_unref(parser);
    return json;
}

static gint64 numeric(MotorControl *motor, const char *name) {
    char *json = state(motor);
    JsonParser *parser = json_parser_new();
    require(json_parser_load_from_data(parser, json, -1, NULL), "valid numeric status JSON");
    gint64 value = json_object_get_int_member(json_node_get_object(json_parser_get_root(parser)), name);
    g_object_unref(parser); g_free(json);
    return value;
}

static void command_parameters(MotorControl *motor, const char *action, unsigned expected,
                               gboolean alternate, const char *speed, const char *duration) {
    GHashTable *fields = g_hash_table_new(g_str_hash, g_str_equal);
    g_hash_table_insert(fields, "action", (char *)action);
    char value[32]; snprintf(value, sizeof(value), "%u", generation);
    g_hash_table_insert(fields, "generation", value);
    if (duration) g_hash_table_insert(fields, "duration_ms", (char *)duration);
    if (speed) g_hash_table_insert(fields, "speed_mm_s", (char *)speed);
    if (alternate) { g_hash_table_insert(fields, "m2", "-1"); g_hash_table_insert(fields, "m4", "-1"); }
    unsigned status;
    char *json = motor_control_command(motor, fields, &status);
    printf("INPUT action=%s generation=%u speed_mm_s=%s duration_ms=%s -> HTTP %u\n",
        action, generation, speed ? speed : "default", duration ? duration : "absent", status);
    JsonParser *parser = json_parser_new();
    require(json_parser_load_from_data(parser, json, -1, NULL), "valid command JSON");
    g_object_unref(parser);
    require(status == expected, json);
    g_free(json); g_hash_table_unref(fields);
}

static void command(MotorControl *motor, const char *action, unsigned expected, gboolean alternate) {
    command_parameters(motor, action, expected, alternate, NULL, "1000");
}

static void receive_exact(const char *expected, int timeout_ms) {
    char data[1024] = {0}; size_t used = 0;
    gint64 until = g_get_monotonic_time() + timeout_ms * 1000;
    while (used < strlen(expected) && used < sizeof(data) - 1 && g_get_monotonic_time() < until) {
        ssize_t n = read(peer, data + used, sizeof(data) - 1 - used);
        if (n > 0) used += (size_t)n;
        else { struct pollfd p = {peer, POLLIN, 0}; (void)poll(&p, 1, 5); }
    }
    printf("OBSERVED RK->VIRTUAL_DRIVER: %s\n", data);
    require(!strcmp(data, expected), "UART bytes differ or missing");
}

static void quiet(int ms) {
    struct pollfd p = {peer, POLLIN, 0};
    require(poll(&p, 1, ms) == 0, "unexpected UART output");
}

static void configure(MotorControl *motor) {
    char *json = state(motor); g_free(json);
    command(motor, "query", 200, FALSE);
    receive_exact("$spd:0,0,0,0#$pwm:0,0,0,0#$read_flash#", 250);
    const char *echo = "$read_flash#";
    require(write(peer, echo, strlen(echo)) == (ssize_t)strlen(echo), "echo write");
    g_usleep(40000);
    json = state(motor); require(strstr(json, "\"ready\":false") != NULL, "echo must not count as reply"); g_free(json);
    const char *reply = "read_flash:OK!\r\nMotor_Version:1.7.3\r\nMotor_type:2\r\nDead_Zone:1600\r\nPulse_Line:13\r\nPulse_Phase:20\r\nwheel_diameter:65.000\r\nP:0.800\t I:0.060\t D:0.500\t \r\n";
    printf("INPUT VIRTUAL_DRIVER->RK: real-format 150-byte configuration (simulated)\n");
    size_t half = strlen(reply) / 2;
    require(write(peer, reply, half) == (ssize_t)half, "first reply half");
    g_usleep(30000);
    json = state(motor); require(strstr(json, "\"ready\":false") != NULL, "truncated reply rejected"); g_free(json);
    require(write(peer, reply + half, strlen(reply) - half) == (ssize_t)(strlen(reply) - half), "second reply half");
    g_usleep(30000);
    json = state(motor); require(strstr(json, "\"ready\":true") != NULL, "complete reply accepted"); g_free(json);
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    int slave; char path[128];
    require(openpty(&peer, &slave, path, NULL, NULL) == 0, "openpty");
    close(slave); require(fcntl(peer, F_SETFL, O_NONBLOCK) == 0, "nonblocking peer");
    printf("VIRTUAL UART ONLY: %s; no physical UART opened\n", path);
    MotorControl *motor = motor_control_new(path);
    char *json = state(motor); require(strstr(json, "\"opened\":true") != NULL, json); g_free(json);
    require(numeric(motor, "duration_ms") == 3000, "default three-second duration");
    require(numeric(motor, "speed_mm_s") == 60, "default target 60mm/s");
    quiet(40);
    command(motor, "forward", 409, FALSE); quiet(30);
    command(motor, "left", 409, FALSE); quiet(30);
    command(motor, "right", 409, FALSE); quiet(30);
    command(motor, "anything", 400, FALSE); quiet(30);
    command(motor, "test_m3", 400, FALSE); quiet(30);
    configure(motor);
    const char *bad_speeds[] = {"", "-20", "10", "310", "25", "60.0", "nan", "999999999999999999999999999999"};
    for (unsigned i = 0; i < G_N_ELEMENTS(bad_speeds); ++i) {
        command_parameters(motor, "forward", 400, FALSE, bad_speeds[i], "1000"); quiet(20);
    }
    const char *bad_times[] = {"", "0", "1500", "11000", "-1000", "2e3"};
    for (unsigned i = 0; i < G_N_ELEMENTS(bad_times); ++i) {
        command_parameters(motor, "forward", 400, FALSE, "60", bad_times[i]); quiet(20);
    }
    start_feedback();
    gint64 began = g_get_monotonic_time();
    command(motor, "forward", 200, FALSE);
    receive_exact("$upload:1,0,1#$spd:20,20,20,20#", 100);
    g_usleep(120000);
    command(motor, "forward", 409, FALSE);
    command(motor, "backward", 409, FALSE);
    command(motor, "left", 409, FALSE);
    command(motor, "right", 409, FALSE);
    receive_exact("$spd:40,40,40,40#$spd:60,60,60,60#$spd:0,0,0,0#$pwm:0,0,0,0#", 1200);
    double elapsed = (g_get_monotonic_time() - began) / 1000.0;
    printf("OBSERVED forward->zero output interval: %.1f ms\n", elapsed);
    require(elapsed >= 1000 && elapsed <= 1150, "selected one-second stop deadline");
    json = state(motor);
    require(strstr(json, "M1 0.0") != NULL, "actual speed feedback parsed");
    g_free(json);
    generation -= 2; /* 使用本次动作之前的序号，模拟延迟的旧请求。 */
    command(motor, "forward", 409, FALSE); quiet(40); /* 延迟的旧请求不能重新启动。 */
    command(motor, "left", 409, FALSE); quiet(20);
    command(motor, "right", 409, FALSE); quiet(20);
    json = state(motor); g_free(json);
    command(motor, "backward", 200, TRUE);
    receive_exact("$upload:1,0,1#$spd:-20,20,-20,20#", 100);
    command(motor, "stop", 200, FALSE);
    receive_exact("$spd:0,0,0,0#$pwm:0,0,0,0#", 100);
    quiet(550);
    json = state(motor); g_free(json);
    command_parameters(motor, "left", 400, FALSE, "310", "1000"); quiet(20);
    command_parameters(motor, "right", 400, FALSE, "60", "1500"); quiet(20);
    began = g_get_monotonic_time();
    command(motor, "left", 200, FALSE);
    receive_exact("$upload:1,0,1#$spd:-20,20,20,-20#$spd:-40,40,40,-40#", 400);
    gint64 left_before = numeric(motor, "remaining_ms");
    command_parameters(motor, "speed", 200, FALSE, "90", NULL);
    require(numeric(motor, "remaining_ms") <= left_before, "left speed update keeps deadline");
    json = state(motor); require(strstr(json, "\"action\":\"left\"") != NULL, "left action retained while changing speed"); g_free(json);
    command(motor, "right", 409, FALSE);
    receive_exact("$spd:-60,60,60,-60#$spd:-80,80,80,-80#$spd:-90,90,90,-90#$spd:0,0,0,0#$pwm:0,0,0,0#", 1100);
    elapsed = (g_get_monotonic_time() - began) / 1000.0;
    printf("OBSERVED left translation signs, live speed and original stop: %.1f ms\n", elapsed);
    require(elapsed >= 1000 && elapsed <= 1150, "left one-second automatic stop");
    json = state(motor); g_free(json);
    began = g_get_monotonic_time();
    command(motor, "right", 200, FALSE);
    receive_exact("$upload:1,0,1#$spd:20,-20,-20,20#$spd:40,-40,-40,40#", 400);
    gint64 right_before = numeric(motor, "remaining_ms");
    command_parameters(motor, "speed", 200, FALSE, "30", NULL);
    require(numeric(motor, "remaining_ms") <= right_before, "right speed update keeps deadline");
    json = state(motor); require(strstr(json, "\"action\":\"right\"") != NULL, "right action retained while changing speed"); g_free(json);
    command(motor, "left", 409, FALSE);
    receive_exact("$spd:30,-30,-30,30#$spd:0,0,0,0#$pwm:0,0,0,0#", 1100);
    elapsed = (g_get_monotonic_time() - began) / 1000.0;
    printf("OBSERVED right translation signs, live speed and original stop: %.1f ms\n", elapsed);
    require(elapsed >= 1000 && elapsed <= 1150, "right one-second automatic stop");
    json = state(motor); g_free(json);
    command(motor, "left", 200, TRUE);
    receive_exact("$upload:1,0,1#$spd:-20,-20,20,20#", 100);
    command(motor, "stop", 200, FALSE);
    receive_exact("$spd:0,0,0,0#$pwm:0,0,0,0#", 100);
    json = state(motor); g_free(json);
    command(motor, "right", 200, TRUE);
    receive_exact("$upload:1,0,1#$spd:20,20,-20,-20#", 100);
    command(motor, "stop", 200, FALSE);
    receive_exact("$spd:0,0,0,0#$pwm:0,0,0,0#", 100);
    json = state(motor); g_free(json);
    began = g_get_monotonic_time();
    command_parameters(motor, "forward", 200, FALSE, "150", "3000");
    receive_exact("$upload:1,0,1#$spd:20,20,20,20#$spd:40,40,40,40#$spd:60,60,60,60#$spd:80,80,80,80#$spd:100,100,100,100#$spd:120,120,120,120#$spd:140,140,140,140#$spd:150,150,150,150#$spd:0,0,0,0#$pwm:0,0,0,0#", 3300);
    elapsed = (g_get_monotonic_time() - began) / 1000.0;
    printf("OBSERVED selected 15cm/s ramp and 3s stop: %.1f ms\n", elapsed);
    require(elapsed >= 3000 && elapsed < 3200, "three-second stop independent of speed");
    json = state(motor); g_free(json);
    command_parameters(motor, "forward", 200, FALSE, "90", "3000");
    receive_exact("$upload:1,0,1#$spd:20,20,20,20#$spd:40,40,40,40#", 400);
    gint64 before = numeric(motor, "remaining_ms");
    command_parameters(motor, "speed", 200, FALSE, "30", NULL);
    gint64 after = numeric(motor, "remaining_ms");
    require(after <= before && after > before - 100, "live speed keeps original deadline");
    require(numeric(motor, "duration_ms") == 3000 && numeric(motor, "speed_mm_s") == 30, "only target changed");
    receive_exact("$spd:30,30,30,30#", 350);
    command_parameters(motor, "speed", 400, FALSE, "60", "10000");
    quiet(30);
    command_parameters(motor, "speed", 200, FALSE, "90", NULL);
    receive_exact("$spd:50,50,50,50#$spd:70,70,70,70#$spd:90,90,90,90#", 800);
    json = state(motor); g_free(json);
    command(motor, "stop", 200, FALSE);
    receive_exact("$spd:0,0,0,0#$pwm:0,0,0,0#", 100);
    json = state(motor); g_free(json);
    command_parameters(motor, "speed", 409, FALSE, "60", NULL); quiet(30);
    command_parameters(motor, "forward", 200, FALSE, "300", "10000");
    receive_exact("$upload:1,0,1#$spd:20,20,20,20#", 100);
    require(numeric(motor, "duration_ms") == 10000 && numeric(motor, "remaining_ms") > 9800 && numeric(motor, "speed_mm_s") == 300, "ten-second duration and 30cm/s speed upper bounds accepted");
    command(motor, "stop", 200, FALSE);
    receive_exact("$spd:0,0,0,0#$pwm:0,0,0,0#", 100);
    json = state(motor); g_free(json);
    command(motor, "forward", 200, FALSE); receive_exact("$upload:1,0,1#$spd:20,20,20,20#", 100);
    motor_control_free(motor);
    receive_exact("$spd:0,0,0,0#$pwm:0,0,0,0#$upload:0,0,0#", 100);
    stop_feedback();
    motor = motor_control_new(path);
    configure(motor);
    command(motor, "forward", 200, FALSE);
    receive_exact("$upload:1,0,1#$spd:20,20,20,20#", 100);
    receive_exact("$spd:40,40,40,40#$spd:0,0,0,0#$pwm:0,0,0,0#", 500);
    json = state(motor);
    require(strstr(json, "\"ready\":false") != NULL && strstr(json, "\"moving\":false") != NULL, "missing feedback stops and disarms");
    g_free(json);
    command(motor, "forward", 409, FALSE);
    motor_control_free(motor);
    receive_exact("$upload:0,0,0#", 100);
    close(peer);
    puts("Verified: forward/backward/left/right serial patterns and per-wheel calibration; lateral live speed and original deadlines; 2..30cm/s and 1..10s bounds; clamped speed ramp; 1s/3s deadlines; exact serial frames; invalid/stale/busy requests; missing feedback and shutdown stop. Physical movement NOT tested.");
    return 0;
}
