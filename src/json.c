#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "json.h"

typedef struct { const char* p; int depth; } JP;

#define JSON_MAX_DEPTH 64

static void jskip(JP* j)
{
    while (*j->p == ' ' || *j->p == '\t' || *j->p == '\r' || *j->p == '\n')
        j->p++;
}

static JVal* jnew(JType t)
{
    JVal* v = (JVal*)calloc(1, sizeof(JVal));
    if (v) v->t = t;
    return v;
}

void json_free(JVal* v)
{
    int i;
    if (!v) return;
    if (v->s) free(v->s);
    if (v->keys) {
        for (i = 0; i < v->n; i++)
            if (v->keys[i]) free(v->keys[i]);
        free(v->keys);
    }
    if (v->items) {
        for (i = 0; i < v->n; i++)
            json_free(v->items[i]);
        free(v->items);
    }
    free(v);
}

static int jadd(JVal* c, char* key, JVal* val)
{
    if (c->n >= c->cap) {
        int nc = c->cap ? c->cap * 2 : 8;
        char** nk = (char**)realloc(c->keys, nc * sizeof(char*));
        JVal** nv = (JVal**)realloc(c->items, nc * sizeof(JVal*));
        if (nk) c->keys = nk;
        if (nv) c->items = nv;
        if (!nk || !nv) return 0;
        c->cap = nc;
    }
    c->keys[c->n] = key;
    c->items[c->n] = val;
    c->n++;
    return 1;
}

