// A Node.js host for goose-wasm's modules, wasi-threads ones included: a
// second host beside wasmtime (docs/design/threads.md, Hosts), since
// wasmtime 47 and later no longer run wasi-threads.
//
//   node scripts/wasihost.mjs module.wasm [args...]
//
// or, for a module built with --header, from a JS host:
//
//   import { loadReactor } from './wasihost.mjs';
//   const { exports, memory } = loadReactor('lib.wasm');
//
// It runs the module's _start with the current directory preopened as "."
// and no environment, as `wasmtime run --dir . module.wasm` does. A module
// that imports the shared memory (threads.h) gets one; each thread the
// module asks for with wasi.thread-spawn is an instance of its own on a
// Worker, entered at wasi_thread_start. The program runs on a Worker too,
// whose stack can be made as deep as the engine stack wasmtime is given.
//
// WASI preview1 is implemented here for what the backend imports
// (backend.h, ImportWasi; runtime.h), over the memory the module exports.
// Each thread has its own descriptor table: os opens, uses and closes a
// descriptor within one call.
import { Worker, isMainThread, parentPort, workerData } from 'node:worker_threads';
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import { fileURLToPath } from 'node:url';
import v8 from 'node:v8';

// The engine stack of the program's and each thread's Worker, in MB: as
// deep as recursion goes before the module reports the overflow itself
// (README: hosts; difftest gives wasmtime 256 MB). On Windows a Worker's
// stack is committed whole when it starts, so a program with many threads
// takes this much memory per thread; $WASIHOST_STACK_MB sets another size.
const STACK_MB = Number(process.env.WASIHOST_STACK_MB) || 256;
// What abort(3) exits with, which a trap ends the process with.
const TRAP_EXIT = process.platform === 'win32' ? 3 : 134;

const SELF = fileURLToPath(import.meta.url);

if (!isMainThread) {
    if (workerData && workerData.role === 'spawner') runSpawner(workerData);
    else if (workerData && workerData.role === 'thread') runThread(workerData);
} else if (process.argv[1] && path.resolve(process.argv[1]) === SELF) {
    runCommand();
}

// `node wasihost.mjs module.wasm [args...]`: the module as a WASI command,
// its program on a Worker, as are the threads it spawns; this thread starts
// them and ends the process.
function runCommand() {
    const [file, ...rest] = process.argv.slice(2);
    if (!file) {
        process.stderr.write('usage: node wasihost.mjs module.wasm [args...]\n');
        process.exit(2);
    }
    const { module, memory, ctl } = compile(file);
    const common = { module, memory, ctl, args: [file, ...rest], cwd: process.cwd() };
    const workers = new Set();
    let ending = false;
    // Ends the process with every thread: the Workers are stopped, and with
    // nothing left to run the process ends with the code. Not process.exit:
    // on Windows it trips libuv's assertion (async.c, a handle closing)
    // while V8 still compiles the module in the background.
    const end = (code) => {
        if (ending) return;
        ending = true;
        process.exitCode = code;
        for (const w of workers) w.terminate();
    };
    const start = (data) => {
        if (ending) return;
        const w = new Worker(SELF, {
            workerData: { ...common, ...data, role: 'thread' },
            resourceLimits: { stackSizeMb: STACK_MB },
            stdout: false, stderr: false,
        });
        workers.add(w);
        w.on('exit', () => workers.delete(w));
        w.on('message', (m) => {
            if (m.spawn) start(m.spawn);
            else if ('exit' in m) end(m.exit);
        });
        w.on('error', (e) => {
            // A trap, on any thread, ends the process.
            if (!ending) fs.writeSync(2, `Error: wasm trap: ${e && e.message ? e.message : e}\n`);
            end(TRAP_EXIT);
        });
    };
    start({ tid: 0 });
}

// Blocks this thread for good: once it has asked for the process to end,
// nothing more of the module may run on it.
function halt() { for (;;) Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0); }
function sleepMs(ms) { if (ms > 0) Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, ms); }

// The module compiled, and the shared memory it imports, if it does, with
// the limits it declares; ctl[0] is the last thread id issued.
function compile(file) {
    // Exception handling with exnref (Wasm 3.0), which a program with
    // contained workers uses: behind a flag in Node 24, the default later.
    v8.setFlagsFromString('--experimental-wasm-exnref');
    const bytes = fs.readFileSync(file);
    const module = new WebAssembly.Module(bytes);
    const lim = sharedMemoryLimits(bytes);
    const memory = lim ? new WebAssembly.Memory({ initial: lim.min, maximum: lim.max, shared: true }) : null;
    return { module, memory, ctl: new Int32Array(new SharedArrayBuffer(4)) };
}

