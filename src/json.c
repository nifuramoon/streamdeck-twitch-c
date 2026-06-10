#include "json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

static json_val *parse_value(const char **p);

static inline int is_ws(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static void skip_ws(const char **p) {
    while (**p && is_ws(**p)) (*p)++;
}

static char *parse_str_raw(const char **p) {
    skip_ws(p);
    if (**p != '"') return NULL;
    (*p)++;
    size_t cap = 256, len = 0;
    char *s = malloc(cap);
    while (**p && **p != '"') {
        if (**p == '\\') {
            (*p)++;
            if (!**p) break;
            switch (**p) {
                case '"': s[len++] = '"'; break;
                case '\\': s[len++] = '\\'; break;
                case '/': s[len++] = '/'; break;
                case 'n': s[len++] = '\n'; break;
                case 't': s[len++] = '\t'; break;
                case 'r': s[len++] = '\r'; break;
                default: s[len++] = **p; break;
            }
        } else {
            s[len++] = **p;
        }
        if (len >= cap - 1) {
            cap *= 2;
            s = realloc(s, cap);
        }
        (*p)++;
    }
    if (**p == '"') (*p)++;
    s[len] = '\0';
    return s;
}

static json_val *parse_num(const char **p) {
    skip_ws(p);
    char *end;
    double n = strtod(*p, &end);
    if (end == *p) return NULL;
    *p = end;
    json_val *v = calloc(1, sizeof(json_val));
    v->type = JSON_NUM;
    v->num = n;
    return v;
}

static json_val *parse_str(const char **p) {
    char *s = parse_str_raw(p);
    if (!s) return NULL;
    json_val *v = calloc(1, sizeof(json_val));
    v->type = JSON_STR;
    v->str = s;
    return v;
}

static json_val *parse_arr(const char **p) {
    skip_ws(p);
    if (**p != '[') return NULL;
    (*p)++;
    json_val *v = calloc(1, sizeof(json_val));
    v->type = JSON_ARR;
    size_t cap = 64;
    v->arr.items = malloc(sizeof(json_val) * cap);
    v->arr.len = 0;
    skip_ws(p);
    if (**p != ']') {
        while (1) {
            json_val *item = parse_value(p);
            if (!item) { json_free(v); return NULL; }
            if (v->arr.len >= cap) {
                cap *= 2;
                v->arr.items = realloc(v->arr.items, sizeof(json_val) * cap);
            }
            v->arr.items[v->arr.len++] = *item;
            free(item);
            skip_ws(p);
            if (**p == ',') { (*p)++; skip_ws(p); }
            else break;
        }
    }
    if (**p == ']') (*p)++;
    else { json_free(v); return NULL; }
    return v;
}

static json_val *parse_obj(const char **p) {
    skip_ws(p);
    if (**p != '{') return NULL;
    (*p)++;
    json_val *v = calloc(1, sizeof(json_val));
    v->type = JSON_OBJ;
    size_t cap = 64;
    v->obj.pairs = malloc(sizeof(json_pair) * cap);
    v->obj.len = 0;
    skip_ws(p);
    if (**p != '}') {
        while (1) {
            char *key = parse_str_raw(p);
            if (!key) { json_free(v); return NULL; }
            skip_ws(p);
            if (**p != ':') { free(key); json_free(v); return NULL; }
            (*p)++;
            json_val *val = parse_value(p);
            if (!val) { free(key); json_free(v); return NULL; }
            if (v->obj.len >= cap) {
                cap *= 2;
                v->obj.pairs = realloc(v->obj.pairs, sizeof(json_pair) * cap);
            }
            v->obj.pairs[v->obj.len].key = key;
            v->obj.pairs[v->obj.len].val = *val;
            free(val);
            v->obj.len++;
            skip_ws(p);
            if (**p == ',') { (*p)++; skip_ws(p); }
            else break;
        }
    }
    if (**p == '}') (*p)++;
    else { json_free(v); return NULL; }
    return v;
}

static json_val *parse_value(const char **p) {
    skip_ws(p);
    switch (**p) {
        case '"': return parse_str(p);
        case '{': return parse_obj(p);
        case '[': return parse_arr(p);
        case 't': if (memcmp(*p, "true", 4) == 0) { *p += 4; json_val *v = calloc(1, sizeof(json_val)); v->type = JSON_BOOL; v->b = true; return v; } return NULL;
        case 'f': if (memcmp(*p, "false", 5) == 0) { *p += 5; json_val *v = calloc(1, sizeof(json_val)); v->type = JSON_BOOL; v->b = false; return v; } return NULL;
        case 'n': if (memcmp(*p, "null", 4) == 0) { *p += 4; json_val *v = calloc(1, sizeof(json_val)); v->type = JSON_NULL; return v; } return NULL;
        default:  return parse_num(p);
    }
}

json_val *json_parse(const char *input) {
    if (!input) return NULL;
    const char *p = input;
    return parse_value(&p);
}

static json_val *json_clone_val(json_val *v) {
    if (!v) return NULL;
    json_val *c = calloc(1, sizeof(json_val));
    c->type = v->type;
    switch (v->type) {
        case JSON_NULL: break;
        case JSON_BOOL: c->b = v->b; break;
        case JSON_NUM:  c->num = v->num; break;
        case JSON_STR:  c->str = strdup(v->str); break;
        case JSON_ARR:
            c->arr.len = v->arr.len;
            c->arr.items = calloc(c->arr.len, sizeof(json_val));
            for (size_t i = 0; i < c->arr.len; i++)
                c->arr.items[i] = *json_clone_val(&v->arr.items[i]);
            break;
        case JSON_OBJ:
            c->obj.len = v->obj.len;
            c->obj.pairs = calloc(c->obj.len, sizeof(json_pair));
            for (size_t i = 0; i < c->obj.len; i++) {
                c->obj.pairs[i].key = strdup(v->obj.pairs[i].key);
                c->obj.pairs[i].val = *json_clone_val(&v->obj.pairs[i].val);
            }
            break;
    }
    return c;
}

json_val *json_clone(json_val *v) {
    return json_clone_val(v);
}

static void json_free_internal(json_val *v) {
    if (!v) return;
    switch (v->type) {
        case JSON_STR: free(v->str); break;
        case JSON_ARR:
            for (size_t i = 0; i < v->arr.len; i++)
                json_free_internal(&v->arr.items[i]);
            free(v->arr.items);
            break;
        case JSON_OBJ:
            for (size_t i = 0; i < v->obj.len; i++) {
                free(v->obj.pairs[i].key);
                json_free_internal(&v->obj.pairs[i].val);
            }
            free(v->obj.pairs);
            break;
        default: break;
    }
}

void json_free(json_val *v) {
    json_free_internal(v);
    free(v);
}

json_val *json_obj_get(json_val *obj, const char *key) {
    if (!obj || obj->type != JSON_OBJ) return NULL;
    for (size_t i = 0; i < obj->obj.len; i++) {
        if (strcmp(obj->obj.pairs[i].key, key) == 0)
            return &obj->obj.pairs[i].val;
    }
    return NULL;
}

json_val *json_arr_get(json_val *arr, size_t idx) {
    if (!arr || arr->type != JSON_ARR || idx >= arr->arr.len) return NULL;
    return &arr->arr.items[idx];
}

static void json_serialize_val(json_val *v, char **buf, size_t *len, size_t *cap) {
    if (*len + 1 >= *cap) { *cap *= 2; *buf = realloc(*buf, *cap); }
    switch (v->type) {
        case JSON_NULL:
            *len += snprintf(*buf + *len, *cap - *len, "null");
            break;
        case JSON_BOOL:
            *len += snprintf(*buf + *len, *cap - *len, v->b ? "true" : "false");
            break;
        case JSON_NUM: {
            if (v->num == (int64_t)v->num)
                *len += snprintf(*buf + *len, *cap - *len, "%lld", (long long)v->num);
            else
                *len += snprintf(*buf + *len, *cap - *len, "%g", v->num);
            break;
        }
        case JSON_STR: {
            (*buf)[*len] = '"'; (*len)++;
            for (char *c = v->str; *c; c++) {
                if (*len + 2 >= *cap) { *cap *= 2; *buf = realloc(*buf, *cap); }
                switch (*c) {
                    case '"': (*buf)[(*len)++] = '\\'; (*buf)[(*len)++] = '"'; break;
                    case '\\': (*buf)[(*len)++] = '\\'; (*buf)[(*len)++] = '\\'; break;
                    case '\n': (*buf)[(*len)++] = '\\'; (*buf)[(*len)++] = 'n'; break;
                    case '\t': (*buf)[(*len)++] = '\\'; (*buf)[(*len)++] = 't'; break;
                    default: (*buf)[(*len)++] = *c; break;
                }
            }
            if (*len + 1 >= *cap) { *cap *= 2; *buf = realloc(*buf, *cap); }
            (*buf)[(*len)++] = '"';
            break;
        }
        case JSON_ARR: {
            (*buf)[(*len)++] = '[';
            for (size_t i = 0; i < v->arr.len; i++) {
                if (i > 0) { (*buf)[(*len)++] = ','; }
                json_serialize_val(&v->arr.items[i], buf, len, cap);
            }
            if (*len + 1 >= *cap) { *cap *= 2; *buf = realloc(*buf, *cap); }
            (*buf)[(*len)++] = ']';
            break;
        }
        case JSON_OBJ: {
            (*buf)[(*len)++] = '{';
            for (size_t i = 0; i < v->obj.len; i++) {
                if (i > 0) { (*buf)[(*len)++] = ','; }
                json_val key_wrapper = { .type = JSON_STR, .str = v->obj.pairs[i].key };
                json_serialize_val(&key_wrapper, buf, len, cap);
                (*buf)[(*len)++] = ':';
                json_serialize_val(&v->obj.pairs[i].val, buf, len, cap);
            }
            if (*len + 1 >= *cap) { *cap *= 2; *buf = realloc(*buf, *cap); }
            (*buf)[(*len)++] = '}';
            break;
        }
    }
    if (*len < *cap) (*buf)[*len] = '\0';
}

char *json_serialize(json_val *v) {
    size_t cap = 1024, len = 0;
    char *buf = malloc(cap);
    buf[0] = '\0';
    json_serialize_val(v, &buf, &len, &cap);
    return buf;
}