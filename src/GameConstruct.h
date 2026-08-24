/**
*  PlayerCountExt — construction of engine classes that are abstract to the compiler
*
*  WHY THIS EXISTS
*  ---------------
*  HouseClass inherits pure-virtual COM/IPersist methods from AbstractClass —
*  GetClassID / Load / Save / WhatAmI / Size are all declared `= 0` in YRpp's
*  AbstractClass.h. So HouseClass is an ABSTRACT TYPE as far as the compiler is
*  concerned, even though the engine's real vtable makes it concrete at runtime.
*
*  That rules out every normal construction route:
*
*    GameCreate<T>       concept-gated on std::constructible_from -> always
*                        false for an abstract type
*    new T(...)          full abstract-class check -> error C2259
*    new (pMem) T(...)   placement-new is NOT a workaround. The abstract check
*                        fires on the CONSTRUCTION, not the allocation, so this
*                        fails identically. (The repo previously shipped this as
*                        `GameSpawn<T>`; it never compiled.)
*
*  So we do exactly what the engine itself does: allocate raw memory from the
*  game's allocator, then call the constructor THROUGH ITS ADDRESS as a
*  __thiscall function pointer. The compiler is never shown a `new` expression,
*  so the abstract check never fires.
*
*  GPLv3. Built on YRpp + Syringe.
*/

#pragma once

#include <Memory.h>

#include <HouseClass.h>
#include <HouseTypeClass.h>
#include <UnitClass.h>
#include <UnitTypeClass.h>

namespace PlayerCountExt
{
	namespace Engine
	{
		// --- HouseClass ----------------------------------------------------
		//
		// Verified by disassembling ScenarioClass::AssignHouses (0x687F10) in
		// vanilla gamemd.exe (sha1 189a5a868b3cef8d3d1a58ac3cf0a5241675e4ea).
		// All four `new HouseClass` sites inside that function do exactly this:
		//
		//     push $0x160b8      ; sizeof(HouseClass)
		//     call 0x7c8e17      ; operator new  (== YRMemory::Allocate)
		//     ...
		//     push %ecx          ; the HouseTypeClass*
		//     mov  %eax,%ecx     ; ECX = this
		//     call 0x4f54a0      ; HouseClass::HouseClass(HouseTypeClass*)
		//     mov  %eax,%ebp     ; ctor returns `this` in EAX
		//
		// Call sites: 0x687FC3 (human), 0x6881A0 (AI), 0x6882FE (Neutral),
		// 0x688351 (Special).
		//
		// YRMemory::Allocate is itself `JMP(0x7C8E17)` (YRpp Memory.h:52-53),
		// i.e. the identical allocator — so allocation here is byte-for-byte
		// what vanilla does, and YRMemory::Deallocate (0x7C8B3D) remains the
		// correct matching free.
		constexpr size_t HouseClassSize = 0x160B8;
		constexpr DWORD  HouseClassCtorAddress = 0x4F54A0;

		using HouseCtor_t = HouseClass* (__thiscall*)(void*, HouseTypeClass*);

		// We deliberately allocate the ENGINE's size (0x160B8) rather than
		// sizeof(HouseClass). YRpp's HouseClass is a best-effort mapping and
		// may be SMALLER than the real struct; allocating sizeof() would
		// under-allocate and let the engine constructor scribble past the end
		// of the block. The assertion below is the property that actually
		// matters — YRpp's view must FIT INSIDE the engine's allocation.
		static_assert(sizeof(HouseClass) <= HouseClassSize,
			"YRpp's HouseClass exceeds the engine's verified 0x160B8 allocation. "
			"Re-verify the size against the `push $0x160b8` in AssignHouses "
			"(0x687F10) before shipping — do not simply raise this number.");

		// Creates a fully engine-wired HouseClass, exactly as AssignHouses does.
		// Returns nullptr on allocation failure or a null country.
		//
		// NOTE: like vanilla, the memory is NOT zeroed first — the engine's
		// operator new does not zero, and the constructor is responsible for
		// initialisation. Matching vanilla exactly is deliberate.
		inline HouseClass* CreateHouse(HouseTypeClass* pCountry)
		{
			if (!pCountry)
				return nullptr;

			void* const pMem = YRMemory::Allocate(HouseClassSize);
			if (!pMem)
				return nullptr;

			return reinterpret_cast<HouseCtor_t>(HouseClassCtorAddress)(pMem, pCountry);
		}

		// --- UnitClass -----------------------------------------------------
		//
		// Units do NOT need the raw-pointer treatment: the engine's intended
		// idiom is the type's own factory virtual, which sidesteps the abstract
		// problem entirely.
		//
		// FOOTGUN — read before changing this. UnitTypeClass::CreateObject is
		// declared `R0` in YRpp (UnitTypeClass.h:33), and `R0` expands to
		// `{return 0;}` (YRPPCore.h:43). A VIRTUAL call through a real engine
		// object — what we do here — dispatches via the game's own vtable and
		// reaches the real engine function. That is safe. A QUALIFIED call,
		// e.g. `pType->UnitTypeClass::CreateObject(pOwner)`, bypasses the
		// vtable, inlines the R0 stub, and SILENTLY returns nullptr with no
		// warning. Never write one.
		//
		// The returned object is created in limbo; the caller still has to
		// Unlimbo() it to place it on the map.
		inline UnitClass* CreateUnit(UnitTypeClass* pType, HouseClass* pOwner)
		{
			if (!pType || !pOwner)
				return nullptr;

			return static_cast<UnitClass*>(pType->CreateObject(pOwner));
		}
	}
}
