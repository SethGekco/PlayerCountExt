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
*    - Base FOOTPRINT is not checked. A cell can be usable while the space
*      around it is not, so two shifted spawns 20 cells apart may still fight
*      for building room. IsWithinUsableArea answers "can something stand here",
*      not "is there room for a base".
*    - The auto-ally pass at 0x5D74AF allies houses sharing +0x1605C, so houses
*      derived from the same base position may start allied.
*    - 0x007398D3 writes base cells again shortly after us, offset (-1,-1).
*      Harmless so far; unconfirmed what it is for.
*
*  GPLv3.
*/

#include "PlayerCountExt.h"
#include "SpawnConfig.h"
#include "RulesConfig.h"

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
	//
	// ── DIRECTIONS ARE CELL-SPACE, DELIBERATELY ──────────────────────────
	// N is -Y in cell coordinates, not "up the screen". RA2's view is
	// isometric, so cell-north renders towards the top-RIGHT and every label
	// here is rotated ~45 degrees from what a player literally sees.
	//
	// This is intentional and should not be "fixed". The engine's own compass
	// is cell-space — Facing=, waypoint maths, and every other direction a
	// modder already works with — so matching it means existing intuition
	// transfers. A screen-space compass would read more naturally to a new
	// player while disagreeing with everything else in the game.
	//
	// Consequence worth knowing when reading bug reports: someone describing a
	// shifted spawn as "it went south-east" may be describing cell-west. Judge
	// the geometry (does the set of 8 form a ring around the base?) rather than
	// any single direction name.
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
	// Fallback when nothing else supplies a distance. Kept only as the ceiling
	// for the scaled default below — see DefaultShiftDistance().
	constexpr int ShiftDistance = 20; // per-map and rules overrides below

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

	// Enemy-aware seating. Kept switchable because it earned its keep once:
	// a deterministic Fatal Error was wrongly pinned on this change and three
	// fixes were aimed at it before the switch showed the crash persisted with
	// it off. The cause turned out to be a co-loaded DLL rebuilt the same
	// evening (MirageTreesExt), not anything here.
	//
	// false: sweep rings in order, take the first free usable slot.
	// true:  score every ring and prefer bases with fewer seated enemies.
	constexpr bool PreferQuietBases = true;

	// Start positions the current map actually declares. Set from the engine's
	// NumberStartingPoints; until we know it, assume vanilla 8 so ring maths
	// never divides by zero.
	int RealStartCount = 8;

	// The engine's start-cell table, captured at 0x5D6C1D (see there).
	DWORD CachedCellTable = 0;

	// A COPY of the start-cell table, not a pointer to it.
	//
	// The authoritative table arrives as a stack argument at 0x5D6C1D, so a
	// pointer to it is dead the moment that frame unwinds. 0x5D6C1D only fires
	// for houses already seated in HouseIndices, which happens in pass 1 and not
	// in pass 2 — so pass 2 was left reading a different structure that merely
	// looked cell-shaped, giving six bases only three distinct cells (1/3/5 and
	// 2/4 aliased). Houses then "spread" across base numbers that pointed at the
	// same ground.
	//
	// Copying the values instead of the pointer keeps the good table for the
	// whole game.
	constexpr int MaxStartCells = 16;
	DWORD StartCells[MaxStartCells] = {};
	int StartCellCount = 0;

	// The table to use: the trusted copy when we have one, else whatever the
	// later hook managed to find.
	const DWORD* ActiveCellTable()
	{
		if (StartCellCount > 0)
			return StartCells;

		return reinterpret_cast<const DWORD*>(CachedCellTable);
	}

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

	int ClaimedBy[MaxClaims] = {};

	void Claim(DWORD raw, int houseIndex = -1)
	{
		if (ClaimCount < MaxClaims)
		{
			ClaimedBy[ClaimCount] = houseIndex;
			ClaimedCells[ClaimCount++] = raw;
		}
	}

	// Squared distance between two cells. Squared is enough for comparisons and
	// avoids a sqrt; cells are small enough that this cannot overflow.
	int CellDistanceSq(DWORD a, DWORD b)
	{
		PackedCell ca; ca.Raw = a;
		PackedCell cb; cb.Raw = b;

		const int dx = ca.Cell.X - cb.Cell.X;
		const int dy = ca.Cell.Y - cb.Cell.Y;
		return dx * dx + dy * dy;
	}



	// ── Who is already sitting on each base, and who is friendly ─────────
	//
	// With more houses than start positions some houses MUST share a base. Which
	// ones is entirely our choice, and putting a player's enemy 16 cells away
	// while a quieter base is free is the worst of the available options.
	//
	// Team membership is read from spawn.ini rather than the engine's Allies
	// bitfield: it is the same source AllianceFix applies from, and it avoids
	// depending on a HouseClass field offset (YRpp's cell offsets have been
	// wrong repeatedly on this build, so its house offsets are not trusted
	// either).
	constexpr int MaxHouses = 32;
	constexpr int MaxBases = 64;

	DWORD AllyMask[MaxHouses] = {};
	DWORD BaseOccupants[MaxBases] = {};
	bool AllyMaskBuilt = false;

	void BuildAllyMask()
	{
		for (int i = 0; i < MaxHouses; ++i)
			AllyMask[i] = 0;

		static const char* const Suffixes[] =
		{
			"One", "Two", "Three", "Four", "Five", "Six",
			"Seven", "Eight", "Nine", "Ten", "Eleven", "Twelve",
			"Thirteen", "Fourteen", "Fifteen", "Sixteen", "Seventeen", "Eighteen",
			"Nineteen", "Twenty", "TwentyOne", "TwentyTwo", "TwentyThree", "TwentyFour",
			"TwentyFive", "TwentySix", "TwentySeven", "TwentyEight", "TwentyNine"
		};

		for (int i = 0; i < MaxHouses; ++i)
		{
			char section[64];
			std::snprintf(section, sizeof(section), "Multi%d_Alliances", i + 1);

			for (int slot = 0; slot < static_cast<int>(sizeof(Suffixes) / sizeof(Suffixes[0])); ++slot)
			{
				char key[64];
				std::snprintf(key, sizeof(key), "HouseAlly%s", Suffixes[slot]);

				const int v = GetPrivateProfileIntA(section, key, -1, ".\\spawn.ini");
				if (v >= 0 && v < MaxHouses)
				{
					AllyMask[i] |= (1u << v);
					AllyMask[v] |= (1u << i);   // symmetric, regardless of how it was written
				}
			}
		}

		AllyMaskBuilt = true;
	}

	// How many houses already on `base` are NOT allied to `houseIndex`.
	int EnemiesAtBase(int base, int houseIndex)
	{
		if (base < 0 || base >= MaxBases || houseIndex < 0 || houseIndex >= MaxHouses)
			return 0;

		if (!AllyMaskBuilt)
			BuildAllyMask();

		int enemies = 0;
		DWORD occupants = BaseOccupants[base];
		for (int other = 0; occupants; ++other, occupants >>= 1)
		{
			if (!(occupants & 1) || other == houseIndex)
				continue;

			if (!(AllyMask[houseIndex] & (1u << other)))
				++enemies;
		}

		return enemies;
	}

	void OccupyBase(int base, int houseIndex)
	{
		if (base >= 0 && base < MaxBases && houseIndex >= 0 && houseIndex < MaxHouses)
			BaseOccupants[base] |= (1u << houseIndex);
	}

	// How crowded a base already is, regardless of who is on it.
	//
	// Avoiding enemies is not enough on its own: in a 2v14 nearly every base is
	// enemy-free for the AI, so scoring purely on enemies put SEVEN of them on
	// one position while another had one. Crowding is the tiebreaker that
	// spreads allies out.
	int HousesAtBase(int base)
	{
		if (base < 0 || base >= MaxBases)
			return 0;

		int n = 0;
		for (DWORD bits = BaseOccupants[base]; bits; bits >>= 1)
			n += static_cast<int>(bits & 1);

		return n;
	}

	// Distance from `raw` to the nearest already-seated ally and enemy.
	//
	// Used to place unassigned houses sensibly: teammates should end up near one
	// another and rivals as far apart as the map allows. This only ever chooses
	// BETWEEN otherwise-equal free slots — it can never leave a position empty
	// or move anyone off a chosen one, which is the higher rule.
	void NearestTeamDistances(DWORD raw, int houseIndex, int& allyDistSq, int& enemyDistSq)
	{
		allyDistSq = INT_MAX;
		enemyDistSq = INT_MAX;

		if (!AllyMaskBuilt)
			BuildAllyMask();

		for (int i = 0; i < ClaimCount; ++i)
		{
			const int other = ClaimedBy[i];
			if (other < 0 || other == houseIndex || other >= MaxHouses)
				continue;

			const int d = CellDistanceSq(raw, ClaimedCells[i]);
			const bool allied = houseIndex >= 0 && houseIndex < MaxHouses
				&& (AllyMask[houseIndex] & (1u << other)) != 0;

			int& slot = allied ? allyDistSq : enemyDistSq;
			if (d < slot)
				slot = d;
		}
	}


	// Can a house actually stand on this cell?
	//
	// MapClass::IsWithinUsableArea covers both "is this cell on the map at all"
	// and the level/terrain checks, which is what we need: an earlier attempt
	// used the map-header rectangle and rejected every valid spawn, because that
	// rectangle is not cell space. This is the engine's own answer to the
	// question, so it cannot disagree with the engine.
	//
	// YRpp declares it as IsWithinUsableArea(const CellStruct&, bool), i.e. the
	// cell arrives BY REFERENCE — hence a pointer here, not a packed DWORD.
	constexpr DWORD AddrMapClass = 0x87F7E8;
	using IsWithinUsableArea_t = bool (__thiscall*)(void*, const void*, bool);

	bool CellIsUsable(DWORD raw)
	{
		const auto fn = reinterpret_cast<IsWithinUsableArea_t>(0x578460);
		return fn(reinterpret_cast<void*>(AddrMapClass), &raw, true);
	}

	// Which base position does this cell correspond to? -1 if it is not one of
	// the map's own start cells.
	int BaseIndexOfCell(DWORD raw)
	{
		if (!CachedCellTable)
			return -1;

		const auto table = ActiveCellTable();
		for (int i = 0; i < RealStartCount; ++i)
			if (table[i] == raw)
				return i;
		return -1;
	}

	constexpr int OffNumberStartingPoints = 0x113C;
	constexpr DWORD AddrScenarioPtr = 0xA8B230;

	// Returns whether the scenario actually supplied a count THIS call.
	//
	// It must, because the count is the divisor that decodes a start index into
	// (base, ring) and it has to match the one the client encoded with. When the
	// scenario is not populated yet the old behaviour was to silently keep the
	// previous value — which meant the stale default of 8 — and a shifted index
	// then decoded to the wrong ring: 52 is 5NW at a count of 6 (52/6 = ring 8)
	// but 5SW at a count of 8 (52/8 = ring 6). Same base by coincidence, wrong
	// direction, and a spawn cell written from a base table that is itself not
	// yet final. Callers that place houses must treat false as "not now".
	bool RefreshRealStartCount()
	{
		const auto pScen = *reinterpret_cast<DWORD const volatile*>(AddrScenarioPtr);
		if (!pScen)
			return false;

		const int n = *reinterpret_cast<int const volatile*>(pScen + OffNumberStartingPoints);
		if (n <= 0)
			return false;

		RealStartCount = n;
		return true;
	}

	// -----------------------------------------------------------------------
	// The built-in shift distance, scaled to the map.
	//
	// A fixed 20 cells is only sensible on a large map. On the 80x80 two-player
	// map used for the 9-house test it is a QUARTER of the map per hop, which
	// is how a surplus house ended up marooned on an offshore island: the
	// candidate cell passed the "is this usable ground" test but was nowhere
	// near the spawn it was derived from.
	//
	// Scaling by the smaller dimension keeps the hop proportionate. The floor
	// of 8 is roughly the clearance a construction yard plus its immediate
	// buildings wants, below which shifted spawns would overlap the one they
	// came from; the ceiling is the old 20, so large maps behave exactly as
	// before and nothing regresses.
	//
	// This is only a better DEFAULT, not a correctness fix — a shorter hop
	// makes a marooned spawn less likely but cannot rule it out. Reachability
	// is what actually rules it out, and is handled separately.
	// -----------------------------------------------------------------------
	constexpr int MinShiftDistance = 8;

	int DefaultShiftDistance()
	{
		const int W = *reinterpret_cast<int const volatile*>(AddrMapClass + 0xF4);
		const int H = *reinterpret_cast<int const volatile*>(AddrMapClass + 0xF8);

		const int smaller = (W < H) ? W : H;
		if (smaller <= 0)
			return ShiftDistance; // map not sized yet — fall back to the old constant

		int d = smaller / 6;
		if (d < MinShiftDistance) d = MinShiftDistance;
		if (d > ShiftDistance)    d = ShiftDistance;
		return d;
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
		{
			distance = PlayerCountExt::RulesConfig::ShiftDistance();
			if (distance != INT_MIN) source = "rules ShiftDistance";
		}

		if (distance == INT_MIN)
		{
			distance = DefaultShiftDistance();
			source = "built-in, scaled to map size";
		}

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
	// ── Is there room to actually BUILD here? ───────────────────────────
	//
	// IsWithinUsableArea answers "can something stand on this cell", which is
	// not the same question as "can a construction yard go here". A spawn on a
	// cliff ledge passes the first test and fails the second: the MCV lands,
	// cannot deploy, and cannot drive off the ledge either.
	//
	// The cheap, reliable proxy is FLATNESS. A building needs a contiguous
	// patch at one height; cliffs, ramps and shorelines all show up as a change
	// in cell level. Checking a block around the candidate rejects ledges
	// without needing the pathfinding zones (which do not exist this early) or
	// a full flood fill.
	//
	// Level is at CellClass +0x11B — read straight out of the engine's own
	// IsClearToMove, which does `movsx ecx, BYTE PTR [esi+0x11b]` at 0x4834EF.
	// It is signed.
	// A construction yard is 3x3, so that is what the patch must guarantee.
	//
	// This was 2 (a 5x5 patch, "footprint plus margin") and it was far too
	// strict: it demanded 25 cells at identical level, which almost no ring
	// position on a real map satisfies. Measured over one 16-player game it
	// rejected 179 candidates - every rejection in the run - and left six of
	// sixteen houses with no valid slot at all, falling through to the
	// unguarded sweep. One of those six landed on a cliff, which is the very
	// thing the check exists to prevent.
	//
	// Asking for margin made spawns WORSE, because a house that fails the test
	// everywhere gets no protection at all rather than slightly less.
	constexpr int BuildRoomRadius = 1;

	DWORD CellPointerAt(int x, int y)
	{
		const DWORD arrayBase = *reinterpret_cast<DWORD const volatile*>(AddrMapClass + 0x13C);
		const DWORD bound = *reinterpret_cast<DWORD const volatile*>(AddrMapClass + 0x140);
		if (!arrayBase || !bound || x < 0 || y < 0)
			return 0;

		// The row stride is NOT reliably 512 — MapSizeExt rescales it — so
		// derive it from the bound rather than assuming.
		int shift = 0;
		while ((1u << (2 * (shift + 1))) <= bound && shift < 15)
			++shift;

		const DWORD index = (static_cast<DWORD>(y) << shift) + static_cast<DWORD>(x);
		if (index >= bound)
			return 0;

		return *reinterpret_cast<DWORD const volatile*>(arrayBase + index * 4);
	}

	// CellClass::CanThisExistHere(SpeedType, BuildingTypeClass*, HouseClass*)
	// — YRpp CellClass.h, verified as a real function at 0x47C620 rather than a
	// stub. With a null type and owner this asks the plain terrain question:
	// could a ground vehicle of this SpeedType occupy this cell.
	using CanThisExistHere_t = bool (__thiscall*)(void*, int, void*, void*);
	constexpr DWORD AddrCanThisExistHere = 0x47C620;
	constexpr int SpeedTypeTrack = 1;

	bool CellIsDrivable(int x, int y)
	{
		const DWORD pCell = CellPointerAt(x, y);
		if (!pCell)
			return false;

		const auto fn = reinterpret_cast<CanThisExistHere_t>(AddrCanThisExistHere);
		return fn(reinterpret_cast<void*>(pCell), SpeedTypeTrack, nullptr, nullptr);
	}

	// A spawn needs somewhere to build AND somewhere to go.
	//
	// Flatness alone was not enough: it rejects cliff FACES but happily accepts
	// the flat top of a plateau, which is how a player ended up unable to
	// deploy and unable to drive off. What actually distinguishes a usable
	// spawn is how much connected drivable ground it touches — a ledge, a
	// pillar top and a tiny island all fail that, whatever their flatness.
	//
	// So: a flat immediate footprint for the construction yard, plus a bounded
	// flood fill proving the cell opens onto real space. The fill stops as soon
	// as it has seen enough, so the cost is capped regardless of map size.
	constexpr int MinConnectedCells = 160;

	// Reason codes so the log can distinguish which half of the test bound.
	// Tuning either threshold blind is how you end up with six houses failing
	// open and no idea whether the footprint or the connectivity did it.
	enum class RoomVerdict { Ok, NoCellData, Footprint, TooEnclosed };

	int LastConnectedCells = 0;

	RoomVerdict EvaluateBuildingRoom(DWORD raw)
	{
		PackedCell c;
		c.Raw = raw;

		const DWORD centre = CellPointerAt(c.Cell.X, c.Cell.Y);
		if (!centre)
			return RoomVerdict::NoCellData; // pass 1 — fail OPEN

		// 1. The construction yard footprint must be flat and drivable.
		const int level = static_cast<signed char>(
			*reinterpret_cast<BYTE const volatile*>(centre + 0x11B));

		for (int dy = -BuildRoomRadius; dy <= BuildRoomRadius; ++dy)
		{
			for (int dx = -BuildRoomRadius; dx <= BuildRoomRadius; ++dx)
			{
				const DWORD pCell = CellPointerAt(c.Cell.X + dx, c.Cell.Y + dy);
				if (!pCell)
					return RoomVerdict::Footprint;

				const int otherLevel = static_cast<signed char>(
					*reinterpret_cast<BYTE const volatile*>(pCell + 0x11B));

				if (otherLevel != level || !CellIsDrivable(c.Cell.X + dx, c.Cell.Y + dy))
					return RoomVerdict::Footprint;
			}
		}

		// 2. It must open onto enough connected drivable ground that a player
		//    is not marooned on a ledge or a pillar.
		DWORD seen[MinConnectedCells];
		int seenCount = 0, head = 0;

		seen[seenCount++] = raw;

		while (head < seenCount && seenCount < MinConnectedCells)
		{
			PackedCell cur;
			cur.Raw = seen[head++];

			for (int dy = -1; dy <= 1 && seenCount < MinConnectedCells; ++dy)
			{
				for (int dx = -1; dx <= 1 && seenCount < MinConnectedCells; ++dx)
				{
					if (!dx && !dy)
						continue;

					PackedCell next;
					next.Cell.X = static_cast<short>(cur.Cell.X + dx);
					next.Cell.Y = static_cast<short>(cur.Cell.Y + dy);

					bool already = false;
					for (int i = 0; i < seenCount && !already; ++i)
						already = (seen[i] == next.Raw);

					if (already || !CellIsDrivable(next.Cell.X, next.Cell.Y))
						continue;

					seen[seenCount++] = next.Raw;
				}
			}
		}

		LastConnectedCells = seenCount;
		return (seenCount >= MinConnectedCells) ? RoomVerdict::Ok : RoomVerdict::TooEnclosed;
	}

	bool CellHasBuildingRoom(DWORD raw)
	{
		const RoomVerdict v = EvaluateBuildingRoom(raw);
		return v == RoomVerdict::Ok || v == RoomVerdict::NoCellData;
	}

	// ── Two houses on one start position ────────────────────────────────
	//
	// Picking the same slot as someone else used to be refused at the lobby.
	// It is now allowed: the first house keeps the exact slot and any others
	// are moved to a free compass variant OF THE SAME BASE, so "we both picked
	// 4" puts you both around position 4 rather than scattering one of you
	// across the map.
	//
	// Which variant is chosen is randomised, but it MUST be the same randomness
	// on every machine or clients place the same house differently and the game
	// desyncs. So it is derived from spawn.ini's Seed - host-authoritative and
	// broadcast to everyone - mixed with the base and the house's own index.
	// Same inputs, same answer, everywhere; different game, different layout.
	//
	// Deliberately NOT ScenarioClass::Random: consuming from the game's RNG
	// during setup shifts every later draw, which is a desync of its own if any
	// other DLL draws a different number of times.
	unsigned int MixSeed(unsigned int seed, int base, int houseIndex)
	{
		// FNV-1a over the three inputs; cheap and well-spread.
		unsigned int h = 2166136261u;
		const unsigned int parts[3] =
		{
			seed,
			static_cast<unsigned int>(base),
			static_cast<unsigned int>(houseIndex)
		};

		for (int p = 0; p < 3; ++p)
		{
			for (int b = 0; b < 4; ++b)
			{
				h ^= (parts[p] >> (b * 8)) & 0xFFu;
				h *= 16777619u;
			}
		}

		return h;
	}

	// Fills `order` with rings 1..RingCount-1 in a seed-dependent order.
	// Ring 0 is excluded: it is the exact slot, and the caller has already
	// found it taken.
	void ShuffledRings(int order[], unsigned int seed, int base, int houseIndex)
	{
		const int count = RingCount - 1;
		for (int i = 0; i < count; ++i)
			order[i] = i + 1;

		// Fisher-Yates driven by a deterministic LCG, so the permutation is a
		// pure function of the seed inputs.
		unsigned int state = MixSeed(seed, base, houseIndex) | 1u;
		for (int i = count - 1; i > 0; --i)
		{
			state = state * 1664525u + 1013904223u;
			const int j = static_cast<int>((state >> 16) % static_cast<unsigned int>(i + 1));

			const int tmp = order[i];
			order[i] = order[j];
			order[j] = tmp;
		}
	}

	// Which contender keeps the exact slot when several ask for the same one.
	//
	// Previously this was decided by house order, so the first house in
	// HouseClass::Array always won — which means the human always beat an AI and
	// a host always beat their ally. Nobody chose that; it was just a
	// consequence of the iteration order.
	//
	// The winner is now drawn from the shared seed, so it is genuinely random
	// between the contenders while still being identical on every client.
	// Everyone else falls through to the seeded compass shuffle as before.
	int KeeperForSlot(int base, int ring)
	{
		const auto& spawn = PlayerCountExt::SpawnConfig::Get();
		if (!spawn.Loaded() || RealStartCount <= 0)
			return -1;

		const int count = *reinterpret_cast<int const volatile*>(AddrHouseArrayCount);

		auto wantsThisSlot = [&](int houseIndex) -> bool
		{
			const auto& h = spawn.House(houseIndex + 1);
			if (!h.Defined || h.SpawnLocation < 0)
				return false;

			return (h.SpawnLocation % RealStartCount) == base
				&& (h.SpawnLocation / RealStartCount) == ring;
		};

		int contenders = 0;
		for (int i = 0; i < count; ++i)
			if (wantsThisSlot(i))
				++contenders;

		if (contenders == 0)
			return -1;

		// One asker keeps it outright; no need to consult the seed.
		if (contenders == 1)
		{
			for (int i = 0; i < count; ++i)
				if (wantsThisSlot(i))
					return i;
		}

		const unsigned int seed = static_cast<unsigned int>(spawn.Seed());
		const int pick = static_cast<int>(
			MixSeed(seed, base * RingCount + ring, 0) % static_cast<unsigned int>(contenders));

		int k = 0;
		for (int i = 0; i < count; ++i)
		{
			if (!wantsThisSlot(i))
				continue;

			if (k == pick)
				return i;

			++k;
		}

		return -1;
	}


	// Position of a house in HouseClass::Array. This is also its Multi number
	// minus one, which is what the alliance sections are keyed by.
	int HouseArrayIndex(DWORD pHouse)
	{
		const auto items = *reinterpret_cast<DWORD* const volatile*>(AddrHouseArrayItems);
		const int count = *reinterpret_cast<int const volatile*>(AddrHouseArrayCount);
		if (!items)
			return -1;

		for (int i = 0; i < count; ++i)
			if (items[i] == pHouse)
				return i;

		return -1;
	}

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

	// Copy it while the frame is still live. This is the only place the real
	// table is exposed, and only in the pass where houses are seated.
	if (pTable >= 0x10000 && pTable < 0x80000000 && !(pTable & 3))
	{
		const auto entries = reinterpret_cast<const DWORD*>(pTable);
		const int wanted = (RealStartCount > 0 && RealStartCount <= MaxStartCells)
			? RealStartCount : 8;

		int distinct = 0;
		for (int i = 0; i < wanted; ++i)
		{
			PackedCell e; e.Raw = entries[i];
			if (e.Cell.X <= 0 || e.Cell.Y <= 0 || e.Cell.X > 1024 || e.Cell.Y > 1024)
			{
				distinct = 0;
				break;
			}

			bool repeat = false;
			for (int j = 0; j < i && !repeat; ++j)
				repeat = (entries[j] == entries[i]);

			if (!repeat)
				++distinct;
		}

		// Only replace a copy we already trust with a better one.
		if (distinct > StartCellCount)
		{
			for (int i = 0; i < wanted; ++i)
				StartCells[i] = entries[i];

			StartCellCount = distinct;
			PlayerCountExt::Log("[shift] copied start-cell table from 0x5D6C1D: "
				"%d of %d entries distinct\n", distinct, wanted);
		}
	}

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

	if (!pHouse || !ActiveCellTable())
		return 0;

	// Skip Neutral and Special.
	//
	// HouseClass::Array holds them alongside the real players — an 11-house
	// array is 9 players plus these two — and they have no start position by
	// design. Treating them as houses needing a spawn had them consume slots
	// from the search and compete with the actual surplus player for space,
	// which is why the 9th player still failed to appear even after the search
	// was added.
	//
	// The test is the engine's own: HouseTypeClass + 0x1A6, which vanilla checks
	// at 0x5D74C9 for exactly this purpose before pairing houses in the
	// auto-ally pass.
	const auto pType = *reinterpret_cast<DWORD const volatile*>(pHouse + 0x34);
	if (!pType || *reinterpret_cast<BYTE const volatile*>(pType + 0x1A6))
		return 0;

	const bool countIsReal = RefreshRealStartCount();
	if (RealStartCount <= 0)
		return 0;

	const auto table = ActiveCellTable();
	const auto pHomeCell = reinterpret_cast<DWORD*>(pHouse + 0x5490);

	PackedCell had;
	had.Raw = *pHomeCell;

	// AssignHouses runs twice. The first pass precedes both the terrain load and
	// the scenario's start-position count, so anything placed then is computed
	// from a stale divisor and a non-final cell table. Do not write during it.
	if (!countIsReal)
	{
		static bool warned = false;
		if (!warned)
		{
			warned = true;
			PlayerCountExt::Log("[shift] scenario start count not available yet; leaving placement "
				"to the later pass (decoding now would use the stale default of %d)\n",
				RealStartCount);
		}

		return 0;
	}

	// A table from a previous invocation points at reused stack. Refuse rather
	// than compute offsets from whatever is there now.
	if (!ActiveCellTable())
	{
		static bool warned = false;
		if (!warned)
		{
			warned = true;
			PlayerCountExt::Log("[shift] no start-cell table captured this pass; leaving engine "
				"placement (a table from an earlier pass is dead stack)\n");
		}

		return 0;
	}

	// One probe per pass: is there terrain under the cells at this point? The
	// reachability check can only live here if there is. Read-only.
	if (ClaimCount == 0)
	{
		// Dump the captured start-cell table verbatim.
		//
		// A run showed six bases resolving to only three distinct cells (bases
		// 1/3/5 all reading the same one), which is impossible for a real map
		// and makes every base-relative decision meaningless. StartingPoints
		// held six distinct entries at the same moment, so the fault is in what
		// we capture at [ESP+0x28], not in the scenario. Print it rather than
		// reason about it.
		{
			const auto dump = ActiveCellTable();
			char line[256]; int n = 0;
			n += std::snprintf(line + n, sizeof(line) - n, "[shift] table in use (%s):", StartCellCount > 0 ? "copied from 0x5D6C1D" : "found on the stack");
			for (int i = 0; i < RealStartCount && i < 12 && n < 200; ++i)
			{
				PackedCell e; e.Raw = dump[i];
				n += std::snprintf(line + n, sizeof(line) - n, " [%d]=(%d,%d)", i, e.Cell.X, e.Cell.Y);
			}
			PlayerCountExt::Log("%s  (RealStartCount=%d)\n", line, RealStartCount);
		}

		PlayerCountExt::ProbeCellTerrain("at spawn assignment (0x5D6D3F)", had.Raw);

		// NOTE: alliances are deliberately NOT applied here. This hook runs
		// inside the engine's per-house scenario loop, and making 184 MakeAlly
		// calls mid-iteration crashed deterministically. They are applied at
		// the AssignHouses exit instead, which is between loops rather than
		// inside one.
	}

	// Where does this house want to be?
	//
	// If spawn.ini names a position, that is the player's explicit choice. If
	// not — which is the normal case for AI, and for every house past the
	// map's own count — fall back to whatever the engine assigned and work out
	// which base position that is.
	bool hadNoStart = false;
	bool explicitChoice = false;

	int startIndex = StartIndexFromSpawnIni(pHouse);

	if (startIndex < 0)
	{
		const int engineBase = BaseIndexOfCell(had.Raw);

		// engineBase < 0 means the engine gave this house nothing recognisable —
		// which is exactly what happens to the surplus house once the engine's
		// own start-position search is suppressed. Previously we left it alone,
		// so it spawned nothing and was out before the player saw it: the
		// "there is no 9th player" symptom. Start it at base 0 and let the
		// search below find the first free, usable slot for it.
		startIndex = (engineBase < 0) ? 0 : engineBase;
		hadNoStart = (engineBase < 0);

		if (hadNoStart)
			PlayerCountExt::Log("[shift] house@0x%08X had no start position (engine cell (%d,%d)); "
				"searching for a free one\n", pHouse, had.Cell.X, had.Cell.Y);
	}
	else
	{
		explicitChoice = true;
	}

	int ring = startIndex / RealStartCount;
	const int base = startIndex % RealStartCount;
	int base_out = base;

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
	bool found = false;

	// Ring of this house's own base that it lost a keeper draw for, or -1.
	// The general sweep must not hand it back.
	int lostDrawFor = -1;

	// An EXPLICIT pick is honoured at its own base: if the offset cell is off
	// the map, pull it back along the SAME direction until it is usable, rather
	// than relocating the house to a different base.
	//
	// Relocating was both surprising ("I chose 1S and started across the map")
	// and exploitable — a player could pick a deliberately off-map angle to get
	// moved somewhere better. Clamping toward the edge keeps the choice honest:
	// the worst case is you start nearer your base than you asked.
	if (explicitChoice && ring > 0 && ring < RingCount)
	{
		int fullX = 0, fullY = 0;
		const char* csrc = "built-in default";
		ResolveOffset(base, ring, fullX, fullY, csrc);

		const int unitX = RingOffsets[ring].dX;
		const int unitY = RingOffsets[ring].dY;
		int distance = fullX / (unitX ? unitX : 1);
		if (!unitX) distance = fullY / (unitY ? unitY : 1);
		if (distance < 0) distance = -distance;

		for (int d = distance; d >= 1 && !found; --d)
		{
			PackedCell candidate;
			candidate.Raw = table[base];
			candidate.Cell.X = static_cast<short>(candidate.Cell.X + unitX * d);
			candidate.Cell.Y = static_cast<short>(candidate.Cell.Y + unitY * d);

			if (IsClaimed(candidate.Raw) || !CellIsUsable(candidate.Raw))
				continue;

			target = candidate;
			dX = unitX * d; dY = unitY * d; source = csrc;
			bumped = (d != distance);
			base_out = base;
			found = true;

			if (bumped)
				PlayerCountExt::Log("[shift]   \"%d%s\" clamped to %d cells (map edge)\n",
					base + 1, DirNames[ring], d);
		}
	}

	// The exact slot was taken — another house picked it first, or the whole
	// direction was unusable. Stay on the SAME base and take a free compass
	// variant of it, chosen deterministically from the shared seed.
	//
	// This is what makes "we both picked 4" work: one player keeps 4 and the
	// other lands on some 4X, near where they asked to be, instead of being
	// exiled to a different base. Which variant they get varies per game but is
	// identical on every client.
	//
	// Ring 0 is included when the house asked for a shifted slot, so a duplicate
	// "4NE" can still fall back to plain "4" if that happens to be free.
	if (explicitChoice && !found)
	{
		const unsigned int seed = static_cast<unsigned int>(PlayerCountExt::SpawnConfig::Get().Seed());
		const int selfIndex = HouseArrayIndex(pHouse);

		const int requestedRing = ring;

		int order[RingCount];
		ShuffledRings(order, seed, base, selfIndex);

		// The REQUESTED ring first, then the shuffled others, then ring 0.
		//
		// Trying the request first is not optional. The clamp path above only
		// runs for ring > 0, so a plain "I want position 4" arrived here with
		// found == false having never been tested — and this loop skipped the
		// requested ring as "already established as unavailable". The result
		// was that every explicit ring-0 pick was displaced to a compass
		// variant and its real position left free for someone else to take,
		// which is precisely backwards.
		// Only the drawn keeper may take the exact slot. Everyone else who asked
		// for it goes straight to the shuffle, so which contender keeps it is
		// decided by the seed rather than by house order.
		const bool isKeeper = (KeeperForSlot(base, requestedRing) == selfIndex);

		if (!isKeeper)
			lostDrawFor = requestedRing;

		if (!isKeeper)
			PlayerCountExt::Log("[shift]   \"%d%s\" drawn by another contender; "
				"taking a compass variant of base %d instead\n",
				base + 1, DirNames[requestedRing], base + 1);

		// Two sweeps of THIS base: good ground first, then any free slot.
		//
		// A chosen start is a promise. Buildability is a preference for picking
		// WHICH variant of that position you get — it must never be able to move
		// you to a different position, because a player who selected 4 and was
		// sent to 5 has simply had their choice discarded.
		//
		// This bit: all eight variants of base 4 failed the filter (that corner
		// of the map is rough), the base was declared "full" though nothing was
		// occupying it, and the loser of the draw was exiled to base 5.
		for (int strict = 0; strict < 2 && !found; ++strict)
		{
		const bool preferGoodGround = (strict == 0);

		for (int attempt = isKeeper ? -1 : 0; attempt <= RingCount - 1 && !found; ++attempt)
		{
			const int tryRing = (attempt < 0)
				? requestedRing
				: ((attempt < RingCount - 1) ? order[attempt] : 0);

			if (attempt >= 0 && tryRing == requestedRing)
				continue; // the keeper already had first refusal

			PackedCell candidate;
			candidate.Raw = table[base];
			int cdX = 0, cdY = 0;
			const char* csrc = "built-in default";

			if (tryRing > 0)
			{
				ResolveOffset(base, tryRing, cdX, cdY, csrc);
				candidate.Cell.X = static_cast<short>(candidate.Cell.X + cdX);
				candidate.Cell.Y = static_cast<short>(candidate.Cell.Y + cdY);
			}

			if (IsClaimed(candidate.Raw) || !CellIsUsable(candidate.Raw))
				continue;

			// Only on the first sweep. On the second, any free slot on this
			// base beats being moved off the position entirely.
			if (preferGoodGround && tryRing > 0 && !CellHasBuildingRoom(candidate.Raw))
				continue;

			target = candidate;
			dX = cdX; dY = cdY; source = csrc;
			bumped = true;
			base_out = base;
			ring = tryRing;
			found = true;

			if (tryRing != requestedRing)
				PlayerCountExt::Log("[shift]   \"%d%s\" was taken; sharing base %d as \"%d%s\" "
					"(seeded from spawn.ini Seed=%u, identical on every client)\n",
					base + 1, DirNames[requestedRing], base + 1,
					base + 1, DirNames[tryRing], seed);
		}

		} // strict

		// Only now is the base genuinely full — every one of its nine slots is
		// occupied by someone else. That is the "more players than places" case,
		// where a random position and a shared cell are both acceptable.
		if (!found)
			PlayerCountExt::Log("[shift]   base %d has all 9 slots occupied; "
				"falling back to the general search\n", base + 1);
	}

	// Otherwise (or if even distance 1 was unusable): try later rings of this
	// base, then every other base. Only reached for houses with no choice of
	// their own, or when the chosen direction is entirely unusable.
	// RING-MAJOR, deliberately: every real start position is offered before any
	// shifted one.
	//
	// Shifted positions are an OVERFLOW mechanism, not part of the normal pool.
	// A base-major search (all rings of base 1, then base 2...) could hand a
	// player a shifted slot on base 1 while base 2's real spawn sat empty,
	// which is strictly worse for that player and for map balance. Sweeping
	// ring 0 across every base first means shifted slots only ever appear once
	// the map genuinely has more houses than positions.
	// Ring 0 first, then the quietest overflow slot anywhere.
	//
	// Two rules are in play and they need different orderings:
	//
	//   1. Real start positions are used before any shifted one. Non-negotiable
	//      — a compass variant is overflow, not part of the normal pool.
	//   2. Among OVERFLOW slots, being away from enemies matters more than
	//      being on a low ring. All shifted slots are second-class anyway, so
	//      "further out but clear" beats "close in but next to a rival".
	//
	// Scoring purely ring-major satisfied 1 and broke 2: with 16 houses on 6
	// bases, ring 1 holds exactly 6 slots, so the last house placed took
	// whatever remained — which on a 2v14 was the seat beside the two-player
	// team, even though quieter bases had free slots one ring further out.
	//
	// So: ring 0 is swept on its own, and if nothing is free there, every
	// (base, ring >= 1) pair is scored together and ordered by enemies, then
	// ring, then base. Ties break on scan order, so this stays deterministic.
	const int selfIndex = HouseArrayIndex(pHouse);
	const bool clusterTeams = PlayerCountExt::RulesConfig::ClusterTeams();

	struct Seat { int base, ring, dX, dY, enemies; const char* src; PackedCell cell; };
	Seat best{}; bool haveBest = false;
	int bestAllyD = INT_MAX, bestEnemyD = INT_MAX;

	// Two sweeps. The first demands room to build; if every slot on the map
	// fails that (a genuinely cramped map, or terrain we are reading wrongly),
	// the second drops the requirement rather than leaving the house unplaced.
	//
	// Fail-open is deliberate: a bad spawn is playable, no spawn is not.
	for (int strictness = 0; strictness < 2 && !found; ++strictness)
	{
	const bool requireBuildRoom = (strictness == 0);

	// `found` means the explicit-pick path above already seated this house at
	// the position the player asked for. Do not second-guess it — dropping this
	// guard silently discarded explicit picks and reseated those players
	// elsewhere.
	for (int tryRing = 0; tryRing < RingCount && !found; ++tryRing)
	{
		for (int attempt = 0; attempt < RealStartCount; ++attempt)
		{
			const int tryBase = (base + attempt) % RealStartCount;

			PackedCell candidate;
			candidate.Raw = table[tryBase];
			int cdX = 0, cdY = 0;
			const char* csrc = "built-in default";

			if (tryRing > 0)
			{
				ResolveOffset(tryBase, tryRing, cdX, cdY, csrc);
				candidate.Cell.X = static_cast<short>(candidate.Cell.X + cdX);
				candidate.Cell.Y = static_cast<short>(candidate.Cell.Y + cdY);
			}

			if (IsClaimed(candidate.Raw))
				continue;

			// Losing the draw has to stick. Otherwise a contender rejected from
			// the disputed slot walks straight back into it here, and the house
			// that actually won the draw finds it occupied.
			if (lostDrawFor >= 0 && tryBase == base && tryRing == lostDrawFor)
				continue;

			if (!CellIsUsable(candidate.Raw))
			{
				if (tryRing > 0)
					PlayerCountExt::Log("[shift]   skip \"%d%s\" (%d,%d) — not usable terrain\n",
						tryBase + 1, DirNames[tryRing], candidate.Cell.X, candidate.Cell.Y);
				continue;
			}

			if (!PreferQuietBases)
			{
				// Known-good path: first free usable slot wins outright.
				best = Seat{ tryBase, tryRing, cdX, cdY, 0, csrc, candidate };
				haveBest = true;
				break;
			}

			// A cell that cannot host a base is worse than a further one that
			// can — a player who cannot deploy is out of the game. On the
			// first sweep these are skipped entirely.
			// Ring 0 is the MAP'S OWN start position, not something we invented.
			// The author shipped a working base there, so if our test rejects it
			// the test is wrong, not the map. Validate only the shifted slots we
			// made up.
			//
			// Rejecting real positions was actively harmful: it displaced players
			// off spawns they had explicitly chosen and pushed everyone onto
			// whichever base still had valid shifted cells - eight of sixteen
			// houses onto base 6 in one game.
			if (requireBuildRoom && tryRing > 0)
			{
				const RoomVerdict verdict = EvaluateBuildingRoom(candidate.Raw);
				if (verdict != RoomVerdict::Ok && verdict != RoomVerdict::NoCellData)
				{
					PlayerCountExt::Log("[shift]   skip \"%d%s\" (%d,%d) — %s\n",
						tryBase + 1, DirNames[tryRing], candidate.Cell.X, candidate.Cell.Y,
						verdict == RoomVerdict::Footprint
							? "footprint not flat/drivable"
							: "too enclosed to build in");
					if (verdict == RoomVerdict::TooEnclosed)
						PlayerCountExt::Log("[shift]     (only %d connected drivable cells, want %d)\n",
							LastConnectedCells, MinConnectedCells);
					continue;
				}
			}

			const Seat seat{ tryBase, tryRing, cdX, cdY,
				EnemiesAtBase(tryBase, selfIndex), csrc, candidate };

			// Ring 0 is taken on availability alone, lowest enemies as tiebreak,
			// because rule 1 outranks everything.
			if (tryRing == 0)
			{
				// No early exit on enemies == 0: with a large allied team most
				// bases score zero, and taking the first one stacked everybody
				// onto it. Crowding decides between equals.
				const int crowding = HousesAtBase(tryBase);
				const int bestCrowding = haveBest ? HousesAtBase(best.base) : 0;

				int allyD = INT_MAX, enemyD = INT_MAX;
				NearestTeamDistances(candidate.Raw, selfIndex, allyD, enemyD);

				bool better = !haveBest;
				if (!better && seat.enemies != best.enemies)
					better = seat.enemies < best.enemies;
				else if (clusterTeams)
				{
					if (!better && allyD != bestAllyD)
						better = allyD < bestAllyD;
					else if (!better && enemyD != bestEnemyD)
						better = enemyD > bestEnemyD;
					else if (!better)
						better = crowding < bestCrowding;
				}
				else
				{
					if (!better && crowding != bestCrowding)
						better = crowding < bestCrowding;
					else if (!better && enemyD != bestEnemyD)
						better = enemyD > bestEnemyD;
					else if (!better && allyD != bestAllyD)
						better = allyD < bestAllyD;
				}

				if (better)
				{
					best = seat;
					bestAllyD = allyD;
					bestEnemyD = enemyD;
					haveBest = true;
				}

				continue;
			}

			// Overflow ordering: enemies at the base, then crowding, then team
			// geography, then ring.
			//
			// Geography is the tiebreaker that puts teammates together and
			// rivals apart: among slots that are otherwise equally good, prefer
			// the one furthest from the nearest enemy, and then closest to the
			// nearest ally. It only ever chooses BETWEEN free slots, so it can
			// never leave a position empty or override a chosen one.
			const int crowding = HousesAtBase(tryBase);
			const int bestCrowding = haveBest ? HousesAtBase(best.base) : 0;

			int allyD = INT_MAX, enemyD = INT_MAX;
			NearestTeamDistances(candidate.Raw, selfIndex, allyD, enemyD);

			// STANDARD spreads, TEAM clusters. The only difference is whether
			// crowding (which pushes houses onto fresh bases) or ally distance
			// (which pulls them together) is consulted first.
			bool better = !haveBest;
			if (!better && seat.enemies != best.enemies)
				better = seat.enemies < best.enemies;
			else if (clusterTeams)
			{
				if (!better && allyD != bestAllyD)
					better = allyD < bestAllyD;        // sit with your team
				else if (!better && enemyD != bestEnemyD)
					better = enemyD > bestEnemyD;
				else if (!better && seat.ring != best.ring)
					better = seat.ring < best.ring;
				else if (!better)
					better = crowding < bestCrowding;
			}
			else
			{
				if (!better && crowding != bestCrowding)
					better = crowding < bestCrowding;  // use every position
				else if (!better && enemyD != bestEnemyD)
					better = enemyD > bestEnemyD;
				else if (!better && allyD != bestAllyD)
					better = allyD < bestAllyD;
				else if (!better)
					better = seat.ring < best.ring;
			}

			if (better)
			{
				best = seat;
				bestAllyD = allyD;
				bestEnemyD = enemyD;
				haveBest = true;
			}
		}

		// With scoring off, any seat ends the search (the old behaviour). With it
		// on, only a ring-0 seat does, so a clear slot further out can still win.
		// In STANDARD a free ring-0 seat ends the search: every real position is
		// used before any variant. TEAM relaxes exactly that rule, so keep
		// scoring further rings and let a slot beside an ally beat an untouched
		// position across the map.
		if (haveBest && (!PreferQuietBases || (best.ring == 0 && !clusterTeams)))
			break;
	}

	if (haveBest && !found)
	{
		target = best.cell;
		dX = best.dX; dY = best.dY; source = best.src;
		bumped = (best.base != base) || (best.ring != ring);
		base_out = best.base;
		ring = best.ring;
		found = true;

		if (best.enemies > 0)
			PlayerCountExt::Log("[shift]   every free slot is contested; \"%d%s\" has %d enemy neighbour(s)\n",
				best.base + 1, DirNames[best.ring], best.enemies);
	}

	if (!found && strictness == 0)
		PlayerCountExt::Log("[shift]   nowhere with room to build; retrying without that requirement\n");
	} // strictness

	if (!found)
	{
		PlayerCountExt::Log("[shift] house@0x%08X — no usable free position anywhere; leaving engine placement\n",
			pHouse);
		return 0;
	}

	Claim(target.Raw, selfIndex);
	OccupyBase(base_out, selfIndex);

	if (ring == 0 && !bumped && !hadNoStart)
	{
		// The engine's own answer was already fine — claim it and leave it be.
		//
		// hadNoStart must be excluded here. A house the engine gave nothing
		// lands on base 0 ring 0 with nothing "changed", and returning early
		// left it holding its original garbage cell — it then spawned nothing,
		// which is precisely why the 9th player kept not appearing.
		//
		// This is the path an honoured pick takes when the engine already had
		// it right. The seat came out of the search above, which applies the
		// build-room filter, so it has been validated — but log if it somehow
		// has not, because a silent cliff spawn is exactly what took two rounds
		// to find last time.
		if (!CellHasBuildingRoom(target.Raw))
			PlayerCountExt::Log("[shift] house@0x%08X — WARNING: seated at (%d,%d) which reports "
				"no room to build; the filter let it through\n",
				pHouse, target.Cell.X, target.Cell.Y);

		// Claim it. The comment above always said we did; the call was missing.
		//
		// Without this the seat is invisible to every later house's collision
		// check, so two houses can be handed the same cell — and a contender who
		// lost the keeper draw could take the disputed position through this
		// path without being recorded as holding it, leaving the actual winner
		// to find it "taken" and get displaced to another base entirely.
		Claim(target.Raw, selfIndex);
		OccupyBase(base_out, selfIndex);

		return 0;
	}

	PlayerCountExt::Log("[shift] house@0x%08X -> \"%d%s\"%s — engine had (%d,%d); base %d (%d,%d) + (%+d,%+d) = (%d,%d)  [%s]%s\n",
		pHouse, base_out + 1, DirNames[ring],
		bumped ? " (relocated)" : "",
		had.Cell.X, had.Cell.Y, base_out + 1,
		PackedCell{ table[base_out] }.Cell.X, PackedCell{ table[base_out] }.Cell.Y,
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

	// New pass, new stack frame: the cached table pointer is no longer valid.
	CachedCellTable = 0;   // the pointer dies with the frame; StartCells does not

	// Seating is decided fresh each pass, so who sits where must be too.
	AllyMaskBuilt = false;
	for (int i = 0; i < MaxBases; ++i)
		BaseOccupants[i] = 0;

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
// ---------------------------------------------------------------------------
// Do we have more houses wanting a start position than the map provides?
//
// Neutral and Special sit in HouseClass::Array but need no position by design,
// so counting them would overstate the shortfall — same HouseTypeClass + 0x1A6
// test the shift itself uses.
// ---------------------------------------------------------------------------
static int CountPlayingHouses()
{
	const auto pArrayItems = *reinterpret_cast<DWORD* const volatile*>(AddrHouseArrayItems);
	const int arrayCount = *reinterpret_cast<int const volatile*>(AddrHouseArrayCount);
	if (!pArrayItems)
		return 0;

	int playing = 0;
	for (int i = 0; i < arrayCount; ++i)
	{
		const DWORD pHouse = pArrayItems[i];
		if (!pHouse)
			continue;

		const auto pType = *reinterpret_cast<DWORD const volatile*>(pHouse + 0x34);
		if (pType && !*reinterpret_cast<BYTE const volatile*>(pType + 0x1A6))
			++playing;
	}

	return playing;
}

static bool HasStartShortfall(int& playing)
{
	RefreshRealStartCount();
	if (RealStartCount <= 0)
		return false;

	playing = CountPlayingHouses();
	return playing > RealStartCount;
}

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
// ⚠ GATED, on house count vs position count.
//
// An earlier gate asked whether spawn.ini NAMED a shifted start ("1N"), which
// was the wrong question and hung the game. With every player on Random every
// Multi carries start=-1, so no explicit shifted start exists, the gate stayed
// shut, and the engine ran its search anyway — 9 houses on a 2-position map,
// spinning at 100% forever. Suppression is needed MOST in the case that gate
// excluded.
//
// The right question is whether the shift will supply the missing positions,
// and it does so whenever there are more playing houses than start positions,
// no matter how those houses were assigned. That is also exactly the condition
// under which the engine reaches this code at all (0x688502 jle skips it
// otherwise), so the two agree by construction.
//
// If the shift is compiled out we leave the search alone: it would then be the
// only thing supplying positions, and a hang beats no spawns at all.
// ---------------------------------------------------------------------------
DEFINE_HOOK(0x688508, PlayerCountExt_SpawnShift_SuppressDeficiencySearch, 0x5)
{
	if (!ApplyShift)
		return 0; // the shift is compiled out, so nothing else supplies positions

	int playing = 0;
	if (!HasStartShortfall(playing))
		return 0; // no real shortfall — vanilla behaviour

	PlayerCountExt::Log("[shift] suppressing the engine's start-position search: "
		"%d playing houses vs %d start positions, and the shift supplies the rest "
		"(the search does not terminate)\n", playing, RealStartCount);

	return 0x68864D;
}

// ---------------------------------------------------------------------------
// Bypass the engine's free-start-position picker — 0x5D6CFB.
//
// The per-house loop at 0x5D6CBF..0x5D6D47 turns a house into a base cell. It
// first scans the map's 16-entry HouseIndices table for this house:
//
//     5d6cf9:  test bl,bl
//     5d6cfb:  jne  0x5d6d30        ; seated: cell = startCellTable[edx]
//     5d6cfd:  add  ebp,0x24        ; NOT seated -> ask the engine for a position
//     ...
//     5d6d17:  mov  edx,[ecx]       ; ecx from [esp+0x1c]
//     5d6d25:  call [edx+0xc4]
//     5d6d30:  mov  ecx,[esp+0x28]  ; the seated path
//     5d6d34:  mov  edx,[ecx+edx*4]
//     5d6d38:  mov  ecx,edi         ; edi = the house
//     5d6d3a:  call 0x50e000        ; SetBaseCell
//
// That picker draws from a pool of unused start positions. Neutral and Special
// are never seated, so vanilla calls it twice in every game and it copes. It
// does NOT cope with being drained: on a 2-position map with 9 houses it
// returned garbage (49,0) for the first surplus house and then read a stale
// pointer for the next — C0000005 at 0x5D6D17, ecx = 0x01A44850, ESI = 3.
//
// We do not need the picker at all. Our own hook at 0x5D6D3F assigns the real
// cell a few instructions later, so anything it returns is overwritten anyway.
// Forcing EDX = 0 sends the house down the SEATED path instead, which is the
// well-trodden vanilla route: it reads startCellTable[0], a genuinely valid
// cell, and the shift then relocates it to its proper ring position.
//
// ⚠ Only when there is a real shortfall. Without one the picker is not drained
// and works exactly as it always has, so normal games are left bit-identical —
// including the two calls vanilla makes for Neutral and Special.
//
// Stolen bytes 5: 75 33 (jne, 2) + 83 c5 24 (add ebp,0x24, 3). Because the jne
// itself is stolen we own the branch: 0x5D6D30 takes it, 0x5D6D00 falls through
// (and must re-apply the add ebp,0x24 we consumed).
// ---------------------------------------------------------------------------
DEFINE_HOOK(0x5D6CFB, PlayerCountExt_SpawnShift_BypassBrokenPositionPicker, 0x5)
{
	// Capture the start-cell table here, not only at 0x5D6C1D.
	//
	// It is a STACK argument, so a pointer cached in one invocation is dead
	// stack in the next. 0x5D6C1D only fires for explicitly seated houses, and
	// this very hook routes unseated ones straight to 0x5D6D30 — so on a pass
	// where nothing is seated it never fired at all and the stale pass-1
	// pointer was reused. Offsets then resolved to nonsense like (0,240),
	// every candidate failed the usable-terrain test, and every house fell
	// through to "leaving engine placement" and stayed bunched where the engine
	// had put it.
	//
	// [ESP+0x28] is the same slot 0x5D6D30 reads, and the taken branch pushes
	// nothing between here and there, so the offset is identical.
	// The start-cell table pointer, from one of two known-good offsets.
	//
	// The engine reads it at 0x5D6D30 as [ESP+0x28]: the function allocates a
	// local struct at [esp+0x14] (0x5D6C7B), has 0x688380 fill it, then pushes
	// esi/ebx/ebp/edi, so by our hook that field sits 0x28 up. Reading 0x28
	// nonetheless produced a table where six bases resolved to three cells,
	// while a crash dump showed the genuine pointer at Stack(0x10) — 0x18 away,
	// a Syringe stack-base discrepancy rather than a mistake in the frame
	// arithmetic.
	//
	// So both are tried, most-likely first, and validated before use.
	//
	// ⚠ VALIDATE BEFORE DEREFERENCING. An earlier version scanned the stack and
	// accepted any non-zero slot as a pointer; a slot holding the integer 1
	// passed that test and the read of [1] was an instant access violation. A
	// scavenged stack slot needs a range check, not a null check — and a bounded
	// pair of candidates beats a scan, which will keep finding false positives
	// whenever a small integer lands in the window.
	{
		// How many of the first `wanted` entries are distinct, plausible cells.
		// 0 means "not a table at all"; `wanted` means a perfect one. Anything
		// between is usable but will place people badly, so it is worth saying
		// out loud.
		//
		// ⚠ The range check is what stops this dereferencing garbage. An earlier
		// version scanned the stack accepting any non-zero slot, and a slot
		// holding the integer 1 was read as a pointer — an instant access
		// violation.
		auto TableDistinctCells = [](DWORD ptr, int wanted) -> int
		{
			if (ptr < 0x10000 || ptr >= 0x80000000 || (ptr & 3))
				return 0;

			const auto entries = reinterpret_cast<const DWORD*>(ptr);
			int distinct = 0;

			for (int i = 0; i < wanted; ++i)
			{
				PackedCell e; e.Raw = entries[i];
				if (e.Cell.X <= 0 || e.Cell.Y <= 0 || e.Cell.X > 1024 || e.Cell.Y > 1024)
					return 0; // not cell-shaped: reject the whole candidate

				bool repeat = false;
				for (int j = 0; j < i && !repeat; ++j)
					repeat = (entries[j] == entries[i]);

				if (!repeat)
					++distinct;
			}

			return distinct;
		};

		const int wanted = (RealStartCount > 0 && RealStartCount <= 16) ? RealStartCount : 8;
		const int offsets[] = { 0x28, 0x10 };

		// Score rather than accept/reject. Demanding a PERFECT table meant that
		// when neither offset produced one we kept no table at all — and with no
		// table the shift does nothing, so every house stayed on whatever cell
		// the engine handed it and sixteen players spawned on top of each other.
		// An imperfect table still spreads people out; no table is catastrophic.
		DWORD bestPtr = 0; int bestDistinct = -1, bestOffset = 0;

		for (int k = 0; k < 2; ++k)
		{
			const DWORD candidate = R->Stack32(offsets[k]);
			const int distinct = TableDistinctCells(candidate, wanted);

			if (distinct > bestDistinct)
			{
				bestDistinct = distinct;
				bestPtr = candidate;
				bestOffset = offsets[k];
			}
		}

		if (bestPtr && bestDistinct > 0 && CachedCellTable != bestPtr)
		{
			PlayerCountExt::Log("[shift] start-cell table @0x%08X (stack +0x%02X): "
				"%d of %d entries distinct%s\n",
				bestPtr, bestOffset, bestDistinct, wanted,
				bestDistinct < wanted ? "  <-- ALIASED, spread will be poor" : "");
		}

		if (bestPtr && bestDistinct > 0)
			CachedCellTable = bestPtr;
	}

	GET(DWORD, ebx, EBX);

	if (ebx & 0xFF)
		return 0x5D6D30; // seated in HouseIndices — vanilla taken branch

	int playing = 0;
	if (!ApplyShift || !HasStartShortfall(playing))
	{
		R->EBP(R->EBP() + 0x24); // the add we stole
		return 0x5D6D00;         // vanilla fall-through to the picker
	}

	GET(DWORD, pHouse, EDI);
	PlayerCountExt::Log("[shift] house@0x%08X has no seat in HouseIndices; using start 0 as a "
		"placeholder instead of the engine's picker (%d houses vs %d positions — the picker "
		"runs dry and faults at 0x5D6D17)\n", pHouse, playing, RealStartCount);

	R->EDX(0);
	return 0x5D6D30;
}
