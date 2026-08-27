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
	static constexpr int EngineHouseCeiling = 30;

	// Vanilla AI slots. AISlots.Countries[8] @ 0xA8B29C, and the AI creation
	// loop is bounded by a pointer compare against its end address at 0x6882C5.
	static constexpr int EngineAISlots = 8;

	extern bool Enabled;   // set when -SPAWN is seen on the command line

	void Log(const char* format, ...);

	// Read-only: reports whether a cell object exists for `rawCell` and whether
	// its TERRAIN content has been filled in yet. See Instrumentation.cpp.
	void ProbeCellTerrain(const char* where, DWORD rawCell);
}
