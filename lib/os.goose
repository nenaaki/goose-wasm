// goose-wasm's os: served in place of upstream's stdlib/os.goose
// (lib/README.md). Upstream's extern fns here are implemented by its C
// runtime over the platform's C library; here they are that C ported to
// Goose over WASI preview 1, behaving as wasi-libc's C library does where
// the platform shows through (decided 2026-11-07): a relative path names a
// file under the preopened directory "." (the working directory), an
// absolute one under the preopened directory its longest prefix names.
//
// Derived from upstream Goose at commit
// 2ed1a63dde2ddff777fbb7f4790412b4f00cd47d: stdlib/os.goose (everything
// but the extern declarations, kept as it was) and src/runtime/runtime_os.h
// (gs_os_read_file through gs_os_random_u64 and the helpers they use).
// Original work: Goose, https://github.com/aardappel/goose, by Wouter van
// Oortmerssen, licensed under the Apache License, Version 2.0
// (upstream/goose/LICENSE). Changes: the extern declarations of the os
// functions replaced by Goose functions of the same signatures, ported
// from the POSIX branch of runtime_os.h: stdio, open(2) and opendir(3)
// replaced by the WASI primitives below (gs_wasi_*, which the backend
// implements); a path resolved against WASI's preopened directories as
// wasi-libc resolves it; stdin buffered in a global per thread program, as
// stdio buffers it; the directory a rename lands in not synced, and a new
// atomic file not given its predecessor's permissions, which WASI cannot
// do; resource_dir() unknown, as WASI has no executable to locate.

import std;

// Return unused complete pages of this thread program's data stacks to the
// OS. Live values, limited-array capacity and virtual reservations remain;
// future growth still works. Call after a large workload, not for every
// temporary. False means an OS request failed.
extern fn trim_memory() -> bool = "gs_trim_memory";

// WASI preview 1, thinly (src/wasm/backend.h, WasiBody). Errors are WASI
// errno values, negated where a count comes back otherwise.
extern fn wasi::args(out: u8[>..]&) -> i64 = "gs_wasi_args";          // every argument, NUL-ended; the count or -1
extern fn wasi::environ(out: u8[>..]&) -> i64 = "gs_wasi_environ";    // every NAME=value, NUL-ended
extern fn wasi::clock(id: i32) -> i64 = "gs_wasi_clock";
extern fn wasi::sleep(ns: i64) = "gs_wasi_sleep";
extern fn wasi::random() -> u64 = "gs_wasi_random";                   // 0 where there is none
extern fn wasi::prestat(fd: i32, out: u8[>..]&) -> i32 = "gs_wasi_prestat";
extern fn wasi::open(dirfd: i32, path: const u8[:], oflags: i32, rights: i64, fdflags: i32) -> i64 = "gs_wasi_open";
extern fn wasi::read(fd: i32, out: u8[>..]&) -> i64 = "gs_wasi_read";
extern fn wasi::write(fd: i32, data: const u8[:]) -> i64 = "gs_wasi_write";
extern fn wasi::close(fd: i32) -> i32 = "gs_wasi_close";
extern fn wasi::sync(fd: i32) -> i32 = "gs_wasi_sync";
extern fn wasi::filetype(dirfd: i32, path: const u8[:]) -> i32 = "gs_wasi_filetype";
extern fn wasi::path(op: i32, dirfd: i32, path: const u8[:]) -> i32 = "gs_wasi_path";
extern fn wasi::rename(fromfd: i32, from: const u8[:], tofd: i32, to: const u8[:]) -> i32 = "gs_wasi_rename";
extern fn wasi::readdir(fd: i32, cookie: u64, out: u8[>..]&) -> i64 = "gs_wasi_readdir";

// WASI's numbers: errno values, file types, open flags, rights.
const wasi::EBADF = 8;
const wasi::EEXIST = 20;
const wasi::DIRECTORY = 3;
const wasi::O_CREAT = 1;
const wasi::O_DIRECTORY = 2;
const wasi::O_EXCL = 4;
const wasi::O_TRUNC = 8;
const wasi::FD_APPEND = 1;
// Rights as wasi-libc asks them for O_RDONLY and O_WRONLY, from which a
// host takes the mode it opens a file in: fd_read, fd_seek, fd_tell,
// fd_readdir and fd_filestat_get; fd_datasync, fd_sync, fd_write,
// fd_allocate and fd_filestat_set_size with fd_seek and fd_tell.
const wasi::READ: i64 = 0x20402e;
const wasi::WRITE: i64 = 0x400175;
const wasi::MKDIR = 0;
const wasi::RMDIR = 1;
const wasi::UNLINK = 2;

