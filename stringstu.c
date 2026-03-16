#include "stringstu.h"
#include <stdlib.h>
#include <string.h>

int string_free(string_t *s) {
    if (s && s->data) {
        free(s->data);
        s->data = NULL;
        s->length = 0;
        return 0;
    }
    return -1;
}

int string_init(string_t *s, const char *data, size_t length) {
    if (!s || length == 0 || !data) return -1;
    s->data = malloc(length + 1);
    if (s->data) {
        memcpy(s->data, data, length);
        s->data[length] = '\0';
        s->length = length;
        return 0;
    }
    return -1;
}