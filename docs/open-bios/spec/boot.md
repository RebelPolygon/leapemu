# Spec: cartridge validation and boot handoff

Clean-room specification. See `../CLEANROOM.md`. Each fact lists its provenance.

**Provenance tags**
- **[obs]** black-box observation in leapemu (tool and command given);
- **[cart]** read from cartridge data;
- **[pub]** public documentation;
- **[hyp]** hypothesis, not yet confirmed.

Tools: `tools/abi/ribdump.py`, `build/callmap` (`tools/abi/callmap.cpp`), and
`build/cartreads` (`tools/abi/cartreads.cpp`). The experiments modify copies of
cartridge images and fix up their checksums.

## 1. Image layout

The header at 0x100 and the RIB table follow
[Leapster-Tools](https://github.com/lfhacks/Leapster-Tools) **[pub]**. See also
`../../cart-bios-abi.md` §1.

All examined cartridges **[cart]**:
- Header version is 1.0 with 1 RIB. The device range is `0x80000000–0x807ffff4` for
  8 MiB images.
- The full-checksum pointer is `0x807ffffc` and the sparse-checksum pointer is
  `0x807ffff8`, so the two checksum words are the last 8 bytes of the image.
- The boot-safe table is 0, and the RIB is at `0x80000144`.
- The `LEAP` header is `4c 45 41 50 00 01 nn 00`: minor 0, major 1, then the group count.

## 2. Checksums

**Full checksum [cart]:** the 32-bit wrap-around sum of all little-endian words from
image offset 0 up to (not including) the sparse-checksum word, stored at
`image_end - 4`. It matched 4 of 4 cartridges tested.

**Sparse checksum [cart]:** the 32-bit wrap-around sum of the little-endian words at
image offsets `0, 0xF94, 2*0xF94, …` while `offset + 4 <= device_end - device_start`.
It is stored at `image_end - 8`.
- It matched 5 of 5 cartridges that have a non-zero value.
- Get Puzzled! (USA) stores 0 and boots **[obs]**. **[hyp]** 0 means "not checked".
- The access pattern (one 32-bit read every 0xF94 bytes, starting at 0xF94 after the
  header reads) was observed with `cartreads` **[obs]**.

**What the system checks at power-on [obs]**, from single-field edits of a Sonic X
(USA) image with the checksums re-computed unless stated:

| Edit | Result |
|---|---|
| Sparse checksum wrong | rejected: "?" cartridge screen |
| Full checksum wrong (sparse right) | boots |
| One byte changed at 0x400000, checksums not updated | boots (not on the sparse grid) |
| "Copyright" verse (product info id 4) text changed | boots |
| Build-user string (product info id 0xD, image offset 0x6e0) one byte changed | rejected |
| id 5 text region copied from another cartridge | rejected |
| Product info id 5 entry removed | never launches (system halts) |

So an open BaseROM that wants compatible behaviour must reject images whose sparse
checksum is wrong. Whether it enforces the security block is a policy choice: the open
BaseROM is not required to, and should not.

## 2b. Launch prerequisites found with homebrew images [obs]

A minimal image built by `homebrew/sdk/mkcart.py` was used to find what the system
requires before it calls a cartridge's entry point. The image is run with the emulator
option that makes LeapFrog's checks pass (see `docs/homebrew.md`: the digest, the
copyright-verse check and the sparse checksum; the image's checksums are valid, so
only the first two matter here).

- **Product ids 2 and 3 are required.**
  - Without them, the system reads the product-info ids, then shows a cartridge icon
    and never enters the cartridge.
  - Record format [cart]: `u16 0x0100`, `u16 count`, 8 reserved bytes, then `count`
    32-bit product ids.
  - Id 3 ("provided") lists the cartridge's own product id.
  - Id 2 ("needed") lists `0x00010001` in all examined cartridges. **[hyp]** This is the
    base system's product id.
