// HLE: sysPrxForUser (threads, TLS, lwmutex, mempool, process) and cellSysmodule.
#include "kernel.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <vector>

extern u32 g_tls_addr, g_tls_filesz, g_tls_memsz;

namespace {
bool g_tls_ready = false;
u32 g_tls_image = 0, g_tls_image_filesz = 0, g_tls_image_memsz = 0;
}  // namespace

// TLS block layout: 0x30-byte TCB followed by the TLS image; r13 points 0x7000 past the image start.
u32 tls_alloc() {
    if (!g_tls_ready) return 0;
    u32 size = g_tls_image_memsz + 0x30;
    u32 a = mem::alloc(size, 0x80);
    memcpy(gmem(a + 0x30), gmem(g_tls_image), g_tls_image_filesz);
    return a + 0x7030;
}

// sys_initialize_tls(thread_id, tls_seg_addr, tls_seg_size, tls_mem_size)
HLE_FUNC(sys_initialize_tls) {
    g_tls_image = (u32)ARG(1);
    g_tls_image_filesz = (u32)ARG(2);
    g_tls_image_memsz = (u32)ARG(3);
    g_tls_ready = true;
    c.r[13] = tls_alloc();
    LOG_I("sys_initialize_tls: image %08x (%x/%x) r13=%08llx", g_tls_image, g_tls_image_filesz, g_tls_image_memsz,
          (unsigned long long)c.r[13]);
    RET(CELL_OK);
}

// sys_ppu_thread_create(u64* id, opd entry, arg, prio, stacksize, flags, name)
HLE_FUNC(sys_ppu_thread_create) {
    u32 id = 0;
    PPUThread* t = thread_create((u32)ARG(1), ARG(2), (s32)ARG(3), (u32)ARG(4), ARG(5) & 1, gstr((u32)ARG(6), 32), &id);
    st64((u32)ARG(0), id);
    thread_start(t);
    RET(CELL_OK);
}
HLE_FUNC(sys_ppu_thread_get_id) { st64((u32)ARG(0), t_self->id); RET(CELL_OK); }
HLE_FUNC(sys_ppu_thread_exit) { thread_exit(c, ARG(0)); }
HLE_FUNC(sys_time_get_system_time) { RET(time_usec()); }
HLE_FUNC(sys_process_is_stack) {
    u32 p = (u32)ARG(0);
    RET(p >= mem::STACK_BASE && p < mem::STACK_BASE + mem::STACK_SIZE);
}
// leaves at once: std::exit ran the static destructors while the other guest threads (SPU workers, renderer) were
// still running, and they faulted on freed state (access violations in the log after every quit)
#ifdef F1CE_PGO_GEN
extern "C" int __llvm_profile_write_file(void);
#endif
[[noreturn]] void process_exit(int code) {
#ifdef F1CE_PGO_GEN
    __llvm_profile_write_file();   // PGO training build: _Exit skips the profile runtime's atexit writer
#endif
    std::fflush(nullptr);   // the log file is buffered
    std::_Exit(code);
}
HLE_FUNC(sys_process_exit) {
    LOG_I("sys_process_exit(%lld)", (long long)(s32)ARG(0));
    process_exit((int)(s32)ARG(0));
}
HLE_FUNC(sys_game_process_exitspawn) {
    LOG_I("sys_game_process_exitspawn(%s)", gstr((u32)ARG(0)).c_str());
    process_exit(0);
}
HLE_FUNC(sys_prx_exitspawn_with_level) { process_exit(0); }

// ------------------------------------------------------------------ lightweight mutex
// sys_lwmutex_t lives in guest memory; the host keeps the actual lock keyed by its address.
namespace {
// Ownership is handed directly to the oldest waiter on unlock (FIFO, like the LV2 sleep queue), so a
// thread that unlocks and immediately relocks cannot starve the others.
struct LwMutex {
    std::mutex m;
    std::condition_variable cv;
    u32 owner = 0, count = 0;
    bool recursive = false;
    std::deque<u32> waiters;
};
std::mutex g_lw_mutex;
std::unordered_map<u32, std::shared_ptr<LwMutex>> g_lwmutexes;

std::shared_ptr<LwMutex> lw_get(u32 addr, bool create = false) {
    std::lock_guard lk(g_lw_mutex);
    auto it = g_lwmutexes.find(addr);
    if (it != g_lwmutexes.end()) return it->second;
    if (!create) {
        // a statically initialised lwmutex that never went through create: treat as recursive
        auto m = std::make_shared<LwMutex>();
        m->recursive = true;
        return g_lwmutexes[addr] = m;
    }
    return g_lwmutexes[addr] = std::make_shared<LwMutex>();
}
}  // namespace

