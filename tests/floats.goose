// Floats: f64 and f32 values, arithmetic and comparisons, conversions
// (int to float, float to int truncating and wrapping modulo 2^64, f32 and
// f64 both ways), and their text: the shortest digits that read back,
// %g-style layout, a whole number ending in .0, inf, -inf and nan (§3.7).
// Thousands of generated values check the digits against upstream's.
struct V { x: f32, y: f64 }

fn main() {
    let specials: f64[16] = [0.0, -0.0, 1.0, -1.0, 0.1, 0.2, 0.3, 1e21, 1e-7, 123456789012345680.0,
                             5e-324, 2.2250738585072014e-308, 1.7976931348623157e308, 1e15, 1e16, 0.0001];
    for x in specials { print(x, " ", -x, " ", x * 3.0, " ", x / 7.0); }
    let zero = specials[0];
    print(1.0 / zero, " ", -1.0 / zero, " ", zero / zero);
    print(0.1 + 0.2, " ", 0.1 as f32, " ", 16777217.0 as f32, " ", 3.4028235e38 as f32 * 10.0);
    var x: f64 = 1.0;
    var y: f32 = 1.0;
    for i in 1..400 {
        x = x * 1.37 + 1.0 / i as f64;
        y = y * 1.11 - 0.5 / i as f32;
        print(i, " ", x, " ", 1.0 / x, " ", y, " ", (i as f64) * 0.1, " ", 1.0 / (i as f64));
    }
    var p: f64 = 1.0;
    for i in 0..1080 {
        print(p, " ", p * 0.3);
        p = p / 2.0;
    }
    var q: f64 = 1.0;
    for i in 0..320 {
        print(q, " ", q * 7.0, " ", q - 1.0);
        q = q * 10.0;
    }
    let big: f64[8] = [9.2233720368547758e18, -9.2233720368547758e18, 1.8446744073709552e19, 3e19,
                       -3e19, 1e30, 123.9, -123.9];
    for b in big { print(b as i64, " ", b as u64, " ", b as i32, " ", b as u8); }
    print(7 as f64 / 2.0, " ", -7 / 2, " ", (2.5 as f32) < 2.6, " ", zero / zero == zero / zero);
    // A cast converts to its own type, whatever wider slot it lands in.
    var g: f64 = 0.5;
    g += 0.1 as f32;
    g *= 3.0;
    let narrowed: f64 = specials[4] as f32;
    var wide = specials[2] as i64;
    wide += 299;
    let k: i64 = wide as i8;
    print(g, " ", narrowed, " ", k);
    let v = V { x: 1.5, y: 2.25 };
    var vs: V[>..] = [v, V { x: -0.1, y: 1e100 }];
    print(v, " ", vs);
}
