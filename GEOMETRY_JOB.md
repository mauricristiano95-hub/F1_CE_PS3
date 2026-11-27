# Native geometry job (branch `native-cpp-geometry-job`)

Goal: run the SPU geometry job of SPURS workloads w1/w2 as native C++ (user decision 2026-11-24, option A1), to cut the
race-start SPU load (most visible with `F1CE_CAR_LOD=0`).

## The job

- One raw SPU blob in the EBOOT data: guest `0x100A1280`, loaded at LS `0x4000`, `0xEC20` bytes (the size is a word of
  its header at LS `0x4014`; `0xC0DEC0DE` at `0x4020`). Code and data mixed, ~15k words, 31 direct call targets.
- Job manager services through a table whose LS address is stored at `0x12C00` (`0x1340` in the dumps): `+0x44` =
  `0x1670` (job end, native in `spu_pm.cpp`), `+0x48` = `0x1968` (releases buffers through `0x2CE8`, native), `+0x34` =
  `0x2D48` (DMA info, native), `+0x30` `0x1580`, `+0x38` `0x15F0`, `+0x3C` `0x1648`, `+0x40` `0x1650`, `+0x4C` `0x1988`,
  `+0x50` `0x1908`.
- Command interpreter: function `0x5960` (called from `0x4BBC`) saves r80-r99, sets constants (r84-r97 = small offsets,
  r98 = handler table `0x12180`, r99 = length table `0x12230`) and loops at `0x5A28`: it gathers the next 16 halfwords of
  the command stream (pointer kept at `[r82]`, `r82` = context + 352) into two quadwords on the stack (64(r1), 80(r1)) and
  packed into r4 / r5, the opcode is the low byte of the first halfword, `bisl` to the handler at `0x5C84` with r3 =
  context (r83), returns to `0x5C88`, which advances the stream by 2 * length bytes (length table) and loops back to
  `0x5A28` while the opcode was not 0 (`0x5CC4`: `brhz r80`, r80 = opcode == 0 from `0x5C58`): opcode 0 is the last
  command of every stream, run before the loop ends.
- 41 opcodes, handler table at LS `0x12180`; only 15 handlers were ever recorded in play: 0 4 6 8 9 12 14 15 23 24 30
  32 35 37 40.
- De-interleave routines per element type, table at LS `0x12260`: `0xBE00`, `0xBF80`, `0xC190` (x2), `0xC380`, `0xC180`
  (x2).

## Where the time goes (60 fps + LOD0 race start, `SPU_AOT_GEOM_OPS=1` build)

Handlers ~2.5 cores in all. Per mesh piece ops 0 4 6 8 9 12 14 30 35 37 run once (~110k/s); 23 24 32 40 once per ~11
pieces; 15 ~0.7 per piece.

| op | handler | ms/s | us/call | what |
|---|---|---|---|---|
| 30 | `0x9FD8` | ~615 | 5.5 | piece end: job manager `0x1968` (release) then `0x1670` (job end: outputs out, next job in) |
| 0 | `0x8C80` | ~465 | 4.2 | DMA streaming engine: ring space (`0x4C18`), DMA issue (`0x4368`, `wrch ch16..`), ... |
| 35 | `0xAFD0` | ~390 | 3.5 | |
| 9 | `0xB300` | ~365 | 3.3 | |
| 37 | `0x5D30` | ~165 | 1.5 | |
| others | | ~400 | | |

Host profile of the workers (same case): waits 19%, block dispatcher 13% (~65 M dispatches/s, mostly returns of the
job's compiled C functions), DMA path ~10%, native cull + transform 4.5%, translated code ~35% spread thin.

Lesson (2026-11-24): rewriting leaf loops instruction for instruction (`0xBD80`, `0xBE00`) was exact and saved nothing;
the gain must come from whole call trees running natively (no dispatcher round trips) and from doing less work.

## Design

1. Native `0x5960`: runs the command stream itself. At its loop head the SPU-visible state (stack frame and saved
   registers, r82 / r83, the operand copies on the stack, r84-r99) matches the SPU loop at `0x5A28`, so the native code
   can stop there at any time and leave the rest to the SPU code (`pc = 0x5A28`).
2. Opcodes with a native handler run inline; others call the SPU handler through a nested run until the return
   address `0x5C88` (`SPUThread::run_until`). If the SPU halts inside (blocking channel), the native loop gives up: the
   thread resumes in the handler and the SPU loop carries on.
3. Native code takes the common path only; anything unusual (an event pending where the SPU code enables interrupts,
   unexpected values, unknown opcodes) stops at a block boundary with the pc set.
4. Checking: `F1CE_GEOM_CHECK` (full state against the interpreter). Handlers that issue DMA need the dry-DMA mode of
   the job manager check (`spu_pm.cpp` check_fn: puts logged, not written) so a check never writes guest memory twice.
5. Speed: judge only with 60 fps + `F1CE_CAR_LOD=0`, alternating pairs (`F1CE_GEOM_NATIVE=0` vs default).

## Status

- [x] Job located, command set and per-command cost measured, checker (`F1CE_GEOM_CHECK`) and the per-command
  diagnostic (`SPU_AOT_GEOM_OPS=1`).
- [x] `SPUThread::run_until`, native command loop (`spu_fast_cmdlist`, skip at `0x5960`) calling the SPU handlers through
  nested runs: every list runs natively (~70-76k lists/s at the 30 fps race start, ~10.8 commands each), none handed
  back (`geom lists:` log line).
- [x] All 15 handlers as native call trees (`tools/spu_native_fn.py` -> `generated/spu_geom/geom_native.cpp`, 35
  functions, ~6100 instructions): exact where checkable (ops 6 8 12 14 15 23 35, 40k-507k calls each, 0 differences
  with `F1CE_GEOM_CHECK=2`; the cull / transform loops are bypassed in a check, they keep only live state); the others
  touch DMA or the job manager and are verified by game runs. Only stop: op30 at `0x0A088`, interrupts enabled right
  after job end with the job manager's own event pending (correct, the SPU code takes it). 60 fps + LOD0, vsync off,
  three alternating pairs: 30.2/31.7, 28.1/28.7, 27.4/28.6 fps = +4% (on better in every pair).
- [x] Targets of the job's function-pointer tables (de-interleave `0x12260`, vertex format decoders `0x12A60-0x12BDF`,
  `0x12780`) as native functions too, reached through indirect calls (`geom_call`), checked at their own entry: 82
  functions, ~12100 instructions; `0xBE00` 436k / `0xBF80` 46k checked calls, 0 differences. Same benchmark:
  30.3/32.9, 30.5/32.5, 30.1/31.1 fps = +6%.
