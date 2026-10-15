#!/usr/bin/env python3
"""Inventory bin/Win/Data/custom.MPQ/Custom/Shaders/*.bls and cross-check it against the
shader names the engine actually asks for.

Run from the repo root:
    python Tools/bls_inventory.py > Tools/bls_inventory.md
    python Tools/bls_inventory.py -o Tools/bls_inventory.md   (writes UTF-8 itself)

Use -o under PowerShell, whose `>` writes UTF-16.
"""

from __future__ import annotations

import argparse
import io
import os
import re
import struct
import sys

SHADER_ROOT = "bin/Win/Data/custom.MPQ/Custom/Shaders"

# Profile directories CGxDevice::IShaderLoad can walk to on this client (D3D9, caps are
# ps_3_0 / vs_3_0 - see Logs/gx.log). It starts at the caps profile and steps down.
PS_CHAIN = ("ps_3_0", "ps_2_0", "ps_1_1")
VS_CHAIN = ("vs_3_0", "vs_2_0", "vs_1_1")
REACHABLE = PS_CHAIN + VS_CHAIN

ACCEPTED_VERSION = 0x10003

SOURCE_ROOTS = ("Engine", "WoW")
SHADER_CREATE = re.compile(
    r'ShaderCreate\s*\([^;]*?"Shaders\\\\(Vertex|Pixel)"\s*,\s*([^,]+?)\s*,\s*([^)]*?)\)\s*;'
)
STRING_LIT = re.compile(r'"([^"]*)"')
ADD_EFFECT_FILE = re.compile(r'AddEffectFile\s*\(\s*"([^"]+)"')
GET_EFFECT = re.compile(r'GetEffect\s*\(\s*"([^"]+)"')
WFX_EFFECT = re.compile(r'Effect\s*\(\s*([^)]+?)\s*\)')


def chain_for(target: str) -> tuple[str, ...]:
    return PS_CHAIN if target == "Pixel" else VS_CHAIN


def candidates(entries: list[dict], target: str, name: str, classic: bool) -> list[dict]:
    """Files in the pack that could answer this ShaderCreate, best profile first."""
    if classic:
        roots = ("Classic/Pixel",) if target == "Pixel" else ("Classic/Vertex",)
    else:
        roots = chain_for(target)
    hits = [e for e in entries if e["profile"] in roots and e["name"].lower() == name.lower()]
    order = {p: i for i, p in enumerate(roots)}
    return sorted(hits, key=lambda e: order[e["profile"]])


def profile_of(rel: str) -> str:
    """'Pixel\\ps_3_0\\UI.bls' -> 'ps_3_0';  'Classic\\Pixel\\UI.bls' -> 'Classic/Pixel'."""
    parts = rel.replace("\\", "/").split("/")
    if parts[0].lower() == "classic":
        return "/".join(parts[:-1])
    return parts[-2] if len(parts) >= 2 else "?"


def read_header(path: str) -> tuple[int, list[int]]:
    with open(path, "rb") as fh:
        head = fh.read(24)

    # On disk the first four bytes are b"HSXG"; the loader reads them as a LE uint32 and
    # compares that against the literal 'GXSH'. Same value, different byte order.
    if len(head) < 12 or head[0:4] != b"HSXG":
        return 0, []

    fields = [struct.unpack_from("<I", head, 4 + 4 * i)[0] for i in range(5)]
    return fields[0], fields[1:]


def scan_pack() -> list[dict]:
    entries = []
    for dirpath, _dirs, files in os.walk(SHADER_ROOT):
        for name in files:
            if not name.lower().endswith(".bls"):
                continue
            full = os.path.join(dirpath, name)
            rel = os.path.relpath(full, SHADER_ROOT)
            version, fields = read_header(full)
            entries.append(
                {
                    "name": name[:-4],
                    "rel": rel,
                    "profile": profile_of(rel),
                    "size": os.path.getsize(full),
                    "version": version,
                    "fields": fields,
                }
            )
    return sorted(entries, key=lambda e: e["rel"].lower())


def scan_source() -> list[tuple[str, str, str, bool]]:
    """(target, name-or-expression, file:line, name-is-dynamic)."""
    found: dict[tuple[str, str], tuple[str, bool]] = {}
    for top in SOURCE_ROOTS:
        for dirpath, _dirs, files in os.walk(top):
            for name in files:
                if not name.endswith((".cpp", ".h")):
                    continue
                full = os.path.join(dirpath, name)
                with open(full, encoding="utf-8", errors="replace") as fh:
                    for lineno, line in enumerate(fh, 1):
                        m = SHADER_CREATE.search(line)
                        if not m:
                            continue
                        target = m.group(1)
                        raw = m.group(2).strip()
                        lits = STRING_LIT.findall(raw)
                        if lits:
                            for lit in lits:
                                found[(target, lit)] = (f"{full}:{lineno}", False)
                        else:
                            found[(target, raw)] = (f"{full}:{lineno}", True)
    return [(t, n, w, d) for (t, n), (w, d) in sorted(found.items())]


