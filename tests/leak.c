/* Deliberately buggy program: built against the MA_VALGRIND allocator,
 * memcheck must report each of these as if ma were the system malloc. */
#include "ma.h"

#include <string.h>

int main(void)
{
    char *leaked = ma_malloc(64);          /* definitely lost: 64 bytes */
    memset(leaked, 1, 64);
    leaked = NULL;

    char *p = ma_malloc(32);
    p[32] = 'x';                           /* heap overflow by one byte */
    ma_free(p);
    return p[0];                           /* use after free */
}
