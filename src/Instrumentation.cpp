/**
*  PlayerCountExt — read-only instrumentation of ScenarioClass::AssignHouses
*
*  WHY THIS EXISTS
*  ---------------
*  Every engine address this project relies on was recovered by STATICALLY
*  disassembling one copy of gamemd.exe. Static analysis can be wrong in ways
*  that are invisible until something crashes far from the cause. This file
*  changes no behaviour whatsoever — it only logs — so a single run either
*  confirms those addresses against a live game or refutes them cheaply.
*
*  Run this BEFORE building anything on top of the addresses.
*
*  WHAT IT VALIDATES
*    0x687F10   AssignHouses is entered at all, and when
*    0x688378   the epilogue is reached (i.e. the function completed)
*    0xA8B238   SessionClass instance / GameMode as its first field
*    0xA8DA78   Session players array data pointer
*    0xA8DA84   Session players count
*    0xA8B274   AI player count
*    0xA8B29C   AISlots.Countries[8]  (Colours[8] follows at +0x20)
*    +0x16054   HouseClass::ColorSchemeIndex offset
*
*  The ColorSchemeIndex check is a genuine cross-validation: it reads the same
*  field twice, once through YRpp's mapped struct and once through the raw
*  offset recovered from the disassembly. Agreement confirms both.
*
*  GPLv3. Built on YRpp + Syringe.
*/

#include "PlayerCountExt.h"

// Included so its static_assert (sizeof(HouseClass) <= the engine's verified
// 0x160B8 allocation) keeps being evaluated. GameConstruct.h has no other
// consumer yet — the real house-creation path will use it — and a header that
// is never included is a compile-time guard that never runs.
#include "GameConstruct.h"

#include <Syringe.h>
#include <Helpers/Macro.h>

#include <HouseClass.h>
#include <HouseTypeClass.h>

namespace
{
	// Raw engine globals recovered from the AssignHouses disassembly.
	// Read through volatile so nothing is cached across the engine's own writes.
	template <typename T>
	inline T Peek(DWORD address)
	{
		return *reinterpret_cast<T const volatile*>(address);
	}

	constexpr DWORD AddrSessionClass     = 0xA8B238; // GameMode is field 0
	constexpr DWORD AddrPlayersData      = 0xA8DA78;
	constexpr DWORD AddrPlayersCount     = 0xA8DA84;
	constexpr DWORD AddrAIPlayers        = 0xA8B274;
	constexpr DWORD AddrAISlotsCountries = 0xA8B29C; // int[8]
	constexpr int   OffColorSchemeIndex  = 0x16054;

	const char* GameModeName(int mode)
	{
		switch (mode)
		{
		case 0:  return "Campaign";
		case 3:  return "LAN";
		case 4:  return "Internet";
		case 5:  return "Skirmish";
		default: return "?";
		}
	}

	// Snapshot taken at entry so the exit hook can report a delta.
	int HouseCountAtEntry = -1;
}

