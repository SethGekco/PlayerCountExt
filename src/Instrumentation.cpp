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
#include "SpawnConfig.h"

#include <Syringe.h>
#include <Helpers/Macro.h>

#include <HouseClass.h>
#include <HouseTypeClass.h>
#include <ScenarioClass.h>

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
	constexpr DWORD AddrAISlotsColors    = 0xA8B2BC; // int[8], also the 0x6882C5 end bound
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

	// -----------------------------------------------------------------------
	// Waypoint / start-position probe.
	//
	// Groundwork for synthesising extra start positions: the plan is to write
	// unused entries of ScenarioClass::Waypoints[702] with coordinates derived
	// from the map's real waypoints, so the engine sees more genuinely normal
	// start positions instead of us hooking unit placement.
	//
	// Before writing to that array we confirm we have the right one. YRpp's
	// struct layout is a best-effort mapping and has been wrong before in this
	// very subsystem (its AISlotsStruct mislabels Countries as StartingSpots),
	// so we cross-check YRpp's computed offset against the +0x632 recovered
	// from the disassembly of the start counter at 0x6883B7
	// (`lea 0x632(%edx),%ecx`). Agreement means the rest of the struct — the
	// fields we actually intend to write — can be trusted.
	// -----------------------------------------------------------------------
	constexpr DWORD AddrScenarioPtr   = 0xA8B230; // holds a ScenarioClass*
	constexpr int   ExpectedWaypointOffset = 0x632;

	void ProbeWaypoints()
	{
		const auto pScen = *reinterpret_cast<ScenarioClass* const volatile*>(AddrScenarioPtr);
		if (!pScen)
		{
			PlayerCountExt::Log("[wp] ScenarioClass::Instance is NULL — probe skipped\n");
			return;
		}

		const auto base = reinterpret_cast<const char*>(pScen);
		const auto wpOffset = static_cast<int>(
			reinterpret_cast<const char*>(&pScen->Waypoints[0]) - base);

		PlayerCountExt::Log("[wp] ScenarioClass::Instance = 0x%08X\n",
			reinterpret_cast<DWORD>(pScen));
		PlayerCountExt::Log("[wp] Waypoints offset: yrpp=0x%X disasm=0x%X %s\n",
			wpOffset, ExpectedWaypointOffset,
			(wpOffset == ExpectedWaypointOffset) ? "ok" : "<<< MISMATCH - do NOT write");
		PlayerCountExt::Log("[wp] NumberStartingPoints = %d  (offset 0x%X)\n",
			pScen->NumberStartingPoints,
			static_cast<int>(reinterpret_cast<const char*>(&pScen->NumberStartingPoints) - base));

		// The engine's "undefined waypoint" sentinel, compared as two WORDs at
		// 0xB05458 / 0xB0545A by the counter loop.
		const short sentX = Peek<short>(0xB05458);
		const short sentY = Peek<short>(0xB0545A);
		PlayerCountExt::Log("[wp] undefined-sentinel = (%d,%d)\n", sentX, sentY);

		// First 16 waypoints: how many the map really defines, and where the
		// free space for synthetic entries begins.
		PlayerCountExt::Log("[wp] Waypoints[0..15]:\n");
		for (int i = 0; i < 16; ++i)
		{
			const auto& wp = pScen->Waypoints[i];
			const bool defined = !(wp.X == sentX && wp.Y == sentY);
			PlayerCountExt::Log("[wp]   wp%-2d = (%5d,%5d) %s\n",
				i, wp.X, wp.Y, defined ? "defined" : "-");
		}

		PlayerCountExt::Log("[wp] StartingPoints[8]:");
		for (int i = 0; i < 8; ++i)
			PlayerCountExt::Log(" (%d,%d)", pScen->StartingPoints[i].X, pScen->StartingPoints[i].Y);
		PlayerCountExt::Log("\n");

		// 16-wide in vanilla — headroom the start->house map already has.
		PlayerCountExt::Log("[wp] HouseIndices[16]:");
		for (int i = 0; i < 16; ++i)
			PlayerCountExt::Log(" %d", pScen->HouseIndices[i]);
		PlayerCountExt::Log("\n");
	}
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

	// AISlots.Countries[8] @0xA8B29C and Colors[8] @0xA8B2BC.
	//
	// NOTE the -1/-3 sentinels SKIP a slot, they do NOT end the loop: all three
	// conditional jumps target 0x6882C2, which is the `add $0x4,%ebx` increment.
	// The loop always walks all 8 slots; EAX counts houses CREATED (incremented
	// only at 0x68817E), not the slot index. The hard bound is the pointer
	// compare at 0x6882C5 against 0xA8B2BC (== &Countries[8] == &Colors[0]).
	PlayerCountExt::Log("[instr] AISlots.Countries[8] @0xA8B29C =");
	for (int i = 0; i < 8; ++i)
		PlayerCountExt::Log(" %d", Peek<int>(AddrAISlotsCountries + i * 4));
	PlayerCountExt::Log("\n");

	PlayerCountExt::Log("[instr] AISlots.Colors[8]    @0xA8B2BC =");
	for (int i = 0; i < 8; ++i)
		PlayerCountExt::Log(" %d", Peek<int>(AddrAISlotsColors + i * 4));
	PlayerCountExt::Log("\n");

	// Sanity: this must be 0x20 (32 bytes = 8 ints) or the layout assumption
	// behind the 0x6882C5 cap is wrong.
	PlayerCountExt::Log("[instr] AISlots stride check: 0xA8B2BC - 0xA8B29C = 0x%X (expect 0x20)\n",
		AddrAISlotsColors - AddrAISlotsCountries);

	// -----------------------------------------------------------------------
	// spawn.ini parse + cross-check.
	//
	// Reload every time: AssignHouses runs twice per game start, and returning
	// to the menu rewrites the file. Load() is a full reset, so this is safe.
	//
	// The cross-check is the point of this block. We believe AISlots[i]
	// corresponds to Multi(i+1), with human-owned slots left at -1. Rather than
	// assert that, print both side by side and let the engine referee it. A
	// MISMATCH line means OUR mapping is wrong, not that the game is.
	// -----------------------------------------------------------------------
	ProbeWaypoints();

	auto& spawn = PlayerCountExt::SpawnConfig::Get();
	spawn.Load();
	spawn.LogSummary();

	if (spawn.Loaded())
	{
		if (spawn.AIPlayers() != aiPlayers)
			PlayerCountExt::Log("[xchk] AIPlayers: spawn.ini=%d engine@0xA8B274=%d   <<< MISMATCH\n",
				spawn.AIPlayers(), aiPlayers);
		else
			PlayerCountExt::Log("[xchk] AIPlayers agrees (%d)\n", aiPlayers);

		PlayerCountExt::Log("[xchk] AISlots[i] vs spawn.ini Multi(i+1):\n");
		for (int i = 0; i < 8; ++i)
		{
			const int engCountry = Peek<int>(AddrAISlotsCountries + i * 4);
			const int engColor   = Peek<int>(AddrAISlotsColors    + i * 4);
			const auto& h = spawn.House(i + 1);

			// A slot the engine left at -1 with no spawn.ini entry is just an
			// unused slot; not worth a line.
			if (engCountry == -1 && !h.Defined)
				continue;

			PlayerCountExt::Log("[xchk]   slot%d Multi%-2d country eng=%-3d ini=%-3d %s | color eng=%-3d ini=%-3d\n",
				i, i + 1, engCountry, h.Country,
				(h.Country == engCountry) ? "ok" : "<<< MISMATCH",
				engColor, h.Color);
		}
	}

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