// GS_OS_PATH_MAX: a path that would not fit, or that holds a NUL byte
// (where C would end it), is refused.
const wasi::PATH_MAX = 4096;
fn wasi::refused(s: const u8[:]) -> bool {
    if s.len >= wasi::PATH_MAX { return true; }
    for c in s { if c == 0 { return true; } }
    return false;
}

// The preopened directories, found once (wasi-libc's preopens.c): each
// one's name, with a leading "./" and a lone "." dropped, as an end offset
// into the names, and its descriptor.
var wasi::pre_names: u8[>..] = [];
var wasi::pre_ends: i64[>..] = [];
var wasi::pre_fds: i32[>..] = [];
var wasi::pre_found = false;

fn wasi::find_preopens() {
    if wasi::pre_found { return; }
    wasi::pre_found = true;
    var fd: i32 = 3;
    loop {
        var name: u8[>..] = [];
        let e = wasi::prestat(fd, name);
        if e == wasi::EBADF { break; }
        if e == 0 {
            // A name as wasi-libc keeps it: without trailing NULs, and
            // without "./" in front; "." alone is the empty prefix.
            var n = name.len;
            while n > 0 && name[n - 1] == 0 { n--; }
            var s: i64 = 0;
            while n - s >= 2 && name[s] == '.' && name[s + 1] == '/' { s += 2; }
            if n - s == 1 && name[s] == '.' { s = n; }
            wasi::pre_names.append(name[s..n]);
            wasi::pre_ends.push(wasi::pre_names.len);
            wasi::pre_fds.push(fd);
        }
        fd++;
    }
}

// Whether preopen name `pre` is a prefix of `path` (wasi-libc's
// prefix_matches): an empty one of a relative path only, "/" of an
// absolute one, else up to a '/' or the end.
fn wasi::prefix_matches(pre: const u8[:], path: const u8[:]) -> bool {
    if pre.len == 0 { return path.len == 0 || path[0] != '/'; }
    if path.len < pre.len { return false; }
    for i in pre.len { if path[i] != pre[i] { return false; } }
    var n = pre.len;
    while n > 0 && pre[n - 1] == '/' { n--; }
    return n == 0 || path.len == pre.len || path[pre.len] == '/' || pre[pre.len - 1] == '/';
}

// The preopened directory a path lies in, and where in the path the part
// below it starts: the longest matching prefix's (wasi-libc's
// __wasilibc_find_relpath). -1 where none does.
fn wasi::resolve(path: const u8[:]) -> i32, i64 {
    wasi::find_preopens();
    var p: i64 = 0;
    while path.len - p >= 2 && path[p] == '.' && path[p + 1] == '/' { p += 2; }
    let rest = path[p..];
    var best: i32 = -1;
    var bestlen: i64 = -1;
    var start: i64 = 0;
    for i in wasi::pre_fds.len {
        let lo = if i == 0 { 0 } else { wasi::pre_ends[i - 1] };
        let pre = wasi::pre_names[lo..wasi::pre_ends[i]];
        if pre.len > bestlen && wasi::prefix_matches(pre, rest) {
            best = wasi::pre_fds[i];
            bestlen = pre.len;
            start = p + pre.len;
        }
    }
    while start < path.len && path[start] == '/' { start++; }
    return best, start;
}

// The part of path below its preopened directory, "." for the directory
// itself.
fn wasi::below(path: const u8[:], start: i64) -> const u8[:] {
    if start >= path.len { return "."; }
    return path[start..];
}

// open(2) of a path: a descriptor, or a negative value.
fn wasi::open_path(path: const u8[:], oflags: i32, rights: i64, fdflags: i32) -> i64 {
    if wasi::refused(path) || path.len == 0 { return -1; }
    let dir, start = wasi::resolve(path);
    if dir < 0 { return -1; }
    return wasi::open(dir, wasi::below(path, start), oflags, rights, fdflags);
}

