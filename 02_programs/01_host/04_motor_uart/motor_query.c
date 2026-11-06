#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define RX_CAPACITY 65536
#define BOARD_SERIAL "8314b1e8ffd0321f"
#define MOTOR_PORT "/dev/ttyS4"

#ifndef TIOCM_LOOP
#define TIOCM_LOOP 0x8000
#endif

struct driver_config {
    char version[32];
    long motor_type;
    long dead_zone;
    long pulse_line;
    long pulse_phase;
    double wheel_diameter;
    double proportional;
    double integral;
    double derivative;
};

static volatile sig_atomic_t interrupted;
static FILE *session_log;

static void report(const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    vfprintf(stdout, format, arguments);
    va_end(arguments);
    if (session_log) {
        va_start(arguments, format);
        vfprintf(session_log, format, arguments);
        va_end(arguments);
        fflush(session_log);
    }
    fflush(stdout);
}

static void on_signal(int signal_number)
{
    interrupted = signal_number;
}

static int64_t monotonic_ms(void)
{
    struct timespec timestamp;
    if (clock_gettime(CLOCK_MONOTONIC, &timestamp) < 0)
        return -1;
    return (int64_t)timestamp.tv_sec * 1000 + timestamp.tv_nsec / 1000000;
}

static int wait_ready(int descriptor, short events, int64_t deadline)
{
    while (!interrupted) {
        int64_t current = monotonic_ms();
        if (current < 0)
            return -1;
        if (current >= deadline)
            return 0;
        struct pollfd request = {.fd = descriptor, .events = events};
        int result = poll(&request, 1, (int)(deadline - current));
        if (result < 0 && errno == EINTR)
            continue;
        if (result <= 0)
            return result;
        if (request.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            errno = EIO;
            return -1;
        }
        if (request.revents & events)
            return 1;
    }
    errno = EINTR;
    return -1;
}

static bool read_text(const char *path, char *buffer, size_t capacity)
{
    FILE *input = fopen(path, "rb");
    if (!input)
        return false;
    size_t count = fread(buffer, 1, capacity - 1, input);
    bool success = !ferror(input) && count < capacity - 1;
    fclose(input);
    buffer[count] = '\0';
    return success;
}

static bool board_and_port_ready(void)
{
    char identity[128];
    char device_path[PATH_MAX];
    char command_line[4096];
    if (!read_text("/proc/device-tree/serial-number", identity, sizeof(identity))) {
        report("无法读取硬件序列号，未打开串口。\n");
        return false;
    }
    identity[strcspn(identity, "\r\n")] = '\0';
    report("设备：板B，实际序列号=%s\n", identity);
    if (strcmp(identity, BOARD_SERIAL) != 0 || geteuid() != 0) {
        report("目标板身份不符，或没有完整核查串口占用所需的root权限。\n");
        return false;
    }
    if (!realpath("/sys/class/tty/ttyS4/device/of_node", device_path) ||
        strcmp(device_path, "/sys/firmware/devicetree/base/serial@2ad70000") != 0) {
        report("UART4设备树映射与既有记录不符，未打开串口。\n");
        return false;
    }
    if (!read_text("/proc/cmdline", command_line, sizeof(command_line)) ||
        strstr(command_line, "console=ttyS4")) {
        report("无法排除系统控制台占用，未打开串口。\n");
        return false;
    }
    report("实际节点：%s → %s\n", MOTOR_PORT, device_path);
    return true;
}