def scan_effects() -> tuple[list[tuple[str, str, str]], list[str], list[str]]:
    """(shader refs from .wfx files, .wfx files present, .wfx files the engine asks for)."""
    refs: list[tuple[str, str, str]] = []
    present: list[str] = []
    requested: list[str] = []

    for top in SOURCE_ROOTS:
        for dirpath, _dirs, files in os.walk(top):
            for name in files:
                if not name.endswith((".cpp", ".h")):
                    continue
                with open(os.path.join(dirpath, name), encoding="utf-8", errors="replace") as fh:
                    text = fh.read()
                requested += [os.path.basename(f) for f in ADD_EFFECT_FILE.findall(text)]

    wfx_dir = os.path.join(SHADER_ROOT, "Effects")
    if not os.path.isdir(wfx_dir):
        return refs, present, sorted(set(requested))

    for name in sorted(os.listdir(wfx_dir)):
        if not name.lower().endswith(".wfx"):
            continue
        present.append(name)
        with open(os.path.join(wfx_dir, name), encoding="utf-8", errors="replace") as fh:
            text = fh.read()
        # Pair each VertexShader() with the PixelShader() that follows it in the same Pass.
        for pass_body in re.findall(r"Pass\s*\((.*?)\)\s*\{(.*?)\}", text, re.S):
            vert = re.search(r"VertexShader\s*\(\s*([^)]+?)\s*\)", pass_body[1])
            pix = re.search(r"PixelShader\s*\(\s*([^)]+?)\s*\)", pass_body[1])
            if vert:
                refs.append(("Vertex", vert.group(1), name))
            if pix:
                refs.append(("Pixel", pix.group(1), name))
    return refs, present, sorted(set(requested))