// stat(2)'s file type, or a negative value.
fn wasi::kind(path: const u8[:]) -> i32 {
    if wasi::refused(path) || path.len == 0 { return -1; }
    let dir, start = wasi::resolve(path);
    if dir < 0 { return -1; }
    return wasi::filetype(dir, wasi::below(path, start));
}

// mkdir(2), rmdir(2), unlink(2): whether it was done.
fn wasi::path_op(op: i32, path: const u8[:]) -> bool {
    if wasi::refused(path) || path.len == 0 { return false; }
    let dir, start = wasi::resolve(path);
    if dir < 0 { return false; }
    return wasi::path(op, dir, wasi::below(path, start)) == 0;
}

// Files. Paths are UTF-8 everywhere, Windows included.

// Appends the whole file; on a failure, nothing.
fn read_file(path: const u8[:], out: u8[>..]&) -> bool {
    let fd = wasi::open_path(path, 0, wasi::READ, 0);
    if fd < 0 { return false; }
    let len = out.len;
    var bad = false;
    loop {
        let n = wasi::read(fd as i32, out);
        if n < 0 { bad = true; break; }
        if n == 0 { break; }
    }
    wasi::close(fd as i32);
    if bad {
        out.pop_n(out.len - len);
        return false;
    }
    return true;
}

// write_file and append_file.
fn wasi::put_file(path: const u8[:], data: const u8[:], append: bool) -> bool {
    let fd = if append { wasi::open_path(path, wasi::O_CREAT, wasi::WRITE, wasi::FD_APPEND) }
             else { wasi::open_path(path, wasi::O_CREAT | wasi::O_TRUNC, wasi::WRITE, 0) };
    if fd < 0 { return false; }
    let ok = wasi::write(fd as i32, data) == data.len;
    let closed = wasi::close(fd as i32) == 0;
    return ok && closed;
}

fn write_file(path: const u8[:], data: const u8[:]) -> bool { return wasi::put_file(path, data, false); }

// All or nothing: a new file beside `path`, flushed to disk and renamed over
// it. Its name is path's with a random tag and ".tmp" added, created only
// if nothing has that name already: one that does belongs to someone else,
// and another tag is drawn.
fn write_file_atomic(path: const u8[:], data: const u8[:]) -> bool {
    if wasi::refused(path) || path.len == 0 { return false; }
    let dir, start = wasi::resolve(path);
    if dir < 0 { return false; }
    let to = wasi::below(path, start);
    for tries in 8 {
        let tag = random_seed();
        var tmp: u8[>..] = [];
        tmp.append(to);
        tmp.push('.');
        var s = 60;
        while s >= 0 {
            tmp.push("0123456789abcdef"[((tag >> s) & 15) as i64]);
            s -= 4;
        }
        tmp.append(".tmp");
        let fd = wasi::open(dir, tmp[..], wasi::O_CREAT | wasi::O_EXCL, wasi::WRITE, 0);
        if fd == -wasi::EEXIST { continue; }
        if fd < 0 { return false; }
        var ok = wasi::write(fd as i32, data) == data.len;
        ok = ok && wasi::sync(fd as i32) == 0;
        if wasi::close(fd as i32) != 0 { ok = false; }
        ok = ok && wasi::rename(dir, tmp[..], dir, to) == 0;
        if !ok { wasi::path(wasi::UNLINK, dir, tmp[..]); }
        return ok;
    }
    return false;
}

fn append_file(path: const u8[:], data: const u8[:]) -> bool { return wasi::put_file(path, data, true); }

// A file, not a directory.
fn file_exists(path: const u8[:]) -> bool {
    let k = wasi::kind(path);
    return k >= 0 && k != wasi::DIRECTORY;
}

// Not rmdir's, which takes an empty directory as well.
fn delete_file(path: const u8[:]) -> bool { return wasi::path_op(wasi::UNLINK, path); }

// Replaces `to`, if there is one, in a single step.
fn rename_file(from: const u8[:], to: const u8[:]) -> bool {
    if wasi::refused(from) || wasi::refused(to) { return false; }
    let fdir, fstart = wasi::resolve(from);
    let tdir, tstart = wasi::resolve(to);
    if fdir < 0 || tdir < 0 { return false; }
    return wasi::rename(fdir, wasi::below(from, fstart), tdir, wasi::below(to, tstart)) == 0;
}

// Directories.

// True if one is there afterwards, one that was there already included.
fn make_dir(path: const u8[:]) -> bool {
    if wasi::path_op(wasi::MKDIR, path) { return true; }
    return is_dir(path);
}