static char* jparse_string_raw(JP* j)
{
    size_t cap = 32, len = 0;
    char* out;
    if (*j->p != '"') return NULL;
    out = (char*)malloc(cap);
    if (!out) return NULL;
    j->p++;
    while (*j->p && *j->p != '"') {
        char c = *j->p++;
        if (len + 8 > cap) {
            char* no;
            cap *= 2;
            no = (char*)realloc(out, cap);
            if (!no) { free(out); return NULL; }
            out = no;
        }
        if (c == '\\') {
            char e = *j->p++;
            switch (e) {
            case 'n': out[len++] = '\n'; break;
            case 't': out[len++] = '\t'; break;
            case 'r': out[len++] = '\r'; break;
            case 'b': out[len++] = '\b'; break;
            case 'f': out[len++] = '\f'; break;
            case 'u':
                /* decode one \uXXXX escape as UTF-8 (BMP only) */
                {
                    unsigned cp = 0;
                    int k, d = 0;
                    for (k = 0; k < 4; k++) {
                        char h = *j->p;
                        if (h >= '0' && h <= '9') d = h - '0';
                        else if (h >= 'a' && h <= 'f') d = h - 'a' + 10;
                        else if (h >= 'A' && h <= 'F') d = h - 'A' + 10;
                        else break;
                        cp = (cp << 4) | (unsigned)d;
                        j->p++;
                    }
                    if (k < 4) {
                        out[len++] = '?';           /* truncated escape */
                    } else if (cp < 0x80) {
                        out[len++] = (char)cp;
                    } else if (cp < 0x800) {
                        out[len++] = (char)(0xC0 | (cp >> 6));
                        out[len++] = (char)(0x80 | (cp & 0x3F));
                    } else if (cp >= 0xD800 && cp <= 0xDFFF) {
                        out[len++] = '?';           /* no surrogate pairs */
                    } else {
                        out[len++] = (char)(0xE0 | (cp >> 12));
                        out[len++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                        out[len++] = (char)(0x80 | (cp & 0x3F));
                    }
                }
                break;
            default: out[len++] = e; break; /* \" \\ \/ */
            }
        } else {
            out[len++] = c;
        }
    }
    if (*j->p == '"') j->p++;
    out[len] = 0;
    return out;
}

static JVal* jparse_value_inner(JP* j);

static JVal* jparse_value(JP* j)
{
    JVal* v;
    if (j->depth >= JSON_MAX_DEPTH) return NULL;  /* nested too deeply */
    j->depth++;
    v = jparse_value_inner(j);
    j->depth--;
    return v;
}

static JVal* jparse_object(JP* j)
{
    JVal* o = jnew(JSON_OBJ);
    if (!o) return NULL;
    j->p++; /* { */
    jskip(j);
    if (*j->p == '}') { j->p++; return o; }
    for (;;) {
        char* key;
        JVal* val;
        jskip(j);
        key = jparse_string_raw(j);
        if (!key) { json_free(o); return NULL; }
        jskip(j);
        if (*j->p != ':') { free(key); json_free(o); return NULL; }
        j->p++;
        val = jparse_value(j);
        if (!val) { free(key); json_free(o); return NULL; }
        if (!jadd(o, key, val)) { free(key); json_free(val); json_free(o); return NULL; }
        jskip(j);
        if (*j->p == ',') { j->p++; continue; }
        if (*j->p == '}') { j->p++; return o; }
        json_free(o);
        return NULL;
    }
}

static JVal* jparse_array(JP* j)
{
    JVal* a = jnew(JSON_ARR);
    if (!a) return NULL;
    j->p++; /* [ */
    jskip(j);
    if (*j->p == ']') { j->p++; return a; }
    for (;;) {
        JVal* val = jparse_value(j);
        if (!val) { json_free(a); return NULL; }
        if (!jadd(a, NULL, val)) { json_free(val); json_free(a); return NULL; }
        jskip(j);
        if (*j->p == ',') { j->p++; continue; }
        if (*j->p == ']') { j->p++; return a; }
        json_free(a);
        return NULL;
    }
}

static JVal* jparse_value_inner(JP* j)
{
    jskip(j);
    if (*j->p == '{') return jparse_object(j);
    if (*j->p == '[') return jparse_array(j);
    if (*j->p == '"') {
        char* s = jparse_string_raw(j);
        JVal* v;
        if (!s) return NULL;
        v = jnew(JSON_STR);
        if (!v) { free(s); return NULL; }
        v->s = s;
        return v;
    }
    if (j->p[0] == 't' && !strncmp(j->p, "true", 4)) {
        JVal* v = jnew(JSON_BOOL);
        if (v) { v->b = 1; j->p += 4; }
        return v;
    }
    if (j->p[0] == 'f' && !strncmp(j->p, "false", 5)) {
        JVal* v = jnew(JSON_BOOL);
        if (v) { v->b = 0; j->p += 5; }
        return v;
    }
    if (j->p[0] == 'n' && !strncmp(j->p, "null", 4)) {
        JVal* v = jnew(JSON_NULL);
        if (v) j->p += 4;
        return v;
    }
    if (*j->p == '-' || (*j->p >= '0' && *j->p <= '9')) {
        char* end;
        double d = strtod(j->p, &end);
        JVal* v = jnew(JSON_NUM);
        if (v) { v->num = d; j->p = end; }
        return v;
    }
    return NULL;
}

JVal* json_parse(const char* text)
{
    JP j;
    JVal* v;
    if (!text) return NULL;
    j.p = text;
    j.depth = 0;
    v = jparse_value(&j);
    return v;
}

JVal* json_get(JVal* obj, const char* key)
{
    int i;
    if (!obj || obj->t != JSON_OBJ) return NULL;
    for (i = 0; i < obj->n; i++)
        if (obj->keys[i] && !strcmp(obj->keys[i], key))
            return obj->items[i];
    return NULL;
}

const char* json_scalar_str(JVal* v, char* buf, int bufsize)
{
    if (!v) return "";
    switch (v->t) {
    case JSON_STR:  return v->s ? v->s : "";
    case JSON_BOOL: return v->b ? "true" : "false";
    case JSON_NUM:
        if ((double)(long long)v->num == v->num)
            _snprintf(buf, bufsize - 1, "%I64d", (long long)v->num);
        else
            _snprintf(buf, bufsize - 1, "%g", v->num);
        buf[bufsize - 1] = 0;
        return buf;
    default: return "";
    }
}
