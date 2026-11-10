## CreateSuspended; trapped MMIO on x86-64 POSIX hosts; guest-CPU scheduling closer to the console; RECOMP_WATCH on Linux

### Problem

On an x86-64 Linux (or Intel macOS) host, `xbox_PosixMmioFault` had no
decoder (only arm64 did), but the APU aperture was still unmapped for
trapping. With `RECOMP_AC97_READY=1`, the first APU register access was an
unhandled SIGSEGV. The AC'97 bus-master reset trap and the OHCI model
switched themselves off on the same hosts, so DirectSound's channel reset
spun forever and titles saw no controllers.

Separately, under `RECOMP_GUEST_LOCK=1`, `PsCreateSystemThreadEx`
soft-released the guest CPU like any other kernel call. The new thread could
take the CPU in that window and run before its creator returned. Red Dead
Revolver's Bink reader thread then read its event handle from a context Bink
had not filled in yet (a stale `0x900` from recycled memory) and crashed.

### Change

- `src/platform/mmio_decode.h`: new `mmio_ctx_t`. On Windows it is
  `CONTEXT`. On x86-64 POSIX it is a struct holding the 16 GPRs, RIP and
  EFLAGS, with `mmio_ctx_from_ucontext` / `mmio_ctx_to_ucontext` to copy them
  from and back to the signal handler's `ucontext_t` (glibc `gregs` order,
  spelled numerically so includers need not define `_GNU_SOURCE`). The
  decoder takes `mmio_ctx_t *` instead of `PCONTEXT`; on Windows that is the
  same type. Callers there are unchanged.
- `src/apu/apu_mmio_hook.c`: the x86-64 hook builds on x86-64 POSIX too, with
  an `apu_hook_handle_mmio_posix` that converts the context. Registers are
  written back only when the access was serviced, so a decode failure leaves
  the context intact for the crash report.
- `src/kernel/xbox_memory_layout.c`: the AC'97 write trap is armed and
  serviced on x86-64 POSIX through the same decoder. `xbox_PosixMmioFault`
  routes AC'97, APU and OHCI on x86-64.
- `src/usb/ohci.c`: `OHCI_TRAP_POSIX_X64`, the same pattern as the arm64 path.
- `src/kernel/kernel_guest_cpu.c`: `xbox_GuestCpuRelease` keeps the CPU
  across `PsCreateSystemThreadEx` (ordinal 255) and returns 0, so the
  dispatcher does not re-acquire. The new host thread does its TIB/TLS setup
  without the CPU and queues in `guest_cpu_join`. The creator never waits on
  it, so this cannot deadlock. On the console a same-priority thread also
  waits behind its creator.

- `src/kernel/kernel_guest_cpu.c` (second change): a waiter no longer
  takes the guest CPU while its holder is inside a kernel call that cannot
  block. Under `RECOMP_GUEST_LOCK` every kernel call soft-releases the CPU,
  and a queued thread's 2 ms poll could find it free inside, say,
  `NtCreateEvent`. Red Dead Revolver's Bink creates its reader thread and
  *then* the event the reader waits on, so about one run in three the reader
  waited on a handle that did not exist yet. A soft-released CPU may now be
  taken by another thread only once the call has lasted
  `RECOMP_GUEST_SOFT_GRACE_MS` (default 10). Hand-offs and calls that may
  block switch at once, as before.
- `src/kernel/xbox_memory_layout.c`: `RECOMP_WATCH` on x86-64 Linux prints
  the writing function (faulting RIP) and a glibc backtrace, where it used
  to print "host stack not available". This is what showed that the OHCI
  model was writing a title global. (The cause there was physical
  contiguous addresses that overlap an 8 MB image; `RECOMP_CONTIG_AVOID_IMAGE=1`
  already handles that. This PR does not change it, but turning it on by
  default for large images may be worth discussing.)

- `src/kernel/kernel_bridge.c`: **`PsCreateSystemThreadEx` honours
  `CreateSuspended`** (argument 7, previously never read). A suspended spawn
  with a handle gets a manual-reset event; the host thread does its setup and
  waits on it before joining the guest CPU; `NtResumeThread` on the thread's
  token sets it (and reports a previous suspend count of 1). This is the root
  cause of the Bink reader crash above; the scheduler changes narrowed it.

- `src/kernel/nv2a_pb_exec.c`, `nv2a_backend.h`: **software-method hook.**
  Under `RECOMP_PB_WALK=dma` XDK D3D, finding the GPU far behind, writes
  `NO_OPERATION(5)` and sleeps until the GPU's interrupt for it sets an event.
  The runtime has no PGRAPH interrupt (plain-memory registers cannot show
  D3D's write-1-to-clear ack, so its DPC would re-handle the method forever).
  `nv2a_pb_set_sw_method_hook` lets a title act on the method when the walk
  reaches it; `xbox_KeSetEventFromHost` (kernel_bridge.c) sets a guest KEVENT
  from that host thread. Red Dead Revolver froze on its legal screen without it.
- `tools/recomp/lifter.py`, `translator.py`: jump tables are read up to 1,024
  entries (was 256). A 571-arm switch lost its upper arms to unresolvable
  indirect jumps.
- `src/kernel/kernel_bridge.c`: `RECOMP_THREAD_DUMP` prints each guest
  thread's latest kernel call, caller, first two arguments and guest stack
  with the 2-second kernel summary. This is what named the D3D wait above.

- `src/kernel/nv2a_pb_exec.c`, `nv2a_combiner.c` (rasteriser):
  - a batch smaller than 8 px either way is treated as object space only
    when it sits near the origin (small screen-space glyphs were dropped);
  - the screen-space path skips a black-diffuse quad whose texcoords run
    16+ pages past its texture (it has no combiners to make it black);
  - per-size Morton tables for swizzled textures, divide-free power-of-two
    wrapping, and register combiners decoded once per program
    (`RECOMP_RC_CHECK=1` compares every pixel with the old evaluator: 0
    differences in 420M pixels);
  - the raster pool is portable (POSIX through the platform shims),
    defaults to cores - 2, and also takes screen-space triangles;
  - a `[GPUPROF]` line with the fps report.

### Testing

- `tests/mmio_decode` builds and passes on Linux x86-64 against the new type.
  It needs an empty `windows.h` shim plus `-DCONTEXT=mmio_ctx_t`, because the
  test itself includes `windows.h`.
- Red Dead Revolver (NTSC-U, XDK 5849) on Linux x86-64 / GCC 13 runs as
  follows:
  - DirectSound initialises, and the AC'97 reset completes on write.
  - APU interrupts are claimed.
  - OHCI goes operational and enumerates the pad.
  - The Rockstar logo and the full title movie play, then the front end's
    legal screen. Bink's reader has not crashed in the runs since (3 of 3;
    about 1 in 3 crashed before).
  - Without the patch it stops at the first APU access.
- Built and run on Windows (MSVC) by the title project's user with the
  first version of this patch; the Windows paths only see the type rename.

### Notes

The game-side fixes (binding D3D's fence semaphore with
`nv2a_pb_set_semaphore_target`, `RECOMP_TITLE_KEVENTS`) are in the title
project, not here.

🤖 Generated with [Claude Code](https://claude.com/claude-code)

https://claude.ai/code/session_018ZHkEYeEFk6i8xbQtNdMwr
