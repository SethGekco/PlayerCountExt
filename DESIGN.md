# PlayerCountExt — Design

Addresses are marked **✔ verified** (disassembled from `gamemd.exe`, or named in
the Antares PDB symbol map, or both) or **⚠ unverified** (a guess — must not be
compiled into a shipped hook). This convention is load-bearing: see rule 1 in
the README.

Reference binary: vanilla `gamemd.exe`, sha1 `189a5a868b3cef8d3d1a58ac3cf0a5241675e4ea`,
md5 `fe2301a1f48841aa084aade100b25335`, 4,813,072 bytes.
Disassembly method: `objdump -D -b binary -m i386 --adjust-vma=0x400000`
(file offset == RVA).

---

## 1. Where the "8" actually lives

There is no single constant. In order of how hard each is to lift:

| # | Wall | Where | Status |
|---|---|---|---|
| 1 | AI creation loop bound | `0x6882C5` ✔ | the real >7-AI wall |
| 2 | House array capacity | `0xA80228` ✔ | dynamic, but needs explicit expansion |
| 3 | Start-position counters | `0x68AF45` ✔, `0x6883E6` ✔ | two of them, not one |
| 4 | Score-screen buffers | `0x5C98F1`–`0x5C9FDD` ⚠ | addresses from prior art, unverified by us |
| 5 | 32-bit per-house bitfield | structural | the real ceiling, ~30 houses |

### 1.1 `ScenarioClass::AssignHouses` — `0x687F10`–`0x68837D` ✔

Single `ret` at `0x68837D`. Four stages:

| Stage | Range | Bound |
|---|---|---|
| Human/player houses | `0x687F59`–`0x688146` | `Session.Players.Count` — **dynamic** |
| AI houses | `0x68814C`–`0x6882CB` | **pointer-bounded to exactly 8** |
| Neutral | `0x6882D1`–`0x688320` | unconditional |
| Special | `0x688325`–`0x68836B` | unconditional |

Recovered facts:

- `HouseClass::HouseClass(HouseTypeClass*)` = **`0x4F54A0`** ✔, `__thiscall`,
  `ECX` = this, one stack arg. Returns `this` in `EAX`. Called 4x at
  `0x687FC3`, `0x6881A0`, `0x6882FE`, `0x688351`.
- `sizeof(HouseClass)` = **`0x160B8`** ✔ (the literal pushed to `operator new`
  at `0x7C8E17`, which is exactly what `YRMemory::Allocate` jumps to).
- `HouseClass::ColorSchemeIndex` at **`+0x16054`** ✔.
- Neutral/Special are resolved **by name string** — `"Neutral"` @ `0x82BA08`,
  `"Special"` @ `0x817318` — through `0x5117D0`, then constructed.

**The human path has an ~8-byte stack flag array** at frame offset `0x24`
(`mov 0x24(%esp,%eax,1),%bl`) inside a `sub $0x4c,%esp` frame, indexed by player
index. It cannot be widened by byte-patching. Harmless while humans ≤ 8; it is
the reason a full reimplementation beats a patch if that ever changes.
*(Strong reading — `esp` shifts across pushes were not fully tracked.)*

**There is no colour-picker hang in YR.** Tiberian Sun's `Assign_Houses` spins
in a `while(true)` looking for a free colour and hangs past 8 houses. YR does
not: it reads a *stored* colour index and converts it with one call to
`SessionClass::GetPlayerColorScheme` (`0x69A310` ✔). Do not port that warning
over from Vinifera. Antares separately lifts every colour bound that does exist
(7 hooks in `src/Misc/Interface.PlayerColors.cpp`).

### 1.2 The AI cap — `0x6882C5` ✔

The single instruction capping AI at 8. It is **not** a `cmp $0x8`:

```asm
688158:  bb 9c b2 a8 00     mov  $0xa8b29c,%ebx   ; &AISlots.Countries[0]
6882c2:  83 c3 04           add  $0x4,%ebx
6882c5:  81 fb bc b2 a8 00  cmp  $0xa8b2bc,%ebx   ; <<< THE CAP
6882cb:  0f 8c 8c fe ff ff  jl   0x68815D
```

`0xA8B2BC − 0xA8B29C = 0x20` = 8 ints. The bound is the *end address* of the
array, baked in as an immediate — which is why grepping for the constant 8 never
finds it. `0xA8B2BC` is simultaneously the start of `Colors[8]`, so an overrun
reads the neighbouring array: **data corruption, not a clean fault.**

**AISlots layout** ✔ (0x20 stride):

