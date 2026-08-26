/**
*  PlayerCountExt — shared start positions with per-house offsets
*
*  THE IDEA
*  --------
*  A map declares N start positions. We want more houses than that, without
*  requiring new maps. So houses beyond the Nth reuse an existing start position
*  but spawn a fixed distance away from it in one of eight compass directions.
*
*  Start index encoding — this is a contract, and the client must agree with it
*  exactly or a player will spawn somewhere other than the slot they picked:
*
*      startIndex = ring * realCount + baseIndex
*
*      ring 0 -> ""    (the map's own start position, untouched)
*      ring 1 -> "N"   ring 2 -> "NE"  ring 3 -> "E"   ring 4 -> "SE"
*      ring 5 -> "S"   ring 6 -> "SW"  ring 7 -> "W"   ring 8 -> "NW"
*
*      label(i) = (i % realCount + 1) + suffix[i / realCount]
*
*  So on a 4-start map, index 6 is "3N" and index 11 is "4NE" — 36 selectable
*  starts from a 4-start map, comfortably past the ~30-house engine ceiling, so
*  the ceiling stays the binding limit rather than the map.
*
*  WHERE — and why it is not the obvious place
*  -------------------------------------------
*  TWO paths assign a house's base cell, and which one runs depends on whether
*  the player picked a start position or left it on Random:
*
*      0x5D6C21 -> SetBaseCell    explicit starts only  (returns to 0x5D6C2A)
*      0x5D6D3A -> SetBaseCell    EVERY house, later    (returns to 0x5D6D3F)
*
*  The second runs last and overwrites the first — 16 calls against 3 in a
*  traced game. So the shift is applied at 0x5D6D3F, after the engine has
*  finished assigning. 0x5D6C1D is hooked only to capture the engine's
*  start-cell table, which is a stack argument exposed nowhere else and is NOT
*  ScenarioClass::StartingPoints (those held (222,145)... while the cells
*  actually assigned were (111,178), (35,102), ...).
*
*  We do NOT write the house's start index (+0x16058). HouseIndices[start] =
*  house allows one house per start, so pointing two houses at the same index
*  makes the engine relocate one to a free position — which presents as "my
*  start selection was ignored". Overriding the resulting cell needs no
*  cooperation from the engine and cannot collide.
*
*  DETERMINISM
*  -----------
*  The offset is a pure function of the start index. No RNG, no time, no pointer
*  ordering — so every client computes an identical cell for identical input and
*  there is no desync surface at all. Deliberately not "random direction from the
*  synced seed", which would also work but has a failure mode this does not.
*
*  ⚠ NOT YET ADDRESSED:
*    - Terrain: the target cell is not validated, so a shifted house can land on
*      water, a cliff, or an occupied cell. Note the map header rectangle
*      (StartX/Width...) CANNOT be used for this — it is not cell space; a check
*      built on it rejected every valid spawn on the test map.
*    - The auto-ally pass at 0x5D74AF allies houses sharing +0x1605C, so houses
*      derived from the same base position may start allied.
*    - 0x007398D3 writes base cells again shortly after us, offset (-1,-1).
*      Harmless so far; unconfirmed what it is for.
*
*  GPLv3.
*/

#include "PlayerCountExt.h"
#include "SpawnConfig.h"

#include <Windows.h>
#include <climits>
#include <cstdio>
#include <cstdlib>

#include <Syringe.h>
#include <Helpers/Macro.h>

namespace
{
	// A CellStruct is two shorts packed into the DWORD the table holds.
	union PackedCell
	{
		DWORD Raw;
		struct { short X; short Y; } Cell;
	};

	// Ring 0 is the unshifted original; rings 1..8 are the compass directions
	// in the order the label suffixes imply.
	struct Offset { int dX; int dY; };

	constexpr Offset RingOffsets[] = {
		{  0,  0 }, // ring 0 — the map's own position
		{  0, -1 }, // N
		{  1, -1 }, // NE
		{  1,  0 }, // E
		{  1,  1 }, // SE
		{  0,  1 }, // S
		{ -1,  1 }, // SW
		{ -1,  0 }, // W
		{ -1, -1 }, // NW
	};

	constexpr int RingCount = static_cast<int>(sizeof(RingOffsets) / sizeof(RingOffsets[0]));

