// Structural == and != (§4.5): structs and fixed arrays memberwise,
// floats by IEEE value, sequences by length then elements across array
// kinds, ADTs by tag then payload; references and slices inside a value
// by identity, as direct operands by what they name.
struct P { x: i64, y: f64 }
struct Q { p: P, tags: u8[3] }
struct R { r: i64&, s: u8[:] }
enum Shape { Circle { r: i64 }, Rect { w: i32, h: i32 }, Dot }

fn main() {
    let a = P { x: 1, y: 0.0 };
    let b = P { x: 1, y: -0.0 };
    let n = P { x: 1, y: 0.0 / 0.0 };
    print(a == b, " ", a != b, " ", n == n, " ", Q { p: a, tags: [1, 2, 3] } == Q { p: b, tags: [1, 2, 3] });
    let xs: i64[3] = [1, 2, 3];
    var ys: i64[>..] = [1, 2];
    print(xs == ys, " ", xs[..2] == ys, " ");
    ys.push(3);
    print(xs == ys, " ", ys == [1, 2, 3], " ", ys[1..] == xs[1..]);
    var word: u8[>..] = "gus";
    let lim: u8[..8] = "gus";
    print(word == "gus", " ", lim == word, " ", "gus" == lim, " ", word != "gum", " ", word[..0] == "");
    var v = 5;
    var w = 5;
    let r1 = R { r: v, s: word[..] };
    let r2 = R { r: w, s: word[..] };
    let r3 = R { r: v, s: word[..] };
    print(r1 == r2, " ", r1 == r3);
    let s1: Shape = Shape.Rect { w: 2, h: 3 };
    let s2: Shape = Shape.Rect { w: 2, h: 3 };
    let s3: Shape = Shape.Rect { w: 2, h: 4 };
    print(s2 == s1, " ", s2 == s3, " ", Shape.Dot == Shape.Dot, " ", s1 == s2);
}
