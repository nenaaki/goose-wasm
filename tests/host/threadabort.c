#include <stdio.h>
#include "upstream_shim.h"
#include "threadabort.h"

int main(int argc, char **argv) {
    goose_init(argc, argv);
    printf("%lld\n", (long long)run(1));
    fflush(stdout);
    printf("%lld\n", (long long)run(5));
    printf("not reached\n");
    return 0;
}
