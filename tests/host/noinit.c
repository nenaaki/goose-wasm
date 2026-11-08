#include <stdio.h>
#include "upstream_shim.h"
#include "noinit.h"

int main(void) {
    printf("before\n");
    fflush(stdout);
    printf("%d\n", answer());
    return 0;
}
