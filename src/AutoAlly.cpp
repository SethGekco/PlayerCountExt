/**
*  PlayerCountExt — stop houses being silently allied for sharing a start index
*
*  THE PROBLEM
*  -----------
*  Vanilla pairs every two houses whose start-location index (HouseClass +
*  0x1605C) matches and mutually allies them, with bAnnounce = false:
*
*      5d74fd:  mov  0x1605c(%ecx),%ecx     ; A's start index
*      5d7503:  cmp  0x1605c(%edx),%ecx     ; == B's?
*      5d7509:  jne  0x5d7524               ; differ -> next pair
*      5d750b:  push $0x0; push %esi; mov %ebx,%ecx; call 0x4f9b70   ; A.MakeAlly(B)
*      5d7515:  push $0x0; push %ebx; mov %esi,%ecx; call 0x4f9b70   ; B.MakeAlly(A)
*
*  That is reasonable vanilla behaviour: two houses on the same start really are
*  sitting on top of each other, so allying them beats having them fight at
*  point-blank range.
*
*  It stops being reasonable once we shift spawns apart. With more players than
*  the map has start positions, houses routinely share an INDEX while occupying
*  completely different CELLS — and the engine allies them anyway. The result is
*  players silently allied with no cause visible in the lobby, in spawn.ini, or
*  in any team setting, which is close to undebuggable from the outside.
*
*  WHAT THIS DOES
*  --------------
*  Hooks the pairing at 0x5D750B and re-checks the premise: if the two houses
*  are on DIFFERENT base cells, they are not sharing a position and the alliance
*  has no basis, so it is skipped. If they genuinely are on the same cell the
*  vanilla calls are performed exactly as before.
*
*  We reproduce both MakeAlly calls rather than falling through, because the
*  stolen bytes are the argument setup for the first one. Returning 0x5D751F
*  rejoins the loop at the instruction after the second call.
*
*  GPLv3.
*/

#include "PlayerCountExt.h"

#include <Syringe.h>
#include <Helpers/Macro.h>

namespace
{
	// HouseClass::MakeAlly(HouseClass*, bool bAnnounce) — YRpp HouseClass.h:219.
	using MakeAlly_t = void (__thiscall*)(void*, void*, bool);
	constexpr DWORD AddrMakeAlly = 0x4F9B70;

	// The base cell the house will actually spawn on. This is the field the
	// spawn shift overrides, so comparing it is what tells us whether two
	// houses really share a position or merely share an index.
	constexpr int OffHomeCell = 0x5490;

	DWORD HomeCellOf(DWORD pHouse)
	{
		return *reinterpret_cast<DWORD const volatile*>(pHouse + OffHomeCell);
	}

	int SuppressedThisGame = 0;
}

// ---------------------------------------------------------------------------
// The auto-ally pair — 0x5D750B.
//
// Stolen bytes 5:
//     5d750b:  6a 00      push $0x0        (2)
//     5d750d:  56         push %esi        (1)
//     5d750e:  8b cb      mov  %ebx,%ecx   (2)
//
// EBX = house A, ESI = house B. Returns to 0x5D751F, i.e. after both calls.
// ---------------------------------------------------------------------------
DEFINE_HOOK(0x5D750B, PlayerCountExt_AutoAlly_OnlyWhenActuallySharing, 0x5)
{
	GET(DWORD, pHouseA, EBX);
	GET(DWORD, pHouseB, ESI);

	if (!pHouseA || !pHouseB)
		return 0x5D751F;

	const DWORD cellA = HomeCellOf(pHouseA);
	const DWORD cellB = HomeCellOf(pHouseB);

	if (cellA != cellB)
	{
		// Same start index, different cells — the shift moved them apart, so
		// the reason for allying them no longer holds.
		if (SuppressedThisGame < 16)
		{
			union { DWORD raw; struct { short X, Y; } c; } a{ cellA }, b{ cellB };
			PlayerCountExt::Log("[ally] not allying house@0x%08X (%d,%d) with house@0x%08X (%d,%d) "
				"— same start index but different cells\n",
				pHouseA, a.c.X, a.c.Y, pHouseB, b.c.X, b.c.Y);
			++SuppressedThisGame;
		}

		return 0x5D751F;
	}

	// Genuinely on the same cell — vanilla behaviour, both directions.
	const auto MakeAlly = reinterpret_cast<MakeAlly_t>(AddrMakeAlly);
	MakeAlly(reinterpret_cast<void*>(pHouseA), reinterpret_cast<void*>(pHouseB), false);
	MakeAlly(reinterpret_cast<void*>(pHouseB), reinterpret_cast<void*>(pHouseA), false);

	return 0x5D751F;
}
