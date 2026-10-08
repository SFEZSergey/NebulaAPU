#!/usr/bin/env python3
# Copyright (C) 2026 NebulaAPU contributors
# SPDX-License-Identifier: GPL-2.0-only
"""Name the addresses the sampler prints.

The runner's sampler reports sites as `module+0xOFFSET`, which is enough to
tell two hot places apart and nothing more. The cycle executable is linked by
MinGW without `-g`, so there is no PDB - but the COFF symbol table survives the
link, and `nm --numeric-sort` recovers about ten thousand text symbols from it.
Nearest-preceding-symbol against the image base turns any offset back into a
function name, with no rebuild and no debug build.

    python tools/sample_symbols.py --binary path/to/AstroBot.cycle.exe 0x28BD7

Piping a sampler line in rewrites every `module+0x...` in place, so a whole
`sampler.busy` report can be read at once:

    grep sampler.busy stderr.log | python tools/sample_symbols.py --binary ...

The offsets only mean anything against the *exact* binary that printed them -
every relink moves them, silently and by a lot. `astro-cycle.ps1` therefore
writes an `AstroBot.cycle.nm` beside the executable at link time; pass that with
`--symbols` to read an older run after the tree has moved on.
"""

from __future__ import annotations

import argparse
import bisect
import re
import shutil
import subprocess
import sys

# The default MinGW image base. A PE can be linked anywhere, so it is a flag,
# but every build of the runner so far has used this one.
DEFAULT_IMAGE_BASE = 0x140000000

SITE = re.compile(r"\b([A-Za-z0-9_.\-]+\.(?:exe|dll|DLL))\+0x([0-9A-Fa-f]+)")


def find_nm(explicit: str | None) -> str:
    if explicit:
        return explicit
    found = shutil.which("nm") or shutil.which("nm.exe")
    if found is None:
        raise SystemExit("nm is not on PATH; pass --nm")
    return found


def read_text_symbols(text: str, origin: str) -> list[tuple[int, str]]:
    rows: list[tuple[int, str]] = []
    for line in text.splitlines():
        parts = line.split(" ", 2)
        # An undefined symbol has no address and so no place on the map.
        if len(parts) < 3 or not parts[0].strip():
            continue
        if parts[1] not in ("t", "T"):
            continue
        try:
            address = int(parts[0], 16)
        except ValueError:
            continue
        rows.append((address, parts[2].strip()))
    if not rows:
        raise SystemExit(f"no text symbols in {origin} - was it stripped?")
    rows.sort()
    return rows


def resolve(
    rows: list[tuple[int, str]],
    addresses: list[int],
    offset: int,
    base: int,
) -> str:
    index = bisect.bisect_right(addresses, base + offset) - 1
    if index < 0:
        return f"0x{offset:X}"
    return f"{rows[index][1]}+0x{base + offset - addresses[index]:X}"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary")
    parser.add_argument(
        "--symbols",
        help="a saved `nm --numeric-sort` dump, as written beside the exe",
    )
    parser.add_argument("--nm")
    parser.add_argument("--image-base", default=hex(DEFAULT_IMAGE_BASE))
    parser.add_argument(
        "--module",
        help="only rewrite sites in this module; defaults to the binary's name",
    )
    parser.add_argument("offsets", nargs="*")
    args = parser.parse_args()

    if not args.binary and not args.symbols:
        raise SystemExit("pass --binary or --symbols")

    base = int(args.image_base, 0)
    if args.symbols:
        origin = args.symbols
        with open(args.symbols, encoding="utf-8", errors="replace") as handle:
            dump = handle.read()
    else:
        origin = args.binary
        dump = subprocess.run(
            [find_nm(args.nm), "--numeric-sort", args.binary],
            capture_output=True,
            text=True,
            errors="replace",
        ).stdout
    rows = read_text_symbols(dump, origin)
    addresses = [address for address, _ in rows]
    name = args.module or origin
    module = name.replace("\\", "/").rsplit("/", 1)[-1]
    if not args.module and module.endswith(".nm"):
        module = module[: -len(".nm")] + ".exe"

    if args.offsets:
        for text in args.offsets:
            offset = int(text, 0)
            print(f"+0x{offset:X} -> {resolve(rows, addresses, offset, base)}")
        return 0

    # No offsets given: rewrite whatever arrives on stdin. Sites in other
    # modules are left alone - these symbols do not describe them.
    def substitute(match: re.Match[str]) -> str:
        if match.group(1).lower() != module.lower():
            return match.group(0)
        offset = int(match.group(2), 16)
        return resolve(rows, addresses, offset, base)

    for line in sys.stdin:
        sys.stdout.write(SITE.sub(substitute, line))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
