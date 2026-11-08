// Fixed values of other shapes crossing to a host: fixed arrays, structs
// holding them, enums and fixed-mode ADTs, by value, as results and through
// references; slices of structs, and a writable slice.

enum Color { Red, Green, Blue }
enum Shape { Dot, Circle { r: f64 }, Rect { w: i32, h: i32 } }

struct V3 { v: f32[3] }
struct Item { id: u16, color: Color, at: V3 }

export fn rev(a: i32[4]) -> i32[4] { return [a[3], a[2], a[1], a[0]]; }

export fn scale(v: V3, k: f32) -> V3 { return V3 { [v.v[0] * k, v.v[1] * k, v.v[2] * k] }; }

export fn next(c: Color) -> Color {
    return match c {
        Red => Color.Green,
        Green => Color.Blue,
        Blue => Color.Red,
    };
}

export fn area(s: Shape) -> f64 {
    return match s {
        Dot => 0.0,
        Circle c => 3.0 * c.r * c.r,
        Rect r => r.w * r.h,
    };
}

export fn grow(s: Shape) -> Shape {
    return match s {
        Dot => Shape.Circle { r: 1.0 },
        Circle c => Shape.Rect { w: (c.r as! i32) + 1, h: 2 },
        Rect => Shape.Dot {},
    };
}

export fn paint(it: Item&, c: Color) {
    it.color = c;
    it.at.v[1] += 1.0;
}

export fn ids(items: const Item[:]) -> i64 {
    var t: i64 = 0;
    for it in items { t = t * 1000 + it.id * 10 + (match it.color { Red => 1, Green => 2, Blue => 3 }); }
    return t;
}

export fn fill(xs: i32[:], v: i32) {
    for i in 0 .. xs.len { xs[i] = v + (i as! i32); }
}
