// Variable-size values (§3.3, §3.6): T[] images with their length field,
// varint fields, structs whose fields after a variable one are found by
// walking; as locals, parameters, results and globals; read, printed and
// compared.
struct Item { name: u8[], qty: i64 }
struct V { n: varint, w: i32, tag: u8[varint], last: varint }
struct Pair { a: i64[], b: u16[u8] }

var gtext: u8[] = "global";
var gv = V { n: 300, w: -2, tag: "g", last: -5 };

fn total(xs: i64[]) -> i64 { var t = 0; for x in xs { t += x; } t }
fn label(n: i64) -> u8[] { if n > 1 { "many" } else { "one" } }
fn mk(k: i64) -> Item { Item { name: label(k), qty: k * 10 } }
fn show(v: V&) { print(v.n, " ", v.w, " ", v.tag, " ", v.last, " ", v); }

fn main() {
    let a: i64[] = [1, 2, 3];
    let s: u8[] = "item_1234";
    print(a, " ", s, " ", a.len, " ", total(copy(a)), " ", s[2]);
    let it = Item { name: "pen", qty: 3 };
    print(it, " ", it.name, " ", it.qty, " ", it.name.len);
    let m = mk(5);
    print(m, " ", mk(1).qty);
    let v = V { n: 300, w: 7, tag: "abc", last: -1 };
    show(v);
    show(gv);
    let p = Pair { a: [5, 6, 7], b: [65535, 1] };
    print(p, " ", p.b[0], " ", p.a == [5, 6, 7], " ", p == Pair { a: [5, 6, 7], b: [65535, 1] });
    print(gtext, " ", gtext.len, " ", v == gv, " ", it == Item { name: "pen", qty: 3 });
    var big: V = V { n: -9223372036854775807 - 1, w: 0, tag: "", last: 9223372036854775807 };
    print(big.n, " ", big.last);
}
