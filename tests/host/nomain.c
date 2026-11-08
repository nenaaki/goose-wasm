#include <stdio.h>
#include "upstream_shim.h"
#include "nomain.h"

int main(int argc, char **argv) {
    goose_init(argc, argv);
    printf("%lld\n", (long long)sum_squares(10));
    fflush(stdout);
    shout(3);
    shout(0);
    printf("%d\n", at(2));
    fflush(stdout);
    printf("%d\n", at(4));
    return 0;
}
