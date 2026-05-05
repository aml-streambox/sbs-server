#include "cjson/cJSON.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} sb_t;

static cJSON *item_new(int type)
{
    cJSON *item = calloc(1, sizeof(*item));
    if (item) {
        item->type = type;
    }
    return item;
}

static void item_append(cJSON *parent, cJSON *item)
{
    cJSON *child;
    if (!parent || !item) {
        return;
    }
    if (!parent->child) {
        parent->child = item;
        return;
    }
    child = parent->child;
    while (child->next) {
        child = child->next;
    }
    child->next = item;
    item->prev = child;
}

static const char *skip_ws(const char *s)
{
    while (s && *s && isspace((unsigned char)*s)) {
        s++;
    }
    return s;
}

static char *parse_string_token(const char **sp)
{
    const char *s = *sp;
    char *out;
    char *dst;

    if (!s || *s != '"') {
        return NULL;
    }
    s++;
    out = malloc(strlen(s) + 1);
    if (!out) {
        return NULL;
    }
    dst = out;
    while (*s && *s != '"') {
        if (*s == '\\') {
            s++;
            if (!*s) {
                free(out);
                return NULL;
            }
            switch (*s) {
            case '"': *dst++ = '"'; break;
            case '\\': *dst++ = '\\'; break;
            case '/': *dst++ = '/'; break;
            case 'b': *dst++ = '\b'; break;
            case 'f': *dst++ = '\f'; break;
            case 'n': *dst++ = '\n'; break;
            case 'r': *dst++ = '\r'; break;
            case 't': *dst++ = '\t'; break;
            default: *dst++ = *s; break;
            }
            s++;
            continue;
        }
        *dst++ = *s++;
    }
    if (*s != '"') {
        free(out);
        return NULL;
    }
    *dst = '\0';
    *sp = s + 1;
    return out;
}

static cJSON *parse_value(const char **sp);

static cJSON *parse_array(const char **sp)
{
    cJSON *array = cJSON_CreateArray();
    const char *s = *sp;

    s = skip_ws(s);
    if (*s != '[') {
        cJSON_Delete(array);
        return NULL;
    }
    s = skip_ws(s + 1);
    if (*s == ']') {
        *sp = s + 1;
        return array;
    }
    while (*s) {
        cJSON *item = parse_value(&s);
        if (!item) {
            cJSON_Delete(array);
            return NULL;
        }
        cJSON_AddItemToArray(array, item);
        s = skip_ws(s);
        if (*s == ']') {
            *sp = s + 1;
            return array;
        }
        if (*s != ',') {
            cJSON_Delete(array);
            return NULL;
        }
        s = skip_ws(s + 1);
    }
    cJSON_Delete(array);
    return NULL;
}

static cJSON *parse_object(const char **sp)
{
    cJSON *object = cJSON_CreateObject();
    const char *s = *sp;

    s = skip_ws(s);
    if (*s != '{') {
        cJSON_Delete(object);
        return NULL;
    }
    s = skip_ws(s + 1);
    if (*s == '}') {
        *sp = s + 1;
        return object;
    }
    while (*s) {
        char *key = parse_string_token(&s);
        cJSON *value;
        if (!key) {
            cJSON_Delete(object);
            return NULL;
        }
        s = skip_ws(s);
        if (*s != ':') {
            free(key);
            cJSON_Delete(object);
            return NULL;
        }
        s = skip_ws(s + 1);
        value = parse_value(&s);
        if (!value) {
            free(key);
            cJSON_Delete(object);
            return NULL;
        }
        cJSON_AddItemToObject(object, key, value);
        free(key);
        s = skip_ws(s);
        if (*s == '}') {
            *sp = s + 1;
            return object;
        }
        if (*s != ',') {
            cJSON_Delete(object);
            return NULL;
        }
        s = skip_ws(s + 1);
    }
    cJSON_Delete(object);
    return NULL;
}

