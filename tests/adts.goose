// Fixed-mode ADTs: construction, match (ADT and integer), constants,
// adaptation, equality of payload-less ADTs, rendering.
enum Shape {
    Circle { r: f64 },
    Rect { w: i32, h: i32 },
    Empty,
}

enum Color { Red, Green, Blue }

struct Holder { s: Shape, c: Color }

fn area(s: Shape) -> f64 {
    match s {
        Circle c => 3.0 * c.r * c.r,
        Rect r => r.w * r.h,
        Empty => 0.0,
    }
}

fn name(c: Color) -> i32 {
    match c {
        Red => 1,
        Green, Blue => 2,
    }
}

fn make(k: i64) -> Shape {
    if k == 0 { Shape.Circle { r: 2.0 } } else if k == 1 { Shape.Rect { w: 3, h: 4 } } else { Shape.Empty }
}

fn classify(x: i64) -> i32 {
    match x {
        0 => 0,
        1..10 => 1,
        -5, 100 => 2,
        _ => 3,
    }
}

fn main() {
    let a: Shape = Shape.Circle { r: 1.5 };
    let b: Shape = Shape.Rect { w: 2, h: 5 };
    let e: Shape = Shape.Empty;
    print(area(a), " ", area(b), " ", area(e));
    print(a);
    print(b);
    print(e);
    let c: Color = Color.Green;
    print(name(c), " ", name(Color.Red), " ", c);
    print(c == Color.Green, " ", c != Color.Blue, " ", c == Color.Red);
    var h = Holder { s: make(1), c: Color.Blue };
    print(h);
    h.s = make(0);
    print(h.s, " ", area(h.s));
    for i in 0..3 { print(make(i)); }
    print(classify(0), classify(5), classify(10), classify(-5), classify(100), classify(7000));
    let t = match c { Red => "r", Green => "g", Blue => "b" };
    print(t);
    let r: Shape& = h.s;
    match r {
        Circle cc => print("circle ", cc.r),
        _ => print("other"),
    }
    let rc = Shape.Rect { w: 7, h: 8 };
    let s2: Shape = rc;
    print(rc, " ", s2, " ", area(s2));
    let cs: Color[3] = [Color.Blue, Color.Red, Color.Green];
    for x in cs { print(x, " ", name(x)); }
}