	// Cells between a shifted spawn and the position it derives from.
	//
	// 12 was the original suggestion; that is tight enough that two MCVs would
	// have heavily overlapping build radii and fight for room. 20 gives each
	// base somewhere to grow while keeping them recognisably "at the same
	// start". Worth revisiting once it can be seen in game.
	constexpr int ShiftDistance = 20; // built-in default; per-map overrides below

	// ── DIAGNOSTIC MODE ───────────────────────────────────────────────────
	// When false, this file OBSERVES but never writes: it still computes and
	// logs what it would do, leaving every engine field untouched, so a run
	// measures genuine vanilla behaviour.
	//
	// Keep this switch. Setting it false is what finally separated our own
	// crashes from a pre-existing one (ours died ~960 debug.log lines in,
	// the other after 850,000), and it is the fastest way to answer "is this
	// us?" without reasoning about it.
	constexpr bool ApplyShift = true;

	// Start positions the current map actually declares. Set from the engine's
	// NumberStartingPoints; until we know it, assume vanilla 8 so ring maths
	// never divides by zero.
	int RealStartCount = 8;

	// The engine's start-cell table, captured at 0x5D6C1D (see there).
	DWORD CachedCellTable = 0;

	// Base cells already taken this pass, so a second house on the same
	// position is bumped to the next ring instead of sharing a cell.
	// Reset once per AssignHouses pass (see the 0x688378 hook).
	constexpr int MaxClaims = 64;
	DWORD ClaimedCells[MaxClaims] {};
	int ClaimCount = 0;

	bool IsClaimed(DWORD raw)
	{
		for (int i = 0; i < ClaimCount; ++i)
			if (ClaimedCells[i] == raw)
				return true;
		return false;
	}

	void Claim(DWORD raw)
	{
		if (ClaimCount < MaxClaims)
			ClaimedCells[ClaimCount++] = raw;
	}

	// Which base position does this cell correspond to? -1 if it is not one of
	// the map's own start cells.
	int BaseIndexOfCell(DWORD raw)
	{
		if (!CachedCellTable)
			return -1;

		const auto table = reinterpret_cast<const DWORD*>(CachedCellTable);
		for (int i = 0; i < RealStartCount; ++i)
			if (table[i] == raw)
				return i;
		return -1;
	}

	constexpr int OffNumberStartingPoints = 0x113C;
	constexpr DWORD AddrScenarioPtr = 0xA8B230;

	void RefreshRealStartCount()
	{
		const auto pScen = *reinterpret_cast<DWORD const volatile*>(AddrScenarioPtr);
		if (!pScen)
			return;

		const int n = *reinterpret_cast<int const volatile*>(pScen + OffNumberStartingPoints);
		if (n > 0)
			RealStartCount = n;
	}

	// -----------------------------------------------------------------------
	// Per-map shift overrides.
	//
	// The map INI is the right home for this: it travels with the map, and
	// every client already holds an identical copy (the host distributes it),
	// so it inherits the same determinism guarantee as spawn.ini. A separate
	// side-file would not — one client could have it and another not, and they
	// would place the same house differently.
	//
	// Schema, in [PlayerCountExt] of the map INI. Most specific wins:
	//
	//   Shift.<base>.<dir>      = dX,dY   explicit cell offset, overrides all
	//   ShiftDistance.<base>.<dir> = N    distance for one spawn+direction
	//   ShiftDistance.<dir>     = N       distance for one direction
	//   ShiftDistance           = N       this map's default
	//   (built-in)                        DefaultShiftDistance
	//
	// <base> is the 1-based start position ("1".."8"), <dir> one of
	// N NE E SE S SW W NW. So a map author can write:
	//
	//   [PlayerCountExt]
	//   ShiftDistance=24            ; roomier than default everywhere
	//   ShiftDistance.1=30          ; spawn 1 has more space
	//   Shift.3.NE=+12,-40          ; spawn 3 NE is around a cliff
	//
	// Absent section = current behaviour, so existing maps are unaffected.
	// -----------------------------------------------------------------------
	constexpr const char* ShiftSection = "PlayerCountExt";
	constexpr const char* DirNames[] = { "", "N", "NE", "E", "SE", "S", "SW", "W", "NW" };

