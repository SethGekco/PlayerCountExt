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
	}
}
