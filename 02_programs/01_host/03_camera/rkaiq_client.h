#ifndef OPTO_RKAIQ_CLIENT_H
#define OPTO_RKAIQ_CLIENT_H

#include <json-glib/json-glib.h>

G_BEGIN_DECLS

#define RKAIQ_CLIENT_ERROR (rkaiq_client_error_quark())
#define RKAIQ_CLIENT_DEFAULT_SOCKET "/tmp/UNIX.domain0"
#define RKAIQ_CLIENT_DEFAULT_TIMEOUT_MS 5000U

typedef enum {
    RKAIQ_CLIENT_ERROR_ARGUMENT,
    RKAIQ_CLIENT_ERROR_IO,
    RKAIQ_CLIENT_ERROR_TIMEOUT,
    RKAIQ_CLIENT_ERROR_PROTOCOL,
    RKAIQ_CLIENT_ERROR_REMOTE,
    RKAIQ_CLIENT_ERROR_JSON
} RkaiqClientError;

GQuark rkaiq_client_error_quark(void);

/*
 * 通过已有3A服务的本地socket读取一个 /uapi/0/ 路径，不创建图像处理引擎。
 * 2026-11-03已在板B 6.9.0运行此C模块，核对曝光/白平衡的读、写及读回。
 * 更换板卡或图像处理库版本后，应重新核对通信布局和参数字段。
 *
 * socket_path须是当前进程能访问的绝对路径；服务使用独立/tmp时要先核对可见性。
 * timeout_ms为1～60000毫秒，等待串行请求、连接和完整收发共用这一期限。
 * 函数同步等待，不应在采集线程、共享图像锁内或网页主循环中调用。
 * 模块在进程内串行处理事务，并持续复用同一路径的连接；失败立即关闭，
 * 不自动重发请求，由下一次显式调用重新连接。首次调用建议至少使用默认5秒期限，
 * 因为现有服务在等待新连接时有3秒盲等。控制层仍需串行保护GET→SET→GET整个流程。
 *
 * 成功时，*value是响应中value节点的独立持有引用，调用方用json_node_unref释放；
 * 失败时*value为NULL，error说明失败阶段。传入的*error须为NULL。
 * 依赖json-glib-1.0（板端已核实1.8.0），构建由主程序统一接入。
 */
gboolean rkaiq_client_get(const gchar *socket_path,
                         const gchar *uapi_path,
                         guint timeout_ms,
                         JsonNode **value,
                         GError **error);

/*
 * 向同一路径发送JSON Patch replace；value为借用节点，调用期间须保持不变，
 * 模块在发送前深拷贝，不修改调用方的JSON。读取并核对现有C工具服务的真实ACK，
 * 包括正文长度为0的156字节回复。成功ACK仍不等于参数生效；适配层必须GET原配置、
 * 局部修改、SET、再GET核实，不能把发送成功或等待超时当作已生效。
 * 超时、串行处理和错误规则与GET一致；本函数不会自行重试、重启服务或创建引擎。
 */
gboolean rkaiq_client_set(const gchar *socket_path,
                         const gchar *uapi_path,
                         JsonNode *value,
                         guint timeout_ms,
                         GError **error);

G_END_DECLS

#endif
