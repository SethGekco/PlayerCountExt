# MegaSkirmish — Project Handoff

> Handoff for a fresh agent (e.g. Claude Code). Assumes no prior context.
> Front-loads current state so you can resume the build loop immediately.

## Goal
A standalone Syringe-injected DLL for **Command & Conquer: Yuri's Revenge**
(`gamemd.exe`, closed-source, 32-bit x86) supporting **>8 houses in the
comp-stomp shape — a few humans against many AI** (up to ~24 AI houses).
Not for more *humans*: the netcode bounds human connections at 8, and that
stays untouched. Drop-in like Phobos — Syringe auto-loads any hook-bearing DLL
in the game folder. Must stay GPLv3 (built on YRpp + Syringe).

**SCOPE CHANGED 2026-08-20 — online is now in scope.** This document previously
said "offline only" on the grounds that modified spawners violate CnCNet's ToS.
**That claim was wrong and is retracted.** CnCNet has officially integrated
Ares + Phobos + a spawner rewrite into its YR client; their rules target
*cheating* (unfair hidden advantage in online/public play), which comp-stomping
is not, and offline play never touches their services at all. The genuine
practical concern is narrower: the CnCNet client flags modified game files
mid-session. Do not re-propagate the ToS claim.

Because YR multiplayer is **lockstep-deterministic** (every client simulates all
AI locally; only human commands cross the wire), 4 humans + 20 AI never
approaches `Connection[7]` / `ListAddress[8]`. The gating problem online is
**desync, not connection count** — see "Determinism rules" below.

**Staging: offline (Skirmish/Campaign) -> LAN -> Internet.** Gate with
`SessionClass::Instance->GameMode` (`0xA8B238`, first field; enum Campaign=0,
LAN=3, Internet=4, Skirmish=5). Vanilla `AssignHouses` itself branches on this
at `0x687FCE`. NOTE: LAN is equally desync-sensitive — gating only on
`Internet` leaves it exposed.

Owner: Rex (GitHub: SethGekco). Works on Kubuntu; **cannot build MSVC locally**
— compiles via **GitHub Actions on a `windows-latest` runner** (repo:
`SethGekco/MegaSkirmish`). YRpp is fetched by CI, not a committed submodule.

## Why this is hard / key constraints
- `gamemd.exe` is closed. We don't recompile the game — we inject a DLL that
  patches it in memory via **Syringe** hooks, using **YRpp**
  (`Phobos-developers/YRpp`) — reverse-engineered headers giving real
  addresses/struct layouts. YRpp is the source of truth, not RA1/TS source.
- **CONFIG CHANNEL — REVERSED 2026-08-20.** This previously said: the CnCNet
  client UI caps at 8 slots, so read our own `megaskirmish.ini` which the client
  never overwrites, NOT `spawn.ini`. **That is now wrong for online play.** A
  client-local config file is a **desync bomb**: if two clients disagree on how
  many AI exist, they build different house arrays and desync on frame one.
  Online, the house set MUST come from the host-authoritative, broadcast
  channel — i.e. `spawn.ini`.
- Consequently the **CnCNet client fork is required infrastructure, not
  optional UX**. It is cheap: `MAX_PLAYER_COUNT = 8` is a single
  `protected const int` in `DXMainClient/.../GameLobby/GameLobbyBase.cs` that
  drives a *loop* building the player dropdowns; layout is INI-driven
  (`PlayerOptionLocationX/Y`, `PlayerOptionVerticalMargin`); and spawn.ini
  writing already emits `[Other1]..[OtherN]` / `Multi1..MultiN` in loops bounded
  by `Players.Count` with **no hardcoded limit**. Client is GPL-3.0, so forking
  is fine. (`megaskirmish.ini` is still fine for a purely offline v1.)
- **32-bit house bitfield ceiling:** `HouseClass::Allies` etc. are DWORD
  bitfields indexed by `ArrayIndex`. With Neutral+Special always present, the
  practical max is **~25 players** (27 of 32 bits). This is likely why the
  Tiberian Sun "25 players" mod stopped at 25.

## Critical architecture decision (already made)
The first approach — let the engine build 8 houses, then **bolt on** extra ones
afterward — was abandoned. Manually-constructed houses lack the engine's native
wiring (diplomacy, AI scheduling, color tables), causing the "house exists but
AI is inert / crashes later" failure.