	// Path to the map INI, built from spawn.ini's [Settings] Scenario. Prefixed
	// ".\\" for the same reason SpawnConfig does — a bare name resolves against
	// the Windows directory, not the game folder.
	const char* MapIniPath()
	{
		static char path[96];
		const char* scenario = PlayerCountExt::SpawnConfig::Get().Scenario();

		if (!scenario || !scenario[0])
			return nullptr;

		std::snprintf(path, sizeof(path), ".\\%s", scenario);
		return path;
	}

	// Reads an int key, or INT_MIN when absent. Deliberately not
	// GetPrivateProfileInt — it parses unsigned and turns negatives into 0.
	int ReadMapInt(const char* key)
	{
		const char* path = MapIniPath();
		if (!path)
			return INT_MIN;

		char buf[64] = {};
		if (!GetPrivateProfileStringA(ShiftSection, key, "", buf, sizeof(buf), path) || !buf[0])
			return INT_MIN;

		char* end = nullptr;
		const long v = std::strtol(buf, &end, 10);
		return (end == buf) ? INT_MIN : static_cast<int>(v);
	}

	// Reads "dX,dY". Returns false when absent or malformed.
	bool ReadMapOffset(const char* key, int& dX, int& dY)
	{
		const char* path = MapIniPath();
		if (!path)
			return false;

		char buf[64] = {};
		if (!GetPrivateProfileStringA(ShiftSection, key, "", buf, sizeof(buf), path) || !buf[0])
			return false;

		char* end = nullptr;
		const long x = std::strtol(buf, &end, 10);
		if (end == buf || *end != ',')
			return false;

		char* end2 = nullptr;
		const long y = std::strtol(end + 1, &end2, 10);
		if (end2 == end + 1)
			return false;

		dX = static_cast<int>(x);
		dY = static_cast<int>(y);
		return true;
	}

	// Resolves the offset for one (base spawn, ring) pair through the cascade.
	// baseIndex is 0-based here; the INI keys are 1-based.
	void ResolveOffset(int baseIndex, int ring, int& dX, int& dY, const char*& source)
	{
		const char* dir = DirNames[ring];
		char key[64];

		// 1. explicit offset for this spawn + direction
		std::snprintf(key, sizeof(key), "Shift.%d.%s", baseIndex + 1, dir);
		if (ReadMapOffset(key, dX, dY))
		{
			source = "map Shift.<base>.<dir>";
			return;
		}

		// 2..4. distance overrides, most specific first
		int distance = INT_MIN;
		source = "built-in default";

		std::snprintf(key, sizeof(key), "ShiftDistance.%d.%s", baseIndex + 1, dir);
		distance = ReadMapInt(key);
		if (distance != INT_MIN) { source = "map ShiftDistance.<base>.<dir>"; }

		if (distance == INT_MIN)
		{
			std::snprintf(key, sizeof(key), "ShiftDistance.%s", dir);
			distance = ReadMapInt(key);
			if (distance != INT_MIN) source = "map ShiftDistance.<dir>";
		}

		if (distance == INT_MIN)
		{
			std::snprintf(key, sizeof(key), "ShiftDistance.%d", baseIndex + 1);
			distance = ReadMapInt(key);
			if (distance != INT_MIN) source = "map ShiftDistance.<base>";
		}

		if (distance == INT_MIN)
		{
			distance = ReadMapInt("ShiftDistance");
			if (distance != INT_MIN) source = "map ShiftDistance";
		}

		if (distance == INT_MIN)
			distance = ShiftDistance;

		dX = RingOffsets[ring].dX * distance;
		dY = RingOffsets[ring].dY * distance;
	}

	constexpr DWORD AddrHouseArrayItems = 0xA8022C;
	constexpr DWORD AddrHouseArrayCount = 0xA80238;

	// The start index the PLAYER chose, taken from spawn.ini rather than from
	// the house — see the note at the call site for why the house's own field
	// cannot be trusted at this point.
	//
	// Returns -1 when spawn.ini says nothing about this house, in which case
	// the caller leaves the engine's own answer alone.
	int StartIndexFromSpawnIni(DWORD pHouse)
	{
		const auto& spawn = PlayerCountExt::SpawnConfig::Get();
		if (!spawn.Loaded())
			return -1;

		const auto items = *reinterpret_cast<DWORD* const volatile*>(AddrHouseArrayItems);
		const int count = *reinterpret_cast<int const volatile*>(AddrHouseArrayCount);
		if (!items || count <= 0)
			return -1;

		for (int i = 0; i < count; ++i)
		{
			if (items[i] != pHouse)
				continue;

			// MultiN is 1-based over the house array.
			const auto& h = spawn.House(i + 1);
			return (h.Defined && h.SpawnLocation >= 0) ? h.SpawnLocation : -1;
		}

		return -1; // not a house from the multiplayer slots (Neutral/Special)
	}
}

