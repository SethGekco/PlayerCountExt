/**
*  PlayerCountExt — shared start positions with per-house offsets
*
*  THE IDEA
*  --------
*  A map declares N start positions. We want more houses than that, without
*  requiring new maps. So houses beyond the Nth reuse an existing start position
*  but spawn a fixed distance away from it in one of eight compass directions.
*
*  Start index encoding — this is a contract, and the client must agree with it
*  exactly or a player will spawn somewhere other than the slot they picked:
*
*      startIndex = ring * realCount + baseIndex
*
*      ring 0 -> ""    (the map's own start position, untouched)
*      ring 1 -> "N"   ring 2 -> "NE"  ring 3 -> "E"   ring 4 -> "SE"
*      ring 5 -> "S"   ring 6 -> "SW"  ring 7 -> "W"   ring 8 -> "NW"
*
*      label(i) = (i % realCount + 1) + suffix[i / realCount]
*
*  So on a 4-start map, index 6 is "3N" and index 11 is "4NE" — 36 selectable
*  starts from a 4-start map, comfortably past the ~30-house engine ceiling, so
*  the ceiling stays the binding limit rather than the map.
*
*  WHERE
*  -----
*  0x5D6C21 is the single instruction that turns a house's start index into a
*  cell, once per house per game:
*
*      5d6c12:  mov  0x16058(%ecx),%esi      ; ESI = house->StartIndex
*      5d6c1d:  mov  0xc(%esp),%eax          ; EAX = cell table
*      5d6c21:  mov  (%eax,%esi,4),%edx      ; EDX = table[startIndex]
*      5d6c24:  push %edx
*      5d6c25:  call 0x50e000                ; house->SetBaseCell(EDX)
*
*  We hook 0x5D6C1D (7 bytes, both movs) and return to the intact `push %edx`,
*  having set EDX ourselves. Nothing downstream — placement, the parser,
*  StartingPoints — needs to change.
*
*  NOTE the field is +0x16058, NOT +0x1605C. Both exist and are written by
*  AssignHouses; +0x1605C drives the auto-ally pass at 0x5D74AF instead. Using
*  the wrong one moves the wrong thing.
*
*  DETERMINISM
*  -----------
*  The offset is a pure function of the start index. No RNG, no time, no pointer
*  ordering — so every client computes an identical cell for identical input and
*  there is no desync surface at all. Deliberately not "random direction from the
*  synced seed", which would also work but has a failure mode this does not.
*
*  ⚠ NOT YET ADDRESSED (both known, neither fatal to testing):
*    - Terrain: the offset cell may be water, cliff or off-map. There is no
*      validity search yet, so a shifted house can land somewhere unusable.
*    - The auto-ally pass at 0x5D74AF allies houses sharing +0x1605C. Houses
*      shifted onto the same base position may therefore start allied.
*
*  GPLv3.
*/

#include "PlayerCountExt.h"
#include "SpawnConfig.h"

#include <Syringe.h>
#include <Helpers/Macro.h>

namespace
{
	// A CellStruct is two shorts packed into the DWORD the table holds.
	union PackedCell
	{
		DWORD Raw;
		struct { short X; short Y; } Cell;
	};

	// Ring 0 is the unshifted original; rings 1..8 are the compass directions
	// in the order the label suffixes imply.
	struct Offset { int dX; int dY; };

	constexpr Offset RingOffsets[] = {
		{  0,  0 }, // ring 0 — the map's own position
		{  0, -1 }, // N
		{  1, -1 }, // NE
		{  1,  0 }, // E
		{  1,  1 }, // SE
		{  0,  1 }, // S
		{ -1,  1 }, // SW
		{ -1,  0 }, // W
		{ -1, -1 }, // NW
	};

	constexpr int RingCount = static_cast<int>(sizeof(RingOffsets) / sizeof(RingOffsets[0]));

	// Cells between a shifted spawn and the position it derives from.
	//
	// 12 was the original suggestion; that is tight enough that two MCVs would
	// have heavily overlapping build radii and fight for room. 20 gives each
	// base somewhere to grow while keeping them recognisably "at the same
	// start". Worth revisiting once it can be seen in game.
	constexpr int ShiftDistance = 20;

	// Start positions the current map actually declares. Set from the engine's
	// NumberStartingPoints; until we know it, assume vanilla 8 so ring maths
	// never divides by zero.
	int RealStartCount = 8;

