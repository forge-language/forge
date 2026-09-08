#ifndef FORGE_JSON_H
#define FORGE_JSON_H

#include <stdint.h>

const char *fr_json_get_string(const char *json, const char *key);
int64_t fr_json_get_int(const char *json, const char *key);
char *fr_json_stringify_str(const char *key, const char *value);
char *fr_json_stringify_int(const char *key, int64_t value);

const char *fr_json_get_path_raw(const char *json, const char *path);
const char *fr_json_get_path_string(const char *json, const char *path);
int64_t fr_json_get_path_int(const char *json, const char *path);
int64_t fr_json_array_len(const char *json_array);
const char *fr_json_array_item(const char *json_array, int64_t index);

#endif
