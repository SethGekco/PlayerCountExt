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
			PlayerCountExt::Log("[shift] start %d exceeds ring table (%d rings x %d starts) — using base %d unshifted\n",
				startIndex, RingCount, RealStartCount, base);

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
