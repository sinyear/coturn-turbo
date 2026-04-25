#include "turbo_json.h"
#include <string.h>
#include <arpa/inet.h>

int32_t turbo_json_get_int(struct json_object *obj, const char *key, int32_t def) {
    if (!obj) return def;
    struct json_object *val = json_object_object_get(obj, key);
    if (!val) return def;
    return (int32_t)json_object_get_int(val);
}

const char* turbo_json_get_string(struct json_object *obj, const char *key, const char *def) {
    if (!obj) return def;
    struct json_object *val = json_object_object_get(obj, key);
    if (!val) return def;
    const char *s = json_object_get_string(val);
    return s ? s : def;
}

struct json_object* turbo_json_response(const char *status) {
    struct json_object *obj = json_object_new_object();
    if (obj) {
        json_object_object_add(obj, "status", json_object_new_string(status));
    }
    return obj;
}

struct json_object* turbo_json_error(int code, const char *msg) {
    struct json_object *obj = json_object_new_object();
    if (obj) {
        json_object_object_add(obj, "status", json_object_new_string("error"));
        json_object_object_add(obj, "code", json_object_new_int(code));
        json_object_object_add(obj, "message", json_object_new_string(msg ? msg : "unknown error"));
    }
    return obj;
}

struct json_object* turbo_json_addr(const struct sockaddr_in *addr) {
    if (!addr) return NULL;

    struct json_object *obj = json_object_new_object();
    if (!obj) return NULL;

    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr->sin_addr, ip, sizeof(ip));

    json_object_object_add(obj, "ip", json_object_new_string(ip));
    json_object_object_add(obj, "port", json_object_new_int(ntohs(addr->sin_port)));

    return obj;
}

int turbo_json_parse_addr(struct json_object *obj, struct sockaddr_in *addr) {
    if (!obj || !addr) return -1;

    const char *ip = turbo_json_get_string(obj, "ip", NULL);
    int port = turbo_json_get_int(obj, "port", -1);

    if (!ip || port < 0 || port > 65535) {
        return -1;
    }

    memset(addr, 0, sizeof(*addr));
    addr->sin_family = AF_INET;
    addr->sin_port = htons((uint16_t)port);

    if (inet_pton(AF_INET, ip, &addr->sin_addr) != 1) {
        return -1;
    }

    return 0;
}
