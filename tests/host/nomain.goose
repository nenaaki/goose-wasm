// A program built for a host may leave main out: goose_init runs only the
// globals' initializers. Exports call each other as Goose functions, print,
// build strings on data stacks, and fail with upstream's runtime errors,
// whose location is the Goose source.

var greeting: u8[] = "hi";
var table: i32[4] = [ 1, 2, 3, 4 ];

export fn square(n: i64) -> i64 { return n * n; }

export fn sum_squares(n: i64) -> i64 {
    var t: i64 = 0;
    for i in 0 .. n { t += square(i); }
    return t;
}

export fn shout(times: i32) {
    var s: u8[>..] = [];
    for i in 0 .. times {
        s.append(greeting);
        s.append("!");
    }
    print(s, " ", s.len);
}

export fn at(i: i64) -> i32 { return table[i]; }
