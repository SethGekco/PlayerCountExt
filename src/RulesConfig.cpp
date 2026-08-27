/**
*  PlayerCountExt — reading our settings out of the rules INI chain
*
*  WHY NOT JUST READ rulesmd.ini
*  -----------------------------
*  Opening the file directly (as SpawnConfig does for spawn.ini, where it is the
*  only option) would miss everything that makes rules *rules*: a mod's own
*  rules file, $Inherits, and the game-mode and map INIs that layer on top. The
*  engine has already resolved all of that by the time it hands the INI object
*  to RulesClass, so we read it there instead.
*
*  THE SEAM
*  --------
*  RulesClass::Read_File (0x668BF0) is the single entry EVERY rules-reading pass
*  goes through — the initial rulesmd.ini (three calls from RulesClass::Init),
*  then the game-mode INI, then the map INI. Its "Addition" name in the Ares and
*  Phobos sources is misleading: it is not only the additional files.
*
*  That ordering is exactly the precedence a modder expects, for free: a later
*  pass overwrites an earlier one, so map beats game-mode beats rulesmd. We only
*  store when the key is actually PRESENT in a pass, so a map that says nothing
*  leaves the mod's value standing rather than clobbering it with a default.
*
*  Antares, Phobos and TraitExt already hook this address. Same-address hooks
*  chain, so ours is a cooperative `return 0` that reads and gets out of the way
*  — it must never redirect, or it would cut the others out of the chain.
*
*  GPLv3.
*/

#include "PlayerCountExt.h"
#include "RulesConfig.h"

#include <Syringe.h>
#include <Helpers/Macro.h>

namespace
{
	// INIClass::ReadInteger — YRpp CCINIClass.h. A real address, not an R0 stub,
	// so calling it directly is safe.
	using ReadInteger_t = int (__thiscall*)(void*, const char*, const char*, int);
	constexpr DWORD AddrReadInteger = 0x5276D0;

	constexpr char SectionName[] = "PlayerCountExt";

	// Distinct from any plausible authored value, so "absent" and "zero" stay
	// separable.
	constexpr int Missing = INT_MIN;

	int CachedShiftDistance = Missing;

	int ReadInt(void* pINI, const char* key)
	{
		const auto fn = reinterpret_cast<ReadInteger_t>(AddrReadInteger);
		const int probe = fn(pINI, SectionName, key, Missing);

		// Guard against a parse that yields the sentinel by coincidence.
		return (probe == Missing) ? Missing : probe;
	}
}

int PlayerCountExt::RulesConfig::ShiftDistance()
{
	return CachedShiftDistance;
}

// ---------------------------------------------------------------------------
// RulesClass::Init — 0x6686C0. Start of a fresh rules load.
//
// Without this the cache would survive across games: load a map that sets
// ShiftDistance, then one that does not, and the first map's value would still
// be in effect. Init marks the beginning of a new load, before any Read_File
// pass, so clearing here means each game rebuilds the value from scratch.
//
// Stolen bytes 6, because the first instruction is `81 ec d0 00 00 00`
// (sub esp,0xd0) and a 5-byte hook would split it.
// ---------------------------------------------------------------------------
DEFINE_HOOK(0x6686C0, PlayerCountExt_RulesConfig_ResetOnInit, 0x6)
{
	CachedShiftDistance = Missing;
	return 0;
}

// ---------------------------------------------------------------------------
// RulesClass::Read_File — 0x668BF0. Fires for rulesmd.ini, the game-mode INI
// and the map INI, in that order.
//
// ECX = RulesClass*, [ESP+0x4] = CCINIClass* (the hook sits at function entry,
// so the return address is at [ESP+0x0] and the first argument at [ESP+0x4]).
//
// Cooperative: always returns 0 so Antares, Phobos and TraitExt keep their
// place in the chain and the vanilla body runs untouched.
// ---------------------------------------------------------------------------
DEFINE_HOOK(0x668BF0, PlayerCountExt_RulesConfig_Read, 0x5)
{
	void* pINI = R->Stack<void*>(0x4);
	if (!pINI)
		return 0;

	const int distance = ReadInt(pINI, "ShiftDistance");
	if (distance != Missing)
	{
		CachedShiftDistance = distance;
		PlayerCountExt::Log("[rules] ShiftDistance = %d\n", distance);
	}

	return 0;
}
