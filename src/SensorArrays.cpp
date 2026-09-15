/**
*  PlayerCountExt — stop houses past the 24th corrupting cell object lists
*
*  THE BUG
*  -------
*  CellClass tracks cloak- and disguise-detection coverage as two arrays of one
*  WORD per house — but only TWENTY-FOUR houses, not 32:
*
*      +0x7C   unsigned short SensorsOfHouses[24]
*      +0xAC   unsigned short DisguiseSensorsOfHouses[24]      (0x7C + 24*2)
*      +0xDC   DWORD          BaseSpacerOfHouses
*      +0xE0   FootClass*     Jumpjet
*      +0xE4   ObjectClass*   FirstObject      <-- the cell's object list head
*      +0xE8   ObjectClass*   AltObject
*
*  The four accessors take the house index straight off the stack and index with
*  no bound whatsoever:
*
*      487150:  mov eax,[esp+0x4]
*      487154:  inc WORD PTR [ecx+eax*2+0x7c]     ; Sensors_AddOfHouse
*      487164:  dec WORD PTR [ecx+eax*2+0x7c]     ; Sensors_RemOfHouse
*      487174:  inc WORD PTR [ecx+eax*2+0xac]     ; DisguiseSensors_AddOfHouse
*      487184:  dec WORD PTR [ecx+eax*2+0xac]     ; DisguiseSensors_RemOfHouse
*
*  Vanilla never exceeds 8 players (10 houses), so 24 is unreachable and the
*  missing check costs nothing. Past it:
*
*    - index 24..31 on SensorsOfHouses writes into DisguiseSensorsOfHouses,
*      producing phantom disguise detection for low-numbered houses;
*    - index 24..31 on DisguiseSensorsOfHouses writes into BaseSpacerOfHouses,
*      Jumpjet, FirstObject and AltObject.
*
*  FirstObject is the head of the cell's object linked list. Corrupting it means
*  nothing can enumerate what is standing in that cell — which presents as units
*  refusing to acquire targets, with no crash and nothing in any log.
*
*  Suspected from exactly that symptom at 30 players (GI and MTNK not auto-
*  attacking), and confirmed at 22 players — 24 houses, the last count that fits
*  — behaving normally.
*
*  WHAT THIS DOES
*  --------------
*  Skips the operation entirely when the index is out of range, rather than
*  clamping it. Clamping to 23 would corrupt house 23's counters instead, giving
*  that house phantom detection — trading a silent bug for a subtler one. The
*  cost of skipping is contained and predictable: houses past the 24th cannot
*  detect cloaked or disguised units. Everything else is untouched.
*
*  In range, the hook returns 0 and Syringe re-executes the stolen bytes, so
*  vanilla behaviour is bit-identical. The stolen bytes are a `mov` and an
*  `inc`/`dec` — no relative branches, no stack arithmetic — so re-executing
*  them in the trampoline is safe.
*
*  NOT a fix for the underlying limit. Widening the arrays is not possible from
*  here: they live inside CellClass, which the engine allocates at a fixed 0x148
*  bytes (`push 0x148` at 0x5663C3), so growing them would shift every field
*  after them across the entire engine.
*
*  GPLv3.
*/

#include "PlayerCountExt.h"

#include <Syringe.h>
#include <Helpers/Macro.h>

namespace
{
	// Both arrays hold 24 entries. Derived from the layout, not assumed:
	// DisguiseSensorsOfHouses sits at 0x7C + 24*2 = 0xAC, which only works if
	// the first array is exactly 24 WORDs long.
	constexpr int SensorHouseCapacity = 24;

	// Report the first few, then stay quiet — this fires per cell per unit and
	// would otherwise bury the log.
	int Reported = 0;

	bool OutOfRange(int houseIndex, const char* what)
	{
		if (houseIndex >= 0 && houseIndex < SensorHouseCapacity)
			return false;

		if (Reported < 8)
		{
			++Reported;
			PlayerCountExt::Log("[sensor] %s for house %d skipped: the array holds %d houses, "
				"and writing past it lands on the cell's object list\n",
				what, houseIndex, SensorHouseCapacity);
		}

		return true;
	}
}

// ---------------------------------------------------------------------------
// Sensors_AddOfHouse / RemOfHouse  — 0x487150 / 0x487160
// DisguiseSensors_AddOfHouse / RemOfHouse — 0x487170 / 0x487180
//
// ECX = CellClass*, [ESP+0x4] = house index. Each is a three-instruction leaf:
// load the argument, inc/dec the slot, `ret 4`.
//
// Stolen bytes cover the load and the arithmetic, so returning 0 reproduces
// vanilla exactly; returning the `ret` address skips the write.
// ---------------------------------------------------------------------------

DEFINE_HOOK(0x487150, PlayerCountExt_Sensors_AddOfHouse, 0x9)
{
	GET_STACK(int, houseIndex, 0x4);
	return OutOfRange(houseIndex, "sensor add") ? 0x487159 : 0;
}

DEFINE_HOOK(0x487160, PlayerCountExt_Sensors_RemOfHouse, 0x9)
{
	GET_STACK(int, houseIndex, 0x4);
	return OutOfRange(houseIndex, "sensor remove") ? 0x487169 : 0;
}

DEFINE_HOOK(0x487170, PlayerCountExt_DisguiseSensors_AddOfHouse, 0xC)
{
	GET_STACK(int, houseIndex, 0x4);
	return OutOfRange(houseIndex, "disguise sensor add") ? 0x48717C : 0;
}

DEFINE_HOOK(0x487180, PlayerCountExt_DisguiseSensors_RemOfHouse, 0xC)
{
	GET_STACK(int, houseIndex, 0x4);
	return OutOfRange(houseIndex, "disguise sensor remove") ? 0x48718C : 0;
}