**The chosen approach: reimplement YR's `ScenarioClass::AssignHouses`
(`0x687F10`) entirely and `Patch_Call` it in**, looping past 8, so every house
gets full native wiring via the engine's own sequence. On YR that sequence is
**construct (`0x4F54A0`) -> colour (`0x69A310` -> `+0x16054`) -> `0x4FCE00` /
`0x50B840` / `0x50BA00` (+ `0x4F6EC0` on the AI path)** — there is **no
`Init_Data`** on YR; that name came from the TS analogue.

The disassembly independently **reinforces** this decision: the human-player
path keeps its assignment flags in an ~8-byte *stack* array, which cannot be
widened by byte-patching. Reimplementation is the only clean route.

## The blueprint — SUPERSEDED by a real YR disassembly (2026-08-20)
`0x687F10` has now been disassembled directly from vanilla `gamemd.exe`
(sha1 `189a5a868b3cef8d3d1a58ac3cf0a5241675e4ea`) with
`objdump -D -b binary -m i386 --adjust-vma=0x400000` (file offset == RVA).
**Use YR's own verified structure below, not the TS analogue.** Full write-up
with instruction bytes lives in the encyclopedia page
`encyclopedia/PlayerCount-HouseLimits.md`.

**The function spans `0x687F10`–`0x68837D`** (single `ret`), in four stages:
human houses (`0x687F59`–`0x688146`, bounded by `Session.Players.Count` —
genuinely **dynamic**), AI houses (`0x68814C`–`0x6882CB`), then Neutral
(`0x6882D1`) and Special (`0x688325`) unconditionally.

Verified facts:
- **`HouseClass::HouseClass(HouseTypeClass*)` = `0x4F54A0`**, `__thiscall`
  (`ECX` = this, one stack arg). Called 4x. **`sizeof(HouseClass) == 0x160B8`**
  (the literal pushed to `operator new` @ `0x7C8E17`). *This answers the build
  blocker below.*
- **The >7-AI wall is `0x6882C5`** — and it is **NOT** a `cmp $0x8`. It is a
  *pointer* compare, `cmp $0xA8B2BC,%ebx`, walking off the end of
  `AISlots.Countries[8]` @ `0xA8B29C` (delta 0x20 = 8 ints). Grepping the
  disassembly for the constant 8 will never find it. Worse: `0xA8B2BC` is also
  where `Colors[8]` begins, so an overrun **silently reads the adjacent array**
  — data corruption, not a clean crash.
- **There are TWO `i < 8` starting-point counters**, not one: Phobos's known
  `0x68AF45`, and a second at `0x6883E6` (in the function at
  `0x688380`–`0x6886AC`). The latter is min'd against
  `(players - observers + AIPlayers)` at `0x68841F`, so it **clamps the
  effective player count**. Both must be lifted.
- `HouseClass::ColorSchemeIndex` is at offset `+0x16054`.
- Globals: `0xA8DA78`/`0xA8DA84` = `Session.Players` data/count;
  `0xA8B274` = AI count; `0xA8B29C` = `AISlots.Countries[8]`;
  `0xA8B238` = `SessionClass`.
- Neutral/Special are found **by name string** (`"Neutral"` @ `0x82BA08`,
  `"Special"` @ `0x817318`) via `0x5117D0`, then constructed.

- **RETRACTED LANDMINE — there is no colour-picker hang in YR.** This document
  previously warned (from Vinifera/TS) that the colour picker is
  `Random_Pick(0, MAX_PLAYERS-1)` in a `while(true)` loop that hangs past 8
  houses. **YR does not do this.** YR reads a *stored* colour index
  (`[player+0x53]` for humans, `AISlots.Colors[i]` for AI) and converts it with
  one call to `SessionClass::GetPlayerColorScheme` (`0x69A310`). No retry loop.
  Separately, **Antares already lifts every colour bound that does exist**
  (7 hooks in `src/Misc/Interface.PlayerColors.cpp`, e.g.
  `Session_SetColor_Unlimited` @ `0x69B7FF`).

- **A stack-buffer caveat on the human path.** `AssignHouses` keeps its
  "player already assigned" flags in a **byte array on the stack** at frame
  offset `0x24` inside a tight `sub $0x4c` frame, with the next local 8 bytes
  later — i.e. ~8 flags. So the human loop, though dynamically *bounded*, is
  not safely widenable by byte-patching; this is a further reason the whole
  function must be reimplemented rather than patched. (Strong reading; `esp`
  shifts across pushes were not fully tracked.) Harmless for 1 human.