// The limits of the memory imported as env.memory, from the binary's import
// section (the JS API does not tell them), or null.
function sharedMemoryLimits(b) {
    let p = 8;
    const uleb = () => {
        let r = 0, s = 0, c;
        do { c = b[p++]; r += (c & 0x7f) * 2 ** s; s += 7; } while (c & 0x80);
        return r;
    };
    const name = () => { const n = uleb(); const s = b.subarray(p, p + n).toString('utf8'); p += n; return s; };
    const limits = () => {
        const flags = b[p++];
        const min = uleb();
        return { min, max: flags & 1 ? uleb() : undefined };
    };
    while (p < b.length) {
        const id = b[p++], size = uleb(), end = p + size;
        if (id !== 2) { p = end; continue; }
        for (let n = uleb(); n > 0; n--) {
            const mod = name(), field = name(), kind = b[p++];
            if (kind === 0) uleb();
            else if (kind === 1) { p++; limits(); }
            else if (kind === 2) {
                const l = limits();
                if (mod === 'env' && field === 'memory') return l;
            } else if (kind === 3) p += 2;
            else if (kind === 4) { p++; uleb(); }
        }
        return null;
    }
    return null;
}

// An instance of the module for one thread, with WASI over the memory it
// exports as `memory`; spawn({ tid, arg }) starts a thread, exit(code)
// ends the process.
function instantiate({ module, memory, ctl, args, cwd, spawn, exit }) {
    let exports = null;
    const mem = () => exports.memory.buffer;
    const view = () => new DataView(mem());
    const bytes = (p, n) => Buffer.from(new Uint8Array(mem(), p, n));  // a copy
    const put = (p, b) => new Uint8Array(mem(), p, b.length).set(b);

    // errno values
    const E = { SUCCESS: 0, ACCES: 2, BADF: 8, EXIST: 20, INVAL: 28, IO: 29, ISDIR: 31, LOOP: 32,
                NAMETOOLONG: 37, NOENT: 44, NOSPC: 51, NOSYS: 52, NOTDIR: 54, NOTEMPTY: 55,
                PERM: 63, NOTCAPABLE: 76, XDEV: 75, BUSY: 10 };
    const fromNode = (e) => ({
        ENOENT: E.NOENT, EEXIST: E.EXIST, ENOTDIR: E.NOTDIR, EISDIR: E.ISDIR, ENOTEMPTY: E.NOTEMPTY,
        EACCES: E.ACCES, EPERM: E.PERM, EBADF: E.BADF, EINVAL: E.INVAL, ELOOP: E.LOOP,
        ENAMETOOLONG: E.NAMETOOLONG, ENOSPC: E.NOSPC, EXDEV: E.XDEV, EBUSY: E.BUSY,
    })[e && e.code] ?? E.IO;
    const guard = (f) => { try { return f(); } catch (e) { return fromNode(e); } };

    // Descriptors: 0-2 the process's, 3 the preopened ".".
    const root = path.resolve(cwd);
    const fds = new Map([
        [0, { kind: 'stdio', fd: 0 }], [1, { kind: 'stdio', fd: 1 }], [2, { kind: 'stdio', fd: 2 }],
        [3, { kind: 'dir', path: root, preopen: '.' }],
    ]);
    let nextFd = 4;
    const newFd = (entry) => { const fd = nextFd++; fds.set(fd, entry); return fd; };
    // A path under a directory descriptor, or an errno: one that leaves
    // the preopened directory is refused, as wasmtime refuses it.
    const resolve = (dirfd, p, n) => {
        const d = fds.get(dirfd);
        if (!d) return E.BADF;
        if (d.kind !== 'dir') return E.NOTDIR;
        const rel = bytes(p, n).toString('utf8');
        if (rel.includes('\0')) return E.INVAL;
        if (path.isAbsolute(rel) || /^[\\/]/.test(rel)) return E.NOTCAPABLE;
        const full = path.resolve(d.path, rel);
        const back = path.relative(root, full);
        if (back === '..' || back.startsWith('..' + path.sep) || path.isAbsolute(back)) return E.NOTCAPABLE;
        return full;
    };
    const fileType = (st) => st.isDirectory() ? 3 : st.isFile() ? 4 : st.isSymbolicLink() ? 7 : 0;
    // The (pointer, length) pairs of an iovec array.
    const iovecs = (iovs, n) => {
        const v = view(), out = [];
        for (let i = 0; i < n; i++) out.push([v.getUint32(iovs + 8 * i, true), v.getUint32(iovs + 8 * i + 4, true)]);
        return out;
    };
    const writeAll = (fd, b) => {
        for (let off = 0; off < b.length;) {
            try { off += fs.writeSync(fd, b, off); } catch (e) { if (e.code !== 'EAGAIN') throw e; }
        }
    };
    const strings = (list) => list.map(s => Buffer.from(s + '\0', 'utf8'));
    const argBufs = strings(args), envBufs = [];
    const sizes = (bufs, countp, sizep) => {
        view().setUint32(countp, bufs.length, true);
        view().setUint32(sizep, bufs.reduce((a, b) => a + b.length, 0), true);
        return E.SUCCESS;
    };
    const fill = (bufs, ptrs, buf) => {
        for (const b of bufs) {
            view().setUint32(ptrs, buf, true);
            put(buf, b);
            ptrs += 4;
            buf += b.length;
        }
        return E.SUCCESS;
    };
    const now = (id) => {
        if (id === 0) return BigInt(Date.now()) * 1000000n + BigInt(Math.floor((performance.now() % 1) * 1e6));
        if (id === 1) return process.hrtime.bigint();
        if (id === 2 || id === 3) return BigInt(process.cpuUsage().user + process.cpuUsage().system) * 1000n;
        return null;
    };

    const wasi = {
        args_sizes_get: (c, s) => sizes(argBufs, c, s),
        args_get: (p, b) => fill(argBufs, p, b),
        environ_sizes_get: (c, s) => sizes(envBufs, c, s),
        environ_get: (p, b) => fill(envBufs, p, b),
        clock_time_get(id, _precision, out) {
            const t = now(id);
            if (t === null) return E.INVAL;
            view().setBigUint64(out, t, true);
            return E.SUCCESS;
        },
        poll_oneoff(subs, events, n, neventsp) {
            // Clock subscriptions sleep until the earliest; others are ready.
            const v = view();
            let wait = Infinity;
            for (let i = 0; i < n; i++) {
                const s = subs + 48 * i;
                if (v.getUint8(s + 8) !== 0) { wait = 0; continue; }
                const id = v.getUint32(s + 16, true), t = v.getBigUint64(s + 24, true);
                const abs = (v.getUint16(s + 40, true) & 1) !== 0;
                const c = now(id) ?? 0n;
                const ns = abs ? t - c : t;
                wait = Math.min(wait, Number(ns > 0n ? ns : 0n) / 1e6);
            }
            if (wait !== Infinity) sleepMs(wait);
            for (let i = 0; i < n; i++) {
                const s = subs + 48 * i, e = events + 32 * i;
                const w = view();
                w.setBigUint64(e, w.getBigUint64(s, true), true);
                w.setUint16(e + 8, 0, true);
                w.setUint8(e + 10, w.getUint8(s + 8));
                for (let k = 11; k < 32; k++) w.setUint8(e + k, 0);
            }
            view().setUint32(neventsp, n, true);
            return E.SUCCESS;
        },
        random_get(p, n) { put(p, crypto.randomBytes(n)); return E.SUCCESS; },
        fd_prestat_get(fd, out) {
            const d = fds.get(fd);
            if (!d || !d.preopen) return E.BADF;
            view().setUint8(out, 0);
            view().setUint32(out + 4, Buffer.byteLength(d.preopen), true);
            return E.SUCCESS;
        },
        fd_prestat_dir_name(fd, p, n) {
            const d = fds.get(fd);
            if (!d || !d.preopen) return E.BADF;
            put(p, Buffer.from(d.preopen).subarray(0, n));
            return E.SUCCESS;
        },
        path_open(dirfd, _dirflags, p, n, oflags, rights, _inherit, fdflags, out) {
            const full = resolve(dirfd, p, n);
            if (typeof full === 'number') return full;
            return guard(() => {
                const write = (rights & 0x40n) !== 0n;  // RIGHTS_FD_WRITE
                const read = (rights & 0x2n) !== 0n;    // RIGHTS_FD_READ
                let st = null;
                try { st = fs.statSync(full); } catch (e) { if (e.code !== 'ENOENT') throw e; }
                if (oflags & 2) {  // O_DIRECTORY
                    if (!st) return E.NOENT;
                    if (!st.isDirectory()) return E.NOTDIR;
                }
                if (st && st.isDirectory()) {
                    if ((oflags & 5) === 5) return E.EXIST;
                    if (write || (oflags & 8)) return E.ISDIR;
                    view().setUint32(out, newFd({ kind: 'dir', path: full }), true);
                    return E.SUCCESS;
                }
                const c = fs.constants;
                let flags = write ? (read ? c.O_RDWR : c.O_WRONLY) : c.O_RDONLY;
                if (oflags & 1) flags |= c.O_CREAT;
                if (oflags & 4) flags |= c.O_EXCL;
                if (oflags & 8) flags |= c.O_TRUNC;
                if (fdflags & 1) flags |= c.O_APPEND;
                const fd = fs.openSync(full, flags);
                view().setUint32(out, newFd({ kind: 'file', fd, path: full }), true);
                return E.SUCCESS;
            });
        },
        fd_read(fd, iovs, n, outp) {
            const d = fds.get(fd);
            if (!d) return E.BADF;
            if (d.kind === 'dir') return E.ISDIR;
            return guard(() => {
                let total = 0;
                for (const [p, len] of iovecs(iovs, n)) {
                    if (!len) continue;
                    const b = Buffer.alloc(len);
                    let got;
                    try { got = fs.readSync(d.fd, b, 0, len, null); }
                    catch (e) { if (e.code === 'EOF' || (d.kind === 'stdio' && e.code === 'EAGAIN')) got = 0; else throw e; }
                    put(p, b.subarray(0, got));
                    total += got;
                    if (got < len) break;
                }
                view().setUint32(outp, total, true);
                return E.SUCCESS;
            });
        },
        fd_write(fd, iovs, n, outp) {
            const d = fds.get(fd);
            if (!d) return E.BADF;
            if (d.kind === 'dir') return E.ISDIR;
            return guard(() => {
                const parts = iovecs(iovs, n).map(([p, len]) => bytes(p, len));
                const b = Buffer.concat(parts);
                writeAll(d.fd, b);
                view().setUint32(outp, b.length, true);
                return E.SUCCESS;
            });
        },
        fd_close(fd) {
            const d = fds.get(fd);
            if (!d) return E.BADF;
            fds.delete(fd);
            return d.kind === 'file' ? guard(() => { fs.closeSync(d.fd); return E.SUCCESS; }) : E.SUCCESS;
        },
        fd_sync(fd) {
            const d = fds.get(fd);
            if (!d) return E.BADF;
            return d.kind === 'file' ? guard(() => { fs.fsyncSync(d.fd); return E.SUCCESS; }) : E.SUCCESS;
        },
        path_filestat_get(dirfd, flags, p, n, out) {
            const full = resolve(dirfd, p, n);
            if (typeof full === 'number') return full;
            return guard(() => {
                const st = (flags & 1) ? fs.statSync(full, { bigint: true }) : fs.lstatSync(full, { bigint: true });
                const v = view();
                for (let k = 0; k < 64; k++) v.setUint8(out + k, 0);
                v.setBigUint64(out, BigInt.asUintN(64, st.dev), true);
                v.setBigUint64(out + 8, BigInt.asUintN(64, st.ino), true);
                v.setUint8(out + 16, fileType(st));
                v.setBigUint64(out + 24, BigInt.asUintN(64, st.nlink), true);
                v.setBigUint64(out + 32, BigInt.asUintN(64, st.size), true);
                v.setBigUint64(out + 40, BigInt.asUintN(64, st.atimeNs), true);
                v.setBigUint64(out + 48, BigInt.asUintN(64, st.mtimeNs), true);
                v.setBigUint64(out + 56, BigInt.asUintN(64, st.ctimeNs), true);
                return E.SUCCESS;
            });
        },
        path_create_directory(dirfd, p, n) {
            const full = resolve(dirfd, p, n);
            return typeof full === 'number' ? full : guard(() => { fs.mkdirSync(full); return E.SUCCESS; });
        },
        path_remove_directory(dirfd, p, n) {
            const full = resolve(dirfd, p, n);
            if (typeof full === 'number') return full;
            if (full === root) return E.BUSY;
            return guard(() => {
                if (!fs.lstatSync(full).isDirectory()) return E.NOTDIR;
                fs.rmdirSync(full);
                return E.SUCCESS;
            });
        },
        path_unlink_file(dirfd, p, n) {
            const full = resolve(dirfd, p, n);
            if (typeof full === 'number') return full;
            return guard(() => {
                if (fs.lstatSync(full).isDirectory()) return E.ISDIR;
                fs.unlinkSync(full);
                return E.SUCCESS;
            });
        },
        path_rename(fromfd, fp, fn, tofd, tp, tn) {
            const from = resolve(fromfd, fp, fn), to = resolve(tofd, tp, tn);
            if (typeof from === 'number') return from;
            if (typeof to === 'number') return to;
            return guard(() => { fs.renameSync(from, to); return E.SUCCESS; });
        },
        fd_readdir(fd, buf, len, cookie, usedp) {
            const d = fds.get(fd);
            if (!d) return E.BADF;
            if (d.kind !== 'dir') return E.NOTDIR;
            return guard(() => {
                if (!d.entries) {
                    d.entries = [{ name: '.', type: 3 }, { name: '..', type: 3 },
                        ...fs.readdirSync(d.path, { withFileTypes: true }).map(e => ({
                            name: e.name, type: e.isDirectory() ? 3 : e.isFile() ? 4 : e.isSymbolicLink() ? 7 : 0 }))];
                }
                // Dirents of 24 bytes and the name, as many as fit; the
                // last cut short, as WASI has a reader ask again.
                const out = [];
                for (let i = Number(cookie); i < d.entries.length; i++) {
                    const e = d.entries[i], name = Buffer.from(e.name, 'utf8');
                    const h = Buffer.alloc(24);
                    h.writeBigUInt64LE(BigInt(i + 1), 0);
                    h.writeUInt32LE(name.length, 16);
                    h.writeUInt8(e.type, 20);
                    out.push(h, name);
                    if (out.reduce((a, b) => a + b.length, 0) >= len) break;
                }
                const all = Buffer.concat(out).subarray(0, len);
                put(buf, all);
                view().setUint32(usedp, all.length, true);
                return E.SUCCESS;
            });
        },
        proc_exit(code) {
            exit(code);
            halt();
        },
    };
    const importObject = {
        wasi_snapshot_preview1: wasi,
        wasi: {
            'thread-spawn'(startArg) {
                const id = Atomics.add(ctl, 0, 1) + 1;
                if (id > 0x1fffffff) return -1;
                spawn({ tid: id, arg: startArg });
                return id;
            },
        },
    };
    if (memory) importObject.env = { memory };
    const instance = new WebAssembly.Instance(module, importObject);
    exports = instance.exports;
    return instance;
}