- **Ids 4 (verse), 0xD, 0xE, 0xF and 0x10 are read during validation.** Id 0x10
  ("build validation") points to 20 bytes that the system compares with a value it
  computes.
- **Content checks performed by LeapFrog BaseROMs:**
  - the 20-byte digest compare;
  - a spot check of 11 characters of the id 4 verse at fixed positions. A single-letter
    edit elsewhere in the verse passes, which is why the id 4 edit in the table above
    booted.
  - Either failure shows the "?" cartridge screen.
  - **The open BaseROM must not implement these checks.**
- With the checks disabled, a homebrew image carrying ids 1–5, 7, 8 and 0xA–0x10 and one
  `0x01000007` boot descriptor is entered at descriptor+16. This works under v1.5, UK v2.1
  and Spanish v1.0.

## 3. Security block (product info id 5) [cart]

The block opens with LeapFrog's "Approved Content" legal text. After it comes a
signature-like record: an ASCII line `leapfrog2 <43 characters from [A-Za-z0-9$]>$`, a
build machine name, a build timestamp, and binary data. It is specific to each
cartridge.

**[obs]** The value compared at power-on is the 20-byte block that product-info id
0x10 points to; see §2b. The id 5 text is not compared directly. The open BaseROM does
not need any of this.

## 4. Boot group (RIB group 0x1000) [cart]

Entries are pairs of 32-bit values, `(type, value)`. Types seen:

- **`0x01000007`: module descriptor pointer.** Native-code (Torus framework)
  cartridges have two, and Pet Pals has one.
- **`0x01030008`: a code address.** Seen in native-code cartridges; its role is not
  yet known.

**Module descriptor [cart]:**

| Offset | Example (Sonic X) | Example (Pet Pals) | Meaning |
|---|---|---|---|
| +0 | `0x100` | `0x100` | version **[hyp]** |
| +4 | `0x3c000140` | `0x3c000200` | RAM address of the module's data area **[hyp]** |
| +8 | `0x001c2ca8` | `0x60` | size of that area **[hyp]** |
| +12 | `0x803f15e0` | `0x80009700` | ROM address of initialised data **[hyp]** |
| +16 | `0x800ef24c` | `0x80008748` | **entry point [obs]**: the first cartridge address executed |
| +20 | `0x800ef290` | `0x8000876c` | second entry (shutdown?) **[hyp]** |
| +24… | … | (name, address) pairs | Pet Pals: exported native functions such as `LF_GetCountryCode` (Flash extension library) |

**Register state at the first entry into cartridge code [obs]** (Sonic X, `callmap`):
- `r0` = the entry address
- `r1 = 0x803f2388`
- `sp = 0x03000718` (on-chip SRAM)
- `gp = 0x3c000100`
- `blink` = a BaseROM return address
- `status32 = 0` (interrupts disabled)

**[hyp]** The Sonic X data area `0x3c000140 + 0x1c2ca8` ends just below
`0x3c1c2e0c`, where the system's service directory lives (see `services.md`). That
suggests the system places its own structures after the module's data area.

## 5. Services (overview) [obs]

Cartridge code reaches system services through function pointers. It never calls
BaseROM code at a hard-coded address.
- During boot, the system writes a directory of pointers into RAM (around `0x3c1c2e0c`
  for Sonic X). Each points to a table of function pointers.
- Cartridge code reads entries from those tables, stores them in its own variables,
  and calls them indirectly.
- Sonic X called 54 distinct services in 60 seconds. `services.md` describes the
  directory and which services are used; what each service does is not specified yet.

## Open questions

- ~~How does cartridge code locate the service directory?~~ Answered: through the
  word at `gp + 4` ([services.md](services.md#1-locating-the-registry-obs)).
- What do boot type `0x01030008` and descriptor field +20 mean?
- How does the system decide between native entry and Flash playback?
- What is the minimum header and product info that the system accepts (apart from the
  security block)?
