#include <stdio.h>
#include "upstream_shim.h"
#include "basic.h"

int main(int argc, char **argv) {
    goose_init(argc, argv);
    goose_init(argc, argv);
    printf("%d\n", c_add(20, 22));
    printf("%d\n", narrow(-5, (uint8_t)300, -300, 65535, 1));
    printf("%d\n", narrow(127, 0, 32767, 0, 0));
    printf("%llu\n", (unsigned long long)wide(-1, 2));
    printf("%llu\n", (unsigned long long)wide(1ll << 62, 1ull << 63));
    printf("%g\n", halve(5.0, 0.25f));
    printf("%g\n", (double)third(1.0f));
    printf("%d %d\n", yes(3), yes(-3));
    printf("%d %d\n", small(200), small(-129));
    P_g p = { 1, 2 };
    P_g q = swap(p);
    printf("%d %d\n", q.x_g, q.y_g);
    Box_g b = { { 10, 20 }, { 30, 40 }, 255 };
    Box_g c = span(b);
    printf("%d %d %d %d %d\n", c.lo_g.x_g, c.lo_g.y_g, c.hi_g.x_g, c.hi_g.y_g, c.tag_g);
    printf("%d %d\n", b.lo_g.x_g, b.tag_g);
    bump(&p);
    bump(&p);
    printf("%d %d\n", p.x_g, p.y_g);
    int32_t xs[] = { 1, 2, 3, -10 };
    sl_i32 sl = { xs, 4 };
    printf("%lld\n", (long long)total(sl));
    sl.len = 0;
    printf("%lld\n", (long long)total(sl));
    uint8_t text[] = "banana";
    sl_u8 st = { text, 6 };
    printf("%d\n", count_byte(st, 'a'));
    int64_t n;
    const uint8_t *s = hello(&n);
    printf("%.*s (%lld)\n", (int)n, s, (long long)n);
    uint8_t w1[] = "one", w2[] = "two";
    sl_u8 a1 = { w1, 3 }, a2 = { w2, 3 };
    s = remember(a1, &n);
    printf("%.*s\n", (int)n, s);
    s = remember(a2, &n);
    printf("%.*s\n", (int)n, s);
    printf("%d\n", count());
    return 0;
}