	constexpr int OffNumberStartingPoints = 0x113C;
	constexpr DWORD AddrScenarioPtr = 0xA8B230;

	void RefreshRealStartCount()
	{
		const auto pScen = *reinterpret_cast<DWORD const volatile*>(AddrScenarioPtr);
		if (!pScen)
			return;

		const int n = *reinterpret_cast<int const volatile*>(pScen + OffNumberStartingPoints);
		if (n > 0)
			RealStartCount = n;
	}

	constexpr DWORD AddrHouseArrayItems = 0xA8022C;
	constexpr DWORD AddrHouseArrayCount = 0xA80238;

	// The start index the PLAYER chose, taken from spawn.ini rather than from
	// the house — see the note at the call site for why the house's own field
	// cannot be trusted at this point.
	//
	// Returns -1 when spawn.ini says nothing about this house, in which case
	// the caller leaves the engine's own answer alone.
	int StartIndexFromSpawnIni(DWORD pHouse)
	{
		const auto& spawn = PlayerCountExt::SpawnConfig::Get();
		if (!spawn.Loaded())
			return -1;

		const auto items = *reinterpret_cast<DWORD* const volatile*>(AddrHouseArrayItems);
		const int count = *reinterpret_cast<int const volatile*>(AddrHouseArrayCount);
		if (!items || count <= 0)
			return -1;

		for (int i = 0; i < count; ++i)
		{
			if (items[i] != pHouse)
				continue;

			// MultiN is 1-based over the house array.
			const auto& h = spawn.House(i + 1);
			return (h.Defined && h.SpawnLocation >= 0) ? h.SpawnLocation : -1;
		}

		return -1; // not a house from the multiplayer slots (Neutral/Special)
	}
}

// ---------------------------------------------------------------------------
// Start index -> cell — 0x5D6C1D.
//
// Stolen 7 bytes:
//     5d6c1d:  8b 44 24 0c    mov 0xc(%esp),%eax      (4)
//     5d6c21:  8b 14 b0       mov (%eax,%esi,4),%edx  (3)
// Returns to 0x5D6C24, the untouched `push %edx`.
//
// ESI = house->StartIndex (+0x16058), ECX = the house, 0xC(%esp) = cell table.
// ---------------------------------------------------------------------------
// ⚠ DEAD PATH — kept compiled out, not deleted, because the reason matters.
//
// 0x5D6C1D looked like the place: it is `mov (%eax,%esi,4),%edx`, a clean
// start-index -> cell lookup feeding SetBaseCell. But an in-game run produced
// ZERO log lines from it, so the loop containing it is simply not reached in a
// normal skirmish — it is one of two callers of 0x50E000 in this region and
// evidently the wrong one.
//
// The live path is the function at 0x5D6C70, where TWO cell sources converge
// (a virtual call at 0x5D6D25, and a table lookup at 0x5D6D30) before a single
// SetBaseCell at 0x5D6D3A. See the live hook below.
//
// Recorded rather than removed: "this address looks right and is never
// executed" is exactly the kind of thing worth not rediscovering.
#define PLAYERCOUNTEXT_SPAWNSHIFT_DEAD_PATH 0
#if PLAYERCOUNTEXT_SPAWNSHIFT_DEAD_PATH

