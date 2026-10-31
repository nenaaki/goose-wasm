// str(...) builds print's text as a u8[>..]; a string literal initializes
// or extends one (§3.7).
struct P { x: i64, y: f64 }
fn main() {
    let n = 7;
    let s = str(n, "/", n * n, " ", P { x: 1, y: 2.5 }, " ", true);
    print(s, " ", s.len);
    var t: u8[>..] = "a";
    t.append(str("b", 3));
    print(t);
    more();
}
var g: u8[>..] = "global";
fn more() {
    var t: u8[>..] = "hello";
    t.append(" world");
    t.push(33);
    print(t, " ", t.len, " ", g);
    g.append(str("!", g.len));
    print(g);
}
