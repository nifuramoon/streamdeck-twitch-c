#ifndef JSON_H
#define JSON_H

#include "common.h"

typedef enum {
    JSON_NULL, JSON_BOOL, JSON_NUM, JSON_STR, JSON_ARR, JSON_OBJ
} json_type;

typedef struct json_val {
    json_type type;
    union {
        bool b;
        double num;
        char *str;
        struct { struct json_val *items; size_t len; } arr;
        struct { struct json_pair *pairs; size_t len; } obj;
    };
} json_val;

typedef struct json_pair {
    char *key;
    json_val val;
} json_pair;

json_val *json_parse(const char *input);
json_val *json_clone(json_val *v);
void json_free(json_val *v);
json_val *json_obj_get(json_val *obj, const char *key);
json_val *json_arr_get(json_val *arr, size_t idx);
char *json_serialize(json_val *v);

#endif
