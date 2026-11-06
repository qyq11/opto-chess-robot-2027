#include <stdio.h>
#include "../rkaiq_client.h"

/* 只读取既有图像处理服务，不启动引擎、不改变曝光或白平衡。 */
int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s /uapi/0/ae_uapi/expSwAttr\n", argv[0]);
        return 2;
    }
    JsonNode *value = NULL;
    GError *error = NULL;
    if (!rkaiq_client_get(RKAIQ_CLIENT_DEFAULT_SOCKET, argv[1], 5000, &value, &error)) {
        fprintf(stderr, "读取失败：%s\n", error ? error->message : "未知错误");
        g_clear_error(&error);
        return 1;
    }
    JsonGenerator *generator = json_generator_new();
    json_generator_set_root(generator, value);
    json_generator_set_pretty(generator, TRUE);
    char *text = json_generator_to_data(generator, NULL);
    puts(text);
    g_free(text);
    g_object_unref(generator);
    json_node_unref(value);
    return 0;
}
