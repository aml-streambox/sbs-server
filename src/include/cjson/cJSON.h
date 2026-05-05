#ifndef SBS_CJSON_COMPAT_H
#define SBS_CJSON_COMPAT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cJSON {
    struct cJSON *next;
    struct cJSON *prev;
    struct cJSON *child;
    int type;
    char *valuestring;
    double valuedouble;
    int valueint;
    char *string;
} cJSON;

enum {
    cJSON_False = 1,
    cJSON_True = 2,
    cJSON_NULL = 4,
    cJSON_Number = 8,
    cJSON_String = 16,
    cJSON_Array = 32,
    cJSON_Object = 64,
};

cJSON *cJSON_Parse(const char *text);
void cJSON_Delete(cJSON *item);
char *cJSON_PrintUnformatted(const cJSON *item);

cJSON *cJSON_CreateObject(void);
cJSON *cJSON_CreateArray(void);
cJSON *cJSON_CreateString(const char *string);
cJSON *cJSON_CreateNumber(double number);
cJSON *cJSON_CreateBool(int boolean_value);
cJSON *cJSON_CreateNull(void);

void cJSON_AddItemToObject(cJSON *object, const char *string, cJSON *item);
void cJSON_AddItemToArray(cJSON *array, cJSON *item);
void cJSON_AddStringToObject(cJSON *object, const char *name, const char *value);
void cJSON_AddNumberToObject(cJSON *object, const char *name, double value);
void cJSON_AddBoolToObject(cJSON *object, const char *name, int boolean_value);
void cJSON_AddNullToObject(cJSON *object, const char *name);

cJSON *cJSON_GetObjectItemCaseSensitive(const cJSON *object, const char *string);
int cJSON_GetArraySize(const cJSON *array);
cJSON *cJSON_GetArrayItem(const cJSON *array, int index);

const char *cJSON_GetStringValue(const cJSON *item);
int cJSON_IsString(const cJSON *item);
int cJSON_IsNumber(const cJSON *item);
int cJSON_IsBool(const cJSON *item);
int cJSON_IsTrue(const cJSON *item);
int cJSON_IsFalse(const cJSON *item);
int cJSON_IsArray(const cJSON *item);
int cJSON_IsObject(const cJSON *item);
int cJSON_IsNull(const cJSON *item);

#ifdef __cplusplus
}
#endif

#endif