// sys_lwmutex_create(lwmutex*, attr*)  attr: protocol, recursive, name[8]
HLE_FUNC(sys_lwmutex_create) {
    auto m = lw_get((u32)ARG(0), true);
    m->recursive = ld32((u32)ARG(1) + 4) == 0x10;
    st32((u32)ARG(0), 0);       // owner
    st32((u32)ARG(0) + 4, 0);   // waiter
    st32((u32)ARG(0) + 8, ld32((u32)ARG(1)) | ld32((u32)ARG(1) + 4));   // attribute
    RET(CELL_OK);
}
HLE_FUNC(sys_lwmutex_destroy) {
    std::lock_guard lk(g_lw_mutex);
    g_lwmutexes.erase((u32)ARG(0));
    RET(CELL_OK);
}
HLE_FUNC(sys_lwmutex_lock) {
    auto m = lw_get((u32)ARG(0));
    std::unique_lock lk(m->m);
    u32 me = t_self->id;
    if (m->owner == me) {
        if (!m->recursive) RET(CELL_EDEADLK);
        m->count++;
        RET(CELL_OK);
    }
    if (m->owner == 0 && m->waiters.empty()) {
        m->owner = me;
        m->count = 1;
        st32((u32)ARG(0), me);
        RET(CELL_OK);
    }
    m->waiters.push_back(me);
    u64 timeout = ARG(1);
    auto handed = [&] { return m->owner == me; };
    if (timeout) {
        if (!m->cv.wait_for(lk, std::chrono::microseconds(timeout), handed)) {
            m->waiters.erase(std::find(m->waiters.begin(), m->waiters.end(), me));
            RET(CELL_ETIMEDOUT);
        }
    } else {
        while (!m->cv.wait_for(lk, std::chrono::seconds(3), handed)) {
            PPUThread* o = thread_find(m->owner);
            LOG_W("lwmutex %08x: thread %08x waiting >3s, owner %08x '%s'%s", (u32)ARG(0), me, m->owner,
                  o ? o->name.c_str() : "?", o && o->finished ? " (EXITED)" : "");
        }
    }
    // the unlocking thread already made us the owner
    m->count = 1;
    st32((u32)ARG(0), me);
    RET(CELL_OK);
}
HLE_FUNC(sys_lwmutex_unlock) {
    auto m = lw_get((u32)ARG(0));
    std::unique_lock lk(m->m);
    if (m->owner != t_self->id) RET(CELL_EPERM);
    if (--m->count == 0) {
        if (!m->waiters.empty()) {
            m->owner = m->waiters.front();
            m->waiters.pop_front();
            st32((u32)ARG(0), m->owner);
        } else {
            m->owner = 0;
            st32((u32)ARG(0), 0);
        }
        lk.unlock();
        m->cv.notify_all();
    }
    RET(CELL_OK);
}

// ------------------------------------------------------------------ memory pools
namespace {
struct MemPool {
    std::mutex m;
    std::vector<u32> free_blocks;
};
IdMap<MemPool> g_pools(0x7a000000);
}  // namespace

// sys_mempool_create(u32* mempool, chunk, chunk_size, block_size, ralignment)
HLE_FUNC(sys_mempool_create) {
    u32 chunk = (u32)ARG(1);
    u64 chunk_size = ARG(2), block = ARG(3), align = ARG(4) ? ARG(4) : 16;
    block = (block + align - 1) & ~(align - 1);
    auto p = std::make_shared<MemPool>();
    u32 start = (u32)((chunk + align - 1) & ~(align - 1));
    for (u64 a = start; a + block <= chunk + chunk_size; a += block) p->free_blocks.push_back((u32)a);
    std::reverse(p->free_blocks.begin(), p->free_blocks.end());
    st32((u32)ARG(0), g_pools.add(p));
    RET(CELL_OK);
}
HLE_FUNC(sys_mempool_destroy) { g_pools.remove(ld32((u32)ARG(0))); RET(CELL_OK); }
HLE_FUNC(sys_mempool_try_allocate_block) {
    auto p = g_pools.get((u32)ARG(0));
    if (!p) RET(0);
    std::lock_guard lk(p->m);
    if (p->free_blocks.empty()) RET(0);
    u32 b = p->free_blocks.back();
    p->free_blocks.pop_back();
    RET(b);
}
HLE_FUNC(sys_mempool_free_block) {
    auto p = g_pools.get((u32)ARG(0));
    if (!p) RET(CELL_ESRCH);
    std::lock_guard lk(p->m);
    p->free_blocks.push_back((u32)ARG(1));
    RET(CELL_OK);
}

// ------------------------------------------------------------------ cellSysmodule
HLE_FUNC(cellSysmoduleLoadModule) {
    LOG_I("cellSysmoduleLoadModule(0x%04x)", (u32)ARG(0));
    RET(CELL_OK);
}
HLE_FUNC(cellSysmoduleUnloadModule) { RET(CELL_OK); }
