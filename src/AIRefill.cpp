/**
*  PlayerCountExt — batch-refill of the 8-wide AISlots arrays
*
*  THE PROBLEM
*  -----------
*  AssignHouses' AI-creation loop is bounded by a POINTER compare at 0x6882C5:
*
*      688158:  mov  $0xa8b29c,%ebx      ; EBX = &AISlots.Countries[0]
*      6882c2:  add  $0x4,%ebx
*      6882c5:  cmp  $0xa8b2bc,%ebx      ; end of Countries[8]  <<< the cap
*      6882cb:  jl   0x68815D
*
*  0xA8B2BC - 0xA8B29C = 0x20 = 8 ints. There is no `cmp $0x8` anywhere; the
*  limit is the array's end ADDRESS baked in as an immediate.
*
*  THE APPROACH — batch-refill, not relocation
*  -------------------------------------------
*  When the loop exhausts the 8 slots we rewrite those same slots with the next
*  batch of houses from spawn.ini and rewind EBX to the base, re-entering the
*  engine's own loop. The engine keeps doing all the real work — construction,
*  colour, handicap, native wiring — and we only change what it reads.
*
*  This avoids relocating AISlots, which would mean finding and repointing every
*  other consumer of 0xA8B29C in the binary.
*
*  TWO FACTS THAT SHAPE THIS CODE, both established by running the game
*  --------------------------------------------------------------------
*  1. The -1/-3 sentinels SKIP a slot; they do NOT end the loop. All three
*     conditional jumps (jge on the AIPlayers test, je on -1, je on -3) target
*     0x6882C2 — the `add $0x4,%ebx` increment. The loop therefore ALWAYS walks
*     all 8 slots. EAX is not a slot index either: it is incremented only on the
*     create path at 0x68817E, so it counts houses CREATED so far. Refilling is
*     about what the slots CONTAIN, never about extending a scan.
*
*  2. AssignHouses runs TWICE per game start, with HouseClass::Array.Count back
*     at 0 on the second pass — the array is torn down and rebuilt, not appended
*     to. So we restore the engine's original slot values at the end of every
*     pass, or the second pass would inherit batch N as its batch 0.
*
*  DETERMINISM
*  -----------
*  Everything here is pure arithmetic over host-broadcast spawn.ini data. No
*  RNG, no local time, no pointer-ordered iteration. See README rule 5 — this is
*  precisely the code path where a desync would originate.
*
*  GPLv3.
*/

#include "PlayerCountExt.h"
#include "SpawnConfig.h"

#include <Syringe.h>
#include <Helpers/Macro.h>

namespace
{
	// AISlots sub-arrays, 0x20 (8 int) stride. Runtime-confirmed.
	constexpr DWORD AddrAISlotsDifficulties = 0xA8B27C;
	constexpr DWORD AddrAISlotsCountries    = 0xA8B29C;
	constexpr DWORD AddrAISlotsColors       = 0xA8B2BC;
	constexpr DWORD AddrAISlotsStarts       = 0xA8B2DC;
	constexpr DWORD AddrAISlotsTeams        = 0xA8B2FC;
	constexpr DWORD AddrAIPlayers           = 0xA8B274;

	// End of Countries[8] — the value the engine compares EBX against, and
	// simultaneously the address of Colors[0].
	constexpr DWORD AISlotsEnd = AddrAISlotsColors;

	// Re-entry points inside AssignHouses.
	constexpr DWORD LoopCondition   = 0x68815D; // cmp 0x20(%esp),%eax
	constexpr DWORD NeutralCreation = 0x6882D1; // push $0x160b8 (Neutral house)

	constexpr int Slots = PlayerCountExt::EngineAISlots; // 8

	int* Countries()    { return reinterpret_cast<int*>(AddrAISlotsCountries); }
	int* Colors()       { return reinterpret_cast<int*>(AddrAISlotsColors); }
	int* Difficulties() { return reinterpret_cast<int*>(AddrAISlotsDifficulties); }
	int* Starts()       { return reinterpret_cast<int*>(AddrAISlotsStarts); }
	int* Teams()        { return reinterpret_cast<int*>(AddrAISlotsTeams); }

	// -----------------------------------------------------------------------
	// State for one AssignHouses pass.
	//
	// Batch 0 is whatever the engine set up for itself (Multi1..Multi8) and we
	// never rewrite it, so a <=8-house game is bit-identical to vanilla with
	// this DLL loaded. We only act once the engine has finished its own pass.
	// -----------------------------------------------------------------------
	struct RefillState
	{
		bool Saved = false;
		int  Batch = 0;

		int OrigCountries[Slots]{};
		int OrigColors[Slots]{};
		int OrigDifficulties[Slots]{};
		int OrigStarts[Slots]{};
		int OrigTeams[Slots]{};

		void Save()
		{
			for (int i = 0; i < Slots; ++i)
			{
				OrigCountries[i]    = Countries()[i];
				OrigColors[i]       = Colors()[i];
				OrigDifficulties[i] = Difficulties()[i];
				OrigStarts[i]       = Starts()[i];
				OrigTeams[i]        = Teams()[i];
			}
			Saved = true;
		}