fn is_dir(path: const u8[:]) -> bool { return wasi::kind(path) == wasi::DIRECTORY; }

// An empty one.
fn delete_dir(path: const u8[:]) -> bool { return wasi::path_op(wasi::RMDIR, path); }

// Little-endian fields of a dirent (WASI's: d_next u64, d_ino u64,
// d_namlen u32, d_type u8, then the name).
fn wasi::le(b: u8[>..]&, at: i64, n: i64) -> u64 {
    var v: u64 = 0;
    var i = n;
    while i > 0 {
        i--;
        v = (v << 8) | b[at + i];
    }
    return v;
}

// Bytewise order (strcmp's on names without NULs).
fn wasi::name_less(a: const u8[:], b: const u8[:]) -> bool {
    let n = min(a.len, b.len);
    for i in n {
        if a[i] != b[i] { return a[i] < b[i]; }
    }
    return a.len < b.len;
}

// Appends each entry's name and a '\n', sorted bytewise, "." and ".." left
// out, so that a listing does not depend on the file system; on a failure
// appends nothing. A name with a newline in it cannot be a line of the
// listing, and is left out.
fn list_dir(path: const u8[:], out: u8[>..]&) -> bool {
    if path.len == 0 { return false; }
    let fd = wasi::open_path(path, wasi::O_DIRECTORY, wasi::READ, 0);
    if fd < 0 { return false; }
    var names: u8[>..] = [];
    var ends: i64[>..] = [];
    var buf: u8[>..] = [];
    var cookie: u64 = 0;
    var ok = true;
    loop {
        buf.clear();
        let used = wasi::readdir(fd as i32, cookie, buf);
        if used < 0 { ok = false; break; }
        // The entries whole in the buffer; the next read starts after the
        // last of them.
        var at: i64 = 0;
        var whole = 0;
        while at + 24 <= used {
            let namlen = wasi::le(buf, at + 16, 4) as! i64;
            if at + 24 + namlen > used { break; }
            cookie = wasi::le(buf, at, 8);
            let name = buf[at + 24..at + 24 + namlen];
            let dot = namlen == 1 && name[0] == '.';
            let dotdot = namlen == 2 && name[0] == '.' && name[1] == '.';
            var newline = false;
            for c in name { if c == '\n' { newline = true; } }
            if !dot && !dotdot && !newline {
                names.append(name);
                ends.push(names.len);
            }
            at += 24 + namlen;
            whole++;
        }
        // A buffer that was not filled held the last entries.
        if used < 65536 || whole == 0 { break; }
    }
    wasi::close(fd as i32);
    if !ok { return false; }
    var order: i64[>..] = [];
    for i in ends.len { order.push(i); }
    sort(order[..]) { a, b =>
        let alo = if a == 0 { 0 } else { ends[a - 1] };
        let blo = if b == 0 { 0 } else { ends[b - 1] };
        wasi::name_less(names[alo..ends[a]], names[blo..ends[b]])
    }
    for i in order {
        let lo = if i == 0 { 0 } else { ends[i - 1] };
        out.append(names[lo..ends[i]]);
        out.push('\n');
    }
    return true;
}

// Standard streams.

// What stdio has read of stdin and not handed out yet: a thread program's
// own, as its globals are.
var wasi::inbuf: u8[>..] = [];
var wasi::inpos: i64 = 0;
var wasi::ineof = false;

// More of stdin into the buffer, once it is used up: false at the end.
fn wasi::fill() -> bool {
    if wasi::inpos < wasi::inbuf.len { return true; }
    if wasi::ineof { return false; }
    wasi::inbuf.clear();
    wasi::inpos = 0;
    if wasi::read(0, wasi::inbuf) <= 0 {
        wasi::ineof = true;
        return false;
    }
    return true;
}

// One line of stdin, no newline; false at end. A carriage return before the
// newline is dropped too.
fn read_line(out: u8[>..]&) -> bool {
    if !wasi::fill() { return false; }
    let start = out.len;
    var newline = false;
    while wasi::fill() {
        let c = wasi::inbuf[wasi::inpos];
        wasi::inpos++;
        if c == '\n' {
            newline = true;
            break;
        }
        out.push(c);
    }
    if newline && out.len > start && out[out.len - 1] == '\r' { out.pop(); }
    return true;
}