| Address | Array |
|---|---|
| `0xA8B274` | AI player **count** |
| `0xA8B27C` | `Difficulties[8]` |
| `0xA8B29C` | `Countries[8]` |
| `0xA8B2BC` | `Colors[8]` — also the loop end-bound |
| `0xA8B2DC` | `Starts[8]` |
| `0xA8B2FC` | `Teams[8]` |

> **⚠ YRpp's `AISlotsStruct` is mislabelled here.** It declares
> `AIDifficulties[8]; StartingSpots[8]; Colours[8]; …`, which makes what it calls
> **`StartingSpots` actually `Countries`**. The disassembly is unambiguous: the
> loop does `mov (%ebx),%edi` then indexes `HouseTypeClass::Array` (`0xA83C9C`)
> with that value. Prefer the raw addresses above over YRpp's field names.

### 1.3 Two start-position counters ✔

`0x68AF45` (Phobos hooks it but leaves `i < 8`) **and** `0x6883E6` — a second,
independent `cmp $0x8` that no framework hooks. The second is min'd against
`(players − observers + AIPlayers)` at `0x68841F`, so it clamps the effective
player count. Anyone lifting only the first stays capped.

Note the working prior art lifts **neither** — it keeps the stock 8 and repairs
the downstream `HouseIndices` table instead, forcing AI starts into `0..7`. That
works, at the cost of houses co-spawning on shared start positions.

### 1.4 The real ceiling — structural

`1u << ArrayIndex` into a `DWORD`, across `HouseClass::Allies`, `AltAllies`,
`TechnoClass::DisplayProductionTo`, `CellClass::BaseSpacerOfHouses`. 32 bits
minus Neutral and Special = **30 houses**. Where between 25 and 30 it actually
breaks is untested.

Do not confuse this with the *country* bitfield (`IndexBitfield<HouseTypeClass*>`,
e.g. `Owners=`), which caps **countries** at 32 on an independent axis. Many
houses may share one country, so more players never requires more countries.

---

## 2. Architecture

**Standalone DLL, not a spawner fork.** Syringe loads it beside Antares/Phobos
and the stock spawner. Nothing to rebase against upstream CnCNet.

**Config from `spawn.ini`.** The client writes it and the host broadcasts it, so
every client agrees. A DLL can read it directly — it is just a file in the game
directory. A client-local INI works offline and desyncs online; we do not use
one. `spawn.ini` already carries `[Other1]..[OtherN]` and `Multi1..MultiN` with
no hardcoded limit in the client's writer.

**The mapping is `AISlots[i] <-> Multi(i+1)`** (MultiN is 1-based), with
human-owned slots left at `-1`:

| `spawn.ini` | Engine |
|---|---|
| `[HouseCountries] MultiN` | `Countries[8]` `0xA8B29C` |
| `[HouseColors] MultiN` | `Colors[8]` `0xA8B2BC` |
| `[HouseHandicaps] MultiN` | `Difficulties[8]` `0xA8B27C` |
| `[SpawnLocations] MultiN` | `Starts[8]` `0xA8B2DC` |
| `[Settings] AIPlayers` | `0xA8B274` |

**RUNTIME-CONFIRMED** across two live skirmishes with different countries and
seeds — every populated slot agreed on both country and colour, zero mismatches.
Conveniently the engine's empty-slot sentinel is also `-1`, so "key absent -> -1"
round-trips against its own convention.

Two Win32 INI footguns handled in `SpawnConfig.cpp`:
- `GetPrivateProfile*` resolves a **bare** filename against the *Windows*
  directory, not the cwd — hence `.\spawn.ini`, or every read silently returns
  its default. *(Proven: the parser finds the file.)*
- `GetPrivateProfileInt` parses as **unsigned** and returns 0 for negatives.
  `-1` would become `0`, a valid country index. We `strtol` the raw string.
  *(Defensive only — observed `spawn.ini` files contain no literal negatives, so
  this path is not yet exercised; the `-1`s in logs come from absent keys.)*

**`AIPlayers` is not clamped by the client** — `GameLobbyBase.cs` writes
`settings.SetIntValue("AIPlayers", AIPlayers.Count)`, a raw count. So raising the
client's `MAX_PLAYER_COUNT` is sufficient to get `AIPlayers > 7` into
`spawn.ini`; this DLL does **not** need to override `0xA8B274`.

**Approach to the AI cap.** Two known routes:

- **Batch-refill** — at `0x6882C5`, refill the stock 8-wide array with the next
  batch and rewind `EBX` to the base, re-entering the engine's own loop. No
  relocation. This is what the prior art does; it caps at 16 because it permits
  a single refill.
