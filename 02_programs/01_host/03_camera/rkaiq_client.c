#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "rkaiq_client.h"

#include <errno.h>
#include <poll.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

/*
 * C工具协议依据（公开版本不是板上6.9.0的完整ABI，不能照搬其参数结构体）：
 * https://raw.githubusercontent.com/JeffyCN/mirrors/rkaiq-2025_08_11/rkaiq/ipc/socket_client.c
 * https://raw.githubusercontent.com/JeffyCN/mirrors/rkaiq-2025_08_11/rkaiq/ipc/message_receiver.c
 * https://raw.githubusercontent.com/JeffyCN/mirrors/rkaiq-2025_08_11/rkaiq/ipc/socket_client.h
 * https://raw.githubusercontent.com/JeffyCN/mirrors/rkaiq-2025_08_11/rkaiq/uAPI2_c/rk_aiq_user_api2_helper.c
 *
 * 头24字节依次为magic、cmd、ret、sequence、payload_size、packet_size；
 * 公开旧协议的两个size均为JSON字节数N，总长N+28。
 * 板B 6.9.0实测回复另有128字节说明：payload_size=N、packet_size=N+156，
 * 成功码为0xff00，JSON从字节152开始；WRITE同样回复，N=0时ACK共156字节。
 * C接收端以size1取正文，size2只收集；请求沿用已实测的N/N两个长度。
 * C发送端未填写响应hash，本模块不以该尾值判断响应是否有效。
 */
enum {
    HEADER_BYTES = 24,
    TRAILER_BYTES = 4,
    NOTE_BYTES = 128,
    MAX_PACKET_BYTES = 512 * 1024,
    WRITE_COMMAND = 0,
    READ_COMMAND = 1
};

static const guint8 packet_magic[4] = { 0x52, 0xaa, 0xff, 0x4b };
static GMutex transaction_mutex;
static GCond transaction_changed;
static gboolean transaction_busy;
/* 以下连接状态只在占有事务时读写，包含回复解析失败后的关闭。 */
static int persistent_fd = -1;
static gchar persistent_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
static guint32 sequence_counter;

typedef struct {
    guint8 *bytes;
    gsize prefix_bytes, payload_length;
    guint32 result_code;
    gboolean extended;
} PacketReply;

GQuark rkaiq_client_error_quark(void) {
    return g_quark_from_static_string("opto-rkaiq-client-error");
}

static gboolean io_error(GError **error, const gchar *stage, int saved_errno) {
    g_set_error(error, RKAIQ_CLIENT_ERROR, RKAIQ_CLIENT_ERROR_IO,
                "%s：%s", stage, g_strerror(saved_errno));
    return FALSE;
}

static gboolean timeout_error(GError **error, const gchar *stage) {
    g_set_error(error, RKAIQ_CLIENT_ERROR, RKAIQ_CLIENT_ERROR_TIMEOUT,
                "%s超时；本次没有收到经过核对的3A参数", stage);
    return FALSE;
}

/* 只用短临界区登记占用；网络等待不持有该锁，等候者也有共同的截止时间。 */
static gboolean begin_transaction(gint64 deadline, GError **error) {
    g_mutex_lock(&transaction_mutex);
    while (transaction_busy) {
        if (!g_cond_wait_until(&transaction_changed, &transaction_mutex, deadline)) {
            g_mutex_unlock(&transaction_mutex);
            return timeout_error(error, "等待其他3A查询结束");
        }
    }
    if (g_get_monotonic_time() >= deadline) {
        g_mutex_unlock(&transaction_mutex);
        return timeout_error(error, "等待3A查询");
    }
    transaction_busy = TRUE;
    g_mutex_unlock(&transaction_mutex);
    return TRUE;
}

static void end_transaction(void) {
    g_mutex_lock(&transaction_mutex);
    transaction_busy = FALSE;
    /* 避免唯一被唤醒者刚好超时，留下其他仍有期限的请求无人唤醒。 */
    g_cond_broadcast(&transaction_changed);
    g_mutex_unlock(&transaction_mutex);
}

static void close_connection(void) {
    if (persistent_fd >= 0) close(persistent_fd);
    persistent_fd = -1;
    persistent_path[0] = '\0';
}

static guint32 read_le32(const guint8 *bytes) {
    return (guint32)bytes[0] | ((guint32)bytes[1] << 8)
         | ((guint32)bytes[2] << 16) | ((guint32)bytes[3] << 24);
}

