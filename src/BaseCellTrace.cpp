/**
*  PlayerCountExt — trace every write to a house's base cell
*
*  WHY
*  ---
*  Two static guesses at "where a house's start position becomes a map cell"
*  have now both been wrong:
*
*    0x5D6C1D  never executed at all (zero log lines in a live game)
*    0x5D6D3F  executes, but too early — every house reports start index 0 and
*              cell (0,0), matching the pass-1 state where StartingPoints is
*              still empty, and it only covers 4 of 10 houses
*
*  Reading more disassembly to produce a third guess has poor odds. Instead,
*  trace the destination: hook the two instructions that actually store a base
*  cell and log the caller's return address. Whatever writes the real
*  coordinates has to come through here, so one run names the site outright
*  rather than inferring it.
*
*  THE THREE SETTERS (all tiny, all leaf functions)
*  ------------------------------------------------
*    0x50DFE0  mov 0x4(%esp),%eax ; mov %eax,0x5494(%ecx)   set current cell
*    0x50DFF0  mov 0xa8ef98,%eax  ; mov %eax,0x5494(%ecx)   invalidate current
*    0x50E000  mov 0x4(%esp),%eax ; mov %eax,0x5490(%ecx)   set home cell
*
*  We hook the STORE instruction of the two that set a real value (6 bytes each,
*  a clean boundary — the entry itself is only 4 bytes and a 5-byte hook there
*  would split the following instruction). At the store: ECX = house,
*  EAX = cell, and [ESP] = the caller's return address, because these are leaf
*  functions with nothing pushed.
*
*  This is pure diagnostics: each hook performs the store it replaced and
*  returns to the following instruction, so behaviour is unchanged.
*
*  GPLv3.
*/

#include "PlayerCountExt.h"

#include <Syringe.h>
#include <Helpers/Macro.h>

namespace
{
	union TraceCell
	{
		DWORD Raw;
		struct { short X; short Y; } Cell;
	};

	// Keep the log readable: a base cell can be written many times over a game
	// (rebuilds, base relocation). We only care about scenario setup, and an
	// unbounded trace would bury it.
	constexpr int MaxTraceLines = 60;
	int TraceLines = 0;

	bool TraceBudget()
	{
		if (TraceLines >= MaxTraceLines)
			return false;

		++TraceLines;
		if (TraceLines == MaxTraceLines)
			PlayerCountExt::Log("[trace] ---- trace budget reached, further writes not logged ----\n");

		return true;
	}

	void Report(const char* what, DWORD pHouse, DWORD raw, DWORD caller)
	{
		if (!TraceBudget())
			return;

		TraceCell c;
		c.Raw = raw;

		// The house's own start index, so the log ties a cell to a start slot.
		const int startIndex = pHouse
			? *reinterpret_cast<int const volatile*>(pHouse + 0x16058)
			: -999;

		PlayerCountExt::Log("[trace] %s house@0x%08X start=%d cell=(%d,%d) <- caller 0x%08X\n",
			what, pHouse, startIndex, c.Cell.X, c.Cell.Y, caller);
	}
}

// ---------------------------------------------------------------------------
// Set CURRENT cell (+0x5494) — the store at 0x50DFE4.
//     50dfe4:  89 81 94 54 00 00    mov %eax,0x5494(%ecx)     (6 bytes)
// ---------------------------------------------------------------------------
DEFINE_HOOK(0x50DFE4, PlayerCountExt_Trace_SetCurrentCell, 0x6)
{
	GET(DWORD, pHouse, ECX);
	GET(DWORD, raw, EAX);
	GET_STACK(DWORD, caller, 0x0);

	Report("cur ", pHouse, raw, caller);

	// Perform the store we replaced.
	if (pHouse)
		*reinterpret_cast<DWORD*>(pHouse + 0x5494) = raw;

	return 0x50DFEA;
}

// ---------------------------------------------------------------------------
// Set HOME cell (+0x5490) — the store at 0x50E004.
//     50e004:  89 81 90 54 00 00    mov %eax,0x5490(%ecx)     (6 bytes)
// ---------------------------------------------------------------------------
DEFINE_HOOK(0x50E004, PlayerCountExt_Trace_SetHomeCell, 0x6)
{
	GET(DWORD, pHouse, ECX);
	GET(DWORD, raw, EAX);
	GET_STACK(DWORD, caller, 0x0);

	Report("home", pHouse, raw, caller);

	if (pHouse)
		*reinterpret_cast<DWORD*>(pHouse + 0x5490) = raw;

	return 0x50E00A;
}
