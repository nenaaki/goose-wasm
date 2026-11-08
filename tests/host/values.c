#include <stdio.h>
#include "upstream_shim.h"
#include "values.h"

static void shape(Shape_g s) {
    if (s.tag == Shape_g_Circle_g_k) printf("circle %g\n", s.u.v_Circle_g.r_g);
    else if (s.tag == Shape_g_Rect_g_k) printf("rect %d %d\n", s.u.v_Rect_g.w_g, s.u.v_Rect_g.h_g);
    else printf("dot\n");
}

int main(int argc, char **argv) {
    goose_init(argc, argv);
    a4_i32 a = { { 1, 2, 3, 4 } };
    a4_i32 r = rev(a);
    printf("%d %d %d %d\n", r.e[0], r.e[1], r.e[2], r.e[3]);
    V3_g v = { { { 1.5f, -2.0f, 0.25f } } };
    V3_g w = scale(v, 2.0f);
    printf("%g %g %g\n", w.v_g.e[0], w.v_g.e[1], w.v_g.e[2]);
    Color_g c = { Color_g_Blue_g_k };
    c = next(c);
    printf("%d\n", c.tag);
    c = next(next(c));
    printf("%d\n", c.tag);
    Shape_g s = { 0 };
    printf("%g\n", area(s));
    s = grow(s);
    shape(s);
    printf("%g\n", area(s));
    s.u.v_Circle_g.r_g = 2.5;
    s = grow(s);
    shape(s);
    printf("%g\n", area(s));
    s = grow(s);
    shape(s);
    Item_g items[3] = { { 7, { Color_g_Red_g_k }, { { { 0, 0, 0 } } } },
                        { 8, { Color_g_Green_g_k }, { { { 0, 1, 0 } } } },
                        { 9, { Color_g_Blue_g_k }, { { { 0, 2, 0 } } } } };
    paint(&items[1], (Color_g) { Color_g_Blue_g_k });
    printf("%d %g\n", items[1].color_g.tag, items[1].at_g.v_g.e[1]);
    sl_Item_g si = { items, 3 };
    printf("%lld\n", (long long)ids(si));
    int32_t xs[5] = { 0 };
    sl_i32 sx = { xs + 1, 3 };
    fill(sx, 10);
    printf("%d %d %d %d %d\n", xs[0], xs[1], xs[2], xs[3], xs[4]);
    return 0;
}
