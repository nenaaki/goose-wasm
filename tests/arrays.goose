// Grow-only arrays on data stacks: literals, push (also through a
// reference parameter, onto the caller's stack), indexing with bounds
// checks, .len, for loops that see elements the body pushes, arrays of
// structs, and stacks released at scope exits, loop iterations and early
// exits so that repeated scopes reuse their memory.
struct P { x: i32, y: i32 }

fn fill(xs: i64[>..]&, n: i64) {
    for i in 0..n { xs.push(i * i); }
}

fn sum(xs: i64[>..]&) -> i64 {
    var s: i64 = 0;
    for x in xs { s += x; }
    s
}

fn first_over(limit: i64) -> i64 {
    var found: i64 = -1;
    for round in 0..5 {
        var tmp: i64[>..] = [];
        fill(tmp, round * 3);
        for x, i in tmp {
            if x > limit {
                found = i;
                break;
            }
        }
        if found >= 0 { return found * 100 + round; }
    }
    found
}

fn main() {
    var a: i64[>..] = [10, 20, 30];
    a.push(40);
    a[1] += 5;
    print(a.len, " ", a[0], " ", a[1], " ", a[3], " ", sum(a));
    var b: i64[>..] = [];
    fill(b, 5);
    print(b.len, " ", sum(b));
    var grow: i32[>..] = [1];
    for g in grow {
        if grow.len < 6 { grow.push(g * 2); }
    }
    print(grow.len, " ", grow[5]);
    var ps: P[>..] = [P { x: 1, y: 2 }];
    ps.push(P { x: 3, y: 4 });
    ps[0].y = 9;
    var t: i32 = 0;
    for p in ps { t += p.x * 10 + p.y; }
    print(ps.len, " ", t, " ", ps[1].x);
    print(first_over(40));
    var total: i64 = 0;
    for k in 0..2000 {
        var big: i64[>..] = [];
        fill(big, 500);
        total += big[499];
    }
    print(total);
    print(a[7]);
}
