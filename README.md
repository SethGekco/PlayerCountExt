# PlayerCountExt

A standalone [Syringe](https://github.com/Ares-Developers/Syringe)-injected DLL
for **Command & Conquer: Red Alert 2 — Yuri's Revenge** that lifts the engine's
8-player limit.

Drop it in the game folder. Syringe loads it automatically, alongside
[Antares](https://github.com/Phobos-developers/Antares), Phobos and the stock
CnCNet spawner — exactly like Phobos. No forked spawner, no forked client
required to run it.

> **Status: early.** The engine analysis is done and largely verified; the
> feature is not built yet. See [DESIGN.md](DESIGN.md) for the verified address
> map and the plan. Nothing here is a working >8 build today.

---

## The one thing worth knowing

**The limit is on HOUSES, not on "players."** Every human player *and* every
computer is a `HouseClass`, plus Neutral and Special. `HouseClass::Array` is a
`DynamicVectorClass` and never caps — what caps is the bit-shift. Each of
`HouseClass::Allies`, `AltAllies`, `TechnoClass::DisplayProductionTo` and
`CellClass::BaseSpacerOfHouses` does `1u << ArrayIndex` into a `DWORD`, so:

```
players + AI + Neutral(1) + Special(1)  ≤  32   →   30 real houses
```

Past index 31 the shift *aliases* (x86 masks the shift count to 5 bits) rather
than faulting, so houses silently begin sharing alliance and spy bits. That is
corruption, not a crash, which is why it is worth knowing before you go looking
for one.

Everything else — the AI slot arrays, the start-position counters, the score
buffers — is a fixed-size array *below* that ceiling, and each is liftable
independently.

---

## Scope

**In scope**
- More than 8 houses in skirmish and multiplayer.
- Many AI against few humans ("comp stomp") as the primary target.
- Online-compatible **by construction** — config is host-authoritative and all
  behaviour is deterministic. Whether it *works* online is untested; see below.

**Out of scope**
- More than 8 *human* players. The network layer (`IPXManagerClass::Connection[7]`,
  `ListAddress::Array[8]`) bounds human connections and stays untouched. YR is
  lockstep — clients simulate all AI locally, so many-AI does not stress it.
- More than ~30 houses. Past that you must widen every per-house bitfield in the
  engine, which is a categorically larger project.
- Start-location work. That belongs to **SpawnExt**, a sibling project: more
  than 8 *start positions* is a different axis from more than 8 *houses*.

---

## Project rules

These are deliberate and they are the point of this repo being separate.

1. **Every address is verified before it ships.** Disassembled, cross-checked
   against the Antares PDB symbol map, or both. An address that is a guess is
   labelled a guess, and a guessed hook does not get compiled in — a wrong
   Syringe patch writes a JMP into the middle of an unrelated instruction and
   crashes arbitrarily far from the cause.
2. **Consult the [YR Hook Encyclopedia](https://github.com/SethGekco/YR-Hook-Encyclopedia)
   before choosing a hook, and contribute findings back after using one** —
   including the ones that turned out wrong. A refuted claim is worth as much as
   a confirmed one.
3. **Antares, not Ares.** Antares is a full reimplementation and a superset.
4. **Instrument before implementing.** Static analysis is a hypothesis. Log the
   engine's own values from a live game and confirm them before building on top.
5. **Determinism is not optional.** Only `ScenarioClass::Instance->Random` may
   touch game state; never a private RNG, never one shared with render-time
   randomness. Iteration order must not depend on pointer addresses. Anything
   else desyncs a networked game on frame one.
6. **Config is host-authoritative.** House configuration comes from `spawn.ini`,
   which the client writes and the host broadcasts. A client-local config file
   works offline and desyncs online, so we do not use one.
7. **Record what is *not* known.** Unverified is a first-class state in the docs
   here; "confirmed" is reserved for things actually confirmed.

---

## Building

Windows-only build (MSVC, `Release|Win32`, C++20). CI does it on every push:

```
msbuild PlayerCountExt.vcxproj /p:Configuration=Release /p:Platform=Win32
```

YRpp is fetched by CI rather than vendored as a submodule. Note that YRpp is a
best-effort mapping of a closed binary and is **not** authoritative on struct
layout — see the `AISlotsStruct` warning in [DESIGN.md](DESIGN.md).

Artifact: `PlayerCountExt-dll`.

---

## Prior art and credit

- **[mmtrt/yrpp-spawner](https://github.com/mmtrt/yrpp-spawner)** + its
  [client fork](https://github.com/mmtrt/xna-cncnet-client/tree/testing) —
  an independent, working **16-player** implementation (GPL-3.0), integrated
  into the CnCNet spawner. It reached two of our key addresses (`0x688158`,
  `0x6882C5`) independently, which is the strongest corroboration our own
  disassembly has. Different architecture from ours by choice, not by
  disagreement; treated here as a reference to compare against.
- **[Vinifera](https://github.com/Vinifera-Developers/Vinifera)** — a readable
  reimplemented `Assign_Houses()` for Tiberian Sun, YR's ancestor engine. Useful
  for structure; note that several of its specifics do **not** carry over to YR.
- **[YRpp](https://github.com/Phobos-developers/YRpp)** and
  **[Syringe](https://github.com/Ares-Developers/Syringe)** — the foundation
  this is built on.

## Licence

GPLv3, inherited from YRpp and Syringe.
