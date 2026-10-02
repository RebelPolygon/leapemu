# JIT recompiler

The JIT translates ARCompact code to host machine code as it is first run, for x86-64 and
64-bit ARM (AArch64) hosts. It is the default CPU backend on both (Tools > Settings > Emulation,
or `--cpu jit|cached|interpreter` in the CLI). On other hosts, and with the experimental
cache timing model, the cached interpreter runs instead.

- `src/core/arc/jit.{h,cpp}`: the shared part. It forms blocks, keeps the executable
  memory and the block cache, and links blocks.
- `jit_x64.cpp` and `jit_a64.cpp`: the two code generators, with the same rules.
- `x64.h` and `a64.h`: their emitters. They have no dependencies and encode only the
  instructions used, from the Intel 64 and IA-32 Architectures Software Developer's
  Manual and the Arm Architecture Reference Manual (A64).

**AArch64.** One generator covers every 64-bit ARM host. ARMv8-A is the same everywhere;
the platforms differ only in how executable memory is handled, and `jit.cpp` deals with
that:
- macOS needs `MAP_JIT` memory, made writable per thread while code is written;
- ARM caches need the instruction cache flushed after code is written;
- x18, reserved on Apple platforms and Windows, is never used.

The differences from the x86 generator that matter for exactness:
- **Flags:** read from NZCV, with N and Z in ARC's order and the carry inverted after a
  subtraction (ARM's C means "no borrow").
- **Carry in:** ADC / SBC set ARM's carry from STATUS32.C first, inverted for SBC.
- **Shifts:** ARM's shifts don't set the carry, so the bit shifted out is taken
  explicitly.

It was developed with qemu-user (`cmake -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-linux-gnu.cmake`,
then `qemu-aarch64 -L /usr/aarch64-linux-gnu`). Its state hashes match the x86 cached
interpreter's for every cartridge and download booted from power-on for 30 s with input,
and for long gameplay runs. It has not been timed on real ARM hardware yet.

## Results are identical

The JIT keeps the cached interpreter's results exactly: the same machine state after
every frame, cycle count included. So input movies, save states and rewind behave the
same on either backend. This rests on a few rules.

- **Guest registers live in memory** (`Cpu::r_`). Host registers are only scratch
  within an instruction, so any instruction can fall back to the interpreter's own
  handler at any point.
- **Time is charged per block, but is exact wherever it is observed.**
  - A block's instruction costs are added up when it is compiled. Data-access wait
    states are added as they happen, and the total is charged when the block exits.
  - Devices read the time during I/O accesses. Before an I/O access, or before calling
    an interpreter handler, the block charges the cost so far first. The clock then
    reads exactly what the interpreter's would at that instruction.
- **Time slices end on the same instruction.** The interpreter runs an instruction only
  if the slice has not ended. A block checks on entry that its last instruction (and
  a delay slot compiled into it) will start before the end of the slice. If not, the
  interpreter runs those instructions one by one. The check uses the most the block
  can take, with every memory access at the slowest wait state.
- **Blocks end where the interpreter's loop looks at the state between
  instructions.**
  - The idle-loop head, where idle skipping decides whether to jump ahead. The hint
    moves between the places that poll the power control register; every place it
    has been ends a block.
  - The end of a zero-overhead loop (`LP_END`). A block that a loop end falls inside
    does not start. A variant that stops at that loop end runs instead.
- **Compiled code never goes stale.** ROM can't change. The few RAM pages that are
  compiled are watched for stores (see below).

## Blocks

Code is compiled from ROM, and from RAM only where a program runs from it (a
Leapster 2 download's image; `Cpu::set_ram_code`). Those RAM pages are watched: a
store to them takes the slow path. A store over an instruction that has been decoded
stops the block after it. That instruction is decoded again before the next one runs,
and the compiled code is dropped, so the result is the same as the interpreter's.
Stores to data in those pages cost only the slow path. Other code in RAM, such as the
BaseROM's interrupt vectors, is interpreted.

A block runs from its first instruction to the first branch, or at most 64
instructions. It also ends at a 64 KiB page boundary, an idle-loop head or a loop
end. Most instructions are translated to native code:
- moves and ALU operations, with their flags (x86 flags map directly onto ARC's
  N, Z, C, V);
- the conditional (`.cc`) forms, `adc` and `sbc`, and the single-operand shifts and
  rotates;
- loads and stores in every addressing mode, including address writeback. A load or
  store looks up the bus's page table inline, and goes out of line only for I/O;
- branches, compare-and-branch, and jumps through a register;
- a taken branch's delay slot, compiled into the taken path;
- the 64-bit multiplies (`mul64`, `mulu64`, `mul64_s`: MLO, MMID and MHI).

Anything else (`lp`, `flag`, `sr` / `lr`, ...) calls the interpreter's handler. The block ends after it if it changed the flow of control or the loop end.

**Linking.** Each exit whose target is known jumps through a slot that first leads back
to the dispatcher. Once the target block exists, the slot is pointed straight at it, so
hot code runs from block to block without returning. A jump through a register (a
function return) looks its target up in the JIT's block cache. Every block checks the
time slice and the loop end on entry, so linked blocks keep the rules above. A block that
would run past the end of the slice doesn't start; the rest of the slice is interpreted.

**Generated code.** Guest registers live in the CPU structure, so most operands are
loaded from and stored to memory. A register stored and loaded again right after (no jump
target between) is not reloaded.

## Speed

20 emulated seconds on a Core i9-13900K, including about 0.1 s of loading:

| Game | Cached interpreter | JIT | |
|---|---|---|---|
| Sonic X (gameplay) | 4.1 s | 0.76 s | 5.4× |
| Go Diego Go! (gameplay) | 4.1 s | 0.72 s | 5.7× |
| SpongeBob (menus) | 2.4 s | 0.70 s | 3.5× |
| Pet Pals | 4.2 s | 1.12 s | 3.8× |
| Go Diego Go! (cutscene) | 2.5 s | 0.73 s | 3.4× |
| Letterpillar (download, gameplay) | 2.95 s | 0.57 s | 5.2× |

Sonic X gameplay, 60 emulated seconds: 1.46 s (41× real time). The native 64-bit
multiplies matter most there: that game runs `mul64_s` about a million times a second.

Flash games gain less: more of their time is spent outside the CPU, and they already
idle-skip most of it.

## Checking and debugging

- `build/jit_diff BIOS CART STATE CYCLES [step] [buttons]` runs the JIT and the cached
  interpreter side by side from a state and compares them after every step. On a
  difference it bisects to the exact cycle. It then prints both CPUs and the code
  there, and the first block that started at a different place or time than the
  interpreter.
- `leapemu-cli --cpu cached` and `--cpu jit` with `--state-hash` must print the
  same hash. This holds for every cartridge booted from power-on for 30 s with some
  input, for the recorded input movies, and for long gameplay runs.
- Environment variables:
  - `LEAPEMU_JIT_STATS=1` prints counts at exit (block runs, interpreted
    instructions, guards, compiles); `=2` also lists the instructions still going
    through the interpreter;
  - `LEAPEMU_JIT_NOCHAIN=1` turns linking off;
  - `LEAPEMU_JIT_DUMP=dir` writes each block's code to `dir/<address>.bin`, for
    `objdump -D -b binary -m i386:x86-64` (on ARM, `aarch64-linux-gnu-objdump -D -b binary
    -m aarch64`);
  - `LEAPEMU_JIT_PERFMAP=1` names each block for `perf` (`/tmp/perf-<pid>.map`), so a
    profile shows guest addresses (`arc_800d03ba`). On a hybrid CPU, record one core
    type (`taskset -c 2 perf record -e cpu_core/cycles/u ...`).

## Bugs found, and what they taught

Each of these broke the state-hash equality with the interpreter, and `jit_diff`
found the cycle where the two first differed. They are the reasons for some of the
rules above.

- **The idle-loop hint moves.** Idle skipping watches the instruction that polls the
  power control register (`0x0180'9008`) from the system's idle task. That is not one
  place: the RTOS polls it from several, and the hint moves between them as tasks
  wait in different ways. A block compiled across a place that later became the hint
  ran past it, so the interpreter's idle check never saw it. The JIT now keeps every
  place the hint has ever been (`idle_pcs_`): a block never extends over one, and a
  new one drops the blocks that might.
- **State left over from an interpreted branch.** When a block falls back to the
  interpreter's handler for a branch, the handler records a pending redirect or delay
  slot in the CPU. If that branch was the block's last instruction, the next block
  started with the flags still set and took the redirect again. The CPU's redirect
  and delay-slot state is now cleared before every block runs.
- **Linked blocks and the end of a time slice.** A block reached through a link
  checks the slice on entry and may decline to run; the dispatcher must then re-read
  the program counter (the declining block did not move it) and check the time again
  before choosing what to run. Using the values from before the linked jump ran an
  instruction twice.
- **An environment variable read on a hot path.** A debugging switch read with
  `getenv` on every block cost far more than the block itself. Such switches are now
  read once.
- **AArch64: a helper that overwrote its input.** The helper that builds `1 << n`
  for the single-bit operations used w1 as scratch while n was still in w1, so it
  shifted by the wrong amount. It now builds the constant in w2. Only the ARM build
  differed from the interpreter.
- **AArch64: a 32-bit null check on a 64-bit pointer.** Null tests on host pointers
  used a 32-bit `cbz`, which also takes a valid pointer whose low 32 bits are zero
  for null. They are 64-bit now.

## Not done yet

- Computing flags only when something reads them.

**Tried and dropped: a register cache.** Guest registers were kept in four host
registers (rbp, r13-r15) within a block and written back before every exit, hook and
fallback. The results were identical to the interpreter, but it was no faster: from 5%
slower to 4% faster across Sonic X, Go Diego Go!, SpongeBob, Letterpillar and Schoolhouse
Rock.
- Blocks are short, since they end at the first branch.
- A guest register is rarely used more than twice in one.
- Memory operands on the CPU structure are already cheap (store forwarding).

The JIT runs at about 1.4 host cycles per emulated cycle. Most of the rest goes on what
keeps timing and interrupts exact: the page-table walk, wait states and attention check
on every memory access, and STATUS32's flags. Dropping the attention checks entirely
would gain about 4%.
