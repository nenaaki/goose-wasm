// Workers and typed queues (spec §11.2) on Wasm threads and wasi-threads
// (docs/design/threads.md): what upstream's runtime_threads.h does with
// native threads, a mutex and condition variables, written as Wasm
// instructions over the module's imported shared memory.
//
// Every thread is an instance of the module (wasi-threads), so what the
// module defines -- memory 0, the data stacks' memories, Wasm globals -- is
// the thread's own, and only the shared memory is common. It holds the
// runtime's state at fixed addresses (shm below), then a heap from which
// spawn packets, queue nodes and abort messages are allocated, as upstream
// mallocs them.
//
// One lock guards all of it: a futex-style mutex on a word of the shared
// memory (memory.atomic.wait32 / notify). Each condition upstream waits on
// is a sequence word here, bumped and notified under the lock; a waiter
// reads it under the lock, unlocks, and waits for it to change.
#pragma once

#include "handles.h"

namespace goosewasm {

namespace shm {
constexpr uint32_t Lock = 0;       // i32: 0 free, 1 held, 2 held with waiters
constexpr uint32_t DoneSeq = 4;    // i32: bumped as each worker ends (gs_threads_done)
constexpr uint32_t NextId = 8;     // i64: the ids issued so far (gs_numthreads)
constexpr uint32_t Brk = 16;       // i32: the heap's end
constexpr uint32_t States = 20;    // i32: a byte per issued id, see below
constexpr uint32_t StatesCap = 24; // i32: its capacity in bytes
constexpr uint32_t Aborted = 28;   // i32: the aborted contained workers' messages no wait took
constexpr uint32_t Free = 32;      // i32 [Classes]: free blocks by size class
constexpr uint32_t Classes = 28;   // blocks of 16 << c bytes, up to 2 GB
constexpr uint32_t Queues = Free + 4 * Classes;  // QueueSize bytes per queue
constexpr uint32_t QueueSize = 32;
// A queue: its first and last node, the sequence its receivers wait on,
// its count and its limit (gs_qstate).
constexpr uint32_t QHead = 0, QTail = 4, QSeq = 8, QCount = 16, QLimit = 24;
// A queue node: the next one, the image's size, the image (gs_qnode).
constexpr uint32_t NodeNext = 0, NodeSize = 8, NodeData = 16;
// A thread's state byte: running, returned, or a contained worker that
// aborted.
constexpr uint8_t Running = 0, Returned = 1, AbortedState = 2;
// A spawned worker's record, handed to wasi_thread_start: its id, its entry
// thunk's number, whether it is contained, its packet and the packet's size.
constexpr uint32_t RecId = 0, RecThunk = 8, RecContained = 12, RecPacket = 16, RecSize = 20, RecBytes = 24;
// An aborted worker's message: the next record, the id, the message.
constexpr uint32_t AbNext = 0, AbId = 8, AbMsg = 16, AbLen = 20, AbBytes = 24;
// Each thread's area for WASI calls, which read and write only the shared
// memory: an iovec at 0, a result at 8, data from 16.
constexpr uint32_t XferData = 16;
constexpr uint32_t XferSize = 16 + (1 << 16);
}  // namespace shm

struct Threads {
    using V = ValType;

    uint32_t nqueues = 0;
    bool contained = false;  // the program starts contained workers
    bool clock = false;      // it waits with a timeout

    // Imports.
    uint32_t spawnimport = 0;  // wasi.thread-spawn (arg i32) -> tid i32
    uint32_t clockimport = 0;  // wasi_snapshot_preview1.clock_time_get

    // The thread's own state, Wasm globals of each instance.
    uint32_t gtid = 0;       // i64: the thread's id, -1 on main's (gs_current_thread_id)
    uint32_t gxfer = 0;      // i32: its WASI area in the shared memory
    uint32_t gcontained = 0; // i32: an abort ends only this worker
    uint32_t gmsg = 0, gmsglen = 0;  // i32: a message handed over (an abort's, a wait's)
    uint32_t tag = 0;        // the exception a contained worker's abort throws

    // Functions.
    uint32_t lock = 0, unlock = 0, condwait = 0, condwaitns = 0, broadcast = 0;
    uint32_t alloc = 0, release = 0, malloc = 0, mfree = 0, xalloc = 0;
    uint32_t spawn = 0, start = 0, wait = 0, now = 0, contain = 0, init = 0;
    uint32_t qput = 0, qget = 0, qwait = 0, qlimit = 0;
    // The workers' entry thunks by number: each a function (sp i32, packet
    // i32), and the first indexed stack its worker's program uses.
    struct Thunk {
        uint32_t index, sp;
    };
    std::vector<Thunk> thunks;