DEFINE_HOOK(0x5D6C1D, PlayerCountExt_SpawnShift_IndexToCell, 0x7)
{
	GET(int, startIndex, ESI);
	GET_STACK(DWORD, pTable, 0xC);

	RefreshRealStartCount();

	// Reproduce the stolen load unconditionally so EAX is what the engine
	// expects at 0x5D6C2F, which does `mov %edi,0x1180(%eax,%esi,4)` — that
	// instruction reuses EAX as the ScenarioClass pointer it reloads at
	// 0x5D6C2A, so we must not leave it holding something surprising here.
	R->EAX(pTable);

	if (startIndex < 0 || RealStartCount <= 0)
		return 0x5D6C24; // nothing sensible to do; let the engine proceed

	const int ring = startIndex / RealStartCount;
	const int base = startIndex % RealStartCount;

	const auto table = reinterpret_cast<const DWORD*>(pTable);

	// Ring 0 is the vanilla path: identical bytes, identical result.
	if (ring == 0 || ring >= RingCount)
	{
		if (ring >= RingCount)
		{
			PlayerCountExt::Log("[shift] start %d exceeds ring table (%d rings x %d starts) — using base %d unshifted\n",
				startIndex, RingCount, RealStartCount, base);
		}
		else
		{
			// Log the unshifted case too. Without this, "the hook ran and took
			// the vanilla path" and "the hook never fired" produce identical
			// logs — which would make a future shift failure impossible to
			// diagnose. Cheap: it is once per house per game.
			PlayerCountExt::Log("[shift] start %d ring0 (realCount=%d) — unshifted, cell (%d,%d)\n",
				startIndex, RealStartCount,
				PackedCell{ table[base] }.Cell.X, PackedCell{ table[base] }.Cell.Y);
		}

		R->EDX(table[base]);
		return 0x5D6C24;
	}

	PackedCell cell;
	cell.Raw = table[base];

	const auto& off = RingOffsets[ring];
	cell.Cell.X = static_cast<short>(cell.Cell.X + off.dX * ShiftDistance);
	cell.Cell.Y = static_cast<short>(cell.Cell.Y + off.dY * ShiftDistance);

	static const char* const Suffix[] = { "", "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
	PlayerCountExt::Log("[shift] start %d = \"%d%s\" -> base %d cell (%d,%d) shifted %d cells to (%d,%d)\n",
		startIndex, base + 1, Suffix[ring], base,
		PackedCell{ table[base] }.Cell.X, PackedCell{ table[base] }.Cell.Y,
		ShiftDistance, cell.Cell.X, cell.Cell.Y);

	R->EDX(cell.Raw);
	return 0x5D6C24;
}

#endif // PLAYERCOUNTEXT_SPAWNSHIFT_DEAD_PATH

// ---------------------------------------------------------------------------
// The live hook — 0x5D6D3F, immediately AFTER SetBaseCell.
//
//     5d6d25:  call *0xc4(%edx)          ; path A: virtual cell computation
//     5d6d30:  mov  (%ecx,%edx,4),%edx   ; path B: direct table lookup
//     5d6d38:  mov  %edi,%ecx            ; EDI = the house
//     5d6d3a:  call 0x50e000             ; SetBaseCell(cell)
//     5d6d3f:  mov  0xa80238,%eax        ; <-- we hook here (5 bytes)
//
// Post-correcting rather than intercepting is deliberate: two different cell
// sources converge on that one setter, so correcting AFTER it handles both with
// a single hook and no need to know which path ran. It also means we never have
// to reproduce the engine's own cell selection — we only adjust its answer.
//
// EDI = the house. The start index is read from the house itself (+0x16058),
// so we do not depend on any register the two paths might set differently.
// ---------------------------------------------------------------------------
DEFINE_HOOK(0x5D6D3F, PlayerCountExt_SpawnShift_AfterSetBaseCell, 0x5)
{
	GET(DWORD, pHouse, EDI);

	if (!pHouse)
		return 0;

	RefreshRealStartCount();

	// Do NOT trust HouseClass+0x16058 here.
	//
	// The CnCNet spawner clamps spawn locations to 0..7, and it does so AFTER
	// AssignHouses — proven in game: our restore at the AssignHouses epilogue
	// saw the field as 0 and set it to 8, yet by the time this hook runs the
	// same house reads 7. Restoring it earlier or later is a race against
	// another DLL's write order, which is not a fight worth having.
	//
	// spawn.ini is host-authoritative and immutable once written, so we read
	// the player's actual choice from there instead. The house's position in
	// HouseClass::Array gives us its MultiN slot (1-based).
	const int startIndex = StartIndexFromSpawnIni(pHouse);

	if (startIndex < 0 || RealStartCount <= 0)
		return 0;

	const int ring = startIndex / RealStartCount;
	const int base = startIndex % RealStartCount;

	// +0x5490 is the cell 0x50E000 just wrote (the getters fall back to it when
	// +0x5494 is the invalid-cell sentinel).
	const auto pHomeCell = reinterpret_cast<DWORD*>(pHouse + 0x5490);

	PackedCell cell;
	cell.Raw = *pHomeCell;

	if (ring == 0)
	{
		PlayerCountExt::Log("[shift] house@0x%08X start %d ring0 (realCount=%d) — unshifted, cell (%d,%d)\n",
			pHouse, startIndex, RealStartCount, cell.Cell.X, cell.Cell.Y);
		return 0;
	}

	if (ring >= RingCount)
	{
		PlayerCountExt::Log("[shift] house@0x%08X start %d exceeds ring table (%d rings x %d starts) — left unshifted\n",
			pHouse, startIndex, RingCount, RealStartCount);
		return 0;
	}

	const auto& off = RingOffsets[ring];
	const short oldX = cell.Cell.X;
	const short oldY = cell.Cell.Y;

	cell.Cell.X = static_cast<short>(oldX + off.dX * ShiftDistance);
	cell.Cell.Y = static_cast<short>(oldY + off.dY * ShiftDistance);

	*pHomeCell = cell.Raw;

	// Keep +0x5494 consistent when it holds a real cell rather than the
	// invalid-cell sentinel, so both getters agree.
	const auto pCurCell = reinterpret_cast<DWORD*>(pHouse + 0x5494);
	if (*pCurCell == *pHomeCell || *pCurCell != 0)
	{
		PackedCell cur;
		cur.Raw = *pCurCell;
		const short sentX = *reinterpret_cast<short const volatile*>(0xA8EF98);
		const short sentY = *reinterpret_cast<short const volatile*>(0xA8EF9A);
		if (!(cur.Cell.X == sentX && cur.Cell.Y == sentY))
		{
			cur.Cell.X = static_cast<short>(cur.Cell.X + off.dX * ShiftDistance);
			cur.Cell.Y = static_cast<short>(cur.Cell.Y + off.dY * ShiftDistance);
			*pCurCell = cur.Raw;
		}
	}

	static const char* const Suffix[] = { "", "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
	PlayerCountExt::Log("[shift] house@0x%08X start %d = \"%d%s\" — (%d,%d) shifted %d cells to (%d,%d)\n",
		pHouse, startIndex, base + 1, Suffix[ring],
		oldX, oldY, ShiftDistance, cell.Cell.X, cell.Cell.Y);

	return 0;
}

// ---------------------------------------------------------------------------
// Restore the unclamped start index — AssignHouses epilogue, 0x688378.
//
// The CnCNet spawner clamps spawn locations to 0..7 before the engine sees
// them (`std::clamp(nSpawnLocations, 0, 7)` in its Spawner.cpp), so a shifted
// selection of 8 arrives as 7 — i.e. "1N" silently becomes "spawn 8". Proven
// in game: spawn.ini carried [SpawnLocations] Multi1=8 while the house's
// +0x16058 read 7.
//
// We cannot fix the spawner from here — it is a separate binary — but we do not
// need to. spawn.ini is host-authoritative and we already parse it, so we
// simply write the value the player actually chose back over the clamped one,
// after AssignHouses has finished populating the house array.
//
// Mapping: [SpawnLocations] MultiN is 1-based over the house array, so
// house[i] takes Multi(i+1). Confirmed by the same run: Multi1 corresponded to
// house[0].
//
// This runs at the AssignHouses epilogue, which is also hooked for
// instrumentation. Same-address hooks CHAIN in Syringe (unlike overlapping
// ones, which corrupt each other), and both handlers return 0, so this is safe.
// ---------------------------------------------------------------------------
DEFINE_HOOK(0x688378, PlayerCountExt_SpawnShift_RestoreStartIndex, 0x5)
{
	auto& spawn = PlayerCountExt::SpawnConfig::Get();
	if (!spawn.Loaded())
		return 0;

	const auto pArrayItems = *reinterpret_cast<DWORD* const volatile*>(0xA8022C);
	const int count = *reinterpret_cast<int const volatile*>(0xA80238);

	if (!pArrayItems || count <= 0)
		return 0;

	for (int i = 0; i < count; ++i)
	{
		const DWORD pHouse = pArrayItems[i];
		if (!pHouse)
			continue;

		const auto& h = spawn.House(i + 1); // MultiN is 1-based
		if (!h.Defined || h.SpawnLocation < 0)
			continue;

		const auto pStart = reinterpret_cast<int*>(pHouse + 0x16058);
		const int current = *pStart;

		if (current == h.SpawnLocation)
			continue; // nothing was clamped

		PlayerCountExt::Log("[shift] house[%d] start index %d -> %d (restored from spawn.ini Multi%d; "
			"the spawner had clamped it)\n",
			i, current, h.SpawnLocation, i + 1);

		*pStart = h.SpawnLocation;
	}

	return 0;
}
