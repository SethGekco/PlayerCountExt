/**
*  PlayerCountExt — give the score screen a record per house
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
*      0xA8D580 - 0xA8D1FC = 900 = 8 x 112 + 4
*
*  so record 8 would begin at 0xA8D57C and the counter at 0xA8D580 sits FOUR
*  BYTES INSIDE IT. A ninth house overwrites the counter with its own name text;
*  the next iteration reads that text as an index. At the observed crash EBP was
*  0x0070006D — UTF-16 characters, not a count.
*
*  WHY RELOCATE RATHER THAN CLAMP
*  ------------------------------
*  The first fix clamped the index so surplus houses reused slot 7. That stopped
*  the crash but threw the data away, which matters far more at 30 players than
*  at 9: two thirds of the results table was simply missing.
*
*  The array cannot grow in place — 0xA8D57C and 0xA8D580 are separate globals
*  sitting immediately after it (0x52CA82 writes both back to back, and the
*  reads at 0x5C9BB8/0x685F58 are direct, not indexed). So the array moves to a
*  buffer of our own and every absolute reference is repointed.
*
*  WHAT IS PATCHED
*  ---------------
*  Record fields are reached as `[esi + <absolute>]` with ESI = index * 112, so
*  each referenced field contributes one displacement to rewrite. Seven distinct
*  ones appear across ~48 sites:
*
*      +0x00  +0x28  +0x2C  +0x30  +0x40  +0x50  +0x60
*
*  Anything reached through a register base — e.g. `lea ebx,[esi+0xa8d1fc]` at
*  0x5C9911, which the name strcpy then writes through — follows automatically
*  once the base displacement is corrected, so those need no patch.
*
*  The counter at 0xA8D580 deliberately does NOT move: it is not part of the
*  array, and the overflow that used to hit it is exactly what this removes.
*
*  GPLv3.
*/

#include "PlayerCountExt.h"

#include <Syringe.h>
#include <Helpers/Macro.h>

#include <cstring>
#include <windows.h>

namespace
{
	constexpr DWORD RecordArray = 0xA8D1FC;
	constexpr DWORD RecordCount = 0xA8D580;
	constexpr int RecordStride = 112;

	// (0xA8D580 - 0xA8D1FC) / 112 == 8, with the counter inside record 8.
	constexpr int VanillaCapacity =
		static_cast<int>(RecordCount - RecordArray) / RecordStride;

	// One per house, matching the engine's own ceiling: 30 players plus Neutral
	// and Special. Costs 3.5 KB, which is nothing next to losing the results.
	constexpr int NewCapacity = PlayerCountExt::EngineHouseCeiling;

	// Field displacements that appear as `[esi + <abs>]`. Derived from the
	// disassembly rather than guessed; see the header comment.
	constexpr int FieldOffsets[] = { 0x00, 0x28, 0x2C, 0x30, 0x40, 0x50, 0x60 };
	constexpr int FieldCount = sizeof(FieldOffsets) / sizeof(FieldOffsets[0]);

	DWORD NewBase = 0;
	bool Relocated = false;

	// The game's .text, read from its own PE headers rather than hardcoded.
	bool TextSection(DWORD& start, DWORD& size)
	{
		const auto module = reinterpret_cast<const BYTE*>(0x400000);
		const auto peOffset = *reinterpret_cast<const DWORD*>(module + 0x3C);
		const auto pe = module + peOffset;

		if (*reinterpret_cast<const DWORD*>(pe) != 0x00004550) // "PE\0\0"
			return false;

		const int sections = *reinterpret_cast<const WORD*>(pe + 6);
		const int optSize = *reinterpret_cast<const WORD*>(pe + 20);
		const BYTE* table = pe + 24 + optSize;

		for (int i = 0; i < sections; ++i)
		{
			const BYTE* entry = table + i * 40;
			if (std::memcmp(entry, ".text", 5) == 0)
			{
				start = 0x400000 + *reinterpret_cast<const DWORD*>(entry + 12);
				size = *reinterpret_cast<const DWORD*>(entry + 8);
				return true;
			}
		}

		return false;
	}

