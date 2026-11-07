// LV2 kernel syscalls used by the game (thread, sync, event, timer, memory, tty, fs).
#include "kernel.h"
#include "lv2_fs.h"

#include <array>
#include <chrono>
#include <deque>
#include <vector>

using namespace std::chrono;

namespace {

template <typename Pred>
bool wait_for(std::unique_lock<std::mutex>& lk, std::condition_variable& cv, u64 timeout_us, Pred pred) {
    if (!timeout_us) { cv.wait(lk, pred); return true; }
    return cv.wait_for(lk, microseconds(timeout_us), pred);
}

// ------------------------------------------------------------------ mutex / cond
struct Mutex {
    std::mutex m;
    std::condition_variable cv;
    u32 owner = 0;
    u32 count = 0;
    bool recursive = false;
};
IdMap<Mutex> g_mutexes(0x85000000);

u32 self_id() { return t_self ? t_self->id : 1; }

s32 mutex_lock(Mutex& mx, u64 timeout) {
    std::unique_lock lk(mx.m);
    u32 me = self_id();
    if (mx.owner == me) {
        if (!mx.recursive) return CELL_EDEADLK;
        mx.count++;
        return CELL_OK;
    }
    if (!wait_for(lk, mx.cv, timeout, [&] { return mx.owner == 0; })) return CELL_ETIMEDOUT;
    mx.owner = me;
    mx.count = 1;
    return CELL_OK;
}
s32 mutex_unlock(Mutex& mx) {
    std::unique_lock lk(mx.m);
    if (mx.owner != self_id()) return CELL_EPERM;
    if (--mx.count == 0) {
        mx.owner = 0;
        lk.unlock();
        mx.cv.notify_one();
    }
    return CELL_OK;
}

struct Cond {
    std::shared_ptr<Mutex> mutex;
    std::condition_variable_any cv;
    std::mutex m;
    u64 signals = 0;   // generation counter
    u32 waiters = 0;
    u32 pending = 0;
};
IdMap<Cond> g_conds(0x86000000);

struct Semaphore {
    std::mutex m;
    std::condition_variable cv;
    s32 value = 0, max = 0;
};
IdMap<Semaphore> g_semas(0x96000000);

// ------------------------------------------------------------------ timers
struct Timer {
    std::mutex m;
    std::condition_variable cv;
    u32 queue = 0;
    u64 source = 0, d1 = 0, d2 = 0;
    u64 next = 0, period = 0;
    bool running = false, destroyed = false;
    std::thread worker;
};
IdMap<Timer> g_timers(0x11000000);

void timer_thread(std::shared_ptr<Timer> t) {
    std::unique_lock lk(t->m);
    while (!t->destroyed) {
        if (!t->running) { t->cv.wait(lk); continue; }
        u64 now = time_usec();
        if (now < t->next) { t->cv.wait_for(lk, microseconds(t->next - now)); continue; }
        if (t->queue) event_queue_push(t->queue, t->source, t->d1, t->d2, t->next);
        if (t->period) t->next += t->period;
        else t->running = false;
    }
}

}  // namespace

// ------------------------------------------------------------------ event queues (shared with HLE)
struct EventQueue {
    std::mutex m;
    std::condition_variable cv;
    std::deque<std::array<u64, 4>> events;
    u32 size = 127;
    u64 key = 0;
    bool destroyed = false;
};
static IdMap<EventQueue> g_queues(0x8d000000);
struct EventPort { u32 queue = 0; u64 name = 0; };
static IdMap<EventPort> g_ports(0x0e000000);

std::shared_ptr<EventQueue> event_queue_get(u32 id) { return g_queues.get(id); }

s32 event_queue_push(u32 queue_id, u64 source, u64 d1, u64 d2, u64 d3) {
    auto q = g_queues.get(queue_id);
    if (!q) return CELL_ESRCH;
    {
        std::lock_guard lk(q->m);
        if (q->events.size() >= q->size) return CELL_EBUSY;
        q->events.push_back({source, d1, d2, d3});
    }
    q->cv.notify_one();
    return CELL_OK;
}

u32 event_queue_create_host(u64 key, u32 size) {
    auto q = std::make_shared<EventQueue>();
    q->key = key;
    q->size = size ? size : 127;
    return g_queues.add(q);
}

