/* pcapload - minimal JSON parser (objects/arrays/strings/numbers/bools/null) */
#ifndef PCAPLOAD_JSON_H
#define PCAPLOAD_JSON_H

typedef enum {
    JSON_NULL,
    JSON_BOOL,
    JSON_NUM,
    JSON_STR,
    JSON_ARR,
    JSON_OBJ
} JType;

typedef struct JVal JVal;
struct JVal {
    JType t;
    int   b;          /* JSON_BOOL */
    double num;       /* JSON_NUM */
    char*  s;         /* JSON_STR */
    char** keys;      /* JSON_OBJ: key per entry (NULL for arrays) */
    JVal** items;     /* JSON_OBJ/JSON_ARR: values */
    int n, cap;
};

JVal* json_parse(const char* text);
void  json_free(JVal* v);
JVal* json_get(JVal* obj, const char* key);
/* Render any scalar (string/number/bool) as a string into buf. Returns buf. */
const char* json_scalar_str(JVal* v, char* buf, int bufsize);

#endif