static bool port_unused(void)
{
    struct stat port_status;
    if (stat(MOTOR_PORT, &port_status) < 0 || !S_ISCHR(port_status.st_mode))
        return false;
    DIR *processes = opendir("/proc");
    if (!processes)
        return false;
    bool available = true;
    struct dirent *process_entry;
    while (available && (process_entry = readdir(processes))) {
        if (!isdigit((unsigned char)process_entry->d_name[0]))
            continue;
        char directory_path[PATH_MAX];
        snprintf(directory_path, sizeof(directory_path), "/proc/%s/fd", process_entry->d_name);
        DIR *descriptors = opendir(directory_path);
        if (!descriptors) {
            if (errno != ENOENT && errno != ESRCH) {
                report("无法核查进程%s的串口占用。\n", process_entry->d_name);
                available = false;
            }
            continue;
        }
        struct dirent *descriptor_entry;
        while ((descriptor_entry = readdir(descriptors))) {
            if (!isdigit((unsigned char)descriptor_entry->d_name[0]))
                continue;
            struct stat descriptor_status;
            if (fstatat(dirfd(descriptors), descriptor_entry->d_name, &descriptor_status, 0) == 0 &&
                S_ISCHR(descriptor_status.st_mode) && descriptor_status.st_rdev == port_status.st_rdev) {
                report("串口已被进程%s占用，拒绝发送。\n", process_entry->d_name);
                available = false;
                break;
            }
        }
        closedir(descriptors);
    }
    closedir(processes);
    return available;
}

static bool save_bytes(const char *directory, const char *name,
                       const unsigned char *bytes, size_t count)
{
    char path[PATH_MAX];
    int length = snprintf(path, sizeof(path), "%s/%s", directory, name);
    if (length < 0 || (size_t)length >= sizeof(path))
        return false;
    int descriptor = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (descriptor < 0)
        return false;
    size_t saved = 0;
    while (saved < count) {
        ssize_t written = write(descriptor, bytes + saved, count - saved);
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0) {
            close(descriptor);
            return false;
        }
        saved += (size_t)written;
    }
    return close(descriptor) == 0;
}

static void show_bytes(const char *direction, const unsigned char *bytes, size_t count)
{
    report("\n[序号1 %s] %zu字节；文本中控制字符用转义显示\nTEXT: ", direction, count);
    for (size_t offset = 0; offset < count; ++offset) {
        unsigned char value = bytes[offset];
        if (value == '\r') report("\\r");
        else if (value == '\n') report("\\n\n");
        else if (value == '\t') report("\\t");
        else if (value == '\\') report("\\\\");
        else if (value >= 32 && value <= 126) report("%c", value);
        else report("\\x%02X", value);
    }
    report("\nHEX:");
    for (size_t offset = 0; offset < count; ++offset) {
        if (offset % 16 == 0) report("\n%04zX: ", offset);
        report("%02X ", bytes[offset]);
    }
    report("\n");
}

static bool parse_integer(const char *text, long *value)
{
    char *end;
    errno = 0;
    long parsed = strtol(text, &end, 10);
    if (errno || end == text)
        return false;
    while (isspace((unsigned char)*end)) ++end;
    if (*end)
        return false;
    *value = parsed;
    return true;
}

static bool parse_decimal(const char *text, double *value)
{
    char *end;
    errno = 0;
    double parsed = strtod(text, &end);
    if (errno || end == text || !isfinite(parsed))
        return false;
    while (isspace((unsigned char)*end)) ++end;
    if (*end)
        return false;
    *value = parsed;
    return true;
}

