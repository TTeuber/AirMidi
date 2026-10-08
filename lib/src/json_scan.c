#include "airmidi/json_scan.h"

#include <string.h>

void json_scan_init(json_scan_t *s, const char *p, size_t n) { *s = (json_scan_t){.p = p, .n = n, .pos = 0}; }

static size_t skip_ws(const char *p, size_t n, size_t i) {
    while (i < n && (p[i] == ' ' || p[i] == '\t' || p[i] == '\n' || p[i] == '\r')) i++;
    return i;
}

// p[i] is the opening quote; returns the index of the closing quote, or n if the string never ends.
static size_t string_end(const char *p, size_t n, size_t i) {
    for (i++; i < n; i++) {
        if (p[i] == '\\')
            i++;  // the escaped character can be a quote
        else if (p[i] == '"')
            return i;
    }
    return n;
}

bool json_scan_next(json_scan_t *s, const char **key, size_t *klen, size_t *value) {
    while (s->pos < s->n) {
        if (s->p[s->pos] != '"') {
            s->pos++;
            continue;
        }
        size_t start = s->pos + 1, end = string_end(s->p, s->n, s->pos);
        if (end >= s->n) break;
        size_t after = skip_ws(s->p, s->n, end + 1);
        s->pos = end + 1;
        if (after < s->n && s->p[after] == ':') {
            // A string followed by a colon is a key. Continue from its value, so a string value is seen next as a
            // string that is not followed by a colon.
            *key = s->p + start, *klen = end - start;
            *value = skip_ws(s->p, s->n, after + 1);
            s->pos = *value;
            return true;
        }
    }
    s->pos = s->n;
    return false;
}

bool json_int_at(const char *p, size_t n, size_t at, int64_t *out) {
    bool neg = at < n && p[at] == '-';
    size_t i = at + neg;
    if (i >= n || p[i] < '0' || p[i] > '9') return false;
    int64_t v = 0;
    for (; i < n && p[i] >= '0' && p[i] <= '9'; i++) {
        if (v > (INT64_MAX - 9) / 10) return false;
        v = v * 10 + (p[i] - '0');
    }
    if (i < n && (p[i] == '.' || p[i] == 'e' || p[i] == 'E')) return false;  // not an integer
    *out = neg ? -v : v;
    return true;
}

bool json_str_at(const char *p, size_t n, size_t at, char *out, size_t cap) {
    if (at >= n || p[at] != '"') return false;
    size_t end = string_end(p, n, at), len = end - at - 1;
    if (end >= n || len + 1 > cap || memchr(p + at + 1, '\\', len)) return false;
    memcpy(out, p + at + 1, len);
    out[len] = 0;
    return true;
}

static bool find(const char *p, size_t n, const char *key, size_t *value) {
    json_scan_t s;
    json_scan_init(&s, p, n);
    const char *k;
    size_t kl, klen = strlen(key);
    while (json_scan_next(&s, &k, &kl, value))
        if (kl == klen && !memcmp(k, key, kl)) return true;
    return false;
}

bool json_find_int(const char *p, size_t n, const char *key, int64_t *out) {
    size_t v;
    return find(p, n, key, &v) && json_int_at(p, n, v, out);
}

bool json_find_str(const char *p, size_t n, const char *key, char *out, size_t cap) {
    size_t v;
    return find(p, n, key, &v) && json_str_at(p, n, v, out, cap);
}
