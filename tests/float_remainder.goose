// Float % is C's fmod (§6.2): an exact remainder with the dividend's sign;
// nan for a zero divisor or an infinite dividend, the dividend for an
// infinite divisor. Also elementwise math on structs and arrays.
struct V3 { x: f64, y: f64, z: f64 }
fn main() {
    let xs: f64[10] = [5.5, -5.5, 1e300, 0.1, 3.0, -0.0, 7.25, 1e-310, 123456789.0, 2.5];
    let ys: f64[10] = [2.0, 2.0, 3.0, 0.03, 3.0, 1.0, -2.0, 3e-311, 0.1, 0.0];
    for i in 10 { print(xs[i], " % ", ys[i], " = ", xs[i] % ys[i]); }
    let inf = 1.0 / 0.0;
    print(inf % 2.0, " ", 5.0 % inf, " ", (7.5 as f32) % 2.0);
    var v = V3 { x: 1.0, y: 2.0, z: 3.0 };
    let w = V3 { x: 0.5, y: 0.25, z: 4.0 };
    v += w;
    print(v, " ", v * 2.0, " ", 1.0 / w, " ", -v, " ", v % w);
    var a: i32[3] = [10, 20, 30];
    a *= 3;
    let b: i32[3] = [1, 2, 3];
    let s: i32[3] = [7, 7, 7];
    print(a, " ", a - b, " ", a / 7, " ", a % s);
}
