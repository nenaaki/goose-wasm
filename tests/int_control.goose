fn main() {
    var a: i8 = 127;
    a += 1;
    let b: u8 = 250;
    let c = b + 10;
    print(a, " ", c);
    print(-7 / 3, " ", -7 % 3, " ", 7 % -3, " ", -7 >> 1, " ", -9223372036854775807 - 1);
    var x: i32 = -2147483647 - 1;
    let y: i32 = -1;
    print(x / y, " ", x % y, " ", x * y);
    let s: u32 = 1;
    print(s << 35, " ", (-1 as i16) >> 20, " ", (0xffff as u16) >> 3, " ", ~(5 as u8));
    var n = 0;
    for i: u8 in 0..256 { n += i; }
    print(n);
    var total: i64 = 0;
    var k = 0;
    while k < 10 {
        k++;
        if k % 2 == 0 { continue; }
        if k > 7 { break; }
        total += k;
    }
    print(total, " ", k);
    let v = block { if total > 3 { break 100; } 5 };
    var cnt = 0;
    let w = loop { cnt++; if cnt == 3 { break cnt * 10; } };
    print(v, " ", w, " ", true && false, " ", 3 < 4 || false, " ", !(1 == 1));
    for i, idx in 5..8 { print(i, ":", idx); }
}
