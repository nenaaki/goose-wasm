// os's files and directories (lib/os.goose over WASI): writes, appends,
// atomic writes and renames that replace, sorted listings, paths with "./"
// and nested directories, and the failures each reports, all inside a
// scratch directory the test makes and removes.
import os;

fn at(dir: const u8[:], name: const u8[:]) -> u8[>..] { str(dir, "/", name) }

fn show(dir: const u8[:]) {
    var names: u8[>..] = [];
    print("list ", list_dir(dir, names), ":");
    write_stdout(names);
}

fn main() {
    let dir = "gs_os_files_tmp";
    print(make_dir(dir), " ", make_dir(at(dir, "inner")), " ", is_dir(str("./", dir)));
    let f = at(dir, "f.txt");
    print(write_file(f, "one"), " ", append_file(f, "+two"), " ", file_exists(f));
    var b: u8[>..] = [];
    print(read_file(str("./", f), b), " ", b);
    print(write_file_atomic(f, "three"), " ", file_exists(f));
    b.clear();
    read_file(f, b);
    print(b);
    let g = at(dir, "inner/g.txt");
    print(write_file(g, "gee"), " ", rename_file(g, f), " ", file_exists(g));
    b.clear();
    read_file(f, b);
    print(b);
    write_file(at(dir, "B"), "");
    write_file(at(dir, "a"), "");
    write_file(at(dir, "_"), "");
    show(dir);
    print(delete_dir(dir), " ", delete_file(at(dir, "inner")), " ", read_file(at(dir, "inner"), b), " ", b);
    print(write_file(at(dir, "missing/x"), "x"), " ", rename_file(at(dir, "nope"), f), " ",
          write_file_atomic("", "x"), " ", file_exists(""), " ", is_dir("."));
    for n in ["f.txt", "B", "a", "_"] { delete_file(at(dir, n)); }
    print(delete_dir(at(dir, "inner")), " ", delete_dir(dir), " ", is_dir(dir));
    var input: u8[>..] = [];
    read_stdin(input);
    var line: u8[>..] = [];
    print(input.len, " ", read_line(line), " ", arg_count() >= 1);
    var e: u8[>..] = [];
    arg(-1, e);
    arg(1000, e);
    print(e.len, " ", env("", e), " ", env("A=B", e));
}
