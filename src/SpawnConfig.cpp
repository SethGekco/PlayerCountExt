/**
*  PlayerCountExt — spawn.ini reader. See SpawnConfig.h for the schema.
*  GPLv3.
*/

#include "SpawnConfig.h"

#include <Windows.h>
#include <climits>
#include <cstdio>
#include <cstdlib>

namespace
{
	// ⚠ FOOTGUN 1: GetPrivateProfile* resolves a BARE filename against the
	// Windows directory, NOT the current directory. Passing "spawn.ini" would
	// silently read C:\Windows\spawn.ini (i.e. nothing) and every lookup would
	// return the default. The ".\\" prefix forces cwd-relative resolution, and
	// the game's cwd is its own folder — the same place our log lands.
	constexpr const char* SpawnIniPath = ".\\spawn.ini";

	// Sentinel distinct from -1, so "key absent" and "key present with value -1"
	// are distinguishable.
	constexpr int Missing = INT_MIN;

	// ⚠ FOOTGUN 2: we deliberately do NOT use GetPrivateProfileIntA. It parses
	// the value as UNSIGNED and documents "if the value of the key is less than
	// zero, the return value is zero." spawn.ini legitimately uses -1 for
	// "unset", so that API would silently turn -1 into 0 — and 0 is a perfectly
	// valid country index (Americans). A wrong-but-plausible value is far worse
	// than a missing one. Read the raw string and parse it ourselves.
	int ReadInt(const char* section, const char* key)
	{
		char buffer[64] = {};
		const DWORD len = GetPrivateProfileStringA(
			section, key, "", buffer, sizeof(buffer), SpawnIniPath);

		if (len == 0 || buffer[0] == '\0')
			return Missing;

		char* end = nullptr;
		const long v = std::strtol(buffer, &end, 10);

		// Reject anything that isn't a clean integer rather than accepting
		// strtol's partial-parse result.
		if (end == buffer)
			return Missing;

		return static_cast<int>(v);
	}

	// Reads [section] MultiN, N being 1-based.
	int ReadMulti(const char* section, int multiIndex)
	{
		char key[16];
		std::snprintf(key, sizeof(key), "Multi%d", multiIndex);
		return ReadInt(section, key);
	}
}

namespace PlayerCountExt
{
	SpawnConfig& SpawnConfig::Get()
	{
		static SpawnConfig instance;
		return instance;
	}

	const HouseSpawnConfig& SpawnConfig::House(int multiIndex) const
	{
		static const HouseSpawnConfig empty{};
		if (multiIndex < 1 || multiIndex > MaxHouses)
			return empty;
		return this->Houses[multiIndex];
	}

	void SpawnConfig::Load()
	{
		// Full reset — this is a re-read, not a merge. Stale values from a
		// previous game would be worse than none.
		for (int i = 0; i <= MaxHouses; ++i)
			this->Houses[i] = HouseSpawnConfig{};

		this->Highest = 0;

		GetPrivateProfileStringA("Settings", "Scenario", "",
			this->ScenarioFile, sizeof(this->ScenarioFile), SpawnIniPath);

		const int players = ReadInt("Settings", "PlayerCount");
		const int ai      = ReadInt("Settings", "AIPlayers");
		const int seed    = ReadInt("Settings", "Seed");

		// If PlayerCount is missing entirely, spawn.ini isn't there (or isn't
		// readable) — report that rather than silently pretending to a config
		// of all zeroes.
		if (players == Missing)
		{
			this->IsLoaded = false;
			this->Players = this->AICount = this->RandomSeed = 0;
			this->ScenarioFile[0] = '\0';
			Log("[spawn] spawn.ini not found or unreadable at \"%s\" — no config.\n",
				SpawnIniPath);
			return;
		}

		this->Players    = players;
		this->AICount    = (ai   == Missing) ? 0 : ai;
		this->RandomSeed = (seed == Missing) ? 0 : seed;

		for (int n = 1; n <= MaxHouses; ++n)
		{
			auto& h = this->Houses[n];

			const int country  = ReadMulti("HouseCountries", n);
			const int color    = ReadMulti("HouseColors",    n);
			const int handicap = ReadMulti("HouseHandicaps", n);
			const int start    = ReadMulti("SpawnLocations", n);

			h.Defined = (country != Missing) || (color != Missing)
			         || (handicap != Missing) || (start != Missing);

			if (!h.Defined)
				continue;

			h.Country       = (country  == Missing) ? -1 : country;
			h.Color         = (color    == Missing) ? -1 : color;
			h.Handicap      = (handicap == Missing) ? -1 : handicap;
			h.SpawnLocation = (start    == Missing) ? -1 : start;

			this->Highest = n;
		}

		this->IsLoaded = true;
	}

	void SpawnConfig::LogSummary() const
	{
		if (!this->IsLoaded)
		{
			Log("[spawn] (not loaded)\n");
			return;
		}

		Log("[spawn] PlayerCount=%d AIPlayers=%d Seed=%d highestMulti=%d scenario=\"%s\"\n",
			this->Players, this->AICount, this->RandomSeed, this->Highest, this->ScenarioFile);

		for (int n = 1; n <= this->Highest; ++n)
		{
			const auto& h = this->Houses[n];
			if (!h.Defined)
			{
				Log("[spawn]   Multi%-2d (undefined)\n", n);
				continue;
			}
			Log("[spawn]   Multi%-2d country=%d color=%d handicap=%d start=%d\n",
				n, h.Country, h.Color, h.Handicap, h.SpawnLocation);
		}
	}
}
