// return E from f (§7.9) returns E as the result of the innermost active
// call of f, every frame in between leaving at once, its defer bodies
// run; a function value's return leaves the function it was written in.
fn search(xs: i64[:], want: i64) -> i64 {
    for x, i in xs { check(x, want, i); }
    -1
}
fn check(x: i64, want: i64, i: i64) {
    defer print("checked ", i);
    if x == want { return i from search; }
}
fn each<F>(n: i64) { for i in n { F(i); } }
fn first_over(n: i64, limit: i64) -> i64 {
    each(n) { if it * it > limit { return it; } };
    -1
}
fn main() {
    print(search([4, 7, 9], 7));
    print(search([4, 7, 9], 5));
    print(first_over(10, 20), " ", first_over(3, 100));
}