// Everything until end of input.
fn read_stdin(out: u8[>..]&) {
    while wasi::fill() {
        out.append(wasi::inbuf[wasi::inpos..]);
        wasi::inpos = wasi::inbuf.len;
    }
}

fn write_stdout(s: const u8[:]) { if s.len > 0 { wasi::write(1, s); } }
fn write_stderr(s: const u8[:]) { if s.len > 0 { wasi::write(2, s); } }
// What print and write_stdout write goes out at once: nothing to flush.
fn flush_stdout() {}

// Arguments and environment.
fn arg_count() -> i64 {
    var all: u8[>..] = [];
    let n = wasi::args(all);
    return if n < 0 { 0 } else { n };
}

// Appends argument i (0 = the program).
fn arg(i: i64, out: u8[>..]&) {
    var all: u8[>..] = [];
    let n = wasi::args(all);
    if i < 0 || i >= n { return; }
    var k: i64 = 0;
    var at: i64 = 0;
    for j in all.len {
        if all[j] == 0 {
            if k == i {
                out.append(all[at..j]);
                return;
            }
            k++;
            at = j + 1;
        }
    }
}

fn env(name: const u8[:], out: u8[>..]&) -> bool {
    if wasi::refused(name) { return false; }
    var all: u8[>..] = [];
    if wasi::environ(all) < 0 { return false; }
    var at: i64 = 0;
    for j in all.len {
        if all[j] != 0 { continue; }
        let entry = all[at..j];
        at = j + 1;
        if entry.len <= name.len || entry[name.len] != '=' { continue; }
        var same = true;
        for i in name.len { if entry[i] != name[i] { same = false; } }
        if same {
            out.append(entry[name.len + 1..]);
            return true;
        }
    }
    return false;
}

// Application resources: the entry source's directory in JIT, the executable's
// directory in AOT (Contents/Resources/ in a macOS .app). Absolute UTF-8 with
// a trailing separator, ready to concatenate. Ordinary file paths still use cwd.
// The builder form appends nothing on failure; the string form aborts on failure.
// A WASI program has no executable to find it by: never known here.
fn resource_dir(out: u8[>..]&) -> bool { return false; }
fn resource_dir() -> u8[>..] {
    var out: u8[>..] = [];
    if !resource_dir(out) { abort("cannot locate the application resource directory"); }
    return out;
}

// The bytes behind args(): grow-only, so the slices handed out stay valid
// however many times it is called.
var argv_text: u8[>..] = [];

fn args() -> u8[:][>..] {
    var out: u8[:][>..] = [];
    for i in arg_count() {
        let start = argv_text.len;
        arg(i, argv_text);
        out.push(argv_text[start..]);
    }
    return out;
}

// Time.
fn time_ns() -> i64 { return wasi::clock(0); }    // wall clock, nanoseconds since the epoch
fn clock_ns() -> i64 { return wasi::clock(1); }   // monotonic, high resolution
fn sleep_ms(ms: i64) {
    // A long sleep goes in parts whose nanoseconds fit.
    var left = ms;
    while left > 0 {
        let part = min(left, 9000000000000);
        wasi::sleep(part * 1000000);
        left -= part;
    }
}

// A nanosecond count past 2^53 rounds to the nearest f64. A sleep is whole
// milliseconds, truncated: `as!`, since that drops the fraction on purpose.
fn time() -> f64 { time_ns() / 1000000000.0 }
fn clock() -> f64 { clock_ns() / 1000000000.0 }
fn sleep(seconds: f64) { sleep_ms((seconds * 1000.0) as! i64) }

// Entropy, for seeding an rng: WASI's random_get, where there is one;
// else, as upstream does without /dev/urandom, a splitmix64 mix of the
// clocks and a count of the calls.
fn wasi::mix64(x0: u64) -> u64 {
    var x = x0 + 0x9e3779b97f4a7c15;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9;
    x = (x ^ (x >> 27)) * 0x94d049bb133111eb;
    return x ^ (x >> 31);
}
var wasi::random_count: u64 = 0;
fn random_seed() -> u64 {
    let r = wasi::random();
    if r != 0 { return r; }
    wasi::random_count += 1;
    var seed = time_ns() as! u64;
    seed = wasi::mix64(seed ^ (clock_ns() as! u64));
    return wasi::mix64(seed ^ wasi::random_count);
}