static cJSON *parse_value(const char **sp)
{
    const char *s = skip_ws(*sp);
    cJSON *item;

    if (!s) {
        return NULL;
    }
    if (*s == '{') {
        item = parse_object(&s);
    } else if (*s == '[') {
        item = parse_array(&s);
    } else if (*s == '"') {
        char *str = parse_string_token(&s);
        if (!str) {
            return NULL;
        }
        item = cJSON_CreateString(str);
        free(str);
    } else if (!strncmp(s, "true", 4)) {
        item = cJSON_CreateBool(1);
        s += 4;
    } else if (!strncmp(s, "false", 5)) {
        item = cJSON_CreateBool(0);
        s += 5;
    } else if (!strncmp(s, "null", 4)) {
        item = cJSON_CreateNull();
        s += 4;
    } else {
        char *end = NULL;
        double num = strtod(s, &end);
        if (end == s) {
            return NULL;
        }
        item = cJSON_CreateNumber(num);
        s = end;
    }

    *sp = s;
    return item;
}

static void sb_reserve(sb_t *sb, size_t extra)
{
    size_t needed = sb->len + extra + 1;
    if (needed <= sb->cap) {
        return;
    }
    while (sb->cap < needed) {
        sb->cap = sb->cap ? sb->cap * 2 : 128;
    }
    sb->buf = realloc(sb->buf, sb->cap);
}

static void sb_append(sb_t *sb, const char *text)
{
    size_t n = strlen(text);
    sb_reserve(sb, n);
    memcpy(sb->buf + sb->len, text, n);
    sb->len += n;
    sb->buf[sb->len] = '\0';
}

static void sb_append_char(sb_t *sb, char ch)
{
    sb_reserve(sb, 1);
    sb->buf[sb->len++] = ch;
    sb->buf[sb->len] = '\0';
}

static void print_string(sb_t *sb, const char *value)
{
    const char *s = value ? value : "";
    sb_append_char(sb, '"');
    while (*s) {
        switch (*s) {
        case '"': sb_append(sb, "\\\""); break;
        case '\\': sb_append(sb, "\\\\"); break;
        case '\n': sb_append(sb, "\\n"); break;
        case '\r': sb_append(sb, "\\r"); break;
        case '\t': sb_append(sb, "\\t"); break;
        default: sb_append_char(sb, *s); break;
        }
        s++;
    }
    sb_append_char(sb, '"');
}

static void print_item(sb_t *sb, const cJSON *item)
{
    const cJSON *child;
    char numbuf[64];

    if (!item) {
        sb_append(sb, "null");
        return;
    }

    switch (item->type) {
    case cJSON_NULL:
        sb_append(sb, "null");
        break;
    case cJSON_False:
        sb_append(sb, "false");
        break;
    case cJSON_True:
        sb_append(sb, "true");
        break;
    case cJSON_Number:
        snprintf(numbuf, sizeof(numbuf), "%.17g", item->valuedouble);
        sb_append(sb, numbuf);
        break;
    case cJSON_String:
        print_string(sb, item->valuestring);
        break;
    case cJSON_Array:
        sb_append_char(sb, '[');
        child = item->child;
        while (child) {
            print_item(sb, child);
            child = child->next;
            if (child) {
                sb_append_char(sb, ',');
            }
        }
        sb_append_char(sb, ']');
        break;
    case cJSON_Object:
        sb_append_char(sb, '{');
        child = item->child;
        while (child) {
            print_string(sb, child->string ? child->string : "");
            sb_append_char(sb, ':');
            print_item(sb, child);
            child = child->next;
            if (child) {
                sb_append_char(sb, ',');
            }
        }
        sb_append_char(sb, '}');
        break;
    default:
        sb_append(sb, "null");
        break;
    }
}

cJSON *cJSON_Parse(const char *text)
{
    const char *s = text;
    cJSON *item;
    if (!text) {
        return NULL;
    }
    item = parse_value(&s);
    if (!item) {
        return NULL;
    }
    s = skip_ws(s);
    if (*s != '\0') {
        cJSON_Delete(item);
        return NULL;
    }
    return item;
}

void cJSON_Delete(cJSON *item)
{
    cJSON *next;
    while (item) {
        next = item->next;
        cJSON_Delete(item->child);
        free(item->valuestring);
        free(item->string);
        free(item);
        item = next;
    }
}