**Vinifera remains useful only as a structural analogue**
(`Vinifera-Developers/Vinifera`, `src/extensions/scenario/scenarioext.cpp`
~lines 874-1105) — house creation is plain `new HouseClass(HouseTypes[idx])`,
AI loop bound is `Players.Count() + Session.Options.AIPlayers`, Neutral+Special
last. Do not import its colour-picker warning.

## Current blocking issue (where you pick up)
Compile errors were being fixed one round at a time via GitHub Actions (down
from 20 -> 3 -> the core issue). The remaining wall: **`HouseClass` and
`UnitClass` are abstract types to the compiler** (they inherit pure-virtual
COM/IPersist methods from `AbstractClass`), so neither `GameCreate<T>`
(concept-gated on `constructible_from`, false for abstract types) nor
placement-`new` works — the compiler does a full abstract-class check on any
`new T(...)`.

**Note the current code does NOT yet implement the fix.** `src/ExtraHouses.cpp`
defines `GameSpawn<T>` using **placement-new** (`new (pMem) T(args...)`), which
still triggers the compiler's abstract-class check and therefore still fails.
Placement-new is not a workaround for an abstract type.

**The fix, now fully specified by the disassembly:** allocate raw memory via
`YRMemory::Allocate(0x160B8)`, then call the constructor **through its address
as a raw `__thiscall` function pointer** — never showing the compiler a `new`,
so the abstract check never fires:

```cpp
// HouseClass::HouseClass(HouseTypeClass*) @ 0x4F54A0, __thiscall,
// ECX = this, one stack arg. sizeof(HouseClass) == 0x160B8. Both verified
// from the four call sites inside AssignHouses.
using HouseCtor_t = HouseClass* (__thiscall*)(void*, HouseTypeClass*);
auto const HouseCtor = reinterpret_cast<HouseCtor_t>(0x4F54A0);

void* pMem = YRMemory::Allocate(0x160B8);
HouseClass* pHouse = pMem ? HouseCtor(pMem, pCountry) : nullptr;
```

Prefer asserting `sizeof(HouseClass) == 0x160B8` at compile time if YRpp's
layout is complete, so a YRpp update that changes the struct is caught early.

(For units the engine's intended idiom is `pUnitType->CreateObject(pOwner)` — a
vtable call — which sidesteps this entirely for the MCV; verify.)

## Open questions — MOSTLY ANSWERED 2026-08-20
1. ~~**YR's `Init_Data` equivalent.**~~ **ANSWERED.** There is no `Init_Data`
   on YR. After `new HouseClass` the function wires the house inline: colour via
   `0x69A310` -> `+0x16054`, then calls `0x4FCE00` (init/handicap), `0x50B840`,
   `0x50BA00`, and `0x4F6EC0` on the AI path. Those four helpers are the
   "native wiring" a reimplementation must replicate; their individual
   signatures are **not yet** disassembled — that is the remaining sub-task.
2. ~~YR's session player-list shape and the `AIPlayers` option location.~~
   **ANSWERED.** `Session.Players` data ptr `0xA8DA78`, count `0xA8DA84`;
   AI count `0xA8B274`; `AISlots.Countries[8]` `0xA8B29C`.
3. ~~Whether YR's color picker is bounded the same way TS's is.~~
   **ANSWERED — it isn't.** See the retracted landmine above.

### Still genuinely open
4. **Are the per-player frame-sync queues sized per *connection* or per
   *house*?** Per-connection is harmless (humans stay <=8); per-house is a hard
   blocker for the online goal. An RE vet described them as "per player, so
   size 7," which reads as per-connection, but this is **unverified against the
   binary**. **Settle this before building around online.** Highest-risk unknown.
5. The signatures of the four wiring helpers in (1).
6. The downstream GUI break list (below) — needs runtime, not disassembly.

## Determinism rules (mandatory once online is in scope)
Cheap to build in now, expensive to retrofit:
- **Only `ScenarioClass::Instance->Random` may touch game state.** Never
  `rand()`, never a private RNG, and never share one RNG between game logic and
  render-time randomness — that exact mistake caused a desync in KratosPP.
- **Deterministic iteration order.** Never iterate by pointer address or hash
  order when creating/ordering houses.
- **Identical DLL build on every client**; plan a version handshake.
- No dependence on local time, filesystem state, or machine-specific data.

## Downstream break sites past house creation (RE-vet roadmap, UNVERIFIED)
Predicted order in which things break once >8 houses exist, from a community
reverse-engineer. Each is a cheap hook-site discovery on the way to 9 houses:
1. Scenario setup — `AssignHouses` *(known, above)*
2. **Recon / radar** — per-house minimap display
3. **Diplomacy** screen
4. **Score** screen (anchor: `SessionClass::MPStats[8]`)
5. **Loading** screen
6. `session/queue.cpp` frame sync — see open question 4

