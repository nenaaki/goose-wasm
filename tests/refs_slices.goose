// References (transparent use, by-reference parameters, .=, optionals,
// .==), fixed arrays, slices of each sequence kind, and strings.
struct Node { val: i64, next: Node? }
fn bump(r: i64&) { r += 1; }
fn total(s: i64[:]) -> i64 { var t: i64 = 0; for x in s { t += x; } t }
fn main() {
    var n: i64 = 5;
    bump(n);
    let r .= n;
    r = r * 2;
    var fixed: i64[4] = [1, 2, 3, 4];
    fixed[2] = 30;
    var g: i64[>..] = [7, 8, 9];
    print(n, " ", total(fixed), " ", total(g[1..3]), " ", fixed.len);
    let s = "hello";
    print(s, " ", s.len, " ", s[1]);
    var a = Node { val: 1, next: null };
    var b = Node { val: 2, next: a };
    let p = b.next;
    if p { print(p.val); }
    print(b.next .== &a);
    more();
}

struct Pair { a: i64, b: i64 }

fn swap(p: Pair&) {
    let t = p.a;
    p.a = p.b;
    p.b = t;
}

fn longest(words: const u8[:][:]) -> i64 {
    var best: i64 = 0;
    for w in words { if w.len > best { best = w.len; } }
    best
}

fn more() {
    var pr = Pair { a: 1, b: 2 };
    swap(pr);
    print(pr.a, " ", pr.b);
    var grid: i32[3] = [7; 3];
    grid[1] += 1;
    for &g in grid { g *= 2; }
    print(grid[0], " ", grid[1], " ", grid[2], " ", grid.len);
    var xs: i64[>..] = [1, 2, 3, 4, 5, 6];
    let mid = xs[1..^1];
    let tail = xs[3..];
    print(mid.len, " ", mid[0], " ", tail[0], " ", total(mid[1..3]));
    let words: const u8[:][3] = ["a", "abcd", "ab"];
    print(longest(words), " ", words[1]);
    var cur: Pair? = null;
    if !cur { print("empty"); }
    cur .= pr;
    if cur { print(cur.b); }
    var r .= xs[2];
    r += 100;
    print(xs[2]);
    let bad = xs[2..9];
    print(bad.len);
}
