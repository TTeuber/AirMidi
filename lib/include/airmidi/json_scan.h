// A forgiving scanner for the few JSON fields MIDI-CI needs on the box: "status" in a Property Exchange reply
// header, "resource" / "resId" in a request header, the resource names in a ResourceList and the "ctrlMapId"s of
// an AllCtrlList. It builds nothing and validates nothing: it walks the text, skips string contents (escapes
// included) and reports every "key": value pair at any depth. The full JSON is parsed by the browser.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *p;
    size_t n, pos;
} json_scan_t;

void json_scan_init(json_scan_t *s, const char *p, size_t n);

// The next key at any depth, in text order: the key's characters (not terminated, escapes left as they are) and
// the offset where its value starts. False at the end of the text.
bool json_scan_next(json_scan_t *s, const char **key, size_t *klen, size_t *value);

// The value at offset `at` as an integer (false if it is not one, or does not fit), or as a string copied into
// out with a terminating NUL (false if it is not a string, contains escapes, or does not fit).
bool json_int_at(const char *p, size_t n, size_t at, int64_t *out);
bool json_str_at(const char *p, size_t n, size_t at, char *out, size_t cap);

// The first value of `key` anywhere in the text.
bool json_find_int(const char *p, size_t n, const char *key, int64_t *out);
bool json_find_str(const char *p, size_t n, const char *key, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