- Host profile after it (workers, 60 fps + LOD0): dispatcher 9.1% (was 13.3%), `spu_fast_cmdlist` 4.4% (the operand
  gathering, 152 instructions per command), native cull 4.2%, DMA path ~10%, `geomfn_08970` 3.3%, `geomfn_08c80` 2.3%,
  `geomfn_0bd80` 1.4%, `geomfn_04c18` 1.3%. Translated code still running there: the job manager and workload w0's job.
- [x] Generated functions on local register copies (SpuLocal, 74 of 82; the 8 with channel instructions stay on the
  thread): exact, but no measurable change (30.4/31.2, 29.9/32.2, 30.4/32.1 fps = +5% against the SPU job, same as
  before within noise): register memory traffic is not what limits this code.
- [x] (2026-11-25) The same generator for workload w0's job and the job manager (`tools/spu_native_fn.py`, images
  GEOM / W0 / PM -> `generated/spu_geom/{geom,w0,pm}_native.cpp`).
  - w0's job: blob guest `0x10094700`, `0xA040` bytes at LS `0x4000` (the frame job list writer; a second small blob
    `0x10093700` alternates with it in w0). Position-independent: entry `0x4030` (header word `0x30`), constructor
    `0x41D8`, main `0x41F0` reached by `br` with r0 set by hand; `brsl rX,next` = pc into a register; interrupts
    off / on with `brsl rT,next; ai rT,rT,8; bi rT (d/e)`; main's switch at `0x5298` through the offset table at
    `0x52A0`; job manager services through the table pointer at `0xE020`. 9 functions, 8329 instructions.
  - Job manager: roots `0xA00` (main loop at `0x2A10`, continuations set by hand: `ila r0,next; br f`), event handler
    `0x21C0`, the services; the hand-written natives of `spu_pm.cpp` entered at their heads. 39 functions, 2510
    instructions. Its hot spots were already native (the DMA and atomics of `spu_pm.cpp`).
  - Entered by the dispatcher (`spu.cpp` `spu_native_entry`, when a block is built and the image's id words at its
    base match): the roots and resume points (return points of calls, targets of interrupt enables), 395 + 63. These
    images never nest runs: code that cannot finish natively (a call out of the image, a job launch, a halt, a pending
    interrupt) unwinds to the dispatcher, which comes back in at the next resume point (a nested run could hold the
    thread for good when the job manager leaves for the SPURS kernel). Every `bi` goes through the label switch;
    backward branches take pending interrupts. `F1CE_W0_NATIVE=0` / `F1CE_PM_GEN=0` switch them off,
    `F1CE_NATIVE_STATS=1` logs entries and early stops (costly), `F1CE_NATIVE_CHECK=1/2/3` compares entries with the
    interpreter on copies (every 4096th / 64th / every entry, DMA puts only logged).
  - Check: 37 w0 and 15 job manager entry points compared, no real difference (the differences seen are the queue line
    other SPUs change between the two runs, at LS `0x90`, and a decrementer read in the w0 entry).
  - Speed, 60 fps + LOD0, vsync off: off 29.2 / 27.9 / 29.5 / 28.5 fps, on 34.0 / 33.1 / 35.2 / 32.9 / 32.6 / 33.7 / 34.3 /
    34.1 fps = +15-20% (SPU instructions per frame 108 M -> ~100 M; the gain is dispatch, not work).
  - OPEN: an intermittent stall with both images on: 1 of 10 stress runs (on3, ~5 s at 1.5-17 fps) and 1 of 5 150 s
    races at 30 fps (`long30`, 15 s of 0.4-1.3 s frames ~105 s into the race; all workers idle, every ready count 0,
    wkl 6/7/8 queues holding jobs, main sleeping in fn 0x48AC34). Not seen in 7 runs with one image only or 5 with
    both off (too few to pin it on one image).
  - Stall hunt (a dump of the SPURS state on frames over 300 ms, `hle_gcm.cpp`): 0 stalls in 10 + 10 paired stress
    runs and 5 + 5 paired 150 s races at 30 fps. In all, 2 stalls in ~44 runs with the generated code, 0 in ~24 without.
