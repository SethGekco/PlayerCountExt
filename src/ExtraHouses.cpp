/**
*  MegaSkirmish — extra-house creation
*
*  Strategy for coexisting with the stock CnCNet spawner:
*  rather than fight the spawner over AssignHouses, we hook a stable LATE
*  point that the engine reaches exactly once, after every house and the
*  human's starting units already exist. A one-shot guard makes it fire a
*  single time per game. We then append our AI houses on top.
*
*  HOOK POINT — see the VERIFY note on the DEFINE_HOOK below. The chosen
*  address is the prime suspect, not gospel; the first debug session
*  confirms or moves it.
*
*  GPLv3. Built on YRpp + Syringe.
*/

#include "MegaSkirmish.h"
#include "GameConstruct.h"

#include <Syringe.h>
#include <Helpers/Macro.h>
#include <Memory.h>

#include <HouseClass.h>
#include <HouseTypeClass.h>
#include <RulesClass.h>
#include <ScenarioClass.h>
#include <SessionClass.h>
#include <UnitClass.h>
#include <UnitTypeClass.h>
#include <MapClass.h>
#include <CellClass.h>

#include <algorithm>

// Construction of the compiler-abstract engine types lives in GameConstruct.h.
// The previous GameSpawn<T> here used placement-new, which does NOT work:
// the abstract-class check fires on the construction, not the allocation.
// See GameConstruct.h for the full explanation and the verified addresses.

namespace
{
	int ColorSchemeFromSpawn(int spawnColor)
	{
		if (spawnColor < 0)
			return 0;

		// VERIFY: RA2/YR pairs color schemes (base + shadow) on some code
		// paths, so the scheme index may be spawnColor*2. If every extra
		// house comes out one-consistent-color-off, flip this to *1.
		return spawnColor * 2;
	}

	HouseClass* MakeAIHouse(int idx, const MegaSkirmish::ExtraHouseConfig& cfg)
	{
		if (cfg.Country < 0 || cfg.Country >= HouseTypeClass::Array.Count)
		{
			MegaSkirmish::Log("[MegaSkirmish] House%d: country %d out of range.\n", idx, cfg.Country);
			return nullptr;
		}

		const auto pCountry = HouseTypeClass::Array.GetItem(cfg.Country);

		// Engine HouseClass ctor (0x4F54A0). Expected to register into
		// HouseClass::Array and assign ArrayIndex — verify via the log line.
		const auto pHouse = MegaSkirmish::Engine::CreateHouse(pCountry);
		if (!pHouse)
		{
			MegaSkirmish::Log("[MegaSkirmish] House%d: creation failed.\n", idx);
			return nullptr;
		}

		pHouse->IsHumanPlayer = false;
		if (cfg.Difficulty >= 0)
			pHouse->AIDifficulty = static_cast<AIDifficulty>(cfg.Difficulty);
		pHouse->ColorSchemeIndex = ColorSchemeFromSpawn(cfg.Color);

		const double f = (cfg.CreditsFactor != 0.0) ? cfg.CreditsFactor : 1.0;
		pHouse->Balance = static_cast<int>(MegaSkirmish::GetConfig()->DefaultCredits * f);

		MegaSkirmish::Log("[MegaSkirmish] House%d -> ArrayIndex %d, country %d (%s)\n",
			idx, pHouse->ArrayIndex, cfg.Country, pCountry->ID);

		// 32-bit bitfield ceiling guard (the reason the cap is 25).
		if (pHouse->ArrayIndex > 31)
			MegaSkirmish::Log("[MegaSkirmish] WARNING: ArrayIndex %d exceeds 32-bit bitfield range — corruption likely!\n",
				pHouse->ArrayIndex);

		return pHouse;
	}

