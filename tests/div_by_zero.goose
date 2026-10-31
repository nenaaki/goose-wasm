// Division by zero aborts in every build (spec §6.2), with the location.
fn main() {
    var z = 0;
    print("before");
    print(10 / z);
}
