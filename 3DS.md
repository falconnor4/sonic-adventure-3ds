# The 3DS target

`make 3ds` builds *Sonic Adventure* for the New 3DS: the game's C, as the PC's
native slice compiles it, with devkitARM for the console's ARM11, linked with
the engine's runtime core, SA1's adapter and client, and a citro3d backend for
the PICA200. The matching reconstruction and the Dreamcast port are untouched:
`make` and `make dc` read none of the 3DS target's make file or sources.

[ENGINE.md](ENGINE.md#build-goals) lists every build goal, and
[PORTS.md](PORTS.md) covers the Dreamcast port.

## Goals

| Command | What it builds or checks |
|---|---|
| `make 3ds` | `build/3ds/sa3ds.3dsx`, and the probe |
| `make 3ds-image` | `build/3ds/sa3ds.3dsx` alone |
| `make 3ds-cia` | `build/3ds/sa3ds.cia`, which a title manager such as FBI installs to the HOME Menu |
| `make 3ds-sd` | an SD card's tree under `build/3ds/sd` (`3DS_SD=DIR` for another) |
| `make 3ds-probe` | `build/3ds/sa3ds-probe.3dsx`, which needs no disc |
| `make 3ds-check` | Which sources compile, and which wait on the disc |
| `make devkitarm` | devkitARM from devkitNix, linked as `tools/devkitpro` |

The image reads the disc's data under `build/gen/`, which `make pc` writes from
the disc in `orig/`. The host tools it runs need GNU make, Python 3 and a C++
compiler; on NixOS, `nix-shell` in the checkout gives them (`shell.nix`).

The `.3dsx` and the `.cia` hold data from your disc in their RomFS, and the
icon is the disc's label, and the banner the game's logo from the disc's web
pages (`ICO_ADV.GIF`) with the opening of its title theme
(`engine/3ds/host/art.cpp`), so they are for your own use: don't share them. A
picture of your own at `art/3ds/banner.png` (which git ignores) takes the
logo's place, fitted to 256 by 128 on clear.
The icon can't be clear: the HOME Menu's icons hold no alpha, so the label
lies on white.

The CIA is a New 3DS title, platform SNAKE with a KTR- product code: a
console started it as a CTR title in the Old 3DS's 64 MB however its exheader
asked for 124 MB. Where a console gives the game too little memory all the
same, it says so on the screen, with the figures, and asks for a restart. The
exheader asks for no 804 MHz or L2 cache, which made other homebrew start in
64 MB; the runner turns both on as it starts.

The CIA takes makerom (Project_CTR's) and bannertool (carstene1ns's fork),
which nixpkgs doesn't package: `make 3ds-cia` fetches each release's Linux
build into `tools/3ds-tools/`, checks it by its sum, and on NixOS patches its
loader with patchelf, so run it in `nix-shell`. The CIA's exheader asks for the
New 3DS's 124 MB, 804 MHz and L2 cache (`engine/3ds/sa3ds.rsf`). The CIA also
carries the disc's files in its RomFS, about 1.1 GB in all, so it needs nothing
on the SD card but the DSP's firmware; the game reads `romfs:/disc` where it is
there, and else the SD card's `sa3ds/disc/`, as the `.3dsx` does.

## Running it

The SD card holds:

| Path | What |
|---|---|
| `3ds/sa3ds.3dsx` | the game, which the Homebrew Launcher lists (not needed with the CIA installed) |
| `sa3ds/disc/` | the disc's files: `1ST_READ.BIN`, `SONICADV/` and the rest (not needed with the CIA) |
| `3ds/dspfirm.cdc` | the DSP's firmware, which DSP1 dumps; without it the game is silent |
| `sa3ds/card.bin` | the memory card, which the game makes on its first run |
| `sa3ds/game.log` | the run's log |
| `sa3ds/settings.txt` | optional settings, below |

`settings.txt` takes a setting a line:

| Line | Does |
|---|---|
| `stage N` and `act N` | begin that act as the opening ends, as the PC's `make play` does (`stage 1` `act 0` is Emerald Coast) |
| `keys PATH` | play a script of keys, as the PC's `--keys` does |
| `shots N` | write the top screen to `sa3ds/shots/` every N vsyncs |
| `thread 1` | draw on a thread of its own on the third core (off by default: Azahar runs that core slowly) |

The 3DS's A, B, X, Y, START and the D-pad are the Dreamcast pad's; L or ZL is
the left trigger and R or ZR the right; the circle pad is the stick. HOME and
the power button work as in any application.

Under Azahar (nixpkgs' `azahar`), boot the `.3dsx` with its SD directory holding
the tree above. Azahar emulates the old 3DS's 268 MHz by default; at its CPU
clock setting of 300% it runs as fast as a New 3DS's 804 MHz, where the game
holds its 30 frames a second in Emerald Coast. Azahar's HLE DSP plays sound
with any file at `3ds/dspfirm.cdc`.

## What the target does

### The game's C

The target builds what `make pc` builds: every image's C, 1ST_READ.BIN's and
each program the loader reads, the SDK's C the slice takes, SHC's runtime, the
disc's content, `asm2c.py`'s C and play math. Each source goes through make
dc's float pass and `marks.py arm`, the PC's mark pass without the marks: the
adapter's hooks, SHC's `long float` and the float pass's constants, and the
SH-4 layouts' asserts. A pointer, a `long` and an address are 32 bits on the
ARM11 as on the SH-4, so no pointer needs widening.

The VFP runs in RunFast mode, flush-to-zero and default NaN, rounding toward
zero as the SH-4 does: outside RunFast the ARM11 bounces operations to support
code that the 3DS's kernel doesn't give.

### Fixed addresses

A `.3dsx` always loads at `0x00100000`, and maps nothing else
([measured with the probe](#the-memory-domain-measured)). `rambase.py` rewrites
the marked tree, and `--casts` each preprocessed unit, so that:

- main RAM, the game's `0x0c000000` and its `0x8c` and `0xac` views, is 16 MiB
  of BSS at `3DS_RAM`, `0x02000000`, which the linker script places past the
  image's own data and bss;
- a pointer cast of an SH-4 register or a Dreamcast device's address points
  into `sa3ds_devices`, a megabyte of zeros, as the PC maps zeros there;
- the store queues, which Ninja's drawers write vertices through, point into
  `sa3ds_store_queue`;
- `syCacheICI`'s call through the SH-4's P2 view is a plain call.

The disc's data under `build/gen/` does hold RAM addresses, as pointer casts
(the camera layouts name their files' areas); words that merely look like
addresses, as the fonts', stay.

### Programs

The PC links every program at once, its data resident: some 63 MB. Two images
the Dreamcast loads at one origin are never live together (the memory
contract's colive), so `overlays.py` gives each origin a region as long as its
largest program's data and bss, about 6 MB in all. A program's initial data
lies in the RomFS, `romfs:/programs/NAME.bin`, and `loads.c` copies it in as the
game's loader reads the program's file.

### The runner and the engine

`engine/3ds/` holds the target's own sources:

| Source | Does |
|---|---|
| `main.c` | the runner: the log, the settings, faults, the RAM map, the game's main |
| `front.c` | the front end: the engine, SA1's client, pacing at 59.94 Hz, the pad |
| `backend.cpp` | the engine's services on citro3d, HID, the SD card and the DSP |
| `threads.c` | the game's threads, switched by an ARM context switch |
| `loads.c` | the programs' data, from the RomFS |
| `symbols.c` | the statics the adapter finds by name, from `romfs:/symbols.bin` |
| `draw.v.pica` | the vertex shader, with the stereoscopic eye shift |
| `host/index.cpp` | the host's index of the disc's textures, `romfs:/textures.idx` |

Drawing reuses the PC's CPU side: `engine/pc/scene.cpp` lights and places each
point as the game does and sorts the blend list (by triangle, as the PICA200
can't sort per pixel). The backend carries the corners into clip space and
draws the batches through the texture combiner. The game draws its 16:9
picture onto the top screen's 400 by 240. With the 3D slider up, each eye's
picture shifts by depth in the vertex shader; the 2D layer stays at the
screen. In play, the HUD's counters, the 2D sprites in the picture's corners,
go to the bottom screen at 1.6 times their size, over a blue gradient that
stands there in every mode.

Play math (`engine/numeric/`) keeps the SH-4's bits but for FIPR and FTRV,
which the 3DS build (`SA_NUMERIC_NATIVE`) computes with the VFP's own
arithmetic: their exact forms took about a third of the ARM11's time in play.

Sound is the PC's AICA engine, `engine/pc/sound.cpp`, rendered on a thread of
its own (the third core where there is one) into the DSP's buffers. Music and
voices stream from the SD card through a 32 KB window rather than whole.

## Not done yet

- Fog, and the points' offset colors (specular highlights), aren't drawn.
- Rumble: the 3DS has none.
- Real hardware is untested: the figures above are Azahar's. The memory the
  image needs, some 54 MB and a 58 MB heap of which about 32 MB is used in a
  stage, assumes the New 3DS's 124 MB application region.
- Play math's results may differ from the Dreamcast's where the SH-4 fuses a
  multiply and add, which the ARM11's VFPv2 can't, so recorded demos may part
  from their recording.

## The memory domain, measured

What a homebrew `.3dsx` can address is narrower than the PC's 4 GB domain;
experiments on Azahar decided the layout:

| Experiment | Result |
|---|---|
| A program linked at `0x0c000000`, `0x0b400000` or `0x00100000` | It always runs at `0x00100000`: the 3DSX format carries no segment addresses |
| `svcControlMemory(MEMOP_ALLOC)` at a fixed address | `0xe0a01bf5` at every address and size |
| `svcCreateMemoryBlock` + `svcMapMemoryBlock` at `0x0c000000` | The map fails `0xe0a01bf5` |
| A `NOLOAD` region of 16 MiB in the image's data segment | Mapped, zeroed and writable |

So the only fixed addresses a `.3dsx` gets are those of its own image, and its
BSS reaches any address between its end and the application region's.

## The toolchain

devkitARM is the only toolchain that builds libctru: its newlib carries
devkitPro's OS-device patches. `make devkitarm` runs

```sh
nix build --no-link --print-out-paths github:bandithedoge/devkitNix#devkitARM
```

and links `tools/devkitpro` to the output's `opt/devkitpro`, keeping the store
path in `tools/devkitpro.txt`. `DEVKITPRO` names the tree, and defaults to that
link.
