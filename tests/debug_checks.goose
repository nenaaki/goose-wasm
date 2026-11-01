// runtime-define: GS_DEBUG=1
// The debug build (§6.2, §6.3): signed arithmetic at its width and `as`
// abort where the value changes; unsigned arithmetic and `as!` wrap.
fn main() {
    let a: u8 = 250;
    let big: i64 = 300;
    let m1: i64 = -1;
    print(a + 10, " ", big as! u8, " ", m1 as! u32);
    let x: i16 = 30000;
    let two: f64 = 2.0;
    let k: i64 = 3;
    print(x - 1, " ", two as i32, " ", (big - 100) as u8, " ", -k, " ", k * 1000000);
    let f: f32 = 7.5;
    print(f as i64);
}