// ---------------------------------------------------------------------------
// Start index -> cell — 0x5D6C1D.
//
// Stolen 7 bytes:
//     5d6c1d:  8b 44 24 0c    mov 0xc(%esp),%eax      (4)
//     5d6c21:  8b 14 b0       mov (%eax,%esi,4),%edx  (3)
// Returns to 0x5D6C24, the untouched `push %edx`.
//
// ESI = house->StartIndex (+0x16058), ECX = the house, 0xC(%esp) = cell table.
// ---------------------------------------------------------------------------
// THE REAL HOOK.
//
// ⚠ This was previously disabled as a "dead path" — wrongly. A run in which
// every player was on Random produced no log lines here, and that was
// generalised into "never reached". A base-cell trace later showed it firing
// via 0x5D6C2A for exactly the houses with an EXPLICIT start index, while the
// Random houses (-2) go through 0x5D6D3F instead:
//
//   [trace] home house@... start=7  cell=(178,111) <- caller 0x005D6C2A
//   [trace] home house@... start=0  cell=(111,178) <- caller 0x005D6C2A
//   [trace] home house@... start=-2 cell=(35,102)  <- caller 0x005D6D3F
//
// Explicitly-chosen starts are precisely the case this feature exists for, so
// this is the right place. The lesson: a hook that is silent under one lobby
// configuration is not thereby dead.
#define PLAYERCOUNTEXT_SPAWNSHIFT_DEAD_PATH 1
#if PLAYERCOUNTEXT_SPAWNSHIFT_DEAD_PATH

DEFINE_HOOK(0x5D6C1D, PlayerCountExt_SpawnShift_CacheTable, 0x7)
{
	GET(int, engineIndex, ESI);
	GET_STACK(DWORD, pTable, 0xC);

	// Stolen instruction. 0x5D6C2F reuses EAX, so this must always happen.
	R->EAX(pTable);

	// Cache the engine's authoritative start-cell table.
	//
	// This is the ONLY place it is exposed: it arrives as a stack argument and
	// is not ScenarioClass::StartingPoints (those held (222,145)... while the
	// real spawn cells on the same map are (111,178), (35,102), ...). We need
	// it later, at 0x5D6D3F, to compute "the cell of base position N".
	//
	// We deliberately do NOT modify anything here any more. Writing a shifted
	// cell at this point is pointless — 0x5D6D3F runs afterwards for every
	// house and overwrites it — and forcing ESI to the base index made two
	// houses claim the same start, which is what caused the engine to relocate
	// one of them to a free position.
	CachedCellTable = pTable;

	R->EDX(reinterpret_cast<const DWORD*>(pTable)[engineIndex < 0 ? 0 : engineIndex]);
	return 0x5D6C24;
}

#endif // PLAYERCOUNTEXT_SPAWNSHIFT_DEAD_PATH