// A Worker running one thread of the module: the program's (tid 0), whose
// _start it calls, or one wasi.thread-spawn asked for. It asks its parent,
// the main thread or a reactor's spawner, for more threads and to end the
// process.
function runThread(data) {
    const exports = instantiate({
        ...data,
        spawn: (s) => parentPort.postMessage({ spawn: s }),
        exit: (code) => parentPort.postMessage({ exit: code }),
    }).exports;
    if (data.tid === 0) {
        exports._start();
        parentPort.postMessage({ exit: 0 });
        halt();
    } else {
        exports.wasi_thread_start(data.tid, data.arg);
    }
}

// A reactor's spawner: a Worker that starts the threads the module asks
// for, on Workers of its own. A host's thread may be blocked in an export
// (thread_wait, qget) while a thread spawns another, so spawns cannot wait
// for the host's thread; neither can the end of the process, which is
// therefore a signal's: on Windows it exits with 1.
function runSpawner(common) {
    const kill = () => process.kill(process.pid, 'SIGTERM');
    const start = (data) => {
        const w = new Worker(SELF, {
            workerData: { ...common, ...data, role: 'thread' },
            resourceLimits: { stackSizeMb: STACK_MB },
            stdout: false, stderr: false,
        });
        w.on('message', (m) => {
            if (m.spawn) start(m.spawn);
            else if ('exit' in m) kill();
        });
        w.on('error', (e) => {
            fs.writeSync(2, `Error: wasm trap: ${e && e.message ? e.message : e}\n`);
            kill();
        });
    };
    parentPort.on('message', (m) => { if (m.spawn) start(m.spawn); });
}

// The module at `file` as a WASI reactor (goose-wasm --header), on the
// calling thread: its instance, its exports, and the memory the exports'
// addresses are in (gs_memory). The host calls goose_init, then the
// exports. With workers or queues, a spawner Worker starts the threads; a
// proc_exit on the host's thread ends the process with its code.
export function loadReactor(file, args = []) {
    const { module, memory, ctl } = compile(file);
    const common = { module, memory, ctl, args: [file, ...args], cwd: process.cwd() };
    let spawner = null;
    if (memory) {
        spawner = new Worker(SELF, { workerData: { ...common, role: 'spawner' }, stdout: false, stderr: false });
        // The host's own end is the process's, threads or not.
        spawner.unref();
    }
    const instance = instantiate({
        ...common,
        spawn: (s) => spawner.postMessage({ spawn: s }),
        exit: (code) => process.exit(code),
    });
    const x = instance.exports;
    return { instance, exports: x, memory: x.gs_memory ?? x.memory };
}