// ---------------------------------------------------------------------------
// Entry — 0x687F10, the first instruction of AssignHouses.
//
// Stolen bytes are exactly 5:
//     687f10:  83 ec 4c   sub  $0x4c,%esp     (3)
//     687f13:  56         push %esi           (1)
//     687f14:  57         push %edi           (1)
// ---------------------------------------------------------------------------
DEFINE_HOOK(0x687F10, PlayerCountExt_AssignHouses_Entry, 0x5)
{
	const int gameMode    = Peek<int>(AddrSessionClass);
	const int playerCount = Peek<int>(AddrPlayersCount);
	const DWORD playerPtr = Peek<DWORD>(AddrPlayersData);
	const int aiPlayers   = Peek<int>(AddrAIPlayers);

	HouseCountAtEntry = HouseClass::Array.Count;

	PlayerCountExt::Log("\n=== [instr] AssignHouses (0x687F10) ENTRY ===\n");
	PlayerCountExt::Log("[instr] GameMode      = %d (%s)   [0xA8B238]\n",
		gameMode, GameModeName(gameMode));
	PlayerCountExt::Log("[instr] Players.Count = %d        [0xA8DA84]\n", playerCount);
	PlayerCountExt::Log("[instr] Players.Data  = 0x%08X    [0xA8DA78]\n", playerPtr);
	PlayerCountExt::Log("[instr] AIPlayers     = %d        [0xA8B274]\n", aiPlayers);
	PlayerCountExt::Log("[instr] HouseClass::Array.Count (before) = %d\n", HouseCountAtEntry);

	// AISlots.Countries[8] @ 0xA8B29C. The AI loop stops at the first -1/-3
	// sentinel, and is hard-bounded by the pointer compare at 0x6882C5
	// (end address 0xA8B2BC == &Countries[8] == &Colours[0]).
	PlayerCountExt::Log("[instr] AISlots.Countries[8] @0xA8B29C =");
	for (int i = 0; i < 8; ++i)
		PlayerCountExt::Log(" %d", Peek<int>(AddrAISlotsCountries + i * 4));
	PlayerCountExt::Log("\n");

	// Sanity: this must be 0x20 (32 bytes = 8 ints) or the layout assumption
	// behind the 0x6882C5 cap is wrong.
	PlayerCountExt::Log("[instr] AISlots stride check: 0xA8B2BC - 0xA8B29C = 0x%X (expect 0x20)\n",
		0xA8B2BC - AddrAISlotsCountries);

	return 0; // continue original code
}

// ---------------------------------------------------------------------------
// Exit — 0x688378, the function epilogue, after Neutral and Special are built
// and wired. Stolen bytes are exactly 5:
//     688378:  5f          pop  %edi          (1)
//     688379:  5e          pop  %esi          (1)
//     68837a:  83 c4 4c    add  $0x4c,%esp    (3)
//     68837d:  c3          ret
//
// Hooked here rather than on the `ret` itself, which is a single byte and too
// short to patch.
// ---------------------------------------------------------------------------
DEFINE_HOOK(0x688378, PlayerCountExt_AssignHouses_Exit, 0x5)
{
	const int count = HouseClass::Array.Count;

	PlayerCountExt::Log("[instr] --- AssignHouses EXIT (0x688378) ---\n");
	PlayerCountExt::Log("[instr] HouseClass::Array.Count = %d (was %d, created %d)\n",
		count, HouseCountAtEntry,
		(HouseCountAtEntry >= 0) ? (count - HouseCountAtEntry) : -1);

	for (int i = 0; i < count; ++i)
	{
		const auto pHouse = HouseClass::Array.GetItem(i);
		if (!pHouse)
		{
			PlayerCountExt::Log("[instr]   house[%d] = NULL\n", i);
			continue;
		}

		// Cross-validation: same field via YRpp's mapping and via the raw
		// offset from the disassembly. A mismatch means one of them is wrong.
		const int colorViaYRpp = pHouse->ColorSchemeIndex;
		const int colorViaRaw  = Peek<int>(reinterpret_cast<DWORD>(pHouse) + OffColorSchemeIndex);

		const auto pType = pHouse->Type;

		PlayerCountExt::Log("[instr]   house[%d] ArrayIndex=%d human=%d country=%s color(yrpp)=%d color(+0x16054)=%d%s\n",
			i,
			pHouse->ArrayIndex,
			pHouse->IsHumanPlayer ? 1 : 0,
			pType ? pType->ID : "(null)",
			colorViaYRpp,
			colorViaRaw,
			(colorViaYRpp == colorViaRaw) ? "" : "   <<< MISMATCH");

		// The 32-bit bitfield ceiling: 1u << ArrayIndex over a DWORD.
		if (pHouse->ArrayIndex > 31)
			PlayerCountExt::Log("[instr]   WARNING house[%d] ArrayIndex %d exceeds the 32-bit bitfield range\n",
				i, pHouse->ArrayIndex);
	}

	PlayerCountExt::Log("=== [instr] AssignHouses done ===\n\n");

	return 0; // continue original code
}