static void write_le32(guint8 *bytes, guint32 value) {
    bytes[0] = (guint8)value;
    bytes[1] = (guint8)(value >> 8);
    bytes[2] = (guint8)(value >> 16);
    bytes[3] = (guint8)(value >> 24);
}

/* 与工具端MurMurHash一致：MurmurHash2，seed=97，按小端读取32位数据。 */
static guint32 payload_hash(const guint8 *bytes, gsize length) {
    const guint32 multiplier = 0x5bd1e995U;
    guint32 result = 97U ^ (guint32)length;
    while (length >= 4) {
        guint32 part = read_le32(bytes);
        part *= multiplier;
        part ^= part >> 24;
        part *= multiplier;
        result = result * multiplier ^ part;
        bytes += 4;
        length -= 4;
    }
    if (length == 3) result ^= (guint32)bytes[2] << 16;
    if (length >= 2) result ^= (guint32)bytes[1] << 8;
    if (length >= 1) {
        result ^= bytes[0];
        result *= multiplier;
    }
    result ^= result >> 13;
    result *= multiplier;
    result ^= result >> 15;
    return result;
}

static gboolean wait_socket(int fd, short events, gint64 deadline,
                            const gchar *stage, GError **error) {
    for (;;) {
        gint64 remaining = deadline - g_get_monotonic_time();
        if (remaining <= 0) return timeout_error(error, stage);
        int timeout = (int)MIN((remaining + 999) / 1000, (gint64)G_MAXINT);
        struct pollfd entry = { .fd = fd, .events = events };
        int result = poll(&entry, 1, timeout);
        if (result < 0) {
            if (errno == EINTR) continue;
            return io_error(error, stage, errno);
        }
        if (result == 0) return timeout_error(error, stage);
        if (entry.revents & POLLNVAL) return io_error(error, stage, EBADF);
        /* HUP时仍可能有完整缓冲数据，交给recv读完后再判断EOF。 */
        if (entry.revents & (events | POLLHUP | POLLERR)) return TRUE;
    }
}

static int connect_socket(const gchar *path, gint64 deadline, GError **error) {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) { io_error(error, "创建3A本地连接", errno); return -1; }
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    gsize path_length = strlen(path);
    memcpy(address.sun_path, path, path_length + 1);
    socklen_t address_size = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + path_length + 1);
    if (connect(fd, (struct sockaddr *)&address, address_size) == 0) return fd;
    int saved_errno = errno;
    if (saved_errno != EINPROGRESS && saved_errno != EALREADY && saved_errno != EINTR) {
        io_error(error, "连接已有3A服务（请核对socket路径及服务状态）", saved_errno);
        close(fd);
        return -1;
    }
    if (!wait_socket(fd, POLLOUT, deadline, "连接已有3A服务", error)) { close(fd); return -1; }
    int socket_error = 0;
    socklen_t error_size = sizeof(socket_error);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &error_size) < 0) {
        io_error(error, "读取3A连接状态", errno);
        close(fd);
        return -1;
    }
    if (socket_error != 0) {
        io_error(error, "连接已有3A服务", socket_error);
        close(fd);
        return -1;
    }
    return fd;
}

static gboolean send_all(int fd, const guint8 *data, gsize length,
                         gint64 deadline, GError **error) {
    gsize sent = 0;
    while (sent < length) {
        if (g_get_monotonic_time() >= deadline) return timeout_error(error, "发送3A查询");
        ssize_t count = send(fd, data + sent, length - sent, MSG_NOSIGNAL);
        if (count > 0) { sent += (gsize)count; continue; }
        if (count < 0 && errno == EINTR) continue;
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (!wait_socket(fd, POLLOUT, deadline, "发送3A查询", error)) return FALSE;
            continue;
        }
        return io_error(error, "发送3A查询", count == 0 ? EPIPE : errno);
    }
    return TRUE;
}

static gboolean receive_all(int fd, guint8 *data, gsize length,
                            gint64 deadline, const gchar *stage, GError **error) {
    gsize received = 0;
    while (received < length) {
        if (g_get_monotonic_time() >= deadline) return timeout_error(error, stage);
        ssize_t count = recv(fd, data + received, length - received, 0);
        if (count > 0) { received += (gsize)count; continue; }
        if (count == 0) {
            g_set_error(error, RKAIQ_CLIENT_ERROR, RKAIQ_CLIENT_ERROR_PROTOCOL,
                        "%s时连接提前关闭，仅收到%" G_GSIZE_FORMAT "/%" G_GSIZE_FORMAT "字节",
                        stage, received, length);
            return FALSE;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            if (!wait_socket(fd, POLLIN, deadline, stage, error)) return FALSE;
            continue;
        }
        return io_error(error, stage, errno);
    }
    return TRUE;
}

