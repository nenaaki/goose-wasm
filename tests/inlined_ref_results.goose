// An inlined call's reference result used as its pointee is read through
// it, as a direct call's is (the body gives the callee's own result type).
fn head(x: i64[>..<]&) -> i64& { x[0] }
fn main() {
    var a: i64[>..<] = [1, 2];
    print(head(a), " ", head(a) + 10);
    let r .= head(a);
    r = 5;
    print(a);
}
