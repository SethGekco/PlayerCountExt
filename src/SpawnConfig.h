/**
*  PlayerCountExt — spawn.ini reader
*
*  spawn.ini is written by the CnCNet client and, in a networked game, is
*  broadcast by the host — so every client sees identical content. That makes it
*  the ONLY safe place to get the house set from. A client-local config file
*  would let two clients build different house arrays and desync on frame one.
*  See README rule 6.
*
*  Schema (confirmed against a real client-generated file, 2026-08-23):
*
*      [Settings]
*      PlayerCount=1        ; human players
*      AIPlayers=2          ; AI count — matches the engine global at 0xA8B274
*      Seed=396867357       ; shared RNG seed
*      Side=2  Color=3      ; the LOCAL player's country/colour
*
*      [HouseCountries]   MultiN=<country index>
*      [HouseColors]      MultiN=<spawn colour index>
*      [HouseHandicaps]   MultiN=<difficulty>
*      [SpawnLocations]   MultiN=<waypoint index>
*
*  MultiN is **1-based**. The mapping to the engine's AISlots arrays is
*  `AISlots[i] <-> Multi(i+1)`, with slots owned by humans holding -1. This was
*  confirmed in a live game: spawn.ini had HouseCountries Multi2=0 / Multi3=6,
*  and the engine's AISlots.Countries[8] @0xA8B29C read {-1, 0, 6, -1, ...}.
*
*  GPLv3.
*/

#pragma once

#include "PlayerCountExt.h"

namespace PlayerCountExt
{
	// One house as spawn.ini describes it. Absent keys stay -1; the engine uses
	// -1 as its own "empty slot" sentinel, so this round-trips cleanly.
	struct HouseSpawnConfig
	{
		bool Defined       = false;  // any key present for this MultiN
		int  Country       = -1;     // [HouseCountries]
		int  Color         = -1;     // [HouseColors]   (spawn index, not scheme)
		int  Handicap      = -1;     // [HouseHandicaps]
		int  SpawnLocation = -1;     // [SpawnLocations]
	};

	class SpawnConfig
	{
	public:
		// Highest MultiN we look for. The engine's hard ceiling is the 32-bit
		// per-house bitfield, so there is no point scanning past it.
		static constexpr int MaxHouses = EngineHouseCeiling;

		static SpawnConfig& Get();

		// Re-reads spawn.ini. Cheap, and safe to call repeatedly — which matters
		// because AssignHouses runs TWICE per game start (verified in-game), and
		// because returning to the menu and starting a new game rewrites the file.
		void Load();

		bool Loaded()      const { return this->IsLoaded; }
		int  PlayerCount() const { return this->Players; }
		int  AIPlayers()   const { return this->AICount; }
		int  Seed()        const { return this->RandomSeed; }

		// [Settings] Scenario — the map INI the client wrote for this game
		// (normally "spawnmap.ini"). Empty if absent. Callers use this rather
		// than assuming a filename, since it is what the engine itself loads.
		const char* Scenario() const { return this->ScenarioFile; }

		// Highest MultiN that had any key defined (0 if none).
		int  HighestDefined() const { return this->Highest; }

		// 1-based, matching the MultiN naming. Out-of-range returns an empty
		// entry rather than failing, so callers need no bounds dance.
		const HouseSpawnConfig& House(int multiIndex) const;

		void LogSummary() const;

	private:
		SpawnConfig() = default;

		bool IsLoaded   = false;
		char ScenarioFile[64] {};
		int  Players    = 0;
		int  AICount    = 0;
		int  RandomSeed = 0;
		int  Highest    = 0;

		// index 0 unused so House(1) is Multi1
		HouseSpawnConfig Houses[MaxHouses + 1];
	};
}
