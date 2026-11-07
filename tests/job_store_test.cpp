#include "kernel.h"

#include <array>
#include <chrono>
#include <iostream>

void ppu_track_job_control(u32 ea);
void ppu_job_store(u32 ea, u64 value, u32 size);
u32 line_generation(u32 ea);
void line_written(u32 ea);

int main(int argc, char** argv) {
    const bool unsafe = argc == 2 && std::string(argv[1]) == "--unsafe";
    alignas(128) std::array<u8, 512> memory{};
    g_base = memory.data();
    constexpr u32 ctrl = 128;
    ppu_track_job_control(ctrl);
    st16(ctrl + 16, 9);   // PPU's submitted count
    st16(ctrl + 18, 8);   // SPU's claimed count, in the same reservation line

    std::array<u8, 128> snapshot;
    auto& reservation = atomic_lock(ctrl);
    reservation.lock();
    memcpy(snapshot.data(), gmem(ctrl), snapshot.size());
    const u16 claimed = __builtin_bswap16((u16)9);
    memcpy(snapshot.data() + 18, &claimed, sizeof(claimed));

    std::atomic<bool> started{false}, done{false};
    std::thread producer([&] {
        started.store(true, std::memory_order_release);
        if (unsafe) st16(ctrl + 16, 10);   // previous implementation
        else ppu_job_store(ctrl + 16, 10, 2);
        done.store(true, std::memory_order_release);
    });
    while (!started.load(std::memory_order_acquire)) std::this_thread::yield();
    if (unsafe) {
        // Explicit ordering recreates a store in the SPU's compare/copy window without a C++ data race.
        while (!done.load(std::memory_order_acquire)) std::this_thread::yield();
    } else {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    // The SPU successfully commits its earlier full-line snapshot.
    memcpy(gmem(ctrl), snapshot.data(), snapshot.size());
    line_written(ctrl);
    reservation.unlock();
    producer.join();
    if (ld16(ctrl + 16) != 10 || ld16(ctrl + 18) != 9) {
        std::cerr << "FAIL: SPU line write lost PPU submission: submitted=" << ld16(ctrl + 16)
                  << " claimed=" << ld16(ctrl + 18) << '\n';
        return 1;
    }

    const u32 generation = line_generation(ctrl);
    ppu_job_store(ctrl + 16, 10, 2);   // even an unchanged store invalidates an older reservation
    if (line_generation(ctrl) != generation + 1) return 2;
    ppu_job_store(ctrl + 64, 0xA5, 1);
    ppu_job_store(ctrl + 66, 0xBEEF, 2);
    ppu_job_store(ctrl + 68, 0x12345678, 4);
    ppu_job_store(ctrl + 72, 0x0123456789ABCDEFull, 8);
    ppu_job_store(ctrl + 124, 0xFEDCBA9876543210ull, 8);   // crosses the tracked line boundary
    if (ld8(ctrl + 64) != 0xA5 || ld16(ctrl + 66) != 0xBEEF || ld32(ctrl + 68) != 0x12345678 ||
        ld64(ctrl + 72) != 0x0123456789ABCDEFull || ld64(ctrl + 124) != 0xFEDCBA9876543210ull) return 3;
    const u32 untracked_generation = line_generation(16);
    ppu_job_store(16, 0x1234, 2);
    if (ld16(16) != 0x1234 || line_generation(16) != untracked_generation) return 4;
    std::cout << "PASS: concurrent PPU/SPU counters preserved; store widths, boundary and reservations checked.\n";
}