char *cJSON_PrintUnformatted(const cJSON *item)
{
    sb_t sb = {0};
    print_item(&sb, item);
    return sb.buf ? sb.buf : strdup("null");
}

cJSON *cJSON_CreateObject(void) { return item_new(cJSON_Object); }
cJSON *cJSON_CreateArray(void) { return item_new(cJSON_Array); }
cJSON *cJSON_CreateString(const char *string)
{
    cJSON *item = item_new(cJSON_String);
    if (item) {
        item->valuestring = strdup(string ? string : "");
    }
    return item;
}

cJSON *cJSON_CreateNumber(double number)
{
    cJSON *item = item_new(cJSON_Number);
    if (item) {
        item->valuedouble = number;
        item->valueint = (int)number;
    }
    return item;
}

cJSON *cJSON_CreateBool(int boolean_value)
{
    return item_new(boolean_value ? cJSON_True : cJSON_False);
}

cJSON *cJSON_CreateNull(void)
{
    return item_new(cJSON_NULL);
}

void cJSON_AddItemToObject(cJSON *object, const char *string, cJSON *item)
{
    if (!object || !item || object->type != cJSON_Object) {
        return;
    }
    item->string = strdup(string ? string : "");
    item_append(object, item);
}

void cJSON_AddItemToArray(cJSON *array, cJSON *item)
{
    if (!array || !item || array->type != cJSON_Array) {
        return;
    }
    item_append(array, item);
}

void cJSON_AddStringToObject(cJSON *object, const char *name, const char *value)
{
    cJSON_AddItemToObject(object, name, cJSON_CreateString(value));
}

void cJSON_AddNumberToObject(cJSON *object, const char *name, double value)
{
    cJSON_AddItemToObject(object, name, cJSON_CreateNumber(value));
}

void cJSON_AddBoolToObject(cJSON *object, const char *name, int boolean_value)
{
    cJSON_AddItemToObject(object, name, cJSON_CreateBool(boolean_value));
}

void cJSON_AddNullToObject(cJSON *object, const char *name)
{
    cJSON_AddItemToObject(object, name, cJSON_CreateNull());
}

cJSON *cJSON_GetObjectItemCaseSensitive(const cJSON *object, const char *string)
{
    cJSON *child;
    if (!object || object->type != cJSON_Object) {
        return NULL;
    }
    child = object->child;
    while (child) {
        if (child->string && strcmp(child->string, string) == 0) {
            return child;
        }
        child = child->next;
    }
    return NULL;
}

int cJSON_GetArraySize(const cJSON *array)
{
    int count = 0;
    cJSON *child;
    if (!array || array->type != cJSON_Array) {
        return 0;
    }
    child = array->child;
    while (child) {
        count++;
        child = child->next;
    }
    return count;
}

cJSON *cJSON_GetArrayItem(const cJSON *array, int index)
{
    cJSON *child;
    int i = 0;
    if (!array || array->type != cJSON_Array || index < 0) {
        return NULL;
    }
    child = array->child;
    while (child) {
        if (i == index) {
            return child;
        }
        i++;
        child = child->next;
    }
    return NULL;
}

const char *cJSON_GetStringValue(const cJSON *item)
{
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

int cJSON_IsString(const cJSON *item) { return item && item->type == cJSON_String; }
int cJSON_IsNumber(const cJSON *item) { return item && item->type == cJSON_Number; }
int cJSON_IsBool(const cJSON *item) { return item && (item->type == cJSON_True || item->type == cJSON_False); }
int cJSON_IsTrue(const cJSON *item) { return item && item->type == cJSON_True; }
int cJSON_IsFalse(const cJSON *item) { return item && item->type == cJSON_False; }
int cJSON_IsArray(const cJSON *item) { return item && item->type == cJSON_Array; }
int cJSON_IsObject(const cJSON *item) { return item && item->type == cJSON_Object; }
int cJSON_IsNull(const cJSON *item) { return item && item->type == cJSON_NULL; }