- [x] Geometry job resume points (dispatcher entries, nesting kept; targets of `ila rT,next; bi rT (e/d)` too: 124
  entries) and indirect calls from job images straight into the job manager's generated code (`spu_native_call_pm`):
  dispatcher entries 1.8 M/s -> 1.3 M/s, no measurable fps change (33.6 / 33.9 / 33.6 against 29.2 / 29.1 / 29.0 off).
  With the generated code the stress test is renderer-bound now (rsx 0.92-0.93 core): further SPU savings show as
  lower CPU use, not fps, on this machine.
- Profile after (60 fps + LOD0): w1/w2 (geometry) ~62% of samples, w0 ~16%, w6/w7/w8 ~10% (the audio jobs, blob
  `0x10080880` `0x1C30` bytes, hot loop `0x5778` already the native idiom `spu_idiom_audio_voice`; code ~5.6%, job
  manager ~4.2%).
- Weak CPU benchmark (`F1CE_CPUS=4`, 60 fps, normal car detail, vsync off): all generated code off 23.4 / 22.9 fps,
  on 25.2 / 25.4 (+9.5%). Host profile there: the workers are 70% in waits (line waits, the GETLLAR yield); of the
  busy time, w0's job main ~16%, the DMA copies (`write_ls`) and the dispatcher, the rest spread thin. GETLLARs are
  mostly atomic updates (480k/s against 420k PUTLLCs, 20% of those fail), not idle polling.
- [x] The other job binaries of the EBOOT (`OTHER_JOBS`: 9 blobs, LS base from their recorded blocks; among them the
  audio jobs and the rain job) as generated images with dispatcher entries, the translated code's idioms and skips
  called where their words match (`F1CE_JOBS_GEN=0`). Checked (no real difference); 4 CPUs: 25.7 / 26.4 / 26.0 fps off,
  25.8 / 26.3 / 26.0 on = no change (their share was small).
- [x] The command loop's statistics (a counter shared by all workers on every command, a mutex on every early stop)
  only with `F1CE_NATIVE_STATS`: 26.1 / 26.0 / 26.1 fps, worker CPU 2.43 against ~2.48 cores.
- Renderer (60 fps normal, rsx 0.95 core): `draw()` 23% + 14% in the driver, `hash_range` 10%, FIFO parsing 9%, flip 5%
  (the index cache eviction walk); per frame vertices ~7 ms, textures 2.2, programs 1.3. Tried: a mixing hash with
  power-of-two buckets for the draw path's maps (their lookups were the hottest addresses of `draw()`): 56.4 fps both
  ways, reverted.
- [x] Renderer index lists (commit 2de4f10): timers in `gather_indices` showed the per-draw list path (lookup + hash
  1.2 ms, build 0.6 ms per frame) behind index regions that fall back for rewritten buffers; ~1350 of ~1700 rebuilt
  lists per frame are one-shot (geometry job output at new places). Keys seen for the first time skip the hash and
  the map; pointer caches in front of the maps. 56.4 / 57.0 / 57.4 -> 57.6 / 58.8 / 58.9 fps, rsx 0.95 -> 0.92, index
  ~2.4 -> ~1.7 ms, flip 5.1% -> 1.2% of the renderer (smaller map to evict).
- Renderer profile after (line tables via `-gline-tables-only` on rsx_vk / rsx_common / rsx.cpp, `llvm-symbolizer`):
  `draw()` 22%, FIFO parsing 10.6%, `hash_range` 9.3% (5.3% the sparse sampling loop: static mesh / texture checks),
  `hash_fp` 2.8% (fragment program hash every draw). Left: fewer sparse checks (a streamed mesh seen a frame later),
  FP hash per address (programs patched in place: needs write tracking), or FIFO parsing on its own thread (large).
  One GL boot run hit "RSX: unmapped IO offset 1f800000" (FIFO parse of float data, before any draw); the rerun and the
  baseline were fine: intermittent, see the FIFO stale tail notes.
- Pre-merge checks (2026-11-25): Linux cross build (zig) clean after fixing three generated functions that ran off
  their end after a trailing stop (commit 4c79354); test AppImage `out/F1CE-x86_64-native-test.AppImage`. Scenario set
  at 30 fps, no FATAL and no stall in any: 420 s race (Silverstone, Vulkan), heavy rain (Spain), Monaco Time Trial
  (AI drive, 240 s), Quick Race -> pause -> Replay (viewer works), OpenGL race (Monza), f1ce_online boot to its menus.
  Not covered: a LAN / online race, the PGO build (not used for releases).
- [ ] Semantic rewrites of the hottest generated functions (host profile names them), DMA-safe check mode for the
  handlers that issue DMA.

## Without the policy module (`F1CE_GEOM_HLE=1`, `runtime/hle_geom.cpp`, 2026-11-26)

Step 1 of `GEOMETRY_HLE_HANDOFF.md`. The worker that the SPURS scheduler gives w1 / w2 (same rules, ready counts and
contention as before) runs the jobs itself instead of the job manager: grab from the queue, the job on a private SPU of
the worker thread (its own code, generated / translated as before), a native stand-in for the PM's services.

