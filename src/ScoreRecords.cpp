/**
*  PlayerCountExt — stop the score-record array overflowing at 9 houses
*
*  THE BUG
*  -------
*  Fatal Error, C0000005 at 0x5C9917, on the score screen of a 16-player game.
*
*  End-of-game statistics are appended to a fixed array of 112-byte records at
*  0xA8D1FC, indexed by a counter at 0xA8D580:
*
*      5c98f1:  mov  eax,ds:0xa8d580     ; counter
*      5c98f6:  mov  ebp,eax             ; index
*      5c98f9:  mov  ds:0xa8d580,eax     ; counter++
*      5c9904:  lea  esi,[ebp*8+0]
*      5c990c:  sub  esi,ebp             ; *7
*      5c990e:  shl  esi,4               ; *16  =>  stride 112
*      5c9917:  mov  DWORD PTR [esi+0xa8d228],0
*      5c9922:  call 0x7ca489            ; strcpy of the house name
*
*  The array holds exactly EIGHT records:
*
*      0xA8D580 - 0xA8D1FC = 900 bytes = 8 x 112 + 4
*
*  so record index 8 begins at 0xA8D57C and the counter at 0xA8D580 sits FOUR
*  BYTES INSIDE IT. Writing a ninth house's record overwrites the counter with
*  its own name text; the next iteration then reads that text as an index. At
*  the observed crash EBP was 0x0070006D — UTF-16 characters, not a count — and
*  the derived address was long out of any mapped region.
*
*  Vanilla can never reach nine, so the layout is safe there and the failure is
*  purely a consequence of lifting the house cap.
*
*  WHAT THIS DOES — AND WHAT IT DOES NOT
*  -------------------------------------
*  This is a STOPGAP that trades fidelity for safety: it clamps the index to the
*  last real slot, so houses past the eighth reuse slot 7 instead of writing
*  outside the array. No corruption, no crash, and the score screen still shows
*  eight rows — the later houses overwrite each other in the final one.
*
*  The real fix is to relocate the array: ~48 absolute displacements reference
*  its seven field offsets (+0, +0x28, +0x2C, +0x30, +0x40, +0x50, +0x60), and
*  repointing them at a larger buffer would give every house its own record.
*  That is a byte-patcher job of the same shape as MapSizeExt's, worth doing
*  deliberately rather than inline with a crash fix.
*
*  GPLv3.
*/

#include "PlayerCountExt.h"

#include <Syringe.h>
#include <Helpers/Macro.h>

namespace
{
	constexpr DWORD AddrRecordArray = 0xA8D1FC;
	constexpr DWORD AddrRecordCount = 0xA8D580;

	// (0xA8D580 - 0xA8D1FC) / 112 == 8 exactly, with the counter landing inside
	// what would be record 8.
	constexpr int RecordStride = 112;
	constexpr int RecordCapacity = static_cast<int>(AddrRecordCount - AddrRecordArray) / RecordStride;

	int Clamped = 0;
}

// ---------------------------------------------------------------------------
// Score-record append — 0x5C98F1.
//
// Stolen bytes 5: a1 80 d5 a8 00  (mov eax,ds:0xa8d580).
//
// We re-do the load ourselves into EAX and let the stolen-byte replacement fall
// through, so the engine's own `mov ebp,eax / inc eax / mov ds:...,eax` runs on
// a value we have already bounded.
// ---------------------------------------------------------------------------
DEFINE_HOOK(0x5C98F1, PlayerCountExt_ScoreRecords_ClampIndex, 0x5)
{
	auto& count = *reinterpret_cast<DWORD volatile*>(AddrRecordCount);

	if (static_cast<int>(count) >= RecordCapacity)
	{
		if (Clamped < 8)
		{
			PlayerCountExt::Log("[score] record index %u would overflow the %d-slot array at "
				"0x%08X and corrupt the counter at 0x%08X; reusing slot %d instead\n",
				count, RecordCapacity, AddrRecordArray, AddrRecordCount, RecordCapacity - 1);
			++Clamped;
		}

		count = static_cast<DWORD>(RecordCapacity - 1);
	}

	R->EAX(count);
	return 0x5C98F6;
}
