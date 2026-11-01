// format(out, ...) (§3.7) appends print's text, without the newline, to a
// growable u8 array: a resizable one, or a limited one under its capacity.
struct P { x: i64, y: f64 }

fn main() {
    var out: u8[>..] = "start:";
    format(out, 12, " ", P { x: 1, y: 2.5 }, " ", true);
    out.push(33);
    format(out, [1, 2]);
    print(out, " ", out.len);
    var lim: u8[..16] = "ab";
    format(lim, "c", 42);
    print(lim, " ", lim.len);
    format(lim, "0123456789abcdef");
    print("unreachable");
}