	bool GiveStartingMCV(HouseClass* pHouse, int waypoint, int idx)
	{
		UnitTypeClass* pMCV = nullptr;
		const auto& baseUnits = RulesClass::Instance->BaseUnit;

		for (int i = 0; i < baseUnits.Count; ++i)
		{
			const auto pType = baseUnits.GetItem(i);
			if (pType && (pType->GetOwners() & (1u << pHouse->Type->ArrayIndex)))
			{
				pMCV = pType;
				break;
			}
		}
		if (!pMCV && baseUnits.Count > 0)
			pMCV = baseUnits.GetItem(0); // any MCV beats none

		if (!pMCV)
		{
			MegaSkirmish::Log("[MegaSkirmish] House%d: no BaseUnit in rules.\n", idx);
			return false;
		}

		CellStruct base = ScenarioClass::Instance->GetWaypointCoords(waypoint);
		const short off = static_cast<short>((idx + 1) * 4); // spread so MCVs don't stack

		// Virtual dispatch through the real vtable — see the R0 footgun note
		// in GameConstruct.h. Creates the unit in limbo; Unlimbo() places it.
		const auto pUnit = MegaSkirmish::Engine::CreateUnit(pMCV, pHouse);
		if (!pUnit)
			return false;

		static const CellStruct probes[] = {
			{0,0},{4,0},{0,4},{-4,0},{0,-4},{4,4},{-4,-4},{8,0},{0,8}
		};

		for (const auto& p : probes)
		{
			CellStruct c { static_cast<short>(base.X + off + p.X),
			               static_cast<short>(base.Y + p.Y) };

			if (const auto pCell = MapClass::Instance.TryGetCellAt(c))
			{
				if (pUnit->Unlimbo(pCell->GetCoords(), DirType::North))
				{
					MegaSkirmish::Log("[MegaSkirmish] House%d: %s placed at %d,%d\n",
						idx, pMCV->ID, c.X, c.Y);
					return true;
				}
			}
		}

		MegaSkirmish::Log("[MegaSkirmish] House%d: all placement probes failed near wp %d.\n", idx, waypoint);
		return false;
	}
}

void MegaSkirmish::CreateExtraHouses()
{
	auto pConfig = GetConfig();

	if (!pConfig->AnyDefined)
		return;

	if (SessionClass::IsCampaign())
	{
		Log("[MegaSkirmish] Campaign mode — extra houses are skirmish-only, skipping.\n");
		return;
	}

	int created = 0;
	for (int i = 0; i < MaxExtraHouses; ++i)
	{
		const auto& cfg = pConfig->Houses[i];
		if (!cfg.Defined)
			continue;

		if (const auto pHouse = MakeAIHouse(i, cfg))
		{
			const int wp = (cfg.SpawnWaypoint >= 0)
				? std::clamp(cfg.SpawnWaypoint, 0, 7)
				: (i % 8); // crude spread over the 8 native start positions

			pHouse->StartingPoint = wp;
			GiveStartingMCV(pHouse, wp, i);
			++created;
		}
	}

	Log("[MegaSkirmish] Done. Created %d extra AI house(s). HouseClass::Array.Count = %d\n",
		created, HouseClass::Array.Count);
}

// ---------------------------------------------------------------------------
// The trigger hook — COMPILE-TIME DISABLED for the instrumentation runs.
//
// Set to 1 only when deliberately testing the (abandoned) bolt-on path.
//
// WHY THIS MUST BE A COMPILE-TIME GATE, NOT A RUNTIME `if`:
// DEFINE_HOOK emits data that Syringe reads at load time to patch a JMP into
// the target address. That patch happens whether or not the hook body decides
// to do anything, so a runtime guard would NOT protect us — the address would
// still be overwritten. 0x6878E0 is an unverified guess (see below); patching
// a JMP into the middle of an unrelated instruction corrupts code and can
// crash arbitrarily far from the patch site. The only way to make it harmless
// is for the hook to not exist in the binary at all.
//
// Keeping it out also keeps the instrumentation result clean: any crash is
// then attributable to the two disassembly-verified hooks in
// Instrumentation.cpp (0x687F10 / 0x688378) rather than to this guess, and the
// house array my exit hook reports is not mutated afterwards by this path.
// ---------------------------------------------------------------------------
#define MEGASKIRMISH_ENABLE_BOLTON_HOUSES 0

#if MEGASKIRMISH_ENABLE_BOLTON_HOUSES

// VERIFY: 0x6878E0 is inside Westwood's scenario-start path, reached once
// after houses and the human's initial units are placed. This is the prime
// candidate for "everything exists, game hasn't started ticking yet."
// If it fires too early (houses not yet built) or never fires, the first
// debug session moves it — candidates in priority order:
//   1. later in ScenarioClass::StartScenario (0x683DB0 region)
//   2. first logic frame  (LogicClass update / 0x55D360 MainLoop entry, guarded)
//   3. immediately after the stock spawner's AssignHooks return
// The one-shot guard means wherever it lands, it runs exactly once.
DEFINE_HOOK(0x6878E0, MegaSkirmish_CreateHouses, 0x6)
{
	if (MegaSkirmish::Enabled && !MegaSkirmish::HousesCreated)
	{
		MegaSkirmish::HousesCreated = true;
		MegaSkirmish::GetConfig()->Load();
		MegaSkirmish::CreateExtraHouses();
	}

	return 0; // continue original code
}

#endif // MEGASKIRMISH_ENABLE_BOLTON_HOUSES