- **Relocate** — point `0x688158`/`0x6882C5` at a larger array. Requires finding
  every other consumer of `0xA8B29C`.

Batch-refill is simpler and proven. Its weakness in the reference implementation
is that refilled slots are *derived* (colours computed, countries/teams cycled)
rather than read from config — but `spawn.ini` already carries per-house data
for the later batches, so a refill that reads it would give fully independent
countries, colours and teams. **That is our intended improvement.**

**Determinism.** See README rule 5. The refill path is exactly where a desync
would originate, so it must be pure arithmetic over host-broadcast data.

---

## 3. Current state

Implemented and CI-green:

- `src/GameConstruct.h` — construction of engine types that are *abstract to the
  compiler*. `HouseClass` inherits five pure virtuals from `AbstractClass`
  (`GetClassID`/`Load`/`Save`/`WhatAmI`/`Size`), so `GameCreate<T>`, `new T`, and
  **placement-`new`** all fail — the abstract check fires on the construction,
  not the allocation. We allocate raw and call `0x4F54A0` through a `__thiscall`
  function pointer, so the compiler never sees a `new` expression. Allocates the
  engine's `0x160B8`, not `sizeof(HouseClass)`, because YRpp's mapping may be
  smaller and would under-allocate.
- `src/Instrumentation.cpp` — two behaviour-free logging hooks (`0x687F10` entry,
  `0x688378` epilogue) that validate every address above against a live game.
  The epilogue is hooked at `0x688378` because the `ret` at `0x68837D` is one
  byte and too short to patch.
- `src/Main.cpp` — `-SPAWN` gate at `0x52F639` ✔ (`YR_CmdLineParse` in the
  Antares PDB; clean 5-byte boundary; `ESI` = argv, `EDI` = argc).

**RUNTIME-VALIDATED 2026-08-23.** A live 1-human + 2-AI skirmish (Antares +
Phobos + CnCNet spawner) confirmed every address in section 1: `0xA8B238`
GameMode, `0xA8DA78`/`0xA8DA84`, `0xA8B274`, `0xA8B29C`, the 0x20 stride, and
`+0x16054` (cross-checked via YRpp's struct *and* the raw offset — agreed on all
five houses). Neutral and Special confirmed created unconditionally and last.

Two findings only a live run produced:

- **⚠ The AI-loop sentinels SKIP; they do not terminate the loop.** All three
  jumps (`jge` on the AIPlayers test, `je` on -1, `je` on -3) target `0x6882C2`,
  which is the `add $0x4,%ebx` **increment**. The loop always walks all 8 slots,
  and `EAX` counts houses *created* (incremented only at `0x68817E`), not the
  slot index. Observed: `Countries[] = {-1, 0, 6, -1, …}` — slot 0 holds the
  sentinel — still produced 2 AI houses, from slots 1 and 2. **A batch-refill
  implementation must account for this**: the loop does not stop early, so
  refilling is about *what the slots contain*, not about extending a scan.
- **⚠ `AssignHouses` runs TWICE per game start**, with `HouseClass::Array.Count`
  back at 0 on the second — torn down and rebuilt, not appended to. Matches the
  two call sites (`0x68745E`, `0x68ACFF`). **Any hook here must be idempotent or
  explicitly one-shot.**

**Deployment note:** Syringe does **not** auto-scan the game folder on this
setup. The inject list is `Resources/Compatibility/Unix/wine-game.sh` line 2;
a DLL absent from it is never loaded — no handshake, no hooks, no log, and the
game behaves perfectly vanilla.

### Next
1. Run the instrumentation; confirm or refute the address map.
2. Read the house set from `spawn.ini`.
3. Batch-refill at `0x6882C5`, reading real per-house config.
4. Milestone is **9 houses**, not 30 — every crash on the way to 9 is a hook site
   found cheaply.

### Open questions
- **Are the per-player frame-sync queues sized per *connection* or per *house*?**
  Per-connection is harmless; per-house blocks the online goal. Unverified, and
  the highest-risk unknown.
- The score screen and loading-screen player indicators reportedly still cap at 8
  in *display*. The loading-screen marks come from the client-side map Preview,
  so they may not be fixable in a DLL at all. For the score screen,
  `0x5CA110` `Game_GetMultiplayerScoreScreenBar` (Antares PDB) sits just past the
  known score hooks and is unhooked — **untested lead**.
- The four house-wiring helpers called after construction (`0x4FCE00`,
  `0x50B840`, `0x50BA00`, `0x4F6EC0`) have not had their signatures recovered.
