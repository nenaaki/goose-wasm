#include <stdio.h>
#include "upstream_shim.h"
#include "threads.h"

int main(int argc, char **argv) {
    goose_init(argc, argv);
    printf("%lld\n", (long long)sum_squares(10));
    printf("%lld\n", (long long)sum_squares(0));
    printf("%d %d\n", contained(1), contained(-1));
    printf("%lld %lld\n", (long long)ask(4), (long long)ask(7));
    printf("%lld\n", (long long)sum_squares(5));
    printf("%lld\n", (long long)stop());
    return 0;
}