// ------------------------------------------------------------------ syscall implementations
namespace {
#define SYS(n) static void sys_##n(PPUContext& c)

// threads
SYS(ppu_thread_yield) { std::this_thread::yield(); RET(CELL_OK); }
SYS(ppu_thread_join) {
    PPUThread* t = thread_find((u32)ARG(0));
    if (!t) RET(CELL_ESRCH);
    std::unique_lock lk(t->m);
    t->cv.wait(lk, [&] { return t->finished.load(); });
    if (ARG(1)) st64((u32)ARG(1), t->exit_status);
    RET(CELL_OK);
}
SYS(ppu_thread_detach) { RET(CELL_OK); }
SYS(ppu_thread_set_priority) {
    if (PPUThread* t = thread_find((u32)ARG(0))) t->priority = (s32)ARG(1);
    RET(CELL_OK);
}
SYS(ppu_thread_get_priority) {
    PPUThread* t = thread_find((u32)ARG(0));
    st32((u32)ARG(1), t ? (u32)t->priority : 1000);
    RET(CELL_OK);
}
SYS(ppu_thread_get_stack_information) {
    PPUThread* t = t_self;
    st32((u32)ARG(0), t->stack_addr);
    st32((u32)ARG(0) + 4, t->stack_size);
    RET(CELL_OK);
}
SYS(ppu_thread_exit) { thread_exit(c, ARG(0)); }

// mutex: create(u32* id, attr*)  attr: protocol, recursive, pshared, adaptive, key, flags, pad, name
SYS(mutex_create) {
    auto mx = std::make_shared<Mutex>();
    mx->recursive = ld32((u32)ARG(1) + 4) == 0x10;   // SYS_SYNC_RECURSIVE
    st32((u32)ARG(0), g_mutexes.add(mx));
    RET(CELL_OK);
}
SYS(mutex_destroy) { RET(g_mutexes.remove((u32)ARG(0)) ? CELL_OK : CELL_ESRCH); }
SYS(mutex_lock) {
    auto mx = g_mutexes.get((u32)ARG(0));
    RET(mx ? mutex_lock(*mx, ARG(1)) : CELL_ESRCH);
}
SYS(mutex_trylock) {
    auto mx = g_mutexes.get((u32)ARG(0));
    if (!mx) RET(CELL_ESRCH);
    std::unique_lock lk(mx->m);
    if (mx->owner == self_id()) {
        if (!mx->recursive) RET(CELL_EDEADLK);
        mx->count++;
        RET(CELL_OK);
    }
    if (mx->owner) RET(CELL_EBUSY);
    mx->owner = self_id();
    mx->count = 1;
    RET(CELL_OK);
}
SYS(mutex_unlock) {
    auto mx = g_mutexes.get((u32)ARG(0));
    RET(mx ? mutex_unlock(*mx) : CELL_ESRCH);
}

// cond: create(u32* id, mutex_id, attr*)
SYS(cond_create) {
    auto cd = std::make_shared<Cond>();
    cd->mutex = g_mutexes.get((u32)ARG(1));
    if (!cd->mutex) RET(CELL_ESRCH);
    st32((u32)ARG(0), g_conds.add(cd));
    RET(CELL_OK);
}
SYS(cond_destroy) { RET(g_conds.remove((u32)ARG(0)) ? CELL_OK : CELL_ESRCH); }
SYS(cond_wait) {
    auto cd = g_conds.get((u32)ARG(0));
    if (!cd) RET(CELL_ESRCH);
    Mutex& mx = *cd->mutex;
    u32 me = self_id();
    std::unique_lock clk(cd->m);
    {
        std::lock_guard mlk(mx.m);
        if (mx.owner != me) RET(CELL_EPERM);
    }
    u32 saved_count = mx.count;
    // release the LV2 mutex while holding the cond lock so a signal cannot be missed
    {
        std::lock_guard mlk(mx.m);
        mx.owner = 0;
        mx.count = 0;
    }
    mx.cv.notify_one();
    cd->waiters++;
    u64 timeout = ARG(1);
    bool ok;
    if (timeout)
        ok = cd->cv.wait_for(clk, microseconds(timeout), [&] { return cd->pending > 0; });
    else {
        cd->cv.wait(clk, [&] { return cd->pending > 0; });
        ok = true;
    }
    cd->waiters--;
    if (ok) cd->pending--;
    clk.unlock();
    std::unique_lock mlk(mx.m);
    mx.cv.wait(mlk, [&] { return mx.owner == 0; });
    mx.owner = me;
    mx.count = saved_count;
    RET(ok ? CELL_OK : CELL_ETIMEDOUT);
}
SYS(cond_signal) {
    auto cd = g_conds.get((u32)ARG(0));
    if (!cd) RET(CELL_ESRCH);
    {
        std::lock_guard lk(cd->m);
        if (cd->pending < cd->waiters) cd->pending++;
    }
    cd->cv.notify_one();
    RET(CELL_OK);
}
SYS(cond_signal_all) {
    auto cd = g_conds.get((u32)ARG(0));
    if (!cd) RET(CELL_ESRCH);
    {
        std::lock_guard lk(cd->m);
        cd->pending = cd->waiters;
    }
    cd->cv.notify_all();
    RET(CELL_OK);
}
SYS(cond_signal_to) { sys_cond_signal(c); }

// semaphore: create(u32* id, attr*, initial, max)
SYS(semaphore_create) {
    auto s = std::make_shared<Semaphore>();
    s->value = (s32)ARG(2);
    s->max = (s32)ARG(3);
    st32((u32)ARG(0), g_semas.add(s));
    RET(CELL_OK);
}
SYS(semaphore_destroy) { RET(g_semas.remove((u32)ARG(0)) ? CELL_OK : CELL_ESRCH); }
SYS(semaphore_wait) {
    auto s = g_semas.get((u32)ARG(0));
    if (!s) RET(CELL_ESRCH);
    std::unique_lock lk(s->m);
    if (!wait_for(lk, s->cv, ARG(1), [&] { return s->value > 0; })) RET(CELL_ETIMEDOUT);
    s->value--;
    RET(CELL_OK);
}
SYS(semaphore_trywait) {
    auto s = g_semas.get((u32)ARG(0));
    if (!s) RET(CELL_ESRCH);
    std::lock_guard lk(s->m);
    if (s->value <= 0) RET(CELL_EBUSY);
    s->value--;
    RET(CELL_OK);
}
SYS(semaphore_post) {
    auto s = g_semas.get((u32)ARG(0));
    if (!s) RET(CELL_ESRCH);
    {
        std::lock_guard lk(s->m);
        s32 n = (s32)ARG(1);
        if (s->value + n > s->max) RET(CELL_EBUSY);
        s->value += n;
    }
    s->cv.notify_all();
    RET(CELL_OK);
}
SYS(semaphore_get_value) {
    auto s = g_semas.get((u32)ARG(0));
    if (!s) RET(CELL_ESRCH);
    std::lock_guard lk(s->m);
    st32((u32)ARG(1), (u32)s->value);
    RET(CELL_OK);
}

// event queue: create(u32* id, attr*, key, size)
SYS(event_queue_create) {
    st32((u32)ARG(0), event_queue_create_host(ARG(2), (u32)ARG(3)));
    RET(CELL_OK);
}
SYS(event_queue_destroy) {
    auto q = g_queues.get((u32)ARG(0));
    if (!q) RET(CELL_ESRCH);
    {
        std::lock_guard lk(q->m);
        q->destroyed = true;
    }
    q->cv.notify_all();
    g_queues.remove((u32)ARG(0));
    RET(CELL_OK);
}
// receive(id, event* (unused), timeout) -> r4..r7 = source, d1, d2, d3
SYS(event_queue_receive) {
    auto q = g_queues.get((u32)ARG(0));
    if (!q) RET(CELL_ESRCH);
    std::unique_lock lk(q->m);
    if (!wait_for(lk, q->cv, ARG(2), [&] { return !q->events.empty() || q->destroyed; })) RET(CELL_ETIMEDOUT);
    if (q->destroyed) RET(CELL_ECANCELED);
    auto e = q->events.front();
    q->events.pop_front();
    c.r[4] = e[0]; c.r[5] = e[1]; c.r[6] = e[2]; c.r[7] = e[3];
    RET(CELL_OK);
}
// tryreceive(id, sys_event_t* array, size, u32* number)
SYS(event_queue_tryreceive) {
    auto q = g_queues.get((u32)ARG(0));
    if (!q) RET(CELL_ESRCH);
    std::lock_guard lk(q->m);
    u32 n = 0, arr = (u32)ARG(1), max = (u32)ARG(2);
    while (n < max && !q->events.empty()) {
        auto e = q->events.front();
        q->events.pop_front();
        for (int k = 0; k < 4; k++) st64(arr + n * 32 + k * 8, e[k]);
        n++;
    }
    st32((u32)ARG(3), n);
    RET(CELL_OK);
}
SYS(event_queue_drain) {
    auto q = g_queues.get((u32)ARG(0));
    if (!q) RET(CELL_ESRCH);
    std::lock_guard lk(q->m);
    q->events.clear();
    RET(CELL_OK);
}
// port: create(u32* id, type, name)
SYS(event_port_create) {
    auto p = std::make_shared<EventPort>();
    p->name = ARG(2);
    u32 id = g_ports.add(p);
    if (!p->name) p->name = ((u64)0xFFFFFFFFull << 32) | id;   // default name: process id | port id
    st32((u32)ARG(0), id);
    RET(CELL_OK);
}
SYS(event_port_destroy) { RET(g_ports.remove((u32)ARG(0)) ? CELL_OK : CELL_ESRCH); }
SYS(event_port_connect_local) {
    auto p = g_ports.get((u32)ARG(0));
    if (!p) RET(CELL_ESRCH);
    if (!g_queues.get((u32)ARG(1))) RET(CELL_ESRCH);
    p->queue = (u32)ARG(1);
    RET(CELL_OK);
}
SYS(event_port_disconnect) {
    auto p = g_ports.get((u32)ARG(0));
    if (!p) RET(CELL_ESRCH);
    p->queue = 0;
    RET(CELL_OK);
}
SYS(event_port_send) {
    auto p = g_ports.get((u32)ARG(0));
    if (!p) RET(CELL_ESRCH);
    if (!p->queue) RET(CELL_ENOTDIR);   // CELL_ENOTCONN
    RET(event_queue_push(p->queue, p->name, ARG(1), ARG(2), ARG(3)));
}

// timers
SYS(timer_create) {
    auto t = std::make_shared<Timer>();
    t->worker = std::thread(timer_thread, t);
    t->worker.detach();
    st32((u32)ARG(0), g_timers.add(t));
    RET(CELL_OK);
}
SYS(timer_destroy) {
    auto t = g_timers.get((u32)ARG(0));
    if (!t) RET(CELL_ESRCH);
    {
        std::lock_guard lk(t->m);
        t->destroyed = true;
    }
    t->cv.notify_all();
    g_timers.remove((u32)ARG(0));
    RET(CELL_OK);
}
// _sys_timer_start(id, base_time, period)
SYS(timer_start) {
    auto t = g_timers.get((u32)ARG(0));
    if (!t) RET(CELL_ESRCH);
    {
        std::lock_guard lk(t->m);
        u64 base = ARG(1), period = ARG(2);
        u64 now = time_usec();
        t->next = base ? base : now + period;
        t->period = period;
        t->running = true;
    }
    t->cv.notify_all();
    RET(CELL_OK);
}
SYS(timer_stop) {
    auto t = g_timers.get((u32)ARG(0));
    if (!t) RET(CELL_ESRCH);
    std::lock_guard lk(t->m);
    t->running = false;
    RET(CELL_OK);
}
// connect_event_queue(timer, queue, name, data1, data2)
SYS(timer_connect_event_queue) {
    auto t = g_timers.get((u32)ARG(0));
    if (!t) RET(CELL_ESRCH);
    std::lock_guard lk(t->m);
    t->queue = (u32)ARG(1);
    t->source = ARG(2) ? ARG(2) : ((u64)0xFFFFFFFFull << 32) | (u32)ARG(0);
    t->d1 = ARG(3);
    t->d2 = ARG(4);
    RET(CELL_OK);
}
SYS(timer_disconnect_event_queue) {
    auto t = g_timers.get((u32)ARG(0));
    if (!t) RET(CELL_ESRCH);
    std::lock_guard lk(t->m);
    t->queue = 0;
    t->running = false;
    RET(CELL_OK);
}
SYS(timer_usleep) {
    sleep_usec(ARG(0));
    // keep the PPU at most one ring slot (reference step 2) ahead of a lagging RSX: the main thread paces its frames
    // with usleep, and the game would otherwise rewrite a command list the RSX is still running (hle_gcm.cpp)
    // Other guest threads can sleep while holding locks needed by main, so they must not wait on the RSX here.
    static const bool no_throttle = getenv("F1CE_NO_RSX_THROTTLE") != nullptr;   // debug
    {   // debug: F1CE_REFS_LOG samples the PPU/RSX distance
        static const bool rl = getenv("F1CE_REFS_LOG") != nullptr;
        static u32 calls = 0;
        if (rl && ++calls % 200 == 0) LOG_I("refs behind: %d", rsx_refs_behind());
    }
    if (!no_throttle && t_self && t_self->name == "main" && rsx_refs_behind() > 2) {
        u64 start = time_usec();
        // up to 15 s: at the race start the RSX can take seconds for the loading list (hundreds of textures)
        // (no limit while the renderer compiles its start-up shaders on the RSX thread)
        while (rsx_refs_behind() > 2 && (time_usec() - start < 15000000 || g_rsx_starting)) sleep_usec(200);
        static u32 n = 0;
        if (n++ < 20) LOG_I("PPU throttled %llu us: RSX behind (refs %d)", (unsigned long long)(time_usec() - start), rsx_refs_behind());
    }
    RET(CELL_OK);
}
SYS(timer_sleep) { sleep_usec(ARG(0) * 1000000ull); RET(CELL_OK); }

// time
SYS(time_get_timezone) {
    st32((u32)ARG(0), 60);
    st32((u32)ARG(1), 0);
    RET(CELL_OK);
}
SYS(time_get_current_time) {
    auto now = system_clock::now().time_since_epoch();
    st64((u32)ARG(0), (u64)duration_cast<seconds>(now).count());
    st64((u32)ARG(1), (u64)(duration_cast<nanoseconds>(now).count() % 1000000000));
    RET(CELL_OK);
}
SYS(time_get_timebase_frequency) { RET(79800000); }

// memory
SYS(memory_container_create) { st32((u32)ARG(0), 0x3F000001); RET(CELL_OK); }
SYS(memory_container_destroy) { RET(CELL_OK); }
// sys_memory_allocate(size, flags, u32* addr)
SYS(memory_allocate) {
    u32 align = ARG(1) == 0x400 ? 0x100000 : 0x10000;   // SYS_MEMORY_PAGE_SIZE_1M : 64K
    u32 a = mem::user_alloc((u32)ARG(0), align);
    if (!a) RET(CELL_ENOMEM);
    st32((u32)ARG(2), a);
    LOG_I("sys_memory_allocate(0x%llx) -> %08x", (unsigned long long)ARG(0), a);
    RET(CELL_OK);
}
SYS(memory_free) { mem::user_free((u32)ARG(0)); RET(CELL_OK); }
SYS(memory_get_user_memory_size) {
    st32((u32)ARG(0), mem::USER_SIZE);
    st32((u32)ARG(0) + 4, mem::user_available());
    RET(CELL_OK);
}

// tty
SYS(tty_read) { st32((u32)ARG(3), 0); RET(CELL_OK); }
SYS(tty_write) {
    std::string s((const char*)gmem((u32)ARG(1)), (size_t)ARG(2));
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    LOG_I("TTY%llu: %s", (unsigned long long)ARG(0), s.c_str());
    if (ARG(3)) st32((u32)ARG(3), (u32)ARG(2));
    RET(CELL_OK);
}

// SPU (direct use is minimal; SPURS is HLE)
SYS(spu_initialize) { RET(CELL_OK); }

// fs
SYS(fs_open) { RET(fs::open(gstr((u32)ARG(0)), (s32)ARG(1), (u32)ARG(2))); }
SYS(fs_read) { RET(fs::read((u32)ARG(0), (u32)ARG(1), ARG(2), (u32)ARG(3))); }
SYS(fs_write) { RET(fs::write((u32)ARG(0), (u32)ARG(1), ARG(2), (u32)ARG(3))); }
SYS(fs_close) { RET(fs::close((u32)ARG(0))); }
SYS(fs_opendir) { RET(fs::opendir(gstr((u32)ARG(0)), (u32)ARG(1))); }
SYS(fs_readdir) { RET(fs::readdir((u32)ARG(0), (u32)ARG(1), (u32)ARG(2))); }
SYS(fs_closedir) { RET(fs::closedir((u32)ARG(0))); }
SYS(fs_stat) { RET(fs::stat(gstr((u32)ARG(0)), (u32)ARG(1))); }
SYS(fs_fstat) { RET(fs::fstat((u32)ARG(0), (u32)ARG(1))); }
SYS(fs_mkdir) { RET(fs::mkdir(gstr((u32)ARG(0)))); }
SYS(fs_rename) { RET(fs::rename(gstr((u32)ARG(0)), gstr((u32)ARG(1)))); }
SYS(fs_rmdir) { RET(fs::rmdir(gstr((u32)ARG(0)))); }
SYS(fs_unlink) { RET(fs::unlink(gstr((u32)ARG(0)))); }
SYS(fs_lseek) { RET(fs::lseek((u32)ARG(0), (s64)ARG(1), (u32)ARG(2), (u32)ARG(3))); }
SYS(fs_fsync) { RET(CELL_OK); }
SYS(fs_utime) { RET(CELL_OK); }
SYS(fs_chmod) { RET(CELL_OK); }
SYS(fs_fcntl) { RET(CELL_OK); }
SYS(fs_truncate) { RET(fs::truncate(gstr((u32)ARG(0)), ARG(1))); }
SYS(fs_ftruncate) { RET(fs::ftruncate((u32)ARG(0), ARG(1))); }

struct SysEntry { const char* name; void (*fn)(PPUContext&); };
#define E(num, n) {num, {"sys_" #n, sys_##n}}
const std::unordered_map<u32, SysEntry> g_syscalls = {
    E(41, ppu_thread_exit), E(43, ppu_thread_yield), E(44, ppu_thread_join), E(45, ppu_thread_detach),
    E(47, ppu_thread_set_priority), E(48, ppu_thread_get_priority), E(49, ppu_thread_get_stack_information),
    E(70, timer_create), E(71, timer_destroy), E(73, timer_start), E(74, timer_stop),
    E(75, timer_connect_event_queue), E(76, timer_disconnect_event_queue),
    E(90, semaphore_create), E(91, semaphore_destroy), E(92, semaphore_wait), E(93, semaphore_trywait),
    E(94, semaphore_post), E(100, mutex_create), E(101, mutex_destroy), E(102, mutex_lock), E(103, mutex_trylock),
    E(104, mutex_unlock), E(105, cond_create), E(106, cond_destroy), E(107, cond_wait), E(108, cond_signal),
    E(109, cond_signal_all), E(110, cond_signal_to), E(114, semaphore_get_value),
    E(128, event_queue_create), E(129, event_queue_destroy), E(130, event_queue_receive),
    E(131, event_queue_tryreceive), E(133, event_queue_drain), E(134, event_port_create), E(135, event_port_destroy),
    E(136, event_port_connect_local), E(137, event_port_disconnect), E(138, event_port_send),
    E(141, timer_usleep), E(142, timer_sleep), E(144, time_get_timezone), E(145, time_get_current_time),
    E(147, time_get_timebase_frequency), E(169, spu_initialize),
    E(341, memory_container_create), E(342, memory_container_destroy), E(348, memory_allocate),
    E(349, memory_free), E(352, memory_get_user_memory_size), E(402, tty_read), E(403, tty_write),
    E(801, fs_open), E(802, fs_read), E(803, fs_write), E(804, fs_close), E(805, fs_opendir), E(806, fs_readdir),
    E(807, fs_closedir), E(808, fs_stat), E(809, fs_fstat), E(811, fs_mkdir), E(812, fs_rename), E(813, fs_rmdir),
    E(814, fs_unlink), E(815, fs_utime), E(817, fs_fcntl), E(818, fs_lseek), E(820, fs_fsync), E(831, fs_truncate),
    E(832, fs_ftruncate), E(834, fs_chmod),
};
#undef E
}  // namespace

void ppu_syscall(PPUContext& c) {
    u32 n = (u32)c.r[11];
    auto it = g_syscalls.find(n);
    if (it == g_syscalls.end()) {
        LOG_W("unimplemented syscall %u at %08x (r3=%llx r4=%llx) -> ENOSYS", n, c.pc, (unsigned long long)c.r[3],
              (unsigned long long)c.r[4]);
        c.r[3] = (u64)(s64)CELL_ENOSYS;
        return;
    }
    LOG_T("syscall %s(%llx, %llx, %llx, %llx)", it->second.name, (unsigned long long)c.r[3],
          (unsigned long long)c.r[4], (unsigned long long)c.r[5], (unsigned long long)c.r[6]);
    if (t_self) t_self->in_hle = it->second.name;
    it->second.fn(c);
    if (t_self) t_self->in_hle = nullptr;
}
