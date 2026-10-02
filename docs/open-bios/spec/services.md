# Spec: the interface registry (system ↔ cartridge services)

Clean-room specification; see `../CLEANROOM.md`. Provenance tags are as in `boot.md`.

Tools: `build/firstuse` (`tools/abi/firstuse.cpp`) and `build/callmap`
(`tools/abi/callmap.cpp`, registry-annotated report). Observations cover Sonic X,
Cars, Cars Supercharged and Go Diego Go (native-code, Torus framework) and Pet Pals
(Flash), 60 s each, on the v1.5 BaseROM.

## 1. Locating the registry [obs]

- At cartridge entry, `gp = 0x3c000100` (boot.md §4).
- Cartridge code reads the 32-bit word at `gp + 4` (`0x3c000104`). This was the first
  cartridge access to the registry in Sonic X: `add r0,gp,4; ld_s r0,[r0]`.
- That word points to the **registry**, which lives in RAM: at `0x3c1c2df4` for Sonic
  X and `0x3c1c2af4` for Cars, i.e. right after each cartridge's module data area.

**Requirement:** the open BaseROM must enter the cartridge with `gp = 0x3c000100` and
a registry pointer at `gp + 4`. Everything else is reached through the registry.

## 2. Registry layout [obs]

| Offset | Size | Contents |
|---|---|---|
| +0 | u16 | version, `0x0100` |
| +2 | u16 | entry count, 16 in every cartridge observed |
| +4 | u32 | 0 |
| +8 + 12·i | 12 bytes | entry *i*: `u32 0`, `u32 tableA`, `u32 tableB` |

- **Each table is an array of 32-bit code pointers.**
- **`tableB = tableA + 0x20` in every entry.** Each interface has 8 leading common
  slots (A[0..7]) followed by its own methods (B[k] = A[8+k]).
- **A[2] exists on almost every interface** and returns 1 when called. **[hyp]** It is a
  lifecycle method, e.g. init.

**Ownership:** entries whose tables are in cartridge ROM are *provided by the
cartridge* and called by the system. Entries in BaseROM space are *provided by the
system*.

| Entry | Native-code cartridges | Pet Pals (Flash) |
|---|---|---|
| 0, 3, 4, 13 | cartridge | system |
| 1, 2, 5–12, 14, 15 | system | system |

## 3. Slots used by the four native-code cartridges [obs]

Counts are calls in 60 s.

**Cartridge → system:**

| Slot | Sonic | Cars | Cars SC | Diego | Notes |
|---|---|---|---|---|---|
| 1.A[0], 1.A[2] | 1, 7 | 1, 7 | 1, 7 | 1, 7 | |
| 1.B[0] | 93 | 41 | 39 | 120 | returns 0 |
| 2.A[0], 2.A[2], 2.B[1], 2.B[3], 2.B[5] | ✓ | ✓ | ✓ | ✓ | 2.B[3] up to 2101 calls |
| 5.A[2], 5.B[0] | ✓ | ✓ | ✓ | ✓ | |
| 7.A[2], 7.B[16] | ✓ | ✓ (134) | ✓ (134) | ✓ (110) | 7.B[16] returns a RAM pointer |
| 8.A[2], 8.B[1], 8.B[3] | ✓ | ✓ | ✓ | ✓ | 8.B[1] returns a RAM pointer |
| 9.A[2], 11.A[2], 14.A[2] | ✓ | ✓ | ✓ | ✓ | |
| 10.B[2], 10.B[3], 10.B[4] | – / 3 / – | – / 81 / 1 | 21 / 73 / 1 | 21 / 73 / 1 | |
| 14.B[3..8], 14.B[17] | ✓ (B[4], B[17] ~3400 each) | | | ✓ | Sonic and Diego only |

**System → cartridge**, into the cartridge's own entries 0, 3, 4 and 13:
- At least 105 different slots of entry 0 were called (slot indices go up to 132 at
  least). The heaviest are
  `0.B[102]`, `0.B[103]`, `0.B[2]`–`0.B[5]`, `0.B[125]` and `0.B[132]`, each called
  thousands of times per minute.
- Several return RAM pointers: `0.B[50]`, `0.B[51]`, `0.B[56]`, `0.B[58]`, `0.B[59]`.
- **[hyp]** Entry 0 is a runtime/allocator interface that the cartridge supplies to the
  system.

## 4. Next steps (specification team)

1. Record per-call behaviour for each slot above:
   - arguments;
   - return value;
   - memory written (ranges, relative to arguments);
   - hardware registers touched;
   - whether the call blocks or switches task.
2. Name each interface by its observed role and document each method as behaviour.
3. Record boot-time registration: which system call installs the cartridge-provided
   entries, and in what order the system calls A[0]/A[2] on each interface.
