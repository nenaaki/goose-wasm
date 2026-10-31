// Variable-mode ADTs (§3.5): the tag, then the named variant's payload,
// packed; variable-size payloads; match with payloads read where they lie;
// printed and compared; in sequential arrays.
enum Shape { Circle { r: i64 }, Label { text: u8[], size: varint }, Dot }

fn area(s: Shape..&) -> i64 {
    match s {
        Circle c => 3 * c.r * c.r,
        Label &l => l.text.len * l.size,
        Dot => 0,
    }
}

fn main() {
    var a: Shape.. = Shape.Circle { r: 2 };
    let b: Shape.. = Shape.Label { text: "hello", size: 300 };
    let c: Shape.. = Shape.Dot;
    print(a, " ", b, " ", c);
    print(area(a), " ", area(b), " ", area(c));
    var all: Shape..[>..] = [];
    all.push(Shape.Label { text: "x", size: -3 });
    all.push(Shape.Dot);
    all.push(Shape.Circle { r: 5 });
    var t = 0;
    for s in all { t += area(s); }
    print(all, " ", t);
    let b2: Shape.. = Shape.Label { text: "hello", size: 300 };
    print(b == b2, " ", b == c, " ", all.len);
}