/* owned_value非NULL时，所有权交给builder；借用的原值须先经过深拷贝。 */
static gchar *request_json(const gchar *path, const gchar *operation,
                           JsonNode *owned_value, gsize *length) {
    JsonBuilder *builder = json_builder_new();
    json_builder_begin_array(builder);
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "op");
    json_builder_add_string_value(builder, operation);
    json_builder_set_member_name(builder, "path");
    json_builder_add_string_value(builder, path);
    if (owned_value) {
        json_builder_set_member_name(builder, "value");
        json_builder_add_value(builder, owned_value);
    }
    json_builder_end_object(builder);
    json_builder_end_array(builder);
    JsonNode *root = json_builder_get_root(builder);
    JsonGenerator *generator = json_generator_new();
    json_generator_set_root(generator, root);
    gchar *result = json_generator_to_data(generator, length);
    g_object_unref(generator);
    json_node_unref(root);
    g_object_unref(builder);
    return result;
}

static gboolean response_value(guint8 *payload, gsize length, const gchar *path,
                               JsonNode **value, GError **error) {
    if (memchr(payload, '\0', length) || !g_utf8_validate((const gchar *)payload, (gssize)length, NULL)) {
        g_set_error_literal(error, RKAIQ_CLIENT_ERROR, RKAIQ_CLIENT_ERROR_JSON,
                            "3A回复包含非法JSON文本字节");
        return FALSE;
    }
    payload[length] = '\0';
    JsonParser *parser = json_parser_new();
    GError *parse_error = NULL;
    if (!json_parser_load_from_data(parser, (const gchar *)payload, (gssize)length, &parse_error)) {
        g_set_error(error, RKAIQ_CLIENT_ERROR, RKAIQ_CLIENT_ERROR_JSON,
                    "3A回复不是有效JSON：%s", parse_error->message);
        g_error_free(parse_error);
        g_object_unref(parser);
        return FALSE;
    }
    JsonNode *root = json_parser_get_root(parser);
    JsonObject *object = NULL;
    if (root && JSON_NODE_HOLDS_ARRAY(root)) {
        JsonArray *array = json_node_get_array(root);
        if (json_array_get_length(array) == 1) {
            JsonNode *item = json_array_get_element(array, 0);
            if (item && JSON_NODE_HOLDS_OBJECT(item)) object = json_node_get_object(item);
        }
    }
    JsonNode *path_node = object ? json_object_get_member(object, "path") : NULL;
    if (!path_node || !JSON_NODE_HOLDS_VALUE(path_node)
        || json_node_get_value_type(path_node) != G_TYPE_STRING
        || g_strcmp0(json_node_get_string(path_node), path) != 0
        || !json_object_has_member(object, "value")) {
        g_set_error_literal(error, RKAIQ_CLIENT_ERROR, RKAIQ_CLIENT_ERROR_PROTOCOL,
                            "3A回复须为单项数组，且path与查询一致、包含value");
        g_object_unref(parser);
        return FALSE;
    }
    *value = json_node_copy(json_object_get_member(object, "value"));
    g_object_unref(parser);
    return TRUE;
}

static gboolean header_error(const guint8 header[HEADER_BYTES],
                              const gchar *reason, GError **error) {
    static const gchar digits[] = "0123456789abcdef";
    gchar text[HEADER_BYTES * 2 + 1];
    for (guint i = 0; i < HEADER_BYTES; ++i) {
        text[i * 2] = digits[header[i] >> 4];
        text[i * 2 + 1] = digits[header[i] & 15];
    }
    text[HEADER_BYTES * 2] = '\0';
    g_set_error(error, RKAIQ_CLIENT_ERROR, RKAIQ_CLIENT_ERROR_PROTOCOL,
                "%s：payload=%u，packet=%u，ret=0x%x，sequence=%u，header=%s",
                reason, read_le32(header + 16), read_le32(header + 20),
                read_le32(header + 8), read_le32(header + 12), text);
    return FALSE;
}

