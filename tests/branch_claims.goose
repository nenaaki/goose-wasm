// What an if's else branch claims on a data stack is released on its own
// path, never on the then branch's, which never claimed it.
fn limited(n: i64) -> i64[..] {
    var result: i64[..] = [..n + 5];
    for i in n { result.push(10 + i); }
    return result;
}
fn main() {
    var out: i64[>..] = [-1];
    for k in 0..3 {
        out.append(if k == 0 { limited(0) } else { limited(k) });
        out.push(-2);
    }
    print(out);
}
