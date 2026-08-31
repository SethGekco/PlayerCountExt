/**
*  PlayerCountExt — start-position storage beyond the vanilla 8
*
*  THE PROBLEM
*  -----------
*  ScenarioClass::StartingPoints is 8 Point2D entries at +0x1140, and
*  HouseIndices[0x10] follows IMMEDIATELY at +0x1180. The map-header parser
*  (0x689F64) reads [Header] Waypoint1..N in a loop bounded by
*  NumberStartingPoints and never clamps it:
*
*      689f70:  lea  0x1140(%esi),%ebx        ; &StartingPoints[0]
*      689faa:  mov  %ecx,(%ebx)              ; [i].X
*      689fac:  mov  %eax,0x4(%ebx)           ; [i].Y
*      689fb9:  add  $0x8,%ebx                ; stride 8
*      689fbc:  cmp  %ecx,%eax                ; while i < NumberStartingPoints
*
*  So a map declaring more than 8 starting points does not fail — it walks
*  straight off the end of StartingPoints and overwrites HouseIndices, i.e. the
*  start->house table. That is a VANILLA defect: it is reachable today with a
*  hand-authored map and nothing to do with raising a player cap.
*
*  WHAT THIS DOES
*  --------------
*  Rather than relocating StartingPoints (which would strand every consumer we
*  have not yet identified), we leave entries 0..7 exactly where the engine puts
*  them and divert only the out-of-range writes into our own buffer:
*
*      index 0..7   -> the engine's own StartingPoints[i]  (untouched, vanilla)
*      index 8..N   -> Extra[i - 8]                        (was: corrupting
*                                                            HouseIndices)
*
*  That has two properties worth stating plainly:
*
*    * It is a strict bug fix on its own. Even with no other feature enabled, a
*      >8-start map stops corrupting the start->house table.
*    * It cannot regress a <=8 map, because for i < 8 we perform exactly the
*      store the stolen instructions would have performed.
*
*  Reading these extra points back is a separate concern — the preview renderer
*  bails entirely above 8 (see 0x6408E2) and the placement path has not been
*  located yet. This file only makes the data exist and stop doing harm.
*
*  GPLv3.
*/

#include "PlayerCountExt.h"

#include <Syringe.h>
#include <Helpers/Macro.h>

namespace
{
	// ScenarioClass field offsets. Runtime-probed, not assumed: the probe in
	// Instrumentation.cpp confirmed Waypoints at +0x632 (matching the disasm's
	// `lea 0x632(%edx),%ecx`) and NumberStartingPoints at +0x113C. Note 0x113C
	// rather than the 0x113A a by-hand walk of the struct would give — there is
	// padding after Waypoints[702]. Do not hand-compute these.
	constexpr int OffNumberStartingPoints = 0x113C;
	constexpr int OffStartingPoints       = 0x1140; // Point2D[8]
	constexpr int OffHouseIndices         = 0x1180; // int[16] — what we stop clobbering

	static_assert(OffStartingPoints + 8 * 8 == OffHouseIndices,
		"StartingPoints[8] must sit immediately before HouseIndices — the whole "
		"point of this file is that overrunning one lands in the other.");

	constexpr int VanillaStarts = 8;

	// How many start positions we can hold in total. The binding limit further
	// downstream is the 32-bit per-house bitfield, so there is no value in going
	// past it.
	constexpr int MaxStarts = PlayerCountExt::EngineHouseCeiling; // 32 houses

	struct Point2D { int X; int Y; };

	// Entries 8 .. MaxStarts-1. Index 0 here is start position 8.
	Point2D Extra[MaxStarts - VanillaStarts] {};

	int HighestSeen = 0; // highest start index observed this map, for logging
}

namespace PlayerCountExt
{
	namespace StartPoints
	{
		// Total start positions available: the engine's 8 plus whatever the map
		// declared beyond that.
		int Count()
		{
			return (HighestSeen + 1 > VanillaStarts) ? (HighestSeen + 1) : VanillaStarts;
		}

		// Reads start position `index`, transparently spanning both stores.
		// Returns false for an out-of-range index rather than guessing.
		bool Get(DWORD scenarioBase, int index, int& x, int& y)
		{
			if (index < 0 || index >= MaxStarts)
				return false;

			if (index < VanillaStarts)
			{
				const auto p = reinterpret_cast<const int*>(
					scenarioBase + OffStartingPoints + index * 8);
				x = p[0];
				y = p[1];
				return true;
			}

			x = Extra[index - VanillaStarts].X;
			y = Extra[index - VanillaStarts].Y;
			return true;
		}
	}
}

// ---------------------------------------------------------------------------
// ScenarioClass::ReadMapHeader — 0x689D40.
//
// This is `lea 0x1140(%esi),%edi`, the instruction that points EDI at
// StartingPoints[0] before the header fields are reset. We take it over purely
// to get a reliable once-per-map reset for our own buffer, then perform the
// stolen instruction ourselves.
//
// ESI = ScenarioClass. Stolen bytes 6: 8d be 40 11 00 00
// ---------------------------------------------------------------------------
DEFINE_HOOK(0x689D40, PlayerCountExt_StartPoints_HeaderReset, 0x6)
{
	GET(DWORD, pScenario, ESI);

	for (auto& p : Extra)
	{
		p.X = 0;
		p.Y = 0;
	}
	HighestSeen = 0;

	// Stolen: EDI = &StartingPoints[0]
	R->EDI(pScenario + OffStartingPoints);
	return 0x689D46;
}

// ---------------------------------------------------------------------------
// The header start-point store — 0x689FAA.
//
//     689faa:  89 0b        mov %ecx,(%ebx)        ; StartingPoints[i].X
//     689fac:  89 43 04     mov %eax,0x4(%ebx)     ; StartingPoints[i].Y
//
// Five bytes covering both stores. On entry ECX = X, EAX = Y, EBX walks the
// array, and 0xC(%esp) holds the loop counter — which is ONE-BASED, because
// 0x689F76 does `inc %eax` before storing it (the key is "Waypoint1" for
// index 0).
//
// We recompute the index from EBX rather than trusting the stack slot as an
// absolute: EBX is the authoritative cursor the engine itself increments, so
// deriving from it stays correct even if the counter's meaning shifts.
// ---------------------------------------------------------------------------
DEFINE_HOOK(0x689FAA, PlayerCountExt_StartPoints_Store, 0x5)
{
	GET(DWORD, pCursor, EBX);
	GET(int, x, ECX);
	GET(int, y, EAX);
	GET_STACK(int, oneBasedIndex, 0xC);

	const int index = oneBasedIndex - 1;

	if (index > HighestSeen)
		HighestSeen = index;

	if (index < VanillaStarts)
	{
		// Exactly the stores we replaced — vanilla behaviour, byte for byte.
		auto const p = reinterpret_cast<int*>(pCursor);
		p[0] = x;
		p[1] = y;
	}
	else if (index < MaxStarts)
	{
		Extra[index - VanillaStarts].X = x;
		Extra[index - VanillaStarts].Y = y;

		PlayerCountExt::Log("[start] captured start %d = (%d,%d) into extension "
			"(vanilla would have overwritten HouseIndices[%d])\n",
			index, x, y, (index - VanillaStarts) * 2);
	}
	else
	{
		// Past even our extension. Dropping it is the safe action: the engine
		// would otherwise write somewhere it does not own.
		PlayerCountExt::Log("[start] DROPPED start %d = (%d,%d) — exceeds %d\n",
			index, x, y, MaxStarts);
	}

	return 0x689FAF;
}