		void Restore()
		{
			if (!Saved)
				return;

			for (int i = 0; i < Slots; ++i)
			{
				Countries()[i]    = OrigCountries[i];
				Colors()[i]       = OrigColors[i];
				Difficulties()[i] = OrigDifficulties[i];
				Starts()[i]       = OrigStarts[i];
				Teams()[i]        = OrigTeams[i];
			}
			Saved = false;
		}
	};

	RefillState State;

	// Writes houses Multi(base+1) .. Multi(base+8) into the 8 engine slots.
	// Returns how many slots received a usable (>= 0) country.
	//
	// Unused slots get -1, the engine's own empty sentinel, so its loop simply
	// skips them (see fact 1 in the file header).
	int FillBatch(int batch)
	{
		const auto& spawn = PlayerCountExt::SpawnConfig::Get();
		const int base = batch * Slots; // 0-based house offset

		int filled = 0;

		for (int i = 0; i < Slots; ++i)
		{
			// MultiN is 1-based: house (base + i) is Multi(base + i + 1).
			const int multi = base + i + 1;
			const auto& h = spawn.House(multi);

			if (!h.Defined || h.Country < 0)
			{
				Countries()[i]    = -1;
				Colors()[i]       = -1;
				Difficulties()[i] = -1;
				Starts()[i]       = -1;
				Teams()[i]        = -1;
				continue;
			}

			Countries()[i]    = h.Country;
			Colors()[i]       = h.Color;
			Difficulties()[i] = h.Handicap;
			Starts()[i]       = h.SpawnLocation;
			Teams()[i]        = -1; // alliances not wired yet — see DESIGN.md

			++filled;

			PlayerCountExt::Log("[refill] batch%d slot%d <- Multi%-2d country=%d color=%d handicap=%d start=%d\n",
				batch, i, multi, h.Country, h.Color, h.Handicap, h.SpawnLocation);
		}

		return filled;
	}
}

// ---------------------------------------------------------------------------
// Loop start — 0x688158, `mov $0xa8b29c,%ebx`.
//
// Taking this over gives us one guaranteed hook per pass, before anything has
// been modified: we snapshot the engine's own slot values and reset the batch
// counter here. That is what makes the second AssignHouses call behave exactly
// like the first (fact 2).
//
// Stolen bytes 5: bb 9c b2 a8 00
// ---------------------------------------------------------------------------
DEFINE_HOOK(0x688158, PlayerCountExt_AIRefill_LoopStart, 0x5)
{
	// Undo anything a previous pass left behind, then re-baseline.
	State.Restore();
	State.Batch = 0;
	State.Save();

	R->EBX(AddrAISlotsCountries);
	return LoopCondition;
}

// ---------------------------------------------------------------------------
// Loop end bound — 0x6882C5, the cap.
//
// EBX has already been advanced by the `add $0x4,%ebx` at 0x6882C2. Below the
// end address, we hand straight back to the engine's loop. At or past it, the
// engine has finished a full 8-slot pass and we decide whether to serve another.
//
// Stolen bytes 6: 81 fb bc b2 a8 00
// ---------------------------------------------------------------------------
DEFINE_HOOK(0x6882C5, PlayerCountExt_AIRefill_EndBound, 0x6)
{
	const DWORD ebx = R->EBX();

	// Mid-scan — nothing to do.
	if (ebx < AISlotsEnd)
		return LoopCondition;

	const int created = static_cast<int>(R->EAX()); // houses created so far
	const int wanted  = *reinterpret_cast<int*>(AddrAIPlayers);

	auto& spawn = PlayerCountExt::SpawnConfig::Get();

	const int nextBatch = State.Batch + 1;
	const int firstHouseOfNextBatch = nextBatch * Slots; // 0-based

	// Serve another batch only if the engine still wants houses, spawn.ini
	// actually describes some, and we stay under the 32-bit bitfield ceiling.
	const bool wantsMore  = created < wanted;
	const bool haveMore   = spawn.Loaded()
		&& spawn.HighestDefined() > firstHouseOfNextBatch;
	const bool underCap   = firstHouseOfNextBatch < PlayerCountExt::EngineHouseCeiling;

	if (wantsMore && haveMore && underCap)
	{
		State.Batch = nextBatch;

		if (const int filled = FillBatch(nextBatch))
		{
			PlayerCountExt::Log("[refill] pass %d done: created=%d wanted=%d -> batch %d (%d slots), rewinding\n",
				nextBatch - 1, created, wanted, nextBatch, filled);

			R->EBX(AddrAISlotsCountries);
			return LoopCondition;
		}

		// Nothing usable in that batch — stop rather than spin forever.
		PlayerCountExt::Log("[refill] batch %d had no usable houses, stopping\n", nextBatch);
	}

	PlayerCountExt::Log("[refill] AI loop complete: created=%d wanted=%d batches=%d\n",
		created, wanted, State.Batch + 1);

	// Hand the engine's own values back before Neutral/Special are built, so
	// nothing downstream — including the second AssignHouses pass — sees our
	// writes.
	State.Restore();

	return NeutralCreation;
}