// ---------------------------------------------------------------------------
// The live hook — 0x5D6D3F, immediately AFTER SetBaseCell.
//
//     5d6d25:  call *0xc4(%edx)          ; path A: virtual cell computation
//     5d6d30:  mov  (%ecx,%edx,4),%edx   ; path B: direct table lookup
//     5d6d38:  mov  %edi,%ecx            ; EDI = the house
//     5d6d3a:  call 0x50e000             ; SetBaseCell(cell)
//     5d6d3f:  mov  0xa80238,%eax        ; <-- we hook here (5 bytes)
//
// Post-correcting rather than intercepting is deliberate: two different cell
// sources converge on that one setter, so correcting AFTER it handles both with
// a single hook and no need to know which path ran. It also means we never have
// to reproduce the engine's own cell selection — we only adjust its answer.
//
// EDI = the house. The start index is read from the house itself (+0x16058),
// so we do not depend on any register the two paths might set differently.
// ---------------------------------------------------------------------------
DEFINE_HOOK(0x5D6D3F, PlayerCountExt_SpawnShift_AfterSetBaseCell, 0x5)
{
	GET(DWORD, pHouse, EDI);

	if (!pHouse || !CachedCellTable)
		return 0;

	RefreshRealStartCount();
	if (RealStartCount <= 0)
		return 0;

	const auto table = reinterpret_cast<const DWORD*>(CachedCellTable);
	const auto pHomeCell = reinterpret_cast<DWORD*>(pHouse + 0x5490);

	PackedCell had;
	had.Raw = *pHomeCell;

	// Where does this house want to be?
	//
	// If spawn.ini names a position, that is the player's explicit choice. If
	// not — which is the normal case for AI, and for every house past the
	// map's own count — fall back to whatever the engine assigned and work out
	// which base position that is.
	int startIndex = StartIndexFromSpawnIni(pHouse);

	if (startIndex < 0)
	{
		const int engineBase = BaseIndexOfCell(had.Raw);
		if (engineBase < 0)
			return 0;   // not on a start cell at all; leave it alone
		startIndex = engineBase;
	}

	int ring = startIndex / RealStartCount;
	const int base = startIndex % RealStartCount;

	// ── More houses than positions ──────────────────────────────────────
	// The engine allows one house per start, and with the deficiency search
	// suppressed a surplus house simply gets no position — it spawns nothing
	// and is out immediately, which presents as "the extra player never
	// showed up".
	//
	// So if this base cell is already taken, bump the house to the next ring
	// of the SAME base until a free (base, ring) is found. Deterministic:
	// houses are processed in array order and the search is a plain ascending
	// scan, so every client reaches the same answer.
	PackedCell target;
	int dX = 0, dY = 0;
	const char* source = "built-in default";
	bool bumped = false;

	while (true)
	{
		if (ring == 0)
		{
			target.Raw = table[base];
			dX = dY = 0;
		}
		else if (ring < RingCount)
		{
			target.Raw = table[base];
			ResolveOffset(base, ring, dX, dY, source);
			target.Cell.X = static_cast<short>(target.Cell.X + dX);
			target.Cell.Y = static_cast<short>(target.Cell.Y + dY);
		}
		else
		{
			PlayerCountExt::Log("[shift] house@0x%08X — no free ring left for base %d; leaving engine placement\n",
				pHouse, base + 1);
			return 0;
		}

		if (!IsClaimed(target.Raw))
			break;

		++ring;
		bumped = true;
	}

	Claim(target.Raw);

	if (ring == 0)
	{
		// Unshifted and unclaimed — the engine's own answer stands.
		return 0;
	}

	PlayerCountExt::Log("[shift] house@0x%08X -> \"%d%s\"%s — engine had (%d,%d); base %d (%d,%d) + (%+d,%+d) = (%d,%d)  [%s]%s\n",
		pHouse, base + 1, DirNames[ring],
		bumped ? " (bumped: base was taken)" : "",
		had.Cell.X, had.Cell.Y, base + 1,
		PackedCell{ table[base] }.Cell.X, PackedCell{ table[base] }.Cell.Y,
		dX, dY, target.Cell.X, target.Cell.Y, source,
		ApplyShift ? "" : "  (diagnostic, not applied)");

	if (!ApplyShift)
		return 0;

	*pHomeCell = target.Raw;

	// Keep +0x5494 in step when it holds a real cell rather than the
	// invalid-cell sentinel, so both getters (0x50DEF0 / 0x50DF30) agree.
	const auto pCurCell = reinterpret_cast<DWORD*>(pHouse + 0x5494);
	PackedCell cur;
	cur.Raw = *pCurCell;
	const short sentX = *reinterpret_cast<short const volatile*>(0xA8EF98);
	const short sentY = *reinterpret_cast<short const volatile*>(0xA8EF9A);
	if (!(cur.Cell.X == sentX && cur.Cell.Y == sentY))
		*pCurCell = target.Raw;

	return 0;
}

