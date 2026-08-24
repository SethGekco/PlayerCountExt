/**
*  PlayerCountExt — standalone Yuri's Revenge DLL for more than 8 houses
*
*  The vanilla "8 player" limit is not one constant. It is a scattered set of
*  fixed-size arrays and loop bounds, sitting under a single real ceiling: the
*  32-bit per-house bitfield. Every player AND every computer is a HouseClass,
*  so the limit is on HOUSES, not on "players".
*
*  Drop the DLL in the game folder; Syringe loads it automatically alongside
*  Antares / Phobos / the stock CnCNet spawner, exactly like Phobos.
*
*  Config comes from spawn.ini — the host-authoritative file the CnCNet client
*  writes and broadcasts. A client-local config file would desync a networked
*  game the moment two clients disagreed on the house count. See DESIGN.md.
*
*  Uses YRpp (Phobos-developers) + Syringe. GPLv3.
*/

#include "PlayerCountExt.h"

#include <Syringe.h>
#include <YRPPCore.h>
#include <Helpers/Macro.h>
#include <Unsorted.h>

#include <cstdio>
#include <cstdarg>

bool PlayerCountExt::Enabled = false;

// ---------------------------------------------------------------------------
// No host (declhost) declaration is needed: Syringe identifies the target
// from the hook addresses themselves, exactly like the CnCNet spawner and
// Phobos, which also omit it. This keeps the DLL loadable across the
// Antares / CnCNet / Steam gamemd variants.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Minimal logger -> playercountext.log next to the game exe. Independent of the
// spawner's logger so this DLL has zero link-time dependency on it.
//
// Flushes every line on purpose: this DLL's whole job at present is producing
// evidence about a crashy subsystem, and an unflushed buffer loses exactly the
// last line you needed.
// ---------------------------------------------------------------------------
void PlayerCountExt::Log(const char* format, ...)
{
	static FILE* pLog = nullptr;
	if (!pLog)
		fopen_s(&pLog, "playercountext.log", "w");

	if (!pLog)
		return;

	va_list args;
	va_start(args, format);
	vfprintf(pLog, format, args);
	va_end(args);
	fflush(pLog);
}

bool __stdcall DllMain(HANDLE hInstance, DWORD dwReason, LPVOID /*reserved*/)
{
	if (dwReason == DLL_PROCESS_ATTACH)
		DisableThreadLibraryCalls(reinterpret_cast<HMODULE>(hInstance));

	return TRUE;
}

// ---------------------------------------------------------------------------
// Command-line gate — 0x52F639, named YR_CmdLineParse in the Antares PDB
// symbol map. Clean 5-byte boundary (xor ebx,ebx + cmp $0x1,%edi); the
// preceding `mov %ecx,%edi` / `mov %edx,%esi` confirm ESI = argv, EDI = argc.
// ---------------------------------------------------------------------------
DEFINE_HOOK(0x52F639, PlayerCountExt_ParseCommandLine, 0x5)
{
	GET(char**, ppArgs, ESI);
	GET(int, nNumArgs, EDI);

	for (int i = 1; i < nNumArgs; ++i)
	{
		if (_stricmp(ppArgs[i], "-SPAWN") == 0)
			PlayerCountExt::Enabled = true;
	}

	if (PlayerCountExt::Enabled)
		PlayerCountExt::Log("[PlayerCountExt] Armed (spawn detected).\n");

	return 0; // continue to original code
}
