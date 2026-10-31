// A for loop over a slice reads its slot again each iteration: a body
// storing another slice there ends the loop by the new one's length.
fn main() {
    var a: i64[>..] = [1, 2, 3];
    var b: i64[>..] = [7];
    var s = a[..];
    for x in s {
        print(x);
        s = b[0..1];
    }
    var c: i64[3] = [1, 2, 3];
    var t: i64[:] = c;
    let r = &t;
    for x in r { print(x); t = b[0..1]; }
}
