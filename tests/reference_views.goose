// Values read through what a call gives: a reference result names its
// storage, an overload taking a scalar by reference writes the variable,
// and an array element passed as a slice is viewed whole.
fn format(out: u8[>..]&, v: u16&) {
    v += 2;
    format(out, "u", v as i64);
}
fn scalar(ru: u16&) { print(ru); }
fn scalars() {
    var n: u16 = 7;
    scalar(n);
    print(n as i64);
    print(n);
    print(n as i64);
}
var grid: i64[3][2] = [[1, 2, 3], [4, 5, 6]];
fn cells() -> i64[3][2] { grid }
fn poke() -> i64 { grid[0] = [7, 8, 9]; 0 }
fn head(s: const i64[:], n: i64) -> i64 { s[0] * 10 + n }
fn main() {
    scalars();
    print(head(cells()[0], poke()));
    print(head(cells()[1][..], poke()));
}
