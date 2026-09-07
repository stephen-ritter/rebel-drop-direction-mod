# RebelDropDirectionMod

A Just Cause 3 mod (x64, MinHook) that makes rebel drops spawn facing the
direction you're looking **when you throw the beacon**, instead of always
facing north.

## What it does

In the base game, rebel drops spawned via the beacon always orient
themselves facing north, regardless of where you're aiming. This mod
hooks the vehicle spawner and rewrites the drop's spawn transform so the
vehicle faces your camera's view direction. The facing is **locked in at
the moment the beacon is thrown**, so moving the camera while the flare
rolls across the ground cannot change the drop's orientation.

Works at every heading (including exact north/south/east/west) and at any
camera pitch, including steep downward looks from a high vantage.

## How it works

**Spawner hook.** The mod hooks the vehicle archetype spawner at
`JustCause3.exe + 0xDD3070`. After letting the original function set up
the spawn matrix, it overwrites the rotation of the 4x4 transform
(column-major, translation preserved at `[12..14]`, `[15] = 1.0`):

| Column  | Value        |
|---------|--------------|
| Right   | `(fZ, 0, -fX)` |
| Up      | `(0, 1, 0)`    |
| Forward | `(fX, 0, fZ)`  |

where `(fX, fZ)` is the horizontal (XZ-projected and normalized) throw-
locked camera forward. World axes: `+Z = north`, `+X = west` (left-handed).

**Throw-time lock.** The mod also hooks the drop lifecycle state machine
at `+0xDEd9e0`. When the drop's state enum (at offset `0x12C`) transitions
into the "beacon thrown / airborne" state — the moment the flare leaves
Rico's hand — the current camera forward is latched into a global. This
is the "initiation" event: the player is still aiming at the throw target.
The spawner hook then applies the latched vector instead of reading the
live camera, which is what makes the orientation immune to camera movement
during the flare's roll. If no throw has been latched yet (or the camera
read failed), the spawner falls back to a live camera read.

**Camera discovery.** The live camera forward is a unit vector stored at
`CCameraManager + 0x044` (the 3rd row of the row-major view matrix that
begins at `+0x024`). The `CCameraManager` is a heap-allocated object, so
there is no fixed address for it. Instead, the mod:

1. Scans committed RW memory for pointers equal to the class vtable
   (`JustCause3.exe + 0x2309C20`).
2. Selects the "richest" candidate (most unit-length vectors in its first
   `0x400` bytes) whose `+0x044` slot holds a unit vector. The heap is
   littered with transient objects that transiently hold the vtable word;
   the richness check distinguishes the real, persistent camera manager
   from that noise.

**No hitches.** The full address-space scan is expensive, so it only ever
runs on a background worker thread, which hunts once at startup and
re-hunts (self-healing) only if the cached manager becomes invalid. The
result is published to a `std::atomic<uintptr_t>`, and the game-thread
hot path — which runs inside the spawner hook — is just an atomic load, a
two-read validation, and a 12-byte read. With logging disabled (the
default), the hooks add nothing but a few cheap reads.

**Safe fallback.** If the camera vector can't be read (e.g., a spawn
within the first sub-second after load, before the worker's initial hunt
completes), the drop spawns with the game's default orientation and
nothing is corrupted. All game-memory reads are SEH-guarded.

## Building

- Visual Studio 2019/2022, x64, Release
- MinHook 1.3.3 via vcpkg (`packages.config` in the repo)

```
msbuild RebelDropDirectionMod.sln /p:Configuration=Release /p:Platform=x64
```

The build produces the plugin directly as
`x64\Release\RebelDropDirectionMod.asi`. No renaming is required — it is
already in the format the ASI loader expects.

## Installation

The mod is loaded by the
[Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader),
a proxy DLL that adds ASI plugin loading to the game process [1].

1. Download the Ultimate ASI Loader and place its DLL in the Just Cause 3
   game directory. It usually works as `dinput8.dll`, but if that doesn't
   work you can rename it to one of the other supported names (e.g.
   `d3d11.dll`, `version.dll`) [1].
2. Copy `RebelDropDirectionMod.asi` (straight from the build output) into
   the game's root directory or into a `scripts`, `plugins`, or `update`
   folder [1].
3. Launch the game. On first launch the mod creates
   `jc3_rebel_orient.ini` in the game directory (see Configuration).

To uninstall, delete the `.asi` file (and the ASI loader DLL if you want
the game back to its initial state).

## Configuration

Logging is **disabled by default**. On first launch the mod creates
`jc3_rebel_orient.ini` next to the game exe (next to `JustCause3.exe`),
containing a default template:

```ini
; RebelDropDirectionMod configuration
; Set Logging=1 to write jc3_rebel_orient.log next to the game exe.
[General]
Logging=0
```

You don't need to create the file by hand — it's created automatically if
it's missing, and never overwritten if it already exists. To enable logging,
set `Logging=1` and relaunch the game; the mod then creates
`jc3_rebel_orient.log` in the game directory automatically.

The setting is read once when the DLL loads, so apply it before launching.
To go silent again, set `Logging=0` (deleting the ini also works; it's
recreated with logging off on the next launch).

When enabled, useful log lines:

- `[Cam] hunt: live manager 0x... (richness N, M candidates)` — the
  background worker locked the camera manager (once at startup, or after an
  invalidation).
- `[Cam] hunt: no live manager found (M candidates)` — discovery failed;
  drops spawn with the default orientation until a re-hunt succeeds.
- `[Lifecycle] state 0 -> 1`, `1 -> 2`, … — the drop's lifecycle state
  machine transitions, used to confirm the state-to-phase mapping.
- `[Lifecycle] initiation - LOCKED facing (x, z)` — the facing vector was
  latched at the beacon-throw moment.
- `[ERROR] exception inside spawner hook` — an SEH-guarded read faulted at
  a spawn; inspect the surrounding lines.
- `[FATAL] ...` — the mod failed to install; the hook(s) are not active.

## Compatibility notes

All offsets are pinned to the `JustCause3.exe` build this project was
developed against (decompiled with Ghidra):

| Constant                  | Value     | Meaning                                        |
|---------------------------|-----------|------------------------------------------------|
| `OFFSET_VEHICLE_SPAWNER`  | `0xDD3070`  | Vehicle archetype spawner (hook)              |
| `OFFSET_DROP_LIFECYCLE`   | `0xDEd9e0`  | Drop lifecycle state machine (initiation hook)|
| `OFFSET_STATE_ENUM`       | `0x12C`   | Drop lifecycle state enum (param_1 + 300)     |
| `RVA_CCAMERAMANAGER_VTABLE` | `0x2309C20` | CCameraManager vtable (discovery anchor)     |
| `OFFSET_CAMERA_FORWARD`   | `0x044`   | Live camera forward (unit vector) inside CCameraManager |

If a game update moves any of these, the mod fails gracefully (drops face
north) rather than crash. To re-pin: re-locate the spawner, re-locate the
lifecycle state machine and its state enum, re-locate the vtable, and
re-run the camera forward flip test (two throws 180° apart; the slot whose
vector flips 180° is the live forward).