Items 2-5 have **no addresses located yet**; they are the next acquisition
targets. Do not treat this ordering as confirmed.

## Useful findings from the framework sweep (Ares/Antares/Phobos/CnCNet-spawner)
- **Phobos already reimplements the entire waypoint subsystem** as a dynamic
  map (`Phobos/src/Ext/Scenario/Hooks.Waypoints.cpp`) — this solves the ">8
  start-position STORAGE" problem; that code is borrowable. Start positions ARE
  waypoints (vanilla range 0..701).
- **BUT** Phobos's starting-point *counter* at `0x68AF45` is still hardcoded
  `for (i = 0; i < 8; ++i)` — must be lifted for >8.
- The "8" limit is scattered across fixed-size structures, all needing
  widening/bypass: `GameModeOptionsClass::AISlots[8]`
  (Difficulties/Countries/Colors/Starts/Allies), `ScenarioClass::StartingPoints[8]`
  + `HouseIndices[0x10]`, `SessionClass::SlotData[8]`/`MPStats[8]`.
- Offline 1-human+N-AI **never touches** the network layer (`ListAddress[8]`,
  `IPXManagerClass::Connection[7]`), which is why >8 AI is tractable and >8
  humans is not.
- `AssignHouses` call sites to `Patch_Call`: `0x68745E` (Read_Scenario_INI) and
  `0x68ACFF` (ScenarioClass::Read_INI).

## Repo / build facts
- Project files:
  - `src/Main.cpp` — DLL entry, `-SPAWN` command-line gate, logger to
    `megaskirmish.log`.
  - `src/Config.cpp` — reads `megaskirmish.ini` (`[MegaSkirmish]` +
    `[House0]`..`[House24]`).
  - `src/ExtraHouses.cpp` — house creation; **currently the bolt-on version, to
    be replaced by the AssignHouses reimplementation.**
  - `src/MegaSkirmish.h`, `MegaSkirmish.vcxproj` (Release|Win32, SYR_VER=2,
    C++20, `/Zc:twoPhase-`), `.github/workflows/build.yml`.
- Build: push to GitHub -> Actions runs MSBuild on windows-latest -> download
  `MegaSkirmish-dll` artifact.
- **YRpp reference-type gotcha** (already learned): `DEFINE_REFERENCE(Type*, ...)`
  = pointer (`->`); `DEFINE_REFERENCE(DynamicVectorClass<...>, ...)` = object
  (`.`). E.g. `HouseTypeClass::Array.Count` (dot), but
  `ScenarioClass::Instance->` and `RulesClass::Instance->` (arrow).
- No `declhost` needed — Syringe auto-detects the host via hook addresses.

## Companion reference
`SethGekco/YR-Hook-Encyclopedia` — an address-keyed registry of YR hooks. A page
`encyclopedia/PlayerCount-HouseLimits.md` documents all of the above
player-count findings with confirmed-vs-unverified provenance. Consult/update it
when working on hooks.

## Immediate next steps
1. Replace `GameSpawn<T>`'s placement-new in `src/ExtraHouses.cpp` with the
   raw-`__thiscall`-function-pointer construction shown above (`0x4F54A0`,
   `0x160B8`) so the build compiles.
2. Get a green build + downloadable DLL. **Toolchain proof only — do not
   attempt the full reimplementation in the same step.**
3. **Add an instrumentation-only hook on `0x687F10`** logging
   `Players.Count` (`0xA8DA84`), AI count (`0xA8B274`) and the resulting
   `HouseClass::Array.Count`. Every address in this document is **static
   analysis of one binary** — this validates them against a live game for
   near-zero cost. Do this *before* building on them.
4. In parallel, settle open question 4 (frame-sync queue sizing) — it is the
   one unknown that could invalidate the online goal.
5. First runtime milestone is **9 houses**, not 25 — every crash on the way to 9
   is a hook site discovered cheaply. Test on a scratch CnCNet install, offline
   (Skirmish) first per the staging plan. Read `megaskirmish.log`; capture any
   crash address and look it up against the encyclopedia registry.

**Note on the current `src/ExtraHouses.cpp`:** it is still the abandoned
*bolt-on* implementation (hooks `0x6878E0` late and appends houses afterward).
The architecture decision above says to replace it with an `AssignHouses`
reimplementation. It is fine to keep it temporarily as a compile target for
step 1, but it should not be the basis of the real feature.
