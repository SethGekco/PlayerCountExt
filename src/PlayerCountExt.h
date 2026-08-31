/**
*  PlayerCountExt — standalone YR DLL for >8 houses
*  See Main.cpp for the project overview. GPLv3.
*/

#pragma once

#include <Windows.h>

class HouseClass;

namespace PlayerCountExt
{
	// The engine's hard ceiling is the 32-bit per-house bitfield: every one of
	// HouseClass::Allies / AltAllies / TechnoClass::DisplayProductionTo /
	// CellClass::BaseSpacerOfHouses does `1u << ArrayIndex` into a DWORD. With
	// Neutral and Special always present that leaves 30 real houses. Past 31 the
	// shift aliases (x86 masks the count to 5 bits) and houses silently begin
	// sharing alliance/spy bits — corruption, not a crash.
	//
	// We do NOT target 30. See DESIGN.md: the first milestone is 9.
	// Players + Neutral + Special must fit in a 32-bit house bitfield.
	//
	// Every house set in YR (HouseClass::Allies, AltAllies,
	// TechnoClass::DisplayProductionTo, CellClass::BaseSpacerOfHouses) does
	// `1u << ArrayIndex` into a DWORD, so index 32 is the first that does not
	// exist. On x86 `shl` masks the shift count to 5 bits, so index 32 aliases
	// onto index 0 — houses would silently begin sharing alliance and spy bits
	// instead of crashing, which is far harder to notice than a fault.
	//
	// 32 total houses is therefore the real ceiling: 30 players, plus Neutral
	// and Special. Going beyond means widening those bitfields everywhere, a
	// substantially larger job than the array and loop limits lifted so far.
	static constexpr int EngineHouseCeiling = 32;

	// Vanilla AI slots. AISlots.Countries[8] @ 0xA8B29C, and the AI creation
	// loop is bounded by a pointer compare against its end address at 0x6882C5.
	static constexpr int EngineAISlots = 8;

	extern bool Enabled;   // set when -SPAWN is seen on the command line

	void Log(const char* format, ...);

	// Read-only: reports whether a cell object exists for `rawCell` and whether
	// its TERRAIN content has been filled in yet. See Instrumentation.cpp.
	void ProbeCellTerrain(const char* where, DWORD rawCell);

	// Applies every [MultiN_Alliances] pair from spawn.ini. Idempotent — MakeAlly
	// is additive, so calling it from more than one seam is safe.
	void ApplyAlliancesFromSpawnIni(const char* where);
}
