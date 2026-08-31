/**
*  PlayerCountExt — rules-level configuration
*
*  Settings a MOD author sets once, as opposed to the per-map, per-spawn and
*  per-direction overrides that live in the map INI (see SpawnShift.cpp).
*
*  GPLv3.
*/

#pragma once

#include <climits>

namespace PlayerCountExt
{
	namespace RulesConfig
	{
		// INT_MIN when no rules pass supplied the key, in which case the caller
		// keeps its own default. Never returns 0 for "absent" — 0 is a value a
		// modder can legitimately mean.
		int ShiftDistance();

		// Should teammates be seated together in preference to using every
		// real start position?
		//
		// false (default) — STANDARD: every real position is filled before any
		//   compass variant, so a team is split up whenever spawns remain
		//   unused. Coordinating who sits where is then the players' business.
		//
		// true — TEAM: a slot beside an ally can win over an untouched position
		//   further away, so teams arrive together at the cost of leaving some
		//   real positions empty.
		//
		// Set in [PlayerCountExt] of rulesmd.ini, a game mode's INI, or a map -
		// later passes win, so a game mode can turn it on without touching the
		// mod default.
		bool ClusterTeams();
	}
}