    uint32_t HeapStart() const { return (shm::Queues + nqueues * shm::QueueSize + 15) / 16 * 16; }
    static uint32_t QueueAt(uint32_t q) { return shm::Queues + q * shm::QueueSize; }

    void Import(Module &m) {
        spawnimport = m.ImportFunc("wasi", "thread-spawn", { { V::I32 }, { V::I32 } });
        if (clock)
            clockimport = m.ImportFunc("wasi_snapshot_preview1", "clock_time_get",
                                       { { V::I32, V::I64, V::I32 }, { V::I32 } });
        m.memimports.push_back(MemImport { "env", "memory", Memory { 1, true, 65536, true } });
    }

    // The globals and the function indices; bodies follow in Define, once
    // the thunks exist.
    void Reserve(Module &m) {
        gtid = m.AddGlobal(V::I64, true, -1);
        gxfer = m.AddGlobal(V::I32, true, 0);
        gcontained = m.AddGlobal(V::I32, true, 0);
        gmsg = m.AddGlobal(V::I32, true, 0);
        gmsglen = m.AddGlobal(V::I32, true, 0);
        if (contained) {
            m.tags.push_back(m.TypeIndex({}));
            tag = 0;
        }
        lock = m.AddFunc({});
        unlock = m.AddFunc({});
        condwait = m.AddFunc({ { V::I32 }, {} }, { V::I32 });
        condwaitns = m.AddFunc({ { V::I32, V::I64 }, {} }, { V::I32 });
        broadcast = m.AddFunc({ { V::I32 }, {} });
        alloc = m.AddFunc({ { V::I32 }, { V::I32 } }, { V::I32, V::I32, V::I32, V::I64 });
        release = m.AddFunc({ { V::I32 }, {} }, { V::I32 });
        malloc = m.AddFunc({ { V::I32, V::I32, V::I32 }, { V::I32 } }, { V::I32 });
        xalloc = m.AddFunc({ { V::I32, V::I32, V::I32 }, { V::I32 } }, { V::I32 });
        mfree = m.AddFunc({ { V::I32 }, {} });
        init = m.AddFunc({});
        spawn = m.AddFunc({ { V::I32, V::I32, V::I32, V::I32 }, { V::I64 } }, { V::I64, V::I32, V::I32, V::I32 });
        start = m.AddFunc({ { V::I32, V::I32 }, {} }, { V::I64, V::I32, V::I32 });
        wait = m.AddFunc({ { V::I64, V::I32, V::I32, V::I32, V::I32 }, { V::I32 } }, { V::I32, V::I32, V::I32 });
        now = m.AddFunc({ {}, { V::I64 } });
        contain = m.AddFunc({ { V::I32, V::I32 }, {} });
        qput = m.AddFunc({ { V::I32, V::I32 }, { V::I32 } }, { V::I32 });
        qget = m.AddFunc({ { V::I32 }, { V::I32 } }, { V::I32 });
        qwait = m.AddFunc({ { V::I32, V::I64 }, { V::I32 } }, { V::I64, V::I32 });
        qlimit = m.AddFunc({ { V::I32, V::I64, V::I32, V::I32, V::I32 }, {} });
    }

    // Texts the runtime's failures print, in static data: the heap running
    // out while spawning, putting or recording an abort, a refused spawn,
    // and the two wait errors and the queue bound's (upstream's gs_panic
    // calls and gs_errmsgs).
    struct Texts {
        uint32_t oomspawn, oomspawnlen, oomqput, oomqputlen, oomabort, oomabortlen, nothread, nothreadlen,
            threadid, threadidlen, threadself, threadselflen, qbound, qboundlen, idspace, idspacelen;
    };
    static void Msg(Code &c, uint32_t text, uint32_t len) {
        c.I32Const(int32_t(text));
        c.I32Const(int32_t(len));
    }