static bool parse_config(const unsigned char *bytes, size_t count, struct driver_config *config)
{
    char text[RX_CAPACITY + 1];
    if (count == 0 || count > RX_CAPACITY || memchr(bytes, '\0', count) ||
        (bytes[count - 1] != '\r' && bytes[count - 1] != '\n'))
        return false;
    memcpy(text, bytes, count);
    text[count] = '\0';
    char *marker = strstr(text, "read_flash:OK!");
    if (!marker || (marker != text && marker[-1] != '\r' && marker[-1] != '\n') ||
        (marker[14] != '\r' && marker[14] != '\n'))
        return false;
    unsigned fields = 0;
    char *context;
    for (char *line = strtok_r(marker, "\r\n", &context); line; line = strtok_r(NULL, "\r\n", &context)) {
        if (strncmp(line, "Motor_Version:", 14) == 0) {
            size_t length = strlen(line + 14);
            if (!length || length >= sizeof(config->version) || (fields & 1u)) return false;
            for (size_t offset = 14; line[offset]; ++offset)
                if (!isdigit((unsigned char)line[offset]) && line[offset] != '.') return false;
            memcpy(config->version, line + 14, length + 1);
            fields |= 1u;
        } else if (strncmp(line, "Motor_type:", 11) == 0) {
            if ((fields & 2u) || !parse_integer(line + 11, &config->motor_type)) return false;
            fields |= 2u;
        } else if (strncmp(line, "Dead_Zone:", 10) == 0) {
            if ((fields & 4u) || !parse_integer(line + 10, &config->dead_zone)) return false;
            fields |= 4u;
        } else if (strncmp(line, "Pulse_Line:", 11) == 0) {
            if ((fields & 8u) || !parse_integer(line + 11, &config->pulse_line)) return false;
            fields |= 8u;
        } else if (strncmp(line, "Pulse_Phase:", 12) == 0) {
            if ((fields & 16u) || !parse_integer(line + 12, &config->pulse_phase)) return false;
            fields |= 16u;
        } else if (strncmp(line, "wheel_diameter:", 15) == 0) {
            if ((fields & 32u) || !parse_decimal(line + 15, &config->wheel_diameter)) return false;
            fields |= 32u;
        } else if (strncmp(line, "P:", 2) == 0) {
            int consumed = -1;
            if ((fields & 64u) || sscanf(line, "P:%lf I:%lf D:%lf %n", &config->proportional,
                &config->integral, &config->derivative, &consumed) != 3 || consumed < 0 ||
                line[consumed] || !isfinite(config->proportional) ||
                !isfinite(config->integral) || !isfinite(config->derivative)) return false;
            fields |= 64u;
        }
    }
    return fields == 127u;
}

static bool parse_voltage(const unsigned char *bytes, size_t count, double *voltage)
{
    char text[RX_CAPACITY + 1];
    if (count > RX_CAPACITY || memchr(bytes, '\0', count))
        return false;
    memcpy(text, bytes, count);
    text[count] = '\0';
    const char *frame = strstr(text, "$Battery:");
    if (!frame)
        return false;
    char *end;
    errno = 0;
    double parsed = strtod(frame + 9, &end);
    if (errno || end == frame + 9 || !isfinite(parsed) || parsed < 0 || strncmp(end, "V#", 2) != 0)
        return false;
    *voltage = parsed;
    return true;
}

static void usage(const char *program)
{
    printf("用法：%s --port /dev/ttyS4 --query config|voltage --log-dir 新目录 [--timeout-ms 3000]\n", program);
    puts("仅用于已核接线和电平的板B：115200/8N1、无流控，一次只查询，无自动重发。\n"
         "config发送$read_flash#；voltage发送$read_vol#，不附加换行。\n"
         "须先协调串口使用；--help和参数错误均不打开UART。日志目录必须不存在。\n"
         "退出0=收到并解析有效回复；2=准备/输入错误；3=超时或无完整有效回复；4=串口/存档错误。");
}