	// Rewrite every occurrence of `from` to `to` within .text.
	//
	// Byte-verified: the dword is re-read under the lock and only written if it
	// still holds the expected value, so a mismatch skips rather than corrupts.
	int Repoint(DWORD textStart, DWORD textSize, DWORD from, DWORD to)
	{
		int patched = 0;

		for (DWORD addr = textStart; addr + 4 <= textStart + textSize; ++addr)
		{
			auto site = reinterpret_cast<DWORD*>(addr);
			if (*site != from)
				continue;

			DWORD old = 0;
			if (!VirtualProtect(site, 4, PAGE_EXECUTE_READWRITE, &old))
				continue;

			if (*site == from)
			{
				*site = to;
				++patched;
			}

			VirtualProtect(site, 4, old, &old);
		}

		return patched;
	}
}

// ---------------------------------------------------------------------------
// Relocate the score-record array — done once, from the command-line parse.
//
// 0x52F639 runs before any scenario is loaded and long before the score screen,
// which is what this needs: the patch must be in place before the first record
// is written, and it only has to happen once per process.
// ---------------------------------------------------------------------------
DEFINE_HOOK(0x52F639, PlayerCountExt_ScoreRecords_Relocate, 0x5)
{
	if (Relocated)
		return 0;

	Relocated = true;

	DWORD textStart = 0, textSize = 0;
	if (!TextSection(textStart, textSize))
	{
		PlayerCountExt::Log("[score] could not locate .text; leaving the %d-record array in place\n",
			VanillaCapacity);
		return 0;
	}

	const SIZE_T bytes = static_cast<SIZE_T>(NewCapacity) * RecordStride;
	NewBase = reinterpret_cast<DWORD>(
		VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));

	if (!NewBase)
	{
		PlayerCountExt::Log("[score] could not allocate %u bytes; leaving the array in place\n",
			static_cast<unsigned>(bytes));
		return 0;
	}

	int total = 0;
	for (int i = 0; i < FieldCount; ++i)
	{
		const DWORD from = RecordArray + FieldOffsets[i];
		const DWORD to = NewBase + FieldOffsets[i];
		const int n = Repoint(textStart, textSize, from, to);

		PlayerCountExt::Log("[score]   field +0x%02X: 0x%08X -> 0x%08X (%d site%s)\n",
			FieldOffsets[i], from, to, n, n == 1 ? "" : "s");
		total += n;
	}

	PlayerCountExt::Log("[score] record array relocated to 0x%08X: %d -> %d records, "
		"%d displacement%s patched\n",
		NewBase, VanillaCapacity, NewCapacity, total, total == 1 ? "" : "s");

	return 0;
}

// ---------------------------------------------------------------------------
// Backstop at the append site — 0x5C98F1.
//
// Stolen bytes 5: a1 80 d5 a8 00  (mov eax,ds:0xa8d580).
//
// The relocation gives room for every house, so this should never fire. It
// stays because the failure it prevents is silent memory corruption rather than
// a fault: if the array somehow did not move, or a game ever exceeded the house
// ceiling, an unbounded index would walk straight off the end again.
// ---------------------------------------------------------------------------
DEFINE_HOOK(0x5C98F1, PlayerCountExt_ScoreRecords_BoundIndex, 0x5)
{
	auto& count = *reinterpret_cast<DWORD volatile*>(RecordCount);
	const int capacity = NewBase ? NewCapacity : VanillaCapacity;

	if (static_cast<int>(count) >= capacity)
	{
		static int warned = 0;
		if (warned < 4)
		{
			++warned;
			PlayerCountExt::Log("[score] record index %u exceeds the %d-record array; "
				"reusing the last slot\n", count, capacity);
		}

		count = static_cast<DWORD>(capacity - 1);
	}

	R->EAX(count);
	return 0x5C98F6;
}