    void Define(Module &m, Runtime &rt, const Texts &tx) {
        // lock(): Drepper's mutex ("Futexes Are Tricky", mutex 2).
        {
            auto &c = m.FuncAt(lock).code;
            c.I32Const(shm::Lock);
            c.I32Const(0);
            c.I32Const(1);
            c.I32AtomicCmpxchg();
            c.I32Eqz();
            c.If();
            c.Return();
            c.End();
            c.Loop();
            c.I32Const(shm::Lock);
            c.I32Const(2);
            c.I32AtomicXchg();
            c.I32Eqz();
            c.If();
            c.Return();
            c.End();
            c.I32Const(shm::Lock);
            c.I32Const(2);
            c.I64Const(-1);
            c.AtomicWait32();
            c.Drop();
            c.Br(0);
            c.End();
        }
        // unlock(): wakes one waiter if there may be any.
        {
            auto &c = m.FuncAt(unlock).code;
            c.I32Const(shm::Lock);
            c.I32Const(1);
            c.I32AtomicSub();
            c.I32Const(1);
            c.I32Ne();
            c.If();
            c.I32Const(shm::Lock);
            c.I32Const(0);
            c.I32AtomicStore();
            c.I32Const(shm::Lock);
            c.I32Const(1);
            c.AtomicNotify();
            c.Drop();
            c.End();
        }
        // condwait(seq): under the lock, waits for seq to change, which a
        // broadcast does; may also return spuriously, as a condition
        // variable may.
        {
            auto &c = m.FuncAt(condwait).code;
            enum { seq, v };
            c.LocalGet(seq);
            c.I32AtomicLoad();
            c.LocalSet(v);
            c.Call(unlock);
            c.LocalGet(seq);
            c.LocalGet(v);
            c.I64Const(-1);
            c.AtomicWait32();
            c.Drop();
            c.Call(lock);
        }
        // condwaitns(seq, ns): the same, giving up after ns nanoseconds.
        {
            auto &c = m.FuncAt(condwaitns).code;
            enum { seq, ns, v };
            c.LocalGet(seq);
            c.I32AtomicLoad();
            c.LocalSet(v);
            c.Call(unlock);
            c.LocalGet(seq);
            c.LocalGet(v);
            c.LocalGet(ns);
            c.AtomicWait32();
            c.Drop();
            c.Call(lock);
        }
        // broadcast(seq): under the lock, wakes every waiter on seq.
        {
            auto &c = m.FuncAt(broadcast).code;
            c.LocalGet(0);
            c.I32Const(1);
            c.I32AtomicAdd();
            c.Drop();
            c.LocalGet(0);
            c.I32Const(-1);
            c.AtomicNotify();
            c.Drop();
        }
        // alloc(n) -> p, under the lock: a block of the smallest class whose
        // 16 << c bytes hold n after the block's 8-byte header, from the
        // class's free list, else from the heap's end, growing the shared
        // memory; 0 when it cannot. The header holds the class, and the
        // next free block while the block is free.
        {
            auto &c = m.FuncAt(alloc).code;
            enum { n, cls, p, size, end };
            c.I32Const(0);
            c.LocalSet(cls);
            c.Block();
            c.Loop();
            c.I64Const(16);
            c.LocalGet(cls);
            c.I64ExtendI32U();
            c.Int(intop::Shl, true);
            c.I64Const(8);
            c.I64Sub();
            c.LocalGet(n);
            c.I64ExtendI32U();
            c.Int(intop::GeU, true);
            c.BrIf(1);
            c.LocalGet(cls);
            c.I32Const(1);
            c.I32Add();
            c.LocalTee(cls);
            c.I32Const(int32_t(shm::Classes));
            c.Int(intop::GeU, false);
            c.If();
            c.I32Const(0);
            c.Return();
            c.End();
            c.Br(0);
            c.End();
            c.End();
            // From the free list.
            c.LocalGet(cls);
            c.I32Const(2);
            c.Int(intop::Shl, false);
            c.I32Load(shm::Free, SharedMem);
            c.LocalTee(p);
            c.If();
            c.LocalGet(cls);
            c.I32Const(2);
            c.Int(intop::Shl, false);
            c.LocalGet(p);
            c.I32Load(4, SharedMem);
            c.I32Store(shm::Free, SharedMem);
            c.LocalGet(p);
            c.I32Const(8);
            c.I32Add();
            c.Return();
            c.End();
            // From the heap's end, below 4 GB.
            c.I32Const(16);
            c.LocalGet(cls);
            c.Int(intop::Shl, false);
            c.LocalSet(size);
            c.I32Const(0);
            c.I32Load(shm::Brk, SharedMem);
            c.LocalTee(p);
            c.I64ExtendI32U();
            c.LocalGet(size);
            c.I64ExtendI32U();
            c.Int(intop::Add, true);
            c.LocalTee(end);
            c.I64Const(int64_t(1) << 32);
            c.Int(intop::GeU, true);
            c.If();
            c.I32Const(0);
            c.Return();
            c.End();
            c.LocalGet(end);
            c.MemorySize(SharedMem);
            c.I64ExtendI32U();
            c.I64Const(16);
            c.Int(intop::Shl, true);
            c.Int(intop::GtU, true);
            c.If();
            c.LocalGet(end);
            c.I64Const(0xffff);
            c.Int(intop::Add, true);
            c.I64Const(16);
            c.Int(intop::ShrU, true);
            c.I32WrapI64();
            c.MemorySize(SharedMem);
            c.I32Sub();
            c.MemoryGrow(SharedMem);
            c.I32Const(-1);
            c.Int(intop::Eq, false);
            c.If();
            c.I32Const(0);
            c.Return();
            c.End();
            c.End();
            c.I32Const(0);
            c.LocalGet(end);
            c.I32WrapI64();
            c.I32Store(shm::Brk, SharedMem);
            c.LocalGet(p);
            c.LocalGet(cls);
            c.I32Store(0, SharedMem);
            c.LocalGet(p);
            c.I32Const(8);
            c.I32Add();
        }
        // release(p), under the lock: onto its class's free list; null is
        // nothing.
        {
            auto &c = m.FuncAt(release).code;
            enum { p, slot };
            c.LocalGet(p);
            c.I32Eqz();
            c.If();
            c.Return();
            c.End();
            c.LocalGet(p);
            c.I32Const(8);
            c.I32Sub();
            c.LocalTee(p);
            c.I32Load(0, SharedMem);
            c.I32Const(2);
            c.Int(intop::Shl, false);
            c.I32Const(shm::Free);
            c.I32Add();
            c.LocalSet(slot);
            c.LocalGet(p);
            c.LocalGet(slot);
            c.I32Load(0, SharedMem);
            c.I32Store(4, SharedMem);
            c.LocalGet(slot);
            c.LocalGet(p);
            c.I32Store(0, SharedMem);
        }
        // malloc(n, msg, len) and free, taking the lock. Where malloc finds
        // no room, the program ends with msg, as upstream's callers of
        // malloc end it (gs_panic).
        {
            auto &c = m.FuncAt(malloc).code;
            enum { n, msg, len, p };
            c.Call(lock);
            c.LocalGet(n);
            c.Call(alloc);
            c.LocalSet(p);
            c.Call(unlock);
            c.LocalGet(p);
            c.I32Eqz();
            c.If();
            c.LocalGet(msg);
            c.LocalGet(len);
            c.Call(rt.panic);
            c.End();
            c.LocalGet(p);
        }
        // xalloc(n, msg, len), under the lock: alloc, or the same end.
        {
            auto &c = m.FuncAt(xalloc).code;
            enum { n, msg, len, p };
            c.LocalGet(n);
            c.Call(alloc);
            c.LocalTee(p);
            c.I32Eqz();
            c.If();
            c.LocalGet(msg);
            c.LocalGet(len);
            c.Call(rt.panic);
            c.End();
            c.LocalGet(p);
        }
        {
            auto &c = m.FuncAt(mfree).code;
            c.Call(lock);
            c.LocalGet(0);
            c.Call(release);
            c.Call(unlock);
        }
        DefineWorkers(m, rt, tx);
        DefineQueues(m, rt, tx);
    }

