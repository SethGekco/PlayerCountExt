/**
*  PlayerCountExt — apply the alliances the spawner cannot express
*
*  THE PROBLEM
*  -----------
*  Teams are resolved to explicit pairwise alliances by the client, which writes
*  them into spawn.ini as
*
*      [Multi3_Alliances]
*      HouseAllyOne=3
*      HouseAllyTwo=4
*      ...
*
*  Two independent limits cut that short, and both only bite past 8 players:
*
*  1. The stock client generated the key suffixes One..Seven and then fell
*     through to "None" + n, emitting keys like HouseAllyNone7 that nothing
*     reads. Correct while 8 players meant at most 7 allies. Our client fork
*     extends the names through Fifteen.
*
*  2. CnCNet-Spawner.dll only contains the strings HouseAllyOne..HouseAllyEight.
*     Even with correct keys it can express at most 8 allies, so a 16-player
*     team cannot be represented through it at all.
*
*  Observed as: a 2v14 game where the fourteen players on Team B were not allied
*  and fought each other.
*
*  WHAT THIS DOES
*  --------------
*  Reads the alliance sections ourselves and applies every pair directly, which
*  removes the dependency on the spawner's string table entirely. MakeAlly is
*  additive, so doing this alongside the spawner is harmless — pairs it already
*  set are simply set again.
*
*  Mapping: HouseClass::Array[i] is Multi(i+1), the same correspondence the
*  spawn shift uses. A value V under [Multi<n>_Alliances] means "ally with
*  Multi(V+1)", i.e. Array[V], because the client writes allyHouseId - 1.
*
*  GPLv3.
*/

#include "PlayerCountExt.h"

#include <Syringe.h>
#include <Helpers/Macro.h>

#include <cstdio>
#include <windows.h>

namespace
{
	// HouseClass::MakeAlly(HouseClass*, bool bAnnounce)
	using MakeAlly_t = void (__thiscall*)(void*, void*, bool);
	constexpr DWORD AddrMakeAlly = 0x4F9B70;

	constexpr DWORD AddrHouseArrayItems = 0xA8022C;
	constexpr DWORD AddrHouseArrayCount = 0xA80238;

	// Must match the client's GetHouseAllyIndexString. Fifteen covers a
	// 16-player game, where one house has fifteen allies.
	const char* const AllySuffixes[] =
	{
		"One", "Two", "Three", "Four", "Five", "Six", "Seven", "Eight",
		"Nine", "Ten", "Eleven", "Twelve", "Thirteen", "Fourteen", "Fifteen"
	};

	constexpr int MaxAllies = sizeof(AllySuffixes) / sizeof(AllySuffixes[0]);

	// Bare filenames resolve against the Windows directory, not the game
	// directory — the same trap SpawnConfig documents.
	constexpr char SpawnIni[] = ".\\spawn.ini";

	bool IsPlayingHouse(DWORD pHouse)
	{
		if (!pHouse)
			return false;

		const auto pType = *reinterpret_cast<DWORD const volatile*>(pHouse + 0x34);
		return pType && !*reinterpret_cast<BYTE const volatile*>(pType + 0x1A6);
	}
}

void PlayerCountExt::ApplyAlliancesFromSpawnIni(const char* where)
{
	static int lastAppliedForCount = -1;

	const auto pArrayItems = *reinterpret_cast<DWORD* const volatile*>(AddrHouseArrayItems);
	const int count = *reinterpret_cast<int const volatile*>(AddrHouseArrayCount);

	if (!pArrayItems || count <= 0)
	{
		PlayerCountExt::Log("[ally] %s: no houses yet (count=%d)\n", where, count);
		return;
	}

	// The pass can run more than once per game; applying twice is harmless but
	// the log noise is not.
	const bool quiet = (lastAppliedForCount == count);
	lastAppliedForCount = count;

	const auto MakeAlly = reinterpret_cast<MakeAlly_t>(AddrMakeAlly);
	int applied = 0, beyondSpawner = 0;

	for (int i = 0; i < count; ++i)
	{
		const DWORD pHouse = pArrayItems[i];
		if (!IsPlayingHouse(pHouse))
			continue;

		char section[64];
		std::snprintf(section, sizeof(section), "Multi%d_Alliances", i + 1);

		for (int slot = 0; slot < MaxAllies; ++slot)
		{
			char key[64];
			std::snprintf(key, sizeof(key), "HouseAlly%s", AllySuffixes[slot]);

			const int value = GetPrivateProfileIntA(section, key, -1, SpawnIni);
			if (value < 0 || value >= count)
				continue;

			const DWORD pAlly = pArrayItems[value];
			if (pAlly == pHouse || !IsPlayingHouse(pAlly))
				continue;

			MakeAlly(reinterpret_cast<void*>(pHouse), reinterpret_cast<void*>(pAlly), false);
			++applied;

			// Slots past the eighth are the ones CnCNet-Spawner.dll has no
			// string for; those are the alliances that were being lost.
			if (slot >= 8)
				++beyondSpawner;
		}
	}

	// Log unconditionally, including zero. A silent hook is indistinguishable
	// from one that never fired, and that ambiguity has already cost a round.
	if (!quiet || applied)
		PlayerCountExt::Log("[ally] %s: applied %d alliance(s) across %d houses "
			"(%d past HouseAllyEight, which the spawner cannot express)\n",
			where, applied, count, beyondSpawner);
}

// ---------------------------------------------------------------------------
// Head of the vanilla auto-ally pass — 0x5D74A1.
//
//     5d74a0:  push ecx
//     5d74a1:  mov  edx,ds:0xa80238    ; <- we hook here, 6 bytes
//
// Hooked one instruction in, deliberately: the entry's `push ecx` moves ESP,
// and stolen bytes that shift the stack are a known way to corrupt a hook. This
// `mov` is stack-neutral. Cooperative `return 0`, so vanilla's pass still runs.
//
// This fires only if the engine reaches the function at all. On a 16-player run
// it logged nothing, which is why the worker is also called from the
// AssignHouses exit — a seam that demonstrably runs every game.
// ---------------------------------------------------------------------------
DEFINE_HOOK(0x5D74A1, PlayerCountExt_AllianceFix_ApplyFromSpawnIni, 0x6)
{
	PlayerCountExt::ApplyAlliancesFromSpawnIni("auto-ally pass (0x5D74A1)");
	return 0;
}