What a geometry job gets from the PM (from the PM's code and a dump of its buffer lists, `F1CE_GEOM_DUMP` in spu_pm.cpp):
- Queue entry `0x00300001 desc`: kind 1, `(w0 >> 16) & 511` = 0x30 bytes of descriptor fetched to LS 0xC00. The
  descriptor is a buffer list: declare set 0 (blocks 0x10.., 0x3C blocks: the binary) and set 1 (block 0x58, 32 KB),
  fetch the binary into set 0 (mode 0x2000: kept while the next job names the same EA), gather set 1 by a DMA list at
  desc + 0x1C (= desc - 0x80), release both declarations, end.
- Job record in main memory, 0x160 bytes per job: matrix (0x40), 0x20 of flags, the DMA list (8 elements), the command
  stream (0x40), the descriptor (0x30), params (0x30: done-window EAs 0x10414F80 / 0x10414E80, output RSX offset and
  EA, ...). The gather list: 0x100 from the command stream on, params, 0x70 shared constants, the matrix, 0x50
  constant (0x1001FC00), then the piece's data in RSX-visible memory (0x20 header, vertex data, index data).
- Command stream: halfwords, opcode in the high byte. A typical piece: `op4(0x100) op6(0xF8,0) op8(0x230,0)
  op9(0x270,1,0) op12(0xE80,0xBC,0) op14(0x130) op15(0x1A0) op30 op35 op37 op0`: operands are offsets into the
  gathered set-1 buffer (params, vertex format, vertex data, source indices with the triangle count 0xBC, shared
  constants, matrix). op30 releases set 1 and calls job end; op35 = transform + cull (0xD400); op37 packs the kept
  triangles to 6-byte index triples (0x10780); op0 streams them out (list put, a 16-byte per-SPU record, a 4-byte put,
  a PUTLLUC).
- Main (0x47F8) declares set 2 (block 0x78, 64 KB, context) and set 3 (block 0xB8, two 30 KB elements) through service
  +0x30 with lists on its stack. Services used: +0x30, +0x34 (DMA info), +0x44 (job end), +0x48 (release).
- Table at LS 0x1340: SPU number, SPURS EA (+0x0C), workload << 16 | job index (+0x10), queue (+0x18), params pointer
  (+0x20). Entry = binary + header word +0x10 (0x4080), r3 = 0x1340, r4 = e0 >> 28, r1 = 0x3FFD0.
- Done: the PM's per-SPU halfword in the queue line (set by the grab's atomic update to the job index, 0xFFFF when it
  holds no job) and counters from type-4 list entries (none seen for geometry jobs).

Result: renders the same (30 fps race, Silverstone); ~10k SPU instructions per job against ~13.4k with the PM, 0
interpreted. A first version with a pool of threads of its own was as fast with 12 hardware threads (41.5 fps stress)
but lost badly with `F1CE_CPUS=8` (31 against 35 fps): the extra threads took cores from the workers. The worker
version was still slower until the job no longer started through `SPUThread::run()`: its `getenv` and flush check on
every one of ~150k jobs/s cost more than the PM saved (run_until directly, FTZ set per workload run).

Speed (carbench, Vulkan, AI drive, three alternating pairs, default = off):
- 60 fps + `F1CE_CAR_LOD=0`, `F1CE_CPUS=8`: off 36.6 / 36.1 / 35.8 fps, on 41.1 / 43.2 / 42.0 (+16%); worker CPU
  4.47-4.53 -> 4.29-4.34 cores; SPU instructions per frame (PM + job) ~13.4k -> ~10k per geometry job.
- 60 fps, normal detail, `F1CE_CPUS=4` (Deck-like): off 25.4 / 26.7 / 26.7, on 32.6 / 32.9 / 32.2 (+24%); worker CPU
  2.68 -> 2.50-2.55 cores.