    void DefineWorkers(Module &m, Runtime &rt, const Texts &tx) {
        // init(), as main starts: the heap, the queues' limits (none: the
        // largest count), main's WASI area.
        {
            auto &c = m.FuncAt(init).code;
            c.I32Const(0);
            c.I32Const(int32_t(HeapStart()));
            c.I32Store(shm::Brk, SharedMem);
            for (uint32_t q = 0; q < nqueues; q++) {
                c.I32Const(int32_t(QueueAt(q)));
                c.I64Const(INT64_MAX);
                c.I64Store(shm::QLimit, SharedMem);
            }
            c.I32Const(int32_t(shm::XferSize));
            Msg(c, tx.oomspawn, tx.oomspawnlen);
            c.Call(malloc);
            c.GlobalSet(gxfer);
        }
        // spawn(thunk, contained, packet, size) -> id (gs_thread_start): the
        // id and the worker's state byte, issued under the lock, then the
        // thread. The packet passes to the worker, which frees it.
        {
            auto &c = m.FuncAt(spawn).code;
            enum { thunk, cont, packet, size, id, rec, cap, nw };
            c.Call(lock);
            c.I32Const(0);
            c.I64Load(shm::NextId, SharedMem);
            c.LocalTee(id);
            c.I64Const(INT32_MAX);  // the states are indexed by an i32
            c.Int(intop::GeS, true);
            c.If();
            Panic(c, rt, tx.idspace, tx.idspacelen);
            c.End();
            c.I32Const(0);
            c.LocalGet(id);
            c.I64Const(1);
            c.Int(intop::Add, true);
            c.I64Store(shm::NextId, SharedMem);
            // The states, doubled (from 64) until the id fits; new ones
            // are Running.
            c.I32Const(0);
            c.I32Load(shm::StatesCap, SharedMem);
            c.LocalTee(cap);
            c.I64ExtendI32U();
            c.LocalGet(id);
            c.Int(intop::LeS, true);
            c.If();
            c.LocalGet(cap);
            c.LocalSet(nw);
            c.Block();
            c.Loop();
            c.LocalGet(nw);
            c.I64ExtendI32U();
            c.LocalGet(id);
            c.Int(intop::GtS, true);
            c.BrIf(1);
            // nw = nw >= 64 ? nw * 2 : 64
            c.LocalGet(nw);
            c.I32Const(1);
            c.Int(intop::Shl, false);
            c.I32Const(64);
            c.LocalGet(nw);
            c.I32Const(64);
            c.Int(intop::GeU, false);
            c.Select();
            c.LocalSet(nw);
            c.Br(0);
            c.End();
            c.End();
            c.LocalGet(nw);
            Msg(c, tx.oomspawn, tx.oomspawnlen);
            c.Call(xalloc);
            c.LocalTee(rec);  // the new states, for now
            c.I32Const(0);
            c.LocalGet(nw);
            c.MemoryFill(SharedMem);
            c.LocalGet(rec);
            c.I32Const(0);
            c.I32Load(shm::States, SharedMem);
            c.LocalGet(cap);
            c.MemoryCopy(SharedMem, SharedMem);
            c.I32Const(0);
            c.I32Load(shm::States, SharedMem);
            c.Call(release);
            c.I32Const(0);
            c.LocalGet(rec);
            c.I32Store(shm::States, SharedMem);
            c.I32Const(0);
            c.LocalGet(nw);
            c.I32Store(shm::StatesCap, SharedMem);
            c.End();
            // The worker's record.
            c.I32Const(int32_t(shm::RecBytes));
            Msg(c, tx.oomspawn, tx.oomspawnlen);
            c.Call(xalloc);
            c.LocalTee(rec);
            c.LocalGet(id);
            c.I64Store(shm::RecId, SharedMem);
            c.LocalGet(rec);
            c.LocalGet(thunk);
            c.I32Store(shm::RecThunk, SharedMem);
            c.LocalGet(rec);
            c.LocalGet(cont);
            c.I32Store(shm::RecContained, SharedMem);
            c.LocalGet(rec);
            c.LocalGet(packet);
            c.I32Store(shm::RecPacket, SharedMem);
            c.LocalGet(rec);
            c.LocalGet(size);
            c.I32Store(shm::RecSize, SharedMem);
            c.Call(unlock);
            c.LocalGet(rec);
            c.Call(spawnimport);
            c.I32Const(0);
            c.Int(intop::LtS, false);
            c.If();
            Panic(c, rt, tx.nothread, tx.nothreadlen);
            c.End();
            c.LocalGet(id);
        }
        // wasi_thread_start(tid, rec): the worker's thread (gs_thread_main).
        // Runs its thunk, a contained one's catching the abort that ends it;
        // then frees what the worker held and publishes that it ended.
        {
            auto &c = m.FuncAt(start).code;
            enum { tid, rec, id, aborted, node };
            c.LocalGet(rec);
            c.I64Load(shm::RecId, SharedMem);
            c.LocalTee(id);
            c.GlobalSet(gtid);
            c.LocalGet(rec);
            c.I32Load(shm::RecContained, SharedMem);
            c.GlobalSet(gcontained);
            c.I32Const(int32_t(shm::XferSize));
            Msg(c, tx.oomspawn, tx.oomspawnlen);
            c.Call(malloc);
            c.GlobalSet(gxfer);
            // The thunk of the record's number, called with the packet.
            auto run = [&] {
                c.Block();
                if (thunks.empty()) {
                    c.Unreachable();
                } else {
                    auto n = uint32_t(thunks.size());
                    Handles::Dispatch(c, n, [&] {
                        c.LocalGet(rec);
                        c.I32Load(shm::RecThunk, SharedMem);
                    }, [&](uint32_t k) {
                        c.I32Const(int32_t(thunks[k].sp));
                        c.LocalGet(rec);
                        c.I32Load(shm::RecPacket, SharedMem);
                        c.Call(thunks[k].index);
                        c.Br(n - k);  // out of the dispatch, to the end of the block
                    });
                }
                c.End();
            };
            if (contained) {
                c.Block();  // out
                c.Block();  // caught: the catch's label, counted outside the try_table
                c.TryTableCatch(BlockType::Empty, tag, 0);
                run();
                c.End();
                c.Br(1);
                c.End();
                c.I32Const(1);
                c.LocalSet(aborted);
                c.End();
            } else {
                run();
            }
            c.Call(lock);
            c.LocalGet(rec);
            c.I32Load(shm::RecPacket, SharedMem);
            c.Call(release);
            c.GlobalGet(gxfer);
            c.Call(release);
            c.I32Const(0);
            c.I32Load(shm::States, SharedMem);
            c.LocalGet(id);
            c.I32WrapI64();
            c.I32Add();
            c.I32Const(shm::AbortedState);
            c.I32Const(shm::Returned);
            c.LocalGet(aborted);
            c.Select();
            c.I32Store8(0, SharedMem);
            c.LocalGet(aborted);
            c.If();
            c.I32Const(int32_t(shm::AbBytes));
            Msg(c, tx.oomabort, tx.oomabortlen);
            c.Call(xalloc);
            c.LocalTee(node);
            c.I32Const(0);
            c.I32Load(shm::Aborted, SharedMem);
            c.I32Store(shm::AbNext, SharedMem);
            c.LocalGet(node);
            c.LocalGet(id);
            c.I64Store(shm::AbId, SharedMem);
            c.LocalGet(node);
            c.GlobalGet(gmsg);
            c.I32Store(shm::AbMsg, SharedMem);
            c.LocalGet(node);
            c.GlobalGet(gmsglen);
            c.I32Store(shm::AbLen, SharedMem);
            c.I32Const(0);
            c.LocalGet(node);
            c.I32Store(shm::Aborted, SharedMem);
            c.End();
            c.LocalGet(rec);
            c.Call(release);
            c.I32Const(shm::DoneSeq);
            c.Call(broadcast);
            c.Call(unlock);
        }
        // wait(id, wantmsg, file, filelen, line) -> returned
        // (gs_thread_wait): blocks until the worker has ended; false for a
        // contained worker that aborted, whose message the first wait that
        // wants it takes (gmsg, gmsglen; 0 when none).
        {
            auto &c = m.FuncAt(wait).code;
            enum { id, want, file, filelen, line, st, prev, node };
            // A failure unlocks first: in a contained worker it unwinds.
            auto fail = [&](uint32_t text, uint32_t len) {
                c.Call(unlock);
                c.I32Const(int32_t(text));
                c.I32Const(int32_t(len));
                c.LocalGet(file);
                c.LocalGet(filelen);
                c.LocalGet(line);
                c.Call(rt.fail);
                c.Unreachable();
            };
            c.Call(lock);
            c.LocalGet(id);
            c.I64Const(0);
            c.Int(intop::LtS, true);
            c.LocalGet(id);
            c.I32Const(0);
            c.I64Load(shm::NextId, SharedMem);
            c.Int(intop::GeS, true);
            c.Int(intop::Or, false);
            c.If();
            fail(tx.threadid, tx.threadidlen);
            c.End();
            c.LocalGet(id);
            c.GlobalGet(gtid);
            c.Int(intop::Eq, true);
            c.If();
            fail(tx.threadself, tx.threadselflen);
            c.End();
            c.Block();
            c.Loop();
            c.I32Const(0);
            c.I32Load(shm::States, SharedMem);
            c.LocalGet(id);
            c.I32WrapI64();
            c.I32Add();
            c.LoadOp(0x2d, 0, SharedMem);  // i32.load8_u
            c.LocalTee(st);
            c.I32Const(shm::Running);
            c.I32Ne();
            c.BrIf(1);
            c.I32Const(shm::DoneSeq);
            c.Call(condwait);
            c.Br(0);
            c.End();
            c.End();
            c.I32Const(0);
            c.GlobalSet(gmsg);
            c.I32Const(0);
            c.GlobalSet(gmsglen);
            c.LocalGet(st);
            c.I32Const(shm::AbortedState);
            c.Int(intop::Eq, false);
            c.LocalGet(want);
            c.I32And();
            c.If();
            // Its record, unlinked: prev is the address of the link to it.
            c.I32Const(shm::Aborted);
            c.LocalSet(prev);
            c.Block();
            c.Loop();
            c.LocalGet(prev);
            c.I32Load(0, SharedMem);
            c.LocalTee(node);
            c.I32Eqz();
            c.BrIf(1);
            c.LocalGet(node);
            c.I64Load(shm::AbId, SharedMem);
            c.LocalGet(id);
            c.Int(intop::Eq, true);
            c.If();
            c.LocalGet(node);
            c.I32Load(shm::AbMsg, SharedMem);
            c.GlobalSet(gmsg);
            c.LocalGet(node);
            c.I32Load(shm::AbLen, SharedMem);
            c.GlobalSet(gmsglen);
            c.LocalGet(prev);
            c.LocalGet(node);
            c.I32Load(shm::AbNext, SharedMem);
            c.I32Store(0, SharedMem);
            c.LocalGet(node);
            c.Call(release);
            c.Br(2);
            c.End();
            c.LocalGet(node);
            c.I32Const(int32_t(shm::AbNext));
            c.I32Add();
            c.LocalSet(prev);
            c.Br(0);
            c.End();
            c.End();
            c.End();
            c.Call(unlock);
            c.LocalGet(st);
            c.I32Const(shm::AbortedState);
            c.I32Ne();
        }
        // now() -> ns on the monotonic clock (gs_qclock_ns).
        {
            auto &c = m.FuncAt(now).code;
            if (clock) {
                c.I32Const(1);  // CLOCKID_MONOTONIC
                c.I64Const(1);
                c.GlobalGet(gxfer);
                c.Call(clockimport);
                c.Drop();
                c.GlobalGet(gxfer);
                c.I64Load(0, SharedMem);
            } else {
                c.I64Const(0);
            }
        }
        // contain(msg, len): a contained worker's abort (gs_fail under
        // gs_contain): its message, from memory 0, kept in the shared heap
        // for thread_wait, and the worker unwound to wasi_thread_start.
        {
            auto &c = m.FuncAt(contain).code;
            enum { msg, len };
            if (contained) {
                c.LocalGet(len);
                Msg(c, tx.oomabort, tx.oomabortlen);
                c.Call(malloc);
                c.GlobalSet(gmsg);
                c.LocalGet(len);
                c.GlobalSet(gmsglen);
                c.GlobalGet(gmsg);
                c.LocalGet(msg);
                c.LocalGet(len);
                c.MemoryCopy(SharedMem, 0);
                c.Throw(tag);
            } else {
                c.Unreachable();
            }
        }
    }

