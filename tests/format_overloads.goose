// User format overloads (§3.7): wherever a T occurs in a print, str or
// format argument, its overload renders it, by value or by reference,
// appending to the builder it is given; structure around it renders as
// usual.
struct Money { cents: i64 }
struct Item { name: u8[..12], price: Money, tags: i64[2] }
struct Pt { x: i32, y: i32 }
enum Shape { Circle { r: i64 }, Dot }

fn format(out: u8[>..]&, m: Money) {
    format(out, "$", m.cents / 100, ".", m.cents % 100 / 10, m.cents % 10);
}
fn format(out: u8[>..]&, p: Pt&) {
    out.push('(');
    format(out, p.x, ",", p.y, " len ", out.len);
    out.push(')');
}

fn main() {
    let m = Money { cents: 1234 };
    print(m);
    print("price: ", m, " and ", Money { cents: 5 });
    let it = Item { name: "pen", price: Money { cents: 99 }, tags: [1, 2] };
    print(it);
    let items: Item[2] = [it, Item { name: "ink", price: Money { cents: 250 }, tags: [3, 4] }];
    print(items);
    var p = Pt { x: 3, y: 4 };
    print("at ", p);
    let s = str("p=", p, " m=", m);
    print(s, " ", s.len);
    var out: u8[>..] = "log:";
    format(out, " ", m, " ", p);
    print(out);
    let r: Pt& = p;
    print(r);
    print(Shape.Circle { r: 2 }, " ", Shape.Dot);
    slots();
}

// A slice's overload takes its slot, which it may re-point.
fn format(out: u8[>..]&, p: u8[:]&) {
    if p.len > 0 {
        out.push(p[0]);
        p = p[1..];
    }
}
var buf: u8[>..] = [];

fn slots() {
    buf.append("abc");
    var v = buf[..];
    print(v, " ", v);
    print(v.len);
    var b: u8[>..] = "";
    let n: i64 = 4;
    b.format(n, v);
    print(b);
}
