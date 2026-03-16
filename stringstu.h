#include <stddef.h>


typedef struct {
    char *data;
    size_t length;
} string_t;


int string_free(string_t *s);
int string_init(string_t *s, const char *data, size_t length);