    // Takes the head node of queue `q` (a local) into local `node`, under
    // the lock (gs_qtake).
    static void Take(Code &c, uint32_t q, uint32_t node) {
        c.LocalGet(q);
        c.I32Load(shm::QHead, SharedMem);
        c.LocalSet(node);
        c.LocalGet(q);
        c.LocalGet(node);
        c.I32Load(shm::NodeNext, SharedMem);
        c.I32Store(shm::QHead, SharedMem);
        c.LocalGet(q);
        c.I32Load(shm::QHead, SharedMem);
        c.I32Eqz();
        c.If();
        c.LocalGet(q);
        c.I32Const(0);
        c.I32Store(shm::QTail, SharedMem);
        c.End();
        c.LocalGet(q);
        c.LocalGet(q);
        c.I64Load(shm::QCount, SharedMem);
        c.I64Const(1);
        c.I64Sub();
        c.I64Store(shm::QCount, SharedMem);
    }

    void DefineQueues(Module &m, Runtime &rt, const Texts &tx) {
        // qput(q, node) -> queued: a node the caller allocated and filled,
        // linked unless the queue holds its limit, which frees it.
        {
            auto &c = m.FuncAt(qput).code;
            enum { q, node, tail };
            c.Call(lock);
            c.LocalGet(q);
            c.I64Load(shm::QCount, SharedMem);
            c.LocalGet(q);
            c.I64Load(shm::QLimit, SharedMem);
            c.Int(intop::GeS, true);
            c.If();
            c.LocalGet(node);
            c.Call(release);
            c.Call(unlock);
            c.I32Const(0);
            c.Return();
            c.End();
            c.LocalGet(q);
            c.LocalGet(q);
            c.I64Load(shm::QCount, SharedMem);
            c.I64Const(1);
            c.Int(intop::Add, true);
            c.I64Store(shm::QCount, SharedMem);
            c.LocalGet(node);
            c.I32Const(0);
            c.I32Store(shm::NodeNext, SharedMem);
            c.LocalGet(q);
            c.I32Load(shm::QTail, SharedMem);
            c.LocalTee(tail);
            c.If();
            c.LocalGet(tail);
            c.LocalGet(node);
            c.I32Store(shm::NodeNext, SharedMem);
            c.Else();
            c.LocalGet(q);
            c.LocalGet(node);
            c.I32Store(shm::QHead, SharedMem);
            c.End();
            c.LocalGet(q);
            c.LocalGet(node);
            c.I32Store(shm::QTail, SharedMem);
            c.LocalGet(q);
            c.I32Const(int32_t(shm::QSeq));
            c.I32Add();
            c.Call(broadcast);
            c.Call(unlock);
            c.I32Const(1);
        }
        // qget(q) -> node: blocks until one is queued.
        {
            auto &c = m.FuncAt(qget).code;
            enum { q, node };
            c.Call(lock);
            c.Block();
            c.Loop();
            c.LocalGet(q);
            c.I32Load(shm::QHead, SharedMem);
            c.BrIf(1);
            c.LocalGet(q);
            c.I32Const(int32_t(shm::QSeq));
            c.I32Add();
            c.Call(condwait);
            c.Br(0);
            c.End();
            c.End();
            Take(c, q, node);
            c.Call(unlock);
            c.LocalGet(node);
        }
        // qwait(q, ms) -> node or 0 (gs_qwait): waits at most ms
        // milliseconds, none at all for zero or less, as qpoll does.
        {
            auto &c = m.FuncAt(qwait).code;
            enum { q, ms, deadline, node };
            constexpr int64_t cap = INT64_MAX / 2000000;
            c.LocalGet(ms);
            c.I64Const(cap);
            c.Int(intop::GtS, true);
            c.If();
            c.I64Const(cap);
            c.LocalSet(ms);
            c.End();
            c.LocalGet(ms);
            c.I64Const(0);
            c.Int(intop::GtS, true);
            c.If();
            c.Call(now);
            c.LocalGet(ms);
            c.I64Const(1000000);
            c.Int(intop::Mul, true);
            c.Int(intop::Add, true);
            c.LocalSet(deadline);
            c.End();
            c.Call(lock);
            c.Block();
            c.Loop();
            c.LocalGet(q);
            c.I32Load(shm::QHead, SharedMem);
            c.BrIf(1);
            c.LocalGet(ms);
            c.I64Const(0);
            c.Int(intop::LeS, true);
            c.BrIf(1);
            c.LocalGet(deadline);
            c.Call(now);
            c.I64Sub();
            c.LocalTee(ms);  // what is left
            c.I64Const(0);
            c.Int(intop::LeS, true);
            c.BrIf(1);
            c.LocalGet(q);
            c.I32Const(int32_t(shm::QSeq));
            c.I32Add();
            c.LocalGet(ms);
            c.Call(condwaitns);
            c.Br(0);
            c.End();
            c.End();
            c.LocalGet(q);
            c.I32Load(shm::QHead, SharedMem);
            c.If();
            Take(c, q, node);
            c.End();
            c.Call(unlock);
            c.LocalGet(node);
        }
        // qlimit(q, n, file, filelen, line) (gs_qlimit).
        {
            auto &c = m.FuncAt(qlimit).code;
            enum { q, n, file, filelen, line };
            c.LocalGet(n);
            c.I64Const(0);
            c.Int(intop::LeS, true);
            c.If();
            c.I32Const(int32_t(tx.qbound));
            c.I32Const(int32_t(tx.qboundlen));
            c.LocalGet(file);
            c.LocalGet(filelen);
            c.LocalGet(line);
            c.Call(rt.fail);
            c.End();
            c.Call(lock);
            c.LocalGet(q);
            c.LocalGet(n);
            c.I64Store(shm::QLimit, SharedMem);
            c.Call(unlock);
        }
    }

    // panic(text) from inside the runtime, which may hold the lock: the
    // lock is left as it is, since panic ends the process.
    static void Panic(Code &c, Runtime &rt, uint32_t text, uint32_t len) {
        c.I32Const(int32_t(text));
        c.I32Const(int32_t(len));
        c.Call(rt.panic);
    }
};

}  // namespace goosewasm
