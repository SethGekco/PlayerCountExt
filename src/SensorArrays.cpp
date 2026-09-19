/**
*  PlayerCountExt — give every house cloak and disguise detection
*
*  THE LIMIT
*  ---------
*  CellClass tracks detection coverage as two arrays of one WORD per house, and
*  both hold only TWENTY-FOUR entries:
*
*      +0x7C   unsigned short SensorsOfHouses[24]
*      +0xAC   unsigned short DisguiseSensorsOfHouses[24]      (0x7C + 24*2)
*      +0xDC   DWORD          BaseSpacerOfHouses
*      +0xE0   FootClass*     Jumpjet
*      +0xE4   ObjectClass*   FirstObject      <-- the cell's object list head
*
*  Vanilla peaks at 10 houses, so nothing ever reached 24 and the accessors were
*  written without a bound. Past it the writes walk into the fields above —
*  eventually FirstObject, which is how this first showed itself: units quietly
*  refusing to acquire targets, no crash, nothing logged.
*
*  An earlier build simply SKIPPED out-of-range operations. That stopped the
*  corruption but left houses past the 24th unable to see cloaked or disguised
*  units, which made 30 players work-but-asymmetric. This replaces that.
*
*  THE FIX: RE-ENCODE, DO NOT RESIZE
*  ---------------------------------
*  The entries are reference counts — incremented when a detector comes into
*  range, decremented when it leaves, and read only as `> 0`. A count that never
*  realistically passes a dozen does not need 16 bits.
*
*  So the same 96 bytes are reinterpreted as two arrays of BYTES:
*
*      +0x7C   unsigned char Sensors[48]           0x7C..0xAB
*      +0xAC   unsigned char DisguiseSensors[48]   0xAC..0xDB
*
*  Each lands exactly where the next field begins — 0xAC and 0xDC respectively —
*  so nothing shifts and nothing overruns. That is 48 house slots where the
*  engine can only ever produce 32, leaving 50% headroom.
*
*  WHY NOT ENLARGE CellClass
*  -------------------------
*  Appending space and moving the arrays there looks cleaner and is a trap:
*  cells are block-copied by `mov ecx,0x52` + `rep movsd` at 0x406401, 0x40640C
*  and 0x57DF11 — 82 dwords, exactly sizeof(CellClass). Anything past 0x148
*  would be silently dropped by those copies. Re-encoding in place is immune
*  because the size never changes, and it also keeps us compatible with
*  MapSizeExt, which reallocates the cell array itself.
*
*  WHY THIS IS SAFE TO DO UNILATERALLY
*  -----------------------------------
*  Exactly seven instructions in the whole binary touch these arrays, and six of
*  them are one-instruction leaf accessors that every other consumer calls
*  through (29 call sites in total):
*
*      0x4870D0  IsSensedByHouse            <- 19 callers
*      0x4870F0  IsDisguiseSensedByHouse    <-  4 callers
*      0x487150  Sensors_AddOfHouse         0x487160  Sensors_RemOfHouse
*      0x487170  DisguiseSensors_AddOfHouse 0x487180  DisguiseSensors_RemOfHouse
*      0x48682D  an inlined visibility test, the only non-accessor
*
*  Verified by scanning for the addressing mode (`*2+0x7c` / `*2+0xac`) across
*  the disassembly rather than trusting a header — YRpp's CellClass offsets are
*  wrong on this build (its LandType and Passability both read uniform garbage),
*  so every offset here comes from the instruction encodings.
*
*  Zero-initialisation needs no change: the region is the same 96 bytes and all
*  zeroes mean "no coverage" under either reading.
*
*  Determinism: the encoding is identical on every client running this DLL, and
*  these counts feed visibility rather than the simulation, so netplay is
*  unaffected. Worth a SyncTraceExt pass regardless.
*
*  SATURATION
*  ----------
*  A byte wraps at 255 where a word wrapped at 65535. Reaching it needs 255
*  detectors covering one cell at once, which is not plausible — but an `inc`
*  that wraps to 0 would silently un-detect a cell, so the counters saturate
*  instead. Saturating costs nothing here because the add/remove paths are
*  already ours; it is not an in-place `inc`/`dec` patch.
*
*  GPLv3.
*/

#include "PlayerCountExt.h"

#include <Syringe.h>
#include <Helpers/Macro.h>

namespace
{
	// Byte slots per array. 0xAC - 0x7C == 48 and 0xDC - 0xAC == 48, so each
	// array ends exactly where the next field starts.
	constexpr int SensorSlots = 48;

	constexpr int SensorsOffset = 0x7C;
	constexpr int DisguiseSensorsOffset = 0xAC;

	constexpr unsigned char SlotMax = 255;

	static_assert(PlayerCountExt::EngineHouseCeiling <= SensorSlots,
		"the re-encoded arrays must cover every house the engine can create");

	// These fire per cell per detector per move; a few lines prove the path is
	// live, more would bury the log.
	int ReportedRange = 0;
	int ReportedSaturation = 0;
	int ReportedUnderflow = 0;

	unsigned char* Slot(DWORD cell, int offset, int house)
	{
		return reinterpret_cast<unsigned char*>(cell + offset + house);
	}

	bool InRange(DWORD cell, int house, const char* what)
	{
		if (cell && house >= 0 && house < SensorSlots)
			return true;

		if (ReportedRange < 8)
		{
			++ReportedRange;
			PlayerCountExt::Log("[sensor] %s skipped for house %d: outside the %d re-encoded "
				"slots (cell %s)\n", what, house, SensorSlots, cell ? "ok" : "NULL");
		}

		return false;
	}

