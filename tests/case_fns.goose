// Case functions (§8.2): a call with an ADT where cases take its variants
// dispatches on the tag; the other arguments run once, in source order.
enum Shape { Circle { r: i64 }, Rect { w: i64, h: i64 }, Dot }
struct Box { a: i64, b: i64 }

fn area(c: Shape.Circle) -> i64 { 3 * c.r * c.r }
fn area(r: Shape.Rect) -> i64 { r.w * r.h }
fn area(d: Shape.Dot) -> i64 { 0 }

fn wrap(s: Shape.Circle, b: Box) -> Box { Box { a: s.r + b.a, b: b.b } }
fn wrap(s: Shape.Rect, b: Box) -> Box { Box { a: s.w, b: s.h + b.b } }
fn wrap(s: Shape.Dot, b: Box) -> Box { b }

fn pair(a: i64, s: Shape.Circle, b: i64) -> i64 { a + s.r + b }
fn pair(a: i64, s: Shape.Rect, b: i64) -> i64 { a * s.w * b }
fn pair(a: i64, s: Shape.Dot, b: i64) -> i64 { a - b }

fn note(s: Shape.Circle) { print("circle ", s); }
fn note(s: Shape.Rect) { print("rect ", s); }
fn note(s: Shape.Dot) { print("dot"); }

fn one(x: i64) -> i64 { print("one"); x }
fn shape(k: i64) -> Shape {
    print("shape");
    if k == 0 { Shape.Circle { r: 2 } } else if k == 1 { Shape.Rect { w: 3, h: 4 } } else { Shape.Dot }
}

fn main() {
    var shapes: Shape[3] = [Shape.Circle { r: 1 }, Shape.Rect { w: 2, h: 5 }, Shape.Dot];
    for s in shapes { print(area(s)); }
    for s in shapes { print(s, " ", area(s)); note(s); }
    print(wrap(shapes[0], Box { a: 1, b: 2 }), " ", wrap(shapes[1], Box { a: 1, b: 2 }));
    for i in 3 { print(pair(one(5), shape(i), one(7))); }
    for i in 3 { print(area(shape(i))); }
}
