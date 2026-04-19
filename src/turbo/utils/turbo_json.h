#ifndef TURBO_JSON_H
#define TURBO_JSON_H

#include <json-c/json.h>
#include <stdint.h>
#include <netinet/in.h>

/**
 * Helper: get integer from JSON object with default value
 * @param obj JSON object
 * @param key Key name
 * @param def Default value if key not found
 * @return Integer value
 */
int32_t turbo_json_get_int(struct json_object *obj, const char *key, int32_t def);

/**
 * Helper: get string from JSON object with default value
 * @param obj JSON object
 * @param key Key name
 * @param def Default value if key not found
 * @return String pointer (never NULL)
 */
const char* turbo_json_get_string(struct json_object *obj, const char *key, const char *def);

/**
 * Helper: create JSON response object with status
 * @param status "ok" or "error"
 * @return New JSON object (caller must json_object_put)
 */
struct json_object* turbo_json_response(const char *status);

/**
 * Helper: create JSON error response
 * @param code HTTP status code
 * @param msg Error message
 * @return New JSON object (caller must json_object_put)
 */
struct json_object* turbo_json_error(int code, const char *msg);

/**
 * Helper: serialize sockaddr_in to JSON object
 * @param addr Socket address
 * @return New JSON object (caller must json_object_put)
 */
struct json_object* turbo_json_addr(const struct sockaddr_in *addr);

/**
 * Helper: parse sockaddr_in from JSON object
 * @param obj JSON object with "ip" and "port" fields
 * @param addr Output address
 * @return 0 on success, -1 on failure
 */
int turbo_json_parse_addr(struct json_object *obj, struct sockaddr_in *addr);

#endif /* TURBO_JSON_H */
