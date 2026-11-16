# Bootstrap 09 — native lifecycle verification

## Purpose

Bootstrap 09 is a read-only diagnostic build. It does not install hooks, patch executable bytes, alter game state, or bypass BattlEye. It retains the version-08 descriptor/object capture and adds narrowly scoped captures to verify leads from an older runtime snapshot.

## Why these targets

The older full-section archive was logged as bootstrap 06. Its log reports a `.text` SHA-256 fingerprint that does not match the archived `.text.bin`. Therefore its disassembly addresses are leads, not established current-build facts.

Bootstrap 09 re-reads these RVAs from the live image after the version-08 hotkey trigger:

| Candidate | RVA | Capture size | Previous interpretation (unconfirmed) |
|---|---:|---:|---|
| DestroyCurrentVehicle - Avatar mode wrapper | `0x1FA430` | `0x180` | Vehicle destruction/cleanup in avatar mode |
| SpawnReplicaVehicle wrapper | `0x1FA530` | `0x180` | Vehicle replica creation |
| Lifecycle caller | `0x204800` | `0x600` | Conditional caller of the two wrappers |
| Cleanup helper | `0x841100` | `0x240` | Helper leading to vehicle cleanup |
| Cleanup helper inner | `0x841780` | `0x300` | Reads candidate fields at object offsets `+0x568` and `+0x570` in the old snapshot |

The capture verifies that each requested range is inside an executable image section before reading it. The output files are raw memory windows; they must be disassembled and compared with the previous snapshot before assigning function semantics.

A second lead is the global pointer slot at RVA `0x343C630`. In the old snapshot it contained `0x1AA7418B080`. Bootstrap 09 captures the slot, then a bounded readable window at the candidate target, and qwords at offsets `+0x1DD8`, `+0x1DE8`, and `+0x1E68`. If those values are plausible external pointers and readable, it captures bounded target windows too. These offsets are hypotheses, not known fields.

## Trigger and output

1. Start the game normally with the version-09 `dinput8.dll` beside `TheCrew2.exe`.
2. Enter Freedrive and wait until the game is stable.
3. Press `Ctrl+Shift+F10` once.
4. The capture is written to `TC2_runtime_capture/capture_<pid>_<tick>/`.
5. Zip the entire `TC2_runtime_capture` directory, including `TC2_Walkmod.log`, and provide the ZIP.

The capture still includes the six version-08 descriptors, their referenced arrays, and the 0x100-byte referenced-object windows. It then writes the five native-code windows and the manager-candidate captures.

## How to interpret results

- `VER09_CODE_OK`: the exact live code window was read and saved. It does not prove the old function label.
- `VER09_CODE_SKIP`: the RVA was outside the image or not in a section marked executable. Do not force a read.
- `VER09_MANAGER_GLOBAL_SLOT`: records the live qword at RVA `0x343C630`.
- `VER09_WINDOW_OK`: a bounded memory window was read. It is not proof that the memory is a complete C++ object.
- `VER09_MANAGER_FIELD`: raw qword at a candidate offset. It is not a semantic field name.
- A null, noncanonical, unaligned, unreadable, or non-executable candidate is a valid negative result. Do not replace it with guessed offsets.

## Project research milestones

| Stage | Work completed | Result / limitation |
|---|---|---|
| Research package 01 | Initial binary/hash probes and DZ.Blood setting inspection | `AllowedOutOfGameplayZone` was later resolved as a local `CITEntityGeographicalSpot` field, not a global walking switch. |
| Static analysis stages 02–05 | Parsed and cross-checked the three `nbCF v2` entity binaries; restored the complete server XML tree; traced avatar archetypes, spawn points, mission atoms, and teleport graphs | Establishes data structure and graph wiring, not native handler semantics. |
| Bootstrap 06 | Safe in-process image/section scan and hash probe | Captured runtime sections. The archived log fingerprint for `.text` does not match the archived `.text.bin`, so old code disassembly remains provisional. |
| Bootstrap 07 | Readable-section/runtime capture iteration | Improved bounded/readability checks and runtime section handling. No verified player-control handoff found. |
| Bootstrap 08 | Targeted descriptor and referenced-object capture | One triggered capture: 6 descriptors, 79 heap-reference arrays, 1 skipped reference, 203/203 object windows captured. HumanPlayer and GhostPlayer share 14 array pointers exactly. No player manager or control handoff identified. |
| Bootstrap 09 | Fresh verification of lifecycle code windows and one global-manager candidate | Intended to confirm or reject the old native-code and manager-root leads. No hook is installed. |

## Current confirmed baseline

- `CITEntityAvatarTC2`, `CITEntityAvatarInterior`, `CITEntityHumanPlayer`, and `CITEntityGhostPlayer` are distinct archetype types in the data.
- Mission `Spawn` and `TeleportEntity` records exist, but the inspected Spawn instance is a narrative-video spawn; TeleportEntity moves an existing entity.
- No confirmed native chain has yet been established for creating the walking avatar in arbitrary Freedrive coordinates and assigning it to the local player's input/camera/physics.
- The central unknown remains the relationship between the player object, its currently controlled entity, and the vehicle/avatar lifecycle.

## Version and branch safety

- Branch: `analysis-05-native-lifecycle-ver09`
- Base: `starter-08-referenced-object-capture`
- `main`, `starter-08-referenced-object-capture`, and prior analysis branches are not modified by this work.
- Bootstrap 09 is diagnostic only. It is not the walking mod.