def main() -> int:
    if not os.path.isdir(SHADER_ROOT):
        print(f"error: {SHADER_ROOT} not found (run from the repo root)", file=sys.stderr)
        return 1

    entries = scan_pack()
    total_mb = sum(e["size"] for e in entries) / 1024 / 1024

    print("# .bls inventory - `Data\\custom.MPQ\\Custom\\Shaders`")
    print()
    print(f"{len(entries)} `.bls` files, {total_mb:.2f} MB.")
    print()
    print("Generated by `python Tools/bls_inventory.py`. Re-run after changing the pack.")
    print()

    print("## How the loader finds a shader")
    print()
    print("`CGxDevice::IShaderLoad` (`Engine/Source/Gx/CGxDevice/CGxDevice.cpp`) builds")
    print("`\"%s\\\\%s\\\\%s.bls\"` from the path, the profile name for the current caps, and the shader name,")
    print("then accepts the file only if the magic reads as `'GXSH'` **and** the version is exactly")
    print(f"`0x{ACCEPTED_VERSION:X}`. Otherwise it steps down one profile and tries again.")
    print()
    print("So two things have to line up: the path must resolve inside the archive, and the container")
    print("version must be the accepted one.")
    print()

    print("## Container versions")
    print()
    print("| version | files | profiles | loader verdict |")
    print("|---|---|---|---|")
    by_version: dict[int, list[dict]] = {}
    for e in entries:
        by_version.setdefault(e["version"], []).append(e)
    for version in sorted(by_version, reverse=True):
        group = by_version[version]
        profiles = sorted({e["profile"] for e in group})
        verdict = "accepted" if version == ACCEPTED_VERSION else "**rejected - version check**"
        print(f"| 0x{version:X} | {len(group)} | {', '.join(profiles)} | {verdict} |")
    print()

    print("## Files per profile directory")
    print()
    print("| profile | files | version | on the loader's profile chain |")
    print("|---|---|---|---|")
    by_profile: dict[str, list[dict]] = {}
    for e in entries:
        by_profile.setdefault(e["profile"], []).append(e)
    for profile in sorted(by_profile):
        group = by_profile[profile]
        versions = sorted({f"0x{e['version']:X}" for e in group})
        if profile in REACHABLE:
            chain = "yes - on the ps_3_0/vs_3_0 chain"
        elif profile.startswith("Classic/"):
            inner = profile.split("/", 1)[1].lower()
            want = "Pixel\\ps_3_0" if inner == "pixel" else "Vertex\\vs_3_0"
            chain = f"**no - path is `{profile.replace('/', chr(92))}`, loader asks for `{want}`**"
        else:
            chain = "no - above the client's caps (SM4/5, tessellation)"
        print(f"| {profile} | {len(group)} | {', '.join(versions)} | {chain} |")
    print()

    print("## Shaders the engine asks for")
    print()
    print("Every `ShaderCreate` call in `Engine/` and `WoW/`. `reachable file` is the highest-profile")
    print("candidate in the pack for that name and target that sits on the loader's chain.")
    print()
    print("| target | name | asked by | reachable file | version | Classic fallback | verdict |")
    print("|---|---|---|---|---|---|---|")

    reachable = [e for e in entries if e["profile"] in REACHABLE]

    missing = []
    blocked = []

    for target, name, where, dynamic in scan_source():
        if dynamic:
            print(f"| {target} | `{name}` | `{where}` | - | - | - | built at runtime |")
            continue

        chain = candidates(entries, target, name, classic=False)
        fall = candidates(entries, target, name, classic=True)

        if chain:
            best = chain[0]
            version = f"0x{best['version']:X}"
            if best["version"] == ACCEPTED_VERSION:
                verdict = "**loadable** - accepted version, on the chain"
            else:
                verdict = "**blocked** - version rejected"
                blocked.append((target, name, best["rel"], best["version"]))
            reach = f"`{best['rel']}` ({best['size']} B)"
        else:
            reach = "**none**"
            version = "-"
            verdict = "**absent - nothing on the chain**"
            missing.append((target, name, where))

        fb = f"`{fall[0]['rel']}`" if fall else "-"
        print(f"| {target} | `{name}` | `{where}` | {reach} | {version} | {fb} | {verdict} |")
    print()

    print("## Effect files (`Effects/*.wfx`)")
    print()
    refs, present, requested = scan_effects()
    print(f"Present in the pack: {', '.join(f'`{p}`' for p in present) or 'none'}")
    print()
    print("`CShaderEffectManager::AddEffectFile` asks (`World.cpp:1704`): "
          + ", ".join(f"`{r}`" for r in requested))
    print()
    print("`.wfx` files define effects by name. The engine fetches those names through")
    print("`CShaderEffectManager::GetEffect`, so a `.wfx` that is present only helps for the names it")
    print("actually defines.")
    print()
    absent = [r for r in requested if r not in present]
    if absent:
        print(f"Missing from the pack: {', '.join(f'`{a}`' for a in absent)}")
    else:
        print("Nothing missing.")
    print()

    wfx_dir = os.path.join(SHADER_ROOT, "Effects")
    defined: set[str] = set()
    if os.path.isdir(wfx_dir):
        for name in os.listdir(wfx_dir):
            if name.lower().endswith(".wfx"):
                with open(os.path.join(wfx_dir, name), encoding="utf-8", errors="replace") as fh:
                    defined |= set(WFX_EFFECT.findall(fh.read()))

    print("| effect the engine asks for | defined by a `.wfx` in the pack |")
    print("|---|---|")

    lookups: dict[str, str] = {}
    for top in SOURCE_ROOTS:
        for dirpath, _dirs, files in os.walk(top):
            for name in files:
                if not name.endswith((".cpp", ".h")):
                    continue
                full = os.path.join(dirpath, name)
                with open(full, encoding="utf-8", errors="replace") as fh:
                    for lineno, line in enumerate(fh, 1):
                        for lit in GET_EFFECT.findall(line):
                            lookups.setdefault(lit, f"{full}:{lineno}")

    for name in sorted(lookups):
        state = "yes" if name in defined else "**no - absent from the pack**"
        print(f"| `{name}` | {state} |")
    print()

    if refs:
        print("Shaders referenced by the `.wfx` files:")
        print()
        print("| wfx | target | shader | on-chain file | version |")
        print("|---|---|---|---|---|")
        for target, name, wfx in sorted(refs):
            chain = candidates(entries, target, name, classic=False)
            if chain:
                state = f"`{chain[0]['rel']}`"
                version = f"0x{chain[0]['version']:X}"
            else:
                state = "**none**"
                version = "-"
            print(f"| `{wfx}` | {target} | `{name}` | {state} | {version} |")
        print()

    print("## Summary")
    print()
    print(f"- {len(entries)} files; "
          f"{len([e for e in entries if e['version'] == ACCEPTED_VERSION])} carry the accepted version 0x{ACCEPTED_VERSION:X}, "
          f"{len([e for e in entries if e['version'] != ACCEPTED_VERSION])} are a different container version.")
    print(f"- {len(entries) - len(reachable)} files sit in directories the loader never "
          f"probes unless the `Custom\\` prefix turns out to be stripped somewhere.")
    print(f"- {len(missing)} names the engine asks for have no file anywhere in the pack"
          + (":" if missing else "."))
    for target, name, where in missing:
        print(f"  - {target} `{name}` (`{where}`)")
    print(f"- {len(blocked)} names have a file on the chain but with a rejected container version"
          + (":" if blocked else "."))
    for target, name, rel, version in blocked:
        print(f"  - {target} `{name}` -> `{rel}` (0x{version:X})")
    print()

    print("## Full file list")
    print()
    print("| file | profile | version | header fields 1-5 | bytes |")
    print("|---|---|---|---|---|")
    for e in entries:
        fields = " ".join(f"0x{f:X}" for f in e["fields"])
        print(f"| `{e['rel']}` | {e['profile']} | 0x{e['version']:X} | {fields} | {e['size']} |")

    return 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("-o", "--out", help="write the report here as UTF-8 instead of stdout")
    args = parser.parse_args()

    if args.out:
        stream = io.open(args.out, "w", encoding="utf-8", newline="\n")
        original, sys.stdout = sys.stdout, stream
        try:
            code = main()
        finally:
            sys.stdout = original
            stream.close()
    else:
        code = main()

    sys.exit(code)