/* 调用者持有事务直到ACK和JSON均核对完；GET和SET都读取服务端真实回复。 */
static gboolean exchange_packet(const gchar *socket_path, guint32 command,
                                 const gchar *request_json, gsize request_length,
                                 gint64 deadline, PacketReply *reply, GError **error) {
    gboolean success = FALSE;
    guint8 *request = NULL, *response = NULL;
    if (request_length > MAX_PACKET_BYTES - HEADER_BYTES - TRAILER_BYTES) {
        g_set_error_literal(error, RKAIQ_CLIENT_ERROR, RKAIQ_CLIENT_ERROR_ARGUMENT, "3A查询报文过长");
        goto done;
    }
    request = g_try_malloc0(HEADER_BYTES + request_length + TRAILER_BYTES);
    if (!request) { io_error(error, "分配3A查询缓存", ENOMEM); goto done; }
    memcpy(request, packet_magic, sizeof(packet_magic));
    write_le32(request + 4, command);
    guint32 request_sequence = ++sequence_counter;
    if (request_sequence == 0) request_sequence = ++sequence_counter;
    write_le32(request + 12, request_sequence);
    write_le32(request + 16, (guint32)request_length);
    write_le32(request + 20, (guint32)request_length);
    memcpy(request + HEADER_BYTES, request_json, request_length);
    write_le32(request + HEADER_BYTES + request_length,
               payload_hash((const guint8 *)request_json, request_length));
    if (persistent_fd >= 0 && strcmp(socket_path, persistent_path) != 0) close_connection();
    if (persistent_fd < 0) {
        persistent_fd = connect_socket(socket_path, deadline, error);
        if (persistent_fd < 0) goto done;
        g_strlcpy(persistent_path, socket_path, sizeof(persistent_path));
    }
    if (!send_all(persistent_fd, request, HEADER_BYTES + request_length + TRAILER_BYTES, deadline, error)) goto done;
    guint8 header[HEADER_BYTES];
    if (!receive_all(persistent_fd, header, sizeof(header), deadline, "读取3A回复头", error)) goto done;
    guint32 payload_length = read_le32(header + 16);
    guint32 packet_length = read_le32(header + 20);
    guint32 sequence = read_le32(header + 12);
    if (memcmp(header, packet_magic, sizeof(packet_magic)) || read_le32(header + 4) != command
        || (sequence != 0 && sequence != request_sequence)) {
        header_error(header, "3A回复标记、命令或序号不符", error);
        goto done;
    }
    if ((payload_length == 0 && command != WRITE_COMMAND)
        || payload_length > MAX_PACKET_BYTES - HEADER_BYTES - TRAILER_BYTES) {
        header_error(header, "3A回复正文长度无效或超出512KiB上限", error);
        goto done;
    }
    gsize prefix_bytes = 0;
    if (packet_length == payload_length) {
        reply->extended = FALSE;
    } else if (packet_length == (gsize)payload_length + HEADER_BYTES + NOTE_BYTES + TRAILER_BYTES
               && packet_length <= MAX_PACKET_BYTES) {
        reply->extended = TRUE;
        prefix_bytes = NOTE_BYTES;
    } else {
        header_error(header, "3A回复长度不符合已核对的旧版或6.9布局", error);
        goto done;
    }
    gsize remaining_bytes = prefix_bytes + payload_length + TRAILER_BYTES;
    response = g_try_malloc(remaining_bytes + 1);
    if (!response) { io_error(error, "分配3A回复缓存", ENOMEM); goto done; }
    if (!receive_all(persistent_fd, response, remaining_bytes, deadline, "读取3A回复正文", error)) goto done;
    response[remaining_bytes] = '\0';
    reply->bytes = response;
    reply->prefix_bytes = prefix_bytes;
    reply->payload_length = payload_length;
    reply->result_code = read_le32(header + 8);
    response = NULL;
    success = TRUE;
done:
    if (!success) close_connection();
    g_free(response);
    g_free(request);
    return success;
}

static gboolean validate_arguments(const gchar *socket_path, const gchar *uapi_path,
                                    guint timeout_ms, GError **error) {
    if (!socket_path || socket_path[0] != '/'
        || strlen(socket_path) >= sizeof(((struct sockaddr_un *)0)->sun_path)
        || !uapi_path || !g_str_has_prefix(uapi_path, "/uapi/0/")
        || strlen(uapi_path) > 1024 || !g_utf8_validate(uapi_path, -1, NULL)
        || timeout_ms == 0 || timeout_ms > 60000) {
        g_set_error_literal(error, RKAIQ_CLIENT_ERROR, RKAIQ_CLIENT_ERROR_ARGUMENT,
                            "请提供有效socket绝对路径、/uapi/0/查询路径和1～60000毫秒期限");
        return FALSE;
    }
    return TRUE;
}

static gboolean reply_succeeded(const PacketReply *reply, GError **error) {
    guint32 expected_result = reply->extended ? 0xff00U : 0U;
    if (reply->result_code == expected_result) return TRUE;
    g_set_error(error, RKAIQ_CLIENT_ERROR, RKAIQ_CLIENT_ERROR_REMOTE,
                "3A服务返回码0x%x不表示成功，服务说明：%.*s", reply->result_code,
                (int)reply->prefix_bytes, (const gchar *)reply->bytes);
    return FALSE;
}

/* 序列化再解析得到完全独立的树；json_node_copy原对象会共享数组/对象，不能代替此步。 */
static JsonNode *deep_copy_value(JsonNode *value, GError **error) {
    JsonGenerator *generator = json_generator_new();
    json_generator_set_root(generator, value);
    gsize length = 0;
    gchar *text = json_generator_to_data(generator, &length);
    g_object_unref(generator);
    if (!text || length > MAX_PACKET_BYTES - HEADER_BYTES - TRAILER_BYTES) {
        g_free(text);
        g_set_error_literal(error, RKAIQ_CLIENT_ERROR, RKAIQ_CLIENT_ERROR_ARGUMENT,
                            "待设置的3A参数无法编码或超出报文上限");
        return NULL;
    }
    JsonParser *parser = json_parser_new();
    GError *parse_error = NULL;
    JsonNode *copy = NULL;
    if (!json_parser_load_from_data(parser, text, (gssize)length, &parse_error)) {
        g_set_error(error, RKAIQ_CLIENT_ERROR, RKAIQ_CLIENT_ERROR_JSON,
                    "待设置的3A参数不是有效JSON：%s", parse_error->message);
        g_error_free(parse_error);
    } else {
        copy = json_node_copy(json_parser_get_root(parser));
    }
    g_object_unref(parser);
    g_free(text);
    return copy;
}

gboolean rkaiq_client_get(const gchar *socket_path, const gchar *uapi_path,
                         guint timeout_ms, JsonNode **value, GError **error) {
    if (value) *value = NULL;
    if (error && *error) return FALSE;
    if (!value) {
        g_set_error_literal(error, RKAIQ_CLIENT_ERROR, RKAIQ_CLIENT_ERROR_ARGUMENT,
                            "请提供3A读取结果的保存位置");
        return FALSE;
    }
    if (!validate_arguments(socket_path, uapi_path, timeout_ms, error)) return FALSE;
    gint64 deadline = g_get_monotonic_time() + (gint64)timeout_ms * 1000;
    gsize request_length = 0;
    gchar *text = request_json(uapi_path, "get", NULL, &request_length);
    if (!text) {
        io_error(error, "生成3A查询JSON", ENOMEM);
        return FALSE;
    }
    PacketReply reply = { 0 };
    if (!begin_transaction(deadline, error)) { g_free(text); return FALSE; }
    gboolean success = exchange_packet(socket_path, READ_COMMAND, text, request_length,
                                        deadline, &reply, error);
    g_free(text);
    if (success) success = reply_succeeded(&reply, error);
    if (success)
        success = response_value(reply.bytes + reply.prefix_bytes, reply.payload_length,
                                  uapi_path, value, error);
    if (!success) close_connection();
    g_free(reply.bytes);
    end_transaction();
    return success;
}

gboolean rkaiq_client_set(const gchar *socket_path, const gchar *uapi_path,
                         JsonNode *value, guint timeout_ms, GError **error) {
    if (error && *error) return FALSE;
    if (!value) {
        g_set_error_literal(error, RKAIQ_CLIENT_ERROR, RKAIQ_CLIENT_ERROR_ARGUMENT,
                            "请提供待设置的3A参数JSON节点");
        return FALSE;
    }
    if (!validate_arguments(socket_path, uapi_path, timeout_ms, error)) return FALSE;
    gint64 deadline = g_get_monotonic_time() + (gint64)timeout_ms * 1000;
    JsonNode *copy = deep_copy_value(value, error);
    if (!copy) return FALSE;
    gsize request_length = 0;
    gchar *text = request_json(uapi_path, "replace", copy, &request_length);
    if (!text) {
        io_error(error, "生成3A设置JSON", ENOMEM);
        return FALSE;
    }
    PacketReply reply = { 0 };
    if (!begin_transaction(deadline, error)) { g_free(text); return FALSE; }
    gboolean success = exchange_packet(socket_path, WRITE_COMMAND, text, request_length,
                                        deadline, &reply, error);
    g_free(text);
    if (success) success = reply_succeeded(&reply, error);
    if (!success) close_connection();
    g_free(reply.bytes);
    end_transaction();
    return success;
}
