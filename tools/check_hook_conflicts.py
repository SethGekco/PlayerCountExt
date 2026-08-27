#!/usr/bin/env python3
"""
Check our Syringe hooks for byte-range conflicts with other DLLs.

WHY THIS EXISTS
---------------
Syringe hooks at the SAME address chain harmlessly - several DLLs can hook one
address and all of them run. Hooks whose stolen-byte ranges merely OVERLAP do
not: each writes a jump over the other's bytes and the result is a wild jump.
The failure is nowhere near the hook, so it is expensive to debug and trivial to
prevent.

The risk is not theoretical for this project. PlayerCountExt is co-loaded with
Antares, Phobos and a dozen sibling Ext DLLs, and several of those - MapSizeExt
in particular - also BYTE-PATCH instructions in place rather than hooking, which
a naive "do two hooks share an address" check would miss entirely.

TWO MODES
---------
  --registry PATH   Check against the YR Hook Encyclopedia's hooks.json, which
                    records the framework hooks (Antares/Ares/Phobos/Kratos) and
                    their stolen-byte counts. Suitable for CI, where no game
                    install exists.

  --dlls DIR        Check against every .syhks00 table found in DIR. This is the
                    stronger check: it sees the DLLs ACTUALLY co-loaded on a real
                    install, including sibling Ext projects the registry does not
                    know about. Run it against the RA2 folder.

  --patch-sources D Additionally treat every gamemd-range address literal in the
                    C++ sources under D as a byte-patch site. Use for projects
                    like MapSizeExt that rewrite instructions in place; an
                    address inside one of our hooked ranges is a conflict even
                    though it is not a hook.

Exit status is 1 if any overlap is found, so CI fails on a real conflict.
Same-address chains are reported but never fail the build.
"""

import argparse
import glob
import json
import os
import re
import struct
import subprocess
import sys

# A Syringe hook always overwrites at least 5 bytes (the jmp it installs), even
# when it declares fewer stolen bytes.
MIN_HOOK_BYTES = 5

GAMEMD_LOW, GAMEMD_HIGH = 0x400000, 0x800000


def our_hooks_from_sources(src_dir):
    """Parse DEFINE_HOOK(addr, name, stolen) out of our own C++."""
    pattern = re.compile(
        r"DEFINE_HOOK(?:_AGAIN)?\s*\(\s*(0x[0-9A-Fa-f]+)\s*,\s*(\w+)\s*,\s*(0x[0-9A-Fa-f]+|\d+)\s*\)"
    )
    found = []
    for path in glob.glob(os.path.join(src_dir, "**", "*.cpp"), recursive=True):
        with open(path, encoding="utf-8", errors="ignore") as handle:
            for match in pattern.finditer(handle.read()):
                addr = int(match.group(1), 16)
                size = int(match.group(3), 16 if match.group(3).startswith("0x") else 10)
                found.append((addr, max(size, MIN_HOOK_BYTES), match.group(2), os.path.basename(path)))
    return sorted(set(found))


def hooks_from_dll(path):
    """Read a DLL's .syhks00 table: 16-byte entries, {addr, stolen, ...}."""
    try:
        out = subprocess.run(["objdump", "-h", path], capture_output=True, text=True).stdout
    except FileNotFoundError:
        sys.exit("objdump not found - install binutils, or use --registry instead")

    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 6 and parts[1] == ".syhks00":
            size, offset = int(parts[2], 16), int(parts[5], 16)
            with open(path, "rb") as handle:
                blob = handle.read()[offset:offset + size]
            entries = []
            for i in range(0, len(blob) - 8, 16):
                addr, stolen = struct.unpack_from("<II", blob, i)
                if addr:
                    entries.append((addr, max(stolen, MIN_HOOK_BYTES)))
            return entries
    return []


def patch_sites(src_dir):
    """Every gamemd-range address literal in a project's sources."""
    sites = set()
    pattern = re.compile(r"0x0?0?([4-7][0-9A-Fa-f]{5})\b")
    for ext in ("*.cpp", "*.h"):
        for path in glob.glob(os.path.join(src_dir, "**", ext), recursive=True):
            with open(path, encoding="utf-8", errors="ignore") as handle:
                for match in pattern.finditer(handle.read()):
                    addr = int(match.group(1), 16)
                    if GAMEMD_LOW <= addr <= GAMEMD_HIGH:
                        sites.add(addr)
    return sites


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--src", default="src", help="our source directory (default: src)")
    ap.add_argument("--registry", help="path to the encyclopedia's registry/hooks.json")
    ap.add_argument("--dlls", help="directory of co-loaded DLLs to check against")
    ap.add_argument("--patch-sources", action="append", default=[],
                    help="source dir of a byte-patching project (repeatable)")
    args = ap.parse_args()

    ours = our_hooks_from_sources(args.src)
    if not ours:
        sys.exit(f"no DEFINE_HOOK found under {args.src!r} - wrong directory?")
    print(f"our hooks: {len(ours)}")

    # (addr, size, label) for everything we must not overlap.
    theirs = []

    if args.registry:
        with open(args.registry, encoding="utf-8") as handle:
            registry = json.load(handle)
        for addr_text, info in registry.items():
            addr = int(addr_text, 16)
            size = MIN_HOOK_BYTES
            for consumer in info.get("consumers", []):
                raw = consumer.get("stolen_bytes")
                if raw:
                    try:
                        size = max(size, int(str(raw), 16))
                    except ValueError:
                        pass
            names = ",".join(info.get("release_frameworks") or ["registry"])
            theirs.append((addr, size, names))
        print(f"registry entries: {len(registry)}")

    if args.dlls:
        for dll in sorted(glob.glob(os.path.join(args.dlls, "*.dll"))):
            name = os.path.basename(dll)
            if name.lower().startswith("playercountext"):
                continue
            for addr, size in hooks_from_dll(dll):
                theirs.append((addr, size, name))
        print(f"co-loaded hook entries: {len(theirs)}")

    for src_dir in args.patch_sources:
        label = os.path.basename(os.path.dirname(src_dir.rstrip("/"))) or src_dir
        sites = patch_sites(src_dir)
        for addr in sites:
            theirs.append((addr, 1, f"{label} (byte patch)"))
        print(f"{label}: {len(sites)} patch site(s)")

    if not theirs:
        sys.exit("nothing to check against - pass --registry and/or --dlls")

    chains, overlaps = [], []
    for addr, size, name, filename in ours:
        for other_addr, other_size, label in theirs:
            if other_addr == addr and other_size != 1:
                chains.append((addr, name, label))
            elif other_addr < addr + size and addr < other_addr + other_size:
                overlaps.append((addr, size, name, filename, other_addr, other_size, label))

    if chains:
        print(f"\n{len(chains)} same-address chain(s) - safe, Syringe runs all of them:")
        for addr, name, label in sorted(set(chains)):
            print(f"  0x{addr:X}  {name}  chains with {label}")

    if overlaps:
        print(f"\n*** {len(overlaps)} OVERLAPPING range(s) - these corrupt each other:")
        for addr, size, name, filename, other_addr, other_size, label in sorted(set(overlaps)):
            print(f"  {filename}: {name} at 0x{addr:X}+{size} "
                  f"(0x{addr:X}-0x{addr + size - 1:X}) overlaps {label} "
                  f"at 0x{other_addr:X}+{other_size}")
        print("\nMove our hook to an instruction boundary outside the other range.")
        return 1

    print("\nno overlapping ranges - clear")
    return 0


if __name__ == "__main__":
    sys.exit(main())