	void AddCoverage(DWORD cell, int offset, int house, const char* what)
	{
		if (!InRange(cell, house, what))
			return;

		auto* const slot = Slot(cell, offset, house);

		if (*slot < SlotMax)
		{
			++*slot;
			return;
		}

		// Held at the ceiling rather than wrapped to zero, which would read as
		// "not covered" and blink the cell out of detection.
		if (ReportedSaturation < 4)
		{
			++ReportedSaturation;
			PlayerCountExt::Log("[sensor] %s for house %d held at %d — that many detectors "
				"cover one cell, which should not happen\n", what, house, SlotMax);
		}
	}

	void RemoveCoverage(DWORD cell, int offset, int house, const char* what)
	{
		if (!InRange(cell, house, what))
			return;

		auto* const slot = Slot(cell, offset, house);

		if (*slot > 0)
		{
			--*slot;
			return;
		}

		// An unmatched remove. Vanilla would wrap to 65535 and leave the cell
		// permanently "detected"; we clamp and say so, because it means an add
		// was lost somewhere rather than that the count is genuinely zero.
		if (ReportedUnderflow < 4)
		{
			++ReportedUnderflow;
			PlayerCountExt::Log("[sensor] %s for house %d with no coverage to remove — "
				"an add was missed\n", what, house);
		}
	}

	bool HasCoverage(DWORD cell, int offset, int house)
	{
		if (!cell || house < 0 || house >= SensorSlots)
			return false;

		return *Slot(cell, offset, house) > 0;
	}
}

// ---------------------------------------------------------------------------
// Readers.
//
// Both are leaves of the shape:
//
//     mov edx,[esp+0x4]          ; house index
//     xor eax,eax
//     cmp WORD PTR [ecx+edx*2+<off>],ax
//     setg al
//     ret 0x4
//
// Returning the `ret` address with EAX set reproduces the contract exactly —
// the result is a full 0 or 1 in EAX because the original zeroed it before
// setting AL.
// ---------------------------------------------------------------------------

DEFINE_HOOK(0x4870D0, PlayerCountExt_Sensors_IsSensedByHouse, 0xE)
{
	GET_STACK(int, house, 0x4);

	R->EAX(HasCoverage(R->ECX(), SensorsOffset, house) ? 1u : 0u);
	return 0x4870DE;
}

DEFINE_HOOK(0x4870F0, PlayerCountExt_Sensors_IsDisguiseSensedByHouse, 0x11)
{
	GET_STACK(int, house, 0x4);

	R->EAX(HasCoverage(R->ECX(), DisguiseSensorsOffset, house) ? 1u : 0u);
	return 0x487101;
}

// ---------------------------------------------------------------------------
// The one inlined read — 0x48682D, inside a visibility test.
//
//     486821:  mov eax,[eax+0x30]      ; house index to test against
//     486824:  cmp ecx,eax
//     486826:  jne 0x48682d            ; <- our hook is this jump's target
//     486828:  mov al,0x1 / ret 0x4
//     48682d:  cmp WORD PTR [edx+eax*2+0x7c],0x0
//     486833:  jg  0x48683a            ; covered    -> fall through to false
//     486835:  mov al,0x1 / ret 0x4    ; not covered-> true
//     48683a:  xor al,al  / ret 0x4
//
// EDX holds the cell (set at 0x486805), EAX the house index. Both branch
// targets set AL themselves, so nothing needs preserving.
//
// The stolen bytes are a single `cmp` with no relative branch inside them, so
// there is nothing for Syringe to mis-relocate.
// ---------------------------------------------------------------------------
DEFINE_HOOK(0x48682D, PlayerCountExt_Sensors_VisibilityCheck, 0x6)
{
	const int house = static_cast<int>(R->EAX());

	return HasCoverage(R->EDX(), SensorsOffset, house)
		? 0x48683A   // covered — the original `jg`
		: 0x486835;  // not covered
}

// ---------------------------------------------------------------------------
// Writers.
//
//     mov eax,[esp+0x4]          ; house index
//     inc/dec WORD PTR [ecx+eax*2+<off>]
//     ret 0x4
//
// ECX is the cell. Each returns its own `ret`, so the vanilla word write never
// runs.
// ---------------------------------------------------------------------------

DEFINE_HOOK(0x487150, PlayerCountExt_Sensors_AddOfHouse, 0x9)
{
	GET_STACK(int, house, 0x4);

	AddCoverage(R->ECX(), SensorsOffset, house, "sensor add");
	return 0x487159;
}

DEFINE_HOOK(0x487160, PlayerCountExt_Sensors_RemOfHouse, 0x9)
{
	GET_STACK(int, house, 0x4);

	RemoveCoverage(R->ECX(), SensorsOffset, house, "sensor remove");
	return 0x487169;
}

DEFINE_HOOK(0x487170, PlayerCountExt_DisguiseSensors_AddOfHouse, 0xC)
{
	GET_STACK(int, house, 0x4);

	AddCoverage(R->ECX(), DisguiseSensorsOffset, house, "disguise sensor add");
	return 0x48717C;
}

DEFINE_HOOK(0x487180, PlayerCountExt_DisguiseSensors_RemOfHouse, 0xC)
{
	GET_STACK(int, house, 0x4);

	RemoveCoverage(R->ECX(), DisguiseSensorsOffset, house, "disguise sensor remove");
	return 0x48718C;
}