int main(int argc, char **argv)
{
    const char *port = NULL;
    const char *query = NULL;
    const char *log_directory = NULL;
    long timeout_ms = 3000;
    for (int argument = 1; argument < argc; ++argument) {
        if (strcmp(argv[argument], "--help") == 0 && argc == 2) {
            usage(argv[0]);
            return 0;
        }
        if (argument + 1 >= argc) {
            usage(argv[0]);
            return 2;
        }
        if (strcmp(argv[argument], "--port") == 0) port = argv[++argument];
        else if (strcmp(argv[argument], "--query") == 0) query = argv[++argument];
        else if (strcmp(argv[argument], "--log-dir") == 0) log_directory = argv[++argument];
        else if (strcmp(argv[argument], "--timeout-ms") == 0) {
            if (!parse_integer(argv[++argument], &timeout_ms) || timeout_ms < 100 || timeout_ms > 10000)
                return 2;
        } else {
            usage(argv[0]);
            return 2;
        }
    }
    if (!port || strcmp(port, MOTOR_PORT) != 0 || !query ||
        (strcmp(query, "config") != 0 && strcmp(query, "voltage") != 0) || !log_directory) {
        usage(argv[0]);
        return 2;
    }
    char log_path[PATH_MAX];
    if (snprintf(log_path, sizeof(log_path), "%s/session.log", log_directory) >= (int)sizeof(log_path) ||
        mkdir(log_directory, 0700) < 0) {
        perror("创建新的日志目录");
        return 2;
    }
    session_log = fopen(log_path, "wx");
    if (!session_log) return 2;
    time_t wall_time = time(NULL);
    report("开始时间（板端本地）：%s", ctime(&wall_time));
    if (!board_and_port_ready() || !port_unused()) {
        fclose(session_log);
        return 2;
    }
    struct sigaction action = {.sa_handler = on_signal};
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) < 0 || sigaction(SIGTERM, &action, NULL) < 0) {
        fclose(session_log);
        return 2;
    }
    int exit_status = 4;
    int descriptor = open(port, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    struct termios saved_settings;
    bool settings_saved = false;
    bool exclusive = false;
    unsigned char received[RX_CAPACITY];
    unsigned char before_rx[4096];
    size_t received_count = 0;
    size_t before_count = 0;
    const unsigned char *command = (const unsigned char *)(strcmp(query, "config") == 0 ? "$read_flash#" : "$read_vol#");
    size_t command_length = strlen((const char *)command);
    size_t submitted = 0;
    if (descriptor < 0) goto cleanup;
    if (ioctl(descriptor, TIOCEXCL) < 0) goto cleanup;
    exclusive = true;
    if (tcgetattr(descriptor, &saved_settings) < 0) goto cleanup;
    settings_saved = true;
    int modem_flags = 0;
    if (ioctl(descriptor, TIOCMGET, &modem_flags) < 0 || (modem_flags & TIOCM_LOOP)) {
        report("无法确认内部回环已关闭，拒绝发送。\n");
        goto cleanup;
    }
    struct termios settings = saved_settings;
    cfmakeraw(&settings);
    settings.c_iflag &= ~(IXON | IXOFF | IXANY);
    settings.c_cflag &= ~(CSIZE | PARENB | CSTOPB | CRTSCTS);
    settings.c_cflag |= CS8 | CLOCAL | CREAD;
    settings.c_cc[VMIN] = 0;
    settings.c_cc[VTIME] = 0;
    if (cfsetispeed(&settings, B115200) < 0 || cfsetospeed(&settings, B115200) < 0 ||
        tcsetattr(descriptor, TCSANOW, &settings) < 0) goto cleanup;
    report("设置：115200 / 8N1 / 无流控 / 无本地回显；只发送一次。\n");
    int64_t before_start = monotonic_ms();
    if (before_start < 0) goto cleanup;
    int64_t quiet_since = before_start;
    int64_t quiet_deadline = before_start + 100;
    while (!interrupted) {
        int ready = wait_ready(descriptor, POLLIN, quiet_deadline);
        if (ready < 0) goto cleanup;
        if (ready == 0) {
            if (monotonic_ms() - quiet_since < 100) {
                report("发送前未获得100ms安静间隔，拒绝发送。\n");
                goto cleanup;
            }
            break;
        }
        if (before_count == sizeof(before_rx) || monotonic_ms() >= before_start + 500) {
            report("发送前持续有数据，无法归属回复，拒绝发送；先核对上传或串口占用。\n");
            goto cleanup;
        }
        ssize_t count = read(descriptor, before_rx + before_count, sizeof(before_rx) - before_count);
        if (count < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (count <= 0) goto cleanup;
        before_count += (size_t)count;
        quiet_since = monotonic_ms();
        int64_t next_quiet = quiet_since + 100;
        quiet_deadline = next_quiet < before_start + 500 ? next_quiet : before_start + 500;
    }
    if (interrupted) goto cleanup;
    if (before_count) {
        report("发送前已有数据，单独留档，不作为本次查询回复。\n");
        show_bytes("驱动→RK，发送前", before_rx, before_count);
    }
    int64_t write_start = monotonic_ms();
    if (write_start < 0) goto cleanup;
    while (submitted < command_length) {
        if (wait_ready(descriptor, POLLOUT, write_start + 1000) != 1) goto cleanup;
        ssize_t count = write(descriptor, command + submitted, command_length - submitted);
        if (count < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (count <= 0) goto cleanup;
        submitted += (size_t)count;
    }
    show_bytes("RK→驱动，串口写入", command, submitted);
    report("以上为内核接受的发送内容，驱动是否回复以下面的RX为准。\n");
    int64_t read_start = monotonic_ms();
    if (read_start < 0) goto cleanup;
    while (!interrupted) {
        int ready = wait_ready(descriptor, POLLIN, read_start + timeout_ms);
        if (ready < 0) goto cleanup;
        if (ready == 0) break;
        if (received_count == sizeof(received)) {
            report("回包超过有界缓冲，停止接收，保留已收到字节。\n");
            goto cleanup;
        }
        ssize_t count = read(descriptor, received + received_count, sizeof(received) - received_count);
        if (count < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (count <= 0) goto cleanup;
        received_count += (size_t)count;
    }
    if (interrupted) goto cleanup;
    show_bytes("驱动→RK，查询后", received, received_count);
    exit_status = 3;
    if (strcmp(query, "config") == 0) {
        struct driver_config config = {0};
        if (parse_config(received, received_count, &config)) {
            report("\n解析结果：固件=%s；类型=%ld；死区=%ld；线数=%ld；减速比=%ld；轮径=%.6gmm\n"
                   "P=%.6g；I=%.6g；D=%.6g\n", config.version, config.motor_type, config.dead_zone,
                   config.pulse_line, config.pulse_phase, config.wheel_diameter,
                   config.proportional, config.integral, config.derivative);
            exit_status = 0;
        }
    } else {
        double voltage;
        if (parse_voltage(received, received_count, &voltage)) {
            report("\n解析结果：驱动报告电压=%.6gV；未与万用表测量比较。\n", voltage);
            exit_status = 0;
        }
    }
    if (exit_status == 3)
        report("\n%ldms接收结束，未取得完整有效的%s回复；不能判定通信打通。\n",
               timeout_ms, query);
cleanup:
    if (exit_status == 4) {
        report("串口操作未完成：%s。未自动重发。\n", interrupted ? "收到终止信号" :
               (errno ? strerror(errno) : "未满足发送或接收条件"));
        show_bytes("RK→驱动，已写入部分", command, submitted);
        show_bytes("驱动→RK，保留已接收部分", received, received_count);
    }
    if (settings_saved && tcsetattr(descriptor, TCSANOW, &saved_settings) < 0) {
        report("恢复原串口格式失败：%s\n", strerror(errno));
        exit_status = 4;
    }
    if (exclusive && ioctl(descriptor, TIOCNXCL) < 0) exit_status = 4;
    if (descriptor >= 0 && close(descriptor) < 0) exit_status = 4;
    if (!save_bytes(log_directory, "tx.bin", command, submitted) ||
        !save_bytes(log_directory, "rx.bin", received, received_count) ||
        !save_bytes(log_directory, "before_rx.bin", before_rx, before_count)) {
        report("原始字节存档失败，不能作为完整验收记录。\n");
        exit_status = 4;
    }
    report("日志：%s；退出码=%d。退出0仅表示本次查询收到并解析回复。\n", log_directory, exit_status);
    bool log_failed = ferror(session_log);
    if (fclose(session_log) != 0 || log_failed) exit_status = 4;
    return exit_status;
}