DEFINE_HOOK(0x688378, PlayerCountExt_SpawnShift_RestoreStartIndex, 0x5)
{
	// One pass per AssignHouses call, and this runs before the 0x5D6D3F loop,
	// so it is the right place to clear the claimed-cell table.
	ClaimCount = 0;

	auto& spawn = PlayerCountExt::SpawnConfig::Get();
	if (!spawn.Loaded())
		return 0;

	const auto pArrayItems = *reinterpret_cast<DWORD* const volatile*>(0xA8022C);
	const int count = *reinterpret_cast<int const volatile*>(0xA80238);

	if (!pArrayItems || count <= 0)
		return 0;

	for (int i = 0; i < count; ++i)
	{
		const DWORD pHouse = pArrayItems[i];
		if (!pHouse)
			continue;

		const auto& h = spawn.House(i + 1); // MultiN is 1-based
		if (!h.Defined || h.SpawnLocation < 0)
			continue;

		// ⚠ Write the BASE index, never the shifted one.
		//
		// ScenarioClass::StartingPoints has exactly 8 entries and HouseIndices
		// follows immediately after it, so a start index of 8+ is an
		// out-of-bounds read for every consumer in the engine and in other
		// DLLs. An earlier revision wrote spawn.ini's raw value (e.g. 8) here
		// and the game died ~960 log lines into the scenario, at a wild jump to
		// unmapped memory — against 22k-655k lines for every unrelated crash in
		// the same period.
		//
		// The shift is carried entirely by the base CELL (see the hook at
		// 0x5D6D3F). Nothing outside this DLL needs to know a house was
		// shifted, and nothing outside this DLL can cope with being told.
		RefreshRealStartCount();
		if (RealStartCount <= 0)
			continue;

		const int baseIndex = h.SpawnLocation % RealStartCount;

		const auto pStart = reinterpret_cast<int*>(pHouse + 0x16058);
		const int current = *pStart;

		if (current == baseIndex)
			continue;

		PlayerCountExt::Log("[shift] %shouse[%d] start index %d -> %d (spawn.ini Multi%d = %d, base of ring %d)\n",
			ApplyShift ? "" : "(diagnostic, not applied) ",
			i, current, baseIndex, i + 1, h.SpawnLocation, h.SpawnLocation / RealStartCount);

		// Deliberately NOT written any more.
		//
		// Forcing +0x16058 to the base made two houses claim the same start
		// position, and the engine responded by relocating one of them to a
		// free spot — the "I filled an available slot" symptom. The engine is
		// now left to assign whatever it likes; we override the resulting CELL
		// at 0x5D6D3F instead, which needs no cooperation from it.
		(void)baseIndex;
	}

	return 0;
}

// ---------------------------------------------------------------------------
// Suppress the engine's "start waypoint deficiency" search — 0x688508.
//
// With more houses than the map has start positions, the engine logs
//
//     Multiplayer start waypoint deficiency - looking for more start positions
//
// and then tries to INVENT the missing positions by scanning the map
// (0x6885B5 -> MapClass at 0x56DC20). In a 9-house game on an 8-start map that
// search does not come back: sampling the process showed the main thread
// spinning in 0x56E838 / 0x57854E with every other thread idle, and the game
// sitting on a black screen indefinitely.
//
// We do not need it. A shifted start ("1N") already supplies a real, distinct
// cell for the extra house, so the deficiency the engine is trying to solve has
// already been solved by the time it looks.
//
//     688502:  jle  0x68864d      ; vanilla "no deficiency" exit
//     688508:  push $0x83dcd4     ; <- we hook here, 5 bytes
//     ...
//     68864d:  xor  %eax,%eax     ; normal continuation
//
// Returning 0x68864D takes exactly the branch vanilla takes when no houses are
// short, which is why it is safe: it is not a novel path.
//
// ⚠ GATED. This only fires when spawn.ini actually uses a shifted start. If a
// map is genuinely short of positions and nothing is supplying them, the search
// is the correct behaviour and is left alone — suppressing it unconditionally
// would trade a hang for houses stacked on one cell.
// ---------------------------------------------------------------------------
DEFINE_HOOK(0x688508, PlayerCountExt_SpawnShift_SuppressDeficiencySearch, 0x5)
{
	auto& spawn = PlayerCountExt::SpawnConfig::Get();
	if (!spawn.Loaded())
		return 0;

	RefreshRealStartCount();
	if (RealStartCount <= 0)
		return 0;

	// Is any house using a start position beyond what the map declares?
	bool usingShiftedStarts = false;
	for (int n = 1; n <= spawn.HighestDefined(); ++n)
	{
		const auto& h = spawn.House(n);
		if (h.Defined && h.SpawnLocation >= RealStartCount)
		{
			usingShiftedStarts = true;
			break;
		}
	}

	if (!usingShiftedStarts)
		return 0; // vanilla behaviour — let the engine search

	PlayerCountExt::Log("[shift] suppressing the engine's start-position search "
		"(shifted starts already supply the extra positions; the search does not terminate)\n");

	return 0x68864D;
}
