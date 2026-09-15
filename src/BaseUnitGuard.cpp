/**
*  PlayerCountExt — don't crash when a house has no buildable base unit
*
*  THE CRASH
*  ---------
*  C0000005 at 0x4F671D, seen repeatedly, and again on the second house of a
*  30-player game:
*
*      4f670b:  mov  eax,ds:0x8871e0      ; RulesClass::Instance
*      4f6710:  mov  ecx,esi              ; ECX = the house
*      4f6712:  add  eax,0x938            ; &Rules->BaseUnit
*      4f6718:  call 0x5051e0             ; FirstBuildableFromArray
*      4f671d:  mov  edx,[eax]            ; <- EAX == NULL, reads the vtable
*      4f6722:  call [edx+0x84]           ; ...to make a virtual call
*
*  Antares replaces 0x5051E0 outright and guards its NULL return at two other
*  call sites (0x4F65BF, 0x5D705E) but not this one — its own source even
*  anticipates the gap. So a house that can build nothing from [General]BaseUnit
*  dereferences NULL here.
*
*  WHY THIS MATTERS MORE NOW
*  -------------------------
*  It is not caused by the higher player count, but the player count makes it
*  near-certain: more houses means more countries drawn per game, so a country
*  whose base unit is momentarily unbuildable is far more likely to appear. At 8
*  players it may never come up.
*
*  WHAT THIS DOES
*  --------------
*  When the lookup returns NULL, skips the virtual call and continues with
*  EBX = 0, then logs which country it was.
*
*  EBX = 0 is the correct value, not merely a safe one. The two results are
*  costs: 0x4F676E does `add ebx,edi` and compares the sum against the house's
*  money. EDI already carries the engine's own "unavailable" sentinel
*  (0x7FFFFFFF, set at 0x4F6706 on the parallel lookup just above — and present
*  in the crash registers). Zero makes the sum exactly that sentinel, preserving
*  "cannot afford this". Using 0x7FFFFFFF for EBX as well would overflow the
*  addition to -2 and invert the comparison into "can afford".
*
*  The log names the country, so a house that genuinely cannot build a base unit
*  becomes a reported data problem instead of a silent fatal error.
*
*  GPLv3.
*/

#include "PlayerCountExt.h"

#include <Syringe.h>
#include <Helpers/Macro.h>

namespace
{
	int Reported = 0;
}

// ---------------------------------------------------------------------------
// 0x4F671D — the unguarded dereference.
//
// Stolen bytes 5:
//     4f671d:  8b 10      mov  edx,[eax]   (2)
//     4f671f:  56         push esi         (1)
//     4f6720:  8b c8      mov  ecx,eax     (2)
//
// EAX = the base unit type or NULL, ESI = the house.
//
// Non-null returns 0 so the stolen bytes re-execute and vanilla proceeds
// untouched. Null skips to 0x4F672A, past both the push and the call — the
// callee would have cleaned that argument, so skipping both keeps the stack
// balanced.
// ---------------------------------------------------------------------------
DEFINE_HOOK(0x4F671D, PlayerCountExt_BaseUnitGuard_NullFromArray, 0x5)
{
	GET(DWORD, pBaseUnitType, EAX);
	if (pBaseUnitType)
		return 0;

	GET(DWORD, pHouse, ESI);

	if (Reported < 8)
	{
		++Reported;

		const char* country = "<unknown>";
		if (pHouse)
		{
			const auto pType = *reinterpret_cast<DWORD const volatile*>(pHouse + 0x34);
			if (pType)
				country = reinterpret_cast<const char*>(pType + 0x24);
		}

		PlayerCountExt::Log("[baseunit] house@0x%08X (country %s) can build nothing from "
			"[General]BaseUnit=; treating it as unaffordable instead of crashing "
			"(Antares leaves this call site unguarded)\n", pHouse, country);
	}

	R->EBX(0u);
	return 0x4F672A;
}