- Per job: ~16 us on a worker, 5.5 GETLLARs (the job's own ring / in-order atomics), ~1% of jobs over 50 us.

Scenario set with `F1CE_GEOM_HLE=1` (30 fps, Vulkan unless noted), no FATAL: 420 s race, heavy rain (Spain), Quick Race
-> pause -> Replay, OpenGL race (Monza), f1ce_online boot, three 150 s races (no stall). Monaco Time Trial: in 1 of 4
runs a cluster of 0.6-1.6 s frames at 216 s (the w1 queue line showed one worker still holding job 0x69 with the head
at 0xAA, i.e. one job ran ~1 s and the in-order jobs after it waited); not seen in 3 more runs, where no job took over
100 ms. OPEN. Diagnostics: `geom hle: job ... took` (jobs over 100 ms), `geom hle jobs:` in the watchdog / stall dump
(each worker's job, its time so far, pc, reservation line), `F1CE_GEOM_HLE_STATS=1` (GETLLARs per job, jobs over
50 us), `F1CE_GEOM_HLE_TRACE=<n>` (the MFC commands of n jobs). The 3.4 s frame of another Time Trial run is the load
point the baseline also has (no SPU work queued during it).

(Superseded: see "Next steps" at the end.) Not done: step 2 (the semantic rewrite). With step 1 the job itself is what is left: ~10k SPU instructions per piece,
~16 us. A rewrite of the 15 opcodes (vertex decode, transform, cull, index packing, the output ring / done records) is
the next big item; the operand layout above is its starting point, and the private SPU of hle_geom.cpp gives an easy
check (same job on both paths, compare the MFC puts).

## Step 2: what a geometry job does to main memory (2026-11-26)

Because a job now runs on a private SPU (`F1CE_GEOM_HLE`), a native replacement only has to reproduce its effects on
guest memory, not its local store. `F1CE_GEOM_HLE_TRACE=<n>` logs every MFC command of n jobs with put data and the
lines the atomics read. A typical mesh piece:
1. Ring allocation (fn 0x4220, called from the streaming engine): GETLLAR / PUTLLC on line 0x10414F80, word 0 = ring
   write pointer (0x35900000.., +size rounded up to 128 bytes, retried when the reservation is lost), word 4 = ring end
   (0x35E00000).
2. PUT of the kept triangles' indices (big-endian u16, 6 bytes per triangle, padded) to the allocated ring space.
3. GET of the per-SPU done records (0x60 bytes at 0x10414F00).
4. PUT of 0x30-0x40 bytes of RSX commands into the piece's slot of a command buffer (0x3124xxxx, from the params):
   `0008181c <ring offset> 00000011` (SET_INDEX_ARRAY_ADDRESS: offset, u16 in main memory), `00041808 00000005`
   (SET_BEGIN_END triangles), `40041824 <count-1 << 24>` (DRAW_INDEX_ARRAY), `00041808 00000000`, zero padding.
5. GETLLAR on the in-order line 0x10414E80 (word 8 = 0x20001000, a kick address; words 12-13 counters), sometimes a
   PUTLLC; a barrier; PUT of this SPU's 16-byte done record at 0x10414F00 + SPU * 0x10 (base, mask bits); for some
   jobs a 4-byte PUT to the kick address and a PUTLLUC of the in-order line.

Per-opcode time with the HLE path (60 fps + LOD0 stress, `F1CE_GEOM_OP_PROF=1`), per job ~16.4 us: setup 1.0 us,
command list 13.5 us (handlers ~11.3, operand gathering ~2.1), rest of main ~1.2 us. Handlers: op35 3.3 us (native
cull loop 1.8, native transform 0.5), op0 2.5, op9 2.0, op12 0.46, op37 0.47, op4 0.3, op8 / op14 / op15 ~0.25;
op40 8.9 us and op32 5.4 us once per ~11 pieces.

Tried: an eight-wide cull with AVX2 gathers from host-order x / y / z arrays: exact (77k checks with
`F1CE_SPU_CULL_CHECK=1/2`) but slower on the Ryzen 5600G (2.6 against 1.8 us): gathers do not beat the four-wide loads
and transposes. Removed. `F1CE_SPU_NOCULL` (keep every triangle) adds ~10% vertices and ~17% draws but needs GPU-side
back-face culling to look the same (single-sided transparent parts); left for step 3.

(Done since: see "Native job" below.) Next: a native job for the common shape (ops 4 6 8 9 12 14 15 30 35 37 0, attribute mask 1) that reads the job record
directly and writes 1-5 itself, checked against the SPU job run dry (puts logged, atomics on a copy).

## Native job (`F1CE_GEOM_NJ=1`, `runtime/geom_native_job.cpp` + `hle_geom.cpp`, 2026-11-26)

Shapes (`F1CE_GEOM_SHAPES=1`): w1 and w2 get the same job counts (the same lists twice). ~96% of jobs are
`4 6 8 9[1] 12 14 [15] 30 35 37 0` with op8 word 2 = 0x00100000 (positions only, the first word varies 0x1C-0x30);
the rest add ops 23 24 32 40 and more attributes (~30 us each, declined: still ~0.5 cores at the stress test).

What the covered shape computes (from the job's code, checked against snapshots `F1CE_GEOM_SNAP` and the checks below):
op9 positions = packed big-endian float3; op12 = u16 triangles; op14 copies 0x68 bytes (viewport words, three projection
rows, the w row, half sizes, near value), op15 0x40 (world matrix, identity without it). 0xD400: Ci = Pi W, the
viewport scale with a +k (0.5 / 0.0625) on y, then the transform and cull of spu_ops.h with constants c34 = 2^-23,
c45 = 2^-22, c55 = 0x28800000, c52 = near, c71 / c69 = scissor width / height, c57 = facing sign from byte 1 of the
viewport word. The cull list is (a b c a) entries compacted in place; the slot after the kept ones holds the last
triangle dropped after them (padding).

The native job keeps the SPU code for everything that reaches main memory (op4, op12, op30, op37, op0: the index ring,
the RSX commands, the in-order completion) and replaces the rest:
- the kept triangles computed at the job's start straight from its input (8-wide transform, 4-wide cull, same
  operations in the same order);
- op35 writes them into the list and sets ctx[384] = kept * 3, ctx[388] = 0, ctx[1152] = 1;
- the stream is rewritten without ops 6 8 9 14 15 (they only fed op35);
- the command list (0x5960) runs from C++: [ctx+352] = the command, r3 = context, r4 / r5 = the 32 stream bytes from
  it (also at 64 / 80 of the loop's frame), r0 = 0x5C88, then [ctx+352] += 2 * length; r1 and r0 as the caller's at
  the end (generated callers compare r0 with the pc).

Checks: `F1CE_GEOM_NJ_CHECK=1` (kept triangles against the job's index put: 1.7 M jobs, 0 differences);
`F1CE_GEOM_NJ=2` (every 16th job twice from the same state, native with dry DMA then the original for real; puts compared
without atomics, the index ring by data, RSX command slots by total index count since the draw chunks follow the ring
address: 127k jobs, 0 differences, also in the other order with `F1CE_GEOM_NJ_REGDIFF=1`). Lesson: the list runner first
read the stream 2 bytes late (a register dump aligned down misread) and "worked" for a few jobs by luck.

Per covered job (stress, `F1CE_GEOM_OP_PROF=1`): 15.6 us with the HLE alone -> 11.8 us (setup 4.05 incl. the native
computation 2.1, run 7.76: op0 2.4, op37 0.47, op12 ~0.46, op4 0.34, op35 0.32, op30 0.15, the job's main and entry ~3.5).

Speed (carbench, three alternating pairs, `F1CE_GEOM_HLE=1` alone against `+ F1CE_GEOM_NJ=1`):
- 60 fps + `F1CE_CAR_LOD=0`, `F1CE_CPUS=8`: 44.9 / (crash, see below) / 44.3 -> 47.4 / 47.1 / 47.4 fps (+6%; the renderer
  is near its limit here, rsx 0.93); worker CPU 4.34-4.38 -> 4.00-4.03 cores.
- 60 fps, normal detail, `F1CE_CPUS=4`: 31.1 / 31.2 / 30.8 -> 34.9 / 35.3 / 36.0 fps (+14%); worker CPU 2.25-2.35 ->
  1.99-2.17 cores. Against the original path (~26.3 fps there) about +35%.
- The HLE-only run of pair 2 died in the menus with the music stream's read fault at 0x40000000 (thread 0x0100000C,
  workload 4, which the HLE does not touch): the MP3 decode poll race of 2026-11-09 on a path its probe does not cover
  ("job polls" never logged). Not caused by this work; the clamp in 0xB3A7C noted then is the fallback.

Scenario set with `F1CE_GEOM_HLE=1 F1CE_GEOM_NJ=1` (30 fps): 420 s race (23 M native jobs), heavy rain Spain, Monaco
Time Trial, Quick Race -> pause -> Replay, f1ce_online boot, three 150 s races: no FATAL, no stall. OpenGL Monza: the
first run died at boot with the known intermittent "RSX: unmapped IO offset 1f800000 (put=00012060 get=0004442c)"
before any 3D (no geometry job had run); the rerun passed (7 M native jobs).

## Step 4 so far: branch `rsx-parse-thread` on top (2026-11-26)

The commit 75c623d (FIFO parsing on its own thread) applied to this branch's tree (its core.cpp hunk merged by hand: the
futex helpers out of the anonymous namespace, 32 line slots kept). With `F1CE_GEOM_HLE=1 F1CE_GEOM_NJ=1`, three
alternating pairs, `F1CE_FIFO_THREAD=0` against the default (on):
- 60 fps + LOD0, 8 CPUs: 48.1 / 48.1 / 47.7 -> 47.9 / 47.4 / 47.6 fps (no change; renderer thread 0.93 -> 0.83 core).
- 60 fps, 4 CPUs: 34.9 / 34.5 / 35.3 -> 37.4 / 38.1 / 39.0 fps (+9%).
In the stress case no thread is saturated any more (workers ~0.8, renderer ~0.8, main ~0.7 core, ~6.2 of 8 CPUs): the
frame is bound by its dependency chain (main waits for jobs, jobs for main), so shorter jobs help and renderer work such
as write tracking (`hash_range` ~9% of the renderer) would not raise the frame rate on this machine. Write tracking
not started: PPU writes would need page protection; worth it only where the renderer thread is the limit.
4 CPUs in all: original path ~26.3 fps -> HLE 31.0 -> native job 35.4 -> parse thread 38.2.

Default since 2026-11-27 (after the user's PC session at 60 fps and the Steam Deck test at Original settings, 16 W max
at the Silverstone start): `F1CE_GEOM_HLE` and `F1CE_GEOM_NJ` are on unless set to 0.

## Skinned pieces (`F1CE_GEOM_SK`, on unless 0, `runtime/geom_skin_job.cpp`, 2026-11-27, branch `geom-skinned-shapes`)

Next step 1 below. Census at the 60 fps + LOD0 race start (`F1CE_GEOM_SHAPES=1`): ~10% of the jobs (not 4%) are
`4 6 8[.. 00140000] 9[3] 12 14 23 24 30 32 35 37 40 0` (drivers, pit crew; 9[7] with a third attribute is 0.6%), and
their command lists took 26.7 us (op40 8.5, op32 4.7, op9 2.7), ~0.47 cores. Each of their handlers except the ones
that reach main memory (4 12 30 37 0) and the cheap 6 8 14 35 is now native code that leaves the local store as the
SPU handler does (outside the stack and the dead w words), and the list runs from C++ (`nj_list`).

What the handlers do (`F1CE_GEOM_SNAP_OP=23` snapshots, now also per handler `snap_<job>_h<handler>_a/b.bin`; the op
filter of the snapshots never worked before: it only counts after 20 s of geometry jobs, i.e. of race):
- Vertex format (op8): two attributes, stride 18: float3 position at 0, half3 normal at 12 (output = input format).
- op9 (0xB300, mask 3): per attribute (highest bit first: normals, positions) an array of nv4 16-byte entries at the
  bump pointer; float = the 16 bytes at the vertex (0xBE00), half = 4 halves to floats ((h & 0x7FFF) << 13 + 0x38000000
  unless 0, sign kept; 0xBF80). Bookkeeping: ctx[704] count, ctx[720 + 4n] arrays, ctx[784 + n] ids, ctx[800 + 4 (id -
  1)] arrays (800 positions, 804 normals); every lane of the bump quad ctx[272..284] moves on by its own size.
- op23 (0xB700, 10 operands): four buffers at the bump pointer (ctx[448..460]): the bone program (one code per four
  vertices, end byte 8 appended), shared bone halfwords, per-vertex bone halfwords, weights (bytes / 255 as floats,
  0xC180: csflt + fma). (A second weight set h8 and float weights are declined: not seen.)
- op24 (0xB280): (h2 + 1) & ~1 bone matrices, 4x4 at the source, the first three columns as rows (0xC580); ctx[464].
- op32 (0xB980 -> 0xEFA0 for mode 3): threaded code (jump tables 0x12A80.. per mode). Codes: 4 one shared bone, 6
  shared bones with a weight quad each, 10 a bone per vertex (halfwords 1 0 3 2 for vertices 0-3) with weight quads, 2
  a bone per vertex without weights, 0 the previous group's matrices, 8 end; bit 2 of a bone halfword = another bone
  follows (for code 10: vertex 2's). Rows fm(w, M) then fma(w, M, acc). Transform (0xF598): positions fma(PY, B,
  fma(C, PZ, fma(A, PX, D))); normals by the cofactor rows (cross products of the rows; which product is rounded and
  which fused differs per element and between 0xF598 and the single-bone path 0xF900), the z term first after codes 4
  and 0, then `n * frsqest(|n|^2)` (fi = identity). In place; the w words keep shuffle leftovers nobody reads.
- op40 (0x6A30): the format kernel pointer ctx[896 + 4k] is 0, so per attribute 0x66B8 -> 0x61C8 -> encoder (0xE200
  float: bytes copied; 0xE700 half: fma(x, 1, 0), truncated, exponent < 112 -> 0, >= 144 -> 0x7BFF). nv rounded up to
  4 vertices, at ctx[320] aligned to 16 (0x4C18; a full 0x7800-byte output buffer would be put out first: declined);
  before it 0x8970 makes a table of nv8 halfwords v << 4 (ctx[928..936]); ctx[864] = vertex offset in the output,
  ctx[1200 + 4n] = ctx + 864, n = ctx[1184]++. op0 then puts indices + vertices in one put and sets RSX 0x1680 /
  0x1688 (position / normal array offsets, stride 18) before the draw.

Checks: `F1CE_GEOM_SK_CHECK=1` runs every native handler and then the SPU handler from the same state and compares the
local store below the stack (w words of the vertex arrays skipped): 650k handler runs, 0 differences.
`F1CE_GEOM_NJ=2` now also runs skinned jobs twice (native then original) and compares their puts (ring data by
content, RSX slots with the vertex / index array offsets zeroed): 205k jobs, 0 differences.

Per handler (stress, `F1CE_GEOM_OP_PROF=1`): op9 2.6 -> 1.0 us, op23 0.8 -> 0.26, op24 0.33 -> 0.11, op32 4.7 -> 2.3,
op40 8.5 -> 2.0; time in jobs (setup + run) 2.26 -> 1.89 cores at the same frame rate.

Speed (carbench, three alternating pairs, `F1CE_GEOM_SK=0` against 1, `F1CE_THREAD_CPU=1`):
- 60 fps + LOD0, 8 CPUs: 45.8 / 45.5 / 45.9 -> 46.4 / 46.9 / 46.9 fps (+2%); SPU workers 3.98-4.07 -> 3.79-3.85 cores.
- 60 fps, normal detail, 4 CPUs: 36.1 / 35.8 / 37.5 -> 36.1 / 36.2 / 36.6 fps (no change; not limited by these jobs).
Scenario: 150 s race at 30 fps (Vulkan) clean, drivers render; Linux cross build compiles.
Left for the skinned shape: op35 (cull, SPU handler with the native loops, ~2 us), op40 / op32 not vectorised
(2.0 / 2.3 us), the 9[7] variant (third attribute, 0.6% of jobs).

## Step 2: the rest of a covered job (2026-11-27, same branch)

Per covered job at the stress start (`F1CE_GEOM_OP_PROF=1`): setup ~4 us (native cull 2.5, transform 0.55, HLE ~0.9),
run ~5.7 us (op0 2.1, job entry 0.9, op37 0.46, op12 0.44, op35 0.31, op4 0.30, after the list 0.24, op30 0.17).
- op12 (0x7C50) and op4 (0x7790) native for covered and skinned jobs (geom_skin_job.cpp, `F1CE_GEOM_NJ_OPS=0` turns
  the covered-job use off). op12: round_up8(ntri) entries (0xBD80 converts eight a step); in covered jobs only the
  entries op37 can pack as padding after the kept ones are written (kn .. round_up8(kn) + 8), the rest is dead (the
  native op35 writes the kept entries): 0.44 -> 0.10 us. op4: 0x90 parameter bytes, and 0xBA80 (both addresses
  aligned) ends with a store at + 0xC0 of the quad that was at + 0x80 before the copy; an optional 16-byte GET:
  0.30 -> ~0.1 us. Checked: SK_CHECK 1.49M handler runs, NJ=2 170k jobs, 0 differences.
- The kept vector of run_job is reused (a vector per job showed as new / delete, 1.6% of the workers).
- Tried and reverted: (1) a third cull rewrite, per-vertex values (cvt, edge flags: exact, since min / max, the
  saturating add and truncation are monotonic) and a branch-free scalar loop per triangle: exact (2.5M jobs) but
  2x slower; pieces have ~1000 triangles and the four-wide loop already does about four per 10 cycles. (2) Not copying
  the positions / triangles of set 1 (the elements holding them, slack bytes copied) and reading them from main memory
  in the native job: exact, 64% of the gathered bytes left in main memory, but no change in setup time (the cull
  takes the cache misses the copy took). Lesson: a stray non-static getenv in a per-job path cost 4 us per job.
- op0 (0x8C80): ~1560 SPU instructions a call (`F1CE_GEOM_OP_PROF` slot 29 counts them). Per-word counts
  (`F1CE_SPU_NOBLOCK=1 F1CE_GEOM_PC_HIST=1` -> op0_hist.txt; note this mode bypasses the idioms, so loops that have one,
  0x4AE4 zeroing and 0x8910 vertex table, look hot there but are not) showed two loops worth an idiom: the vertex-record
  scan at 0x8E08 (8 sets, all records 0 in covered jobs: `spu_idiom_skip_records`) and a 128-byte word copy at 0x9838
  (`spu_idiom_copy_words`); both run all but the last iteration natively, so the registers stay exact. op0 2.07 ->
  1.58 us (instructions -25%); NJ=2 171k jobs, 0 differences. What is left in op0 is ~1000 instructions of straight
  code over the whole protocol (command block, done-record windows, in-order kick); the remaining loops (6 done
  records, 0x9108 / 0x9138) are worth ~50 ns. A native op0 would save another ~1 us a job at the risk of the
  completion protocol.
- Left: op0's protocol (see above), op37 (its first
  allocation initialises the output buffer through the job manager's services +0x30 / +0x34), the job's entry (0.9).
  The SpinLock in the profile (2.6%) is contention on the queue / in-order line atomics.

## Next steps (as of 2026-11-27, ranked by expected payoff)

State: geometry HLE + native job on by default, FIFO parse thread on. PC (Ryzen 5600G): 60 fps held at race starts;
the stress case is bound by the frame's dependency chain, no thread saturated. Steam Deck: Original settings at 30 fps,
16 W max at the Silverstone start; the remaining dips (race start, pit stop) are GPU-bound there.

1. (Done for 9[3], see "Skinned pieces" above.) The declined job shapes (`... 9[3] / 9[7] 12 14 23 24 30 32 35 37 40 0`, 3.5-4% of jobs, ~30 us each, ~0.5 cores
   in the stress case; heavier during the Deck's pit stop: 12-16k SPU instructions per job). Animated / skinned pieces:
   besides indices they write processed vertices to the ring and repoint vertex arrays (RSX 0x1680 / 0x1688). Needs
   the semantics of op23 (10 operands), op24, op32 (~5.4 us) and op40 (~8.9 us). Start with `F1CE_GEOM_SNAP_OP=23`
   snapshots and the trace; check with the run-twice harness (`F1CE_GEOM_NJ=2`, extend the comparison to the vertex
   data in the ring).
2. (Partly done, see "Step 2" above.) The rest of a covered job (11.8 us): op12 (builds the a b c a list) and op37 (packs the kept triangles) could be
   native now that the kept list is known (~1 us with their command overhead); the HLE setup (~1.9 us: descriptor,
   gather list, buffer bookkeeping) and the job's entry / main (~1 us) are the next fat. The native cull (2.4 us) resisted
   two rewrites (AVX2 gathers, branch-free compaction: both slower on Zen 3). op0 (2.4 us, completion protocol) stays
   SPU code unless reverse engineered with care (in-order windows, kick).
3. The frame's dependency chain on the PC: profile what the main PPU thread waits for per frame (job waits, flips) now
   that neither the SPU side nor the renderer is saturated.
4. Deck GPU: the 1280x720 4x MSAA scene pass took ~33 ms during the pit stop (`gpu slow submission` lines). Look at the
   pass breaks / resolves there, or recommend MSAA 2x on the Deck.
5. Step 3 of the handoff (culling on the GPU): keep every triangle and let the GPU cull back faces; removes the cull and
   transform (~3 us a job) but needs the per-piece facing sign on the GPU side and a visual check (single-sided
   transparent parts). `F1CE_SPU_NOCULL=1` shows the cost: ~10% more vertices, ~17% more draws.
6. Step 4 write tracking (instead of `hash_range` / `hash_fp`): parked; it only helps where the renderer thread is the
   limit, and PPU writes would need page protection.

Open issues seen during this work: Monaco Time Trial ~1 s frames in 1 of 4 runs (not reproduced since; watchdog prints
`geom hle jobs:`); the MP3 decode read fault at 0x40000000 once (workload 4, the old race on another path; fallback =
clamp the sample count in 0xB3A7C); the intermittent OpenGL boot fault "unmapped IO offset 1f800000".
