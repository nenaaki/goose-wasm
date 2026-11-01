// Globals: initialized in declaration order before main (an initializer
// with an effect runs even when nothing reads its global), read and written
// from functions, resizable globals on their own stacks, a global frame
// object, and references to globals.
struct P { x: i64, y: i64 }
struct Bag { n: i64, items: i64[>..] }

fn note(s: u8[:], v: i64) -> i64 {
    print("init ", s);
    v
}

var counter: i64 = note("counter", 5);
let unused = note("unused", 1);
var origin = P { x: counter, y: 2 };
var log: i64[>..] = [1, 2];
var bag = Bag { n: 0, items: [7] };

fn bump(by: i64) -> i64 {
    counter += by;
    log.push(counter);
    counter
}

fn grow(b: Bag&, v: i64) {
    b.items.push(v);
    b.n++;
}

fn main() {
    print("main ", counter, " ", origin.x, " ", origin.y);
    bump(3);
    bump(4);
    origin.y = log.len;
    grow(bag, 70);
    grow(bag, 700);
    var total: i64 = 0;
    for x in log { total += x; }
    for x in bag.items { total += x; }
    let r .= origin;
    r.x = 99;
    print(counter, " ", log.len, " ", total, " ", origin.x, " ", origin.y, " ", bag.n);
}
