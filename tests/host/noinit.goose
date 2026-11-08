// An export called before goose_init aborts, as upstream's wrapper does.

fn main() { print("main"); }

export fn answer() -> i32 { return 42; }
