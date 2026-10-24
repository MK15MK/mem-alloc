/* Small program for inspecting the heap in GDB (see tools/ma.gdb):
 *
 *     gdb -x tools/ma.gdb ./build/demo
 *     (gdb) break demo_checkpoint
 *     (gdb) run
 *     (gdb) ma-heap
 */
#include "ma.h"

#include <stdio.h>

void demo_checkpoint(const char *label);

void demo_checkpoint(const char *label)
{
    printf("-- %s --\n", label);
    fflush(stdout);
    ma_dump(1);
}

int main(void)
{
    char *a = ma_malloc(100);
    char *b = ma_malloc(200);
    char *c = ma_malloc(300);
    char *d = ma_malloc(16);
    demo_checkpoint("four blocks allocated");

    ma_free(a);
    ma_free(c);
    demo_checkpoint("a and c freed: two holes");

    ma_free(b);
    demo_checkpoint("b freed: a+b+c coalesced into one chunk");

    char *e = ma_malloc(150);
    demo_checkpoint("150 bytes carved from the front of the merged chunk");

    ma_free(e);
    ma_free(d);
    demo_checkpoint("everything freed: merged back into top");
    return ma_check() == 0 ? 0 : 1;
}
