# manastorm

A fork of [wowdecomp](https://github.com/SatyPardus/wowdecomp), the C++ decompilation of the World of Warcraft
3.3.5a client (`Wow.exe`, build 12340), with modern rendering and quality-of-life features added on top.

The goal is simple: the 3.3.5a game you know, built from source for 64-bit Windows, looking and running better.

- Upstream community: [Discord](https://discord.gg/DsZMGUD8aT)
- Upstream project: [SatyPardus/wowdecomp](https://github.com/SatyPardus/wowdecomp)

## What this fork adds

Everything below is built into the client and configured from **System > Video** in game. No game data is
modified and nothing needs to be patched into the MPQs.

### Rendering

| Feature | What it does |
|---|---|
| Modern water | Replaces the stock liquid shaders with shaded, animated water: waves, ripples, foam, reflections and murky depth. Wave strength and wind are adjustable with the `waterWaves` and `waterWind` cvars. |
| Underwater effects | Blur, waves and depth fog when the camera is below the surface. |
| Ambient occlusion (SSAO) | Darkens creases, corners and contact points. Strength, spread and distance are adjustable, and grass and water are kept out of it. See [docs/AMBIENT-OCCLUSION.md](docs/AMBIENT-OCCLUSION.md). |
| Depth fog | Distance and height haze that hides the edge of the view distance, so far ridges fade into the sky. |
| Sun glow | A soft glow around the sun, dimmed when trees, buildings or mountains cover it. |
| FXAA anti-aliasing | Post-process anti-aliasing that leaves the interface sharp. Five quality levels, including a strong "Soothed" preset. |
| Cascaded shadows | A top-tier shadow mode with an adjustable cascade band range (`shadowBandRange`). |
| Up to 8 hardware lights | The stock client used 4. |
| Longer view distance | The far clip slider now goes up to 3000 yards. |
| Grass and wind | Much denser clutter (up to 12 times the stock maximum), swaying grass, and grass that parts around nearby characters. |
| Sharper text | Optional font supersampling for player names and combat text, with retail-style damage numbers. |

SSAO, depth fog and sun glow read the frame's depth buffer, so they are not applied while multisampling is
enabled.

### Gameplay and interface

| Feature | What it does |
|---|---|
| Unit outline | A reaction-coloured outline around your target and the unit under the cursor, fading out where it would otherwise paint the ground. |
| Loot beam | A beam of light over lootable corpses, tinted by the rarest item inside. It is a visual marker only and sends nothing to the server. |
| Reworked video options | Settings are grouped into categories, each with a description and a scrolling panel, including new Lighting, Physics and Debug pages. |
| Performance overlay | A small movable overlay showing frame rate, frame time, draw calls, triangles and CPU use. |
| No foreground FPS cap | The 200 FPS limit is gone while the window has focus. The background limit (`maxFPSBk`) is kept. |

### Retail content support (optional)

Build with `-DWOWDECOMP_MODERN_ASSETS=ON` to let the client read content from newer game versions: `.db2` tables
and modern `.m2` models. This includes retail mounts, with the animation and flight handling they need. It is off
by default, so the standard build is the plain 3.3.5a client. See
[docs/M2-ANIMATIONS.md](docs/M2-ANIMATIONS.md) and `Engine/Source/ModernAssets/README.md`.

### Stability and performance

- A built-in frame spike logger (see [Diagnosing stutters](#diagnosing-stutters)).
- A configurable multi-worker disk queue for asynchronous file loading.
- Fixes for texture, mip-level, bone-palette and device-reset crashes.
- 64-bit fixes for sizes, caches and queues.
- Quitting no longer waits for the server-alert download.

## Requirements

- Windows 10 or 11
- Visual Studio with the **Desktop development with C++** workload (MSVC and a Windows SDK)
- CMake 3.10 or newer (the one that ships with Visual Studio works)
- A World of Warcraft **3.3.5a (12340)** client. No game data is included in this repository.
- Win32 builds only: [NASM](https://www.nasm.us/) on `PATH`, and `DivxDecoder.dll` from your 3.3.5a client copied
  to `ThirdParty/DivxDecoder/`. The x64 build uses its own movie decoder and needs neither.

## Building

x64 is the main target. In a Developer PowerShell, or any shell with `cmake` on `PATH`:

```powershell
git clone https://github.com/manatactical/wooooow.git
cd wooooow

# configure once (use the generator that matches your Visual Studio)
cmake -S . -B build/x64 -G "Visual Studio 17 2022" -A x64

# build
powershell -NoProfile -ExecutionPolicy Bypass -File Tools/build.ps1
```

Always build with `Tools/build.ps1`. It writes the full log to `<buildDir>/<Target>-<Config>-build.log` and exits
non-zero if the log contains errors or no binary was produced, which a plain piped `cmake --build` does not.
Exit codes: `0` built, `1` cmake failed, `2` errors in the log, `3` no binary or unknown target.

In-source builds are refused, so keep the build directory separate.

The executables are written to `bin/Win/`:

| Target | x64 | Win32 |
|---|---|---|
| `WowClient` | `WowClient64.exe` | `WowClient32.exe` |
| `Wow` | `Wow64.exe` | `Wow32.exe` |

`WowClient` is the client with a crash logger. It opens a console window and writes `WowClient_crash.log` when the
client crashes. `Wow` is the same client without it.

### CMake options

| Option | Default | Effect |
|---|---|---|
| `WOWDECOMP_DISABLE_SIGNATURE_CHECKS` | `OFF` | Skip the MPQ and `.toc.sig` signature checks so modified game data and interface files load |
| `WOWDECOMP_MODERN_ASSETS` | `OFF` | Read modern `.db2` tables and `.m2` models |
| `WOWDECOMP_DEBUG_SYMBOLS` | `OFF` | Build PDBs so crash stacks show function names |
| `WOWCLIENT_CRASH_LOG` | `ON` | Build `WowClient` with the crash logger |
| `WOWCLIENT_OUTPUT_OVERRIDE` | empty | Give the `WowClient` executable a different name |

## Running

1. Copy the `Data` folder of your 3.3.5a client into `bin/Win/`, so you have `bin/Win/Data/`.
2. Point `Data/<locale>/realmlist.wtf` at the server you want to connect to.
3. Run `Tools/deploy-ui.ps1` to copy this fork's interface files (the new video options) into
   `bin/Win/Data/custom.MPQ/`. Close the client first. Building does not do this for you.
4. Start `bin/Win/WowClient64.exe` (or `Wow64.exe`).

Modified interface files need `-DWOWDECOMP_DISABLE_SIGNATURE_CHECKS=ON` unless you have a matching signature.

## Diagnosing stutters

Frame instrumentation is built in and off by default. Turn it on either way:

```powershell
# environment: the value is the spike threshold in milliseconds
$env:WOWLOG_SPIKES = 50; .\bin\Win\WowClient64.exe
```

```text
# or at runtime, from the console
perf on 50      # threshold in ms (default 50)
perf off        # stop; the running window is summarised first
perf dump       # write the rolling summary now
```

Output goes to `Logs\spikes.<pid>.log`:

- One line per frame at or over the threshold, with the worst phase, the worst measured span (`async-wait`,
  `present-d3d`, `m2-gc`, `tex-evict`, ...) and the frame's counters.
- A rolling summary every 5 seconds: frame p50/p95/p99/max, average engine work, spike count, and span totals.

Compare `frame` (the wall-clock gap between ticks) with `work` (time the engine spent running). If `work` is close
to `frame`, the engine is the cause. If `work` is small, the process was waiting on the driver, the disk or the
Windows message pump.

If the log shows `tex-budget-full` next to long `async-wait` spans, raise `asyncLoadBudget` (in MiB, `0` is the
original 4 MiB).

## About the upstream decompilation

Every function in wowdecomp is reconstructed from the original binary and keeps its original address in an
`// OFFSET:` tag, so it can be compared against `Wow.exe`. The source tree follows the original client layout.
Code added by this fork is marked `NOT IN BINARY`.

Measured against the 12340 binary on 2026-10-15 (upstream figures):

| | Ported | Total | Share of the original |
|---|---:|---:|---:|
| Functions | 24,515 | 27,157 | 90.3% |
| Code bytes | 5,270,186 | 5,656,573 | 93.2% |

These numbers show how much of the original binary has a counterpart in the source, not how much of the game
works. The client builds into a complete, playable executable. The untagged remainder is mostly compiler-generated
code, C runtime helpers and third-party libraries (FreeType, zlib, PCRE, parts of FMOD Ex) that are linked from
their upstream sources.

## Repository layout

| Path | Contents |
|---|---|
| `Engine/Source/` | Engine modules: Storm, Gx (Direct3D 9 / OpenGL), Model2, UI framework, sound, Lua, and the fork's render passes (`ModernWater`, `AmbientOcclusion`, `DepthFog`, `SunGlow`, `UnderwaterFx`, `AntiAlias`, `UnitOutline`) |
| `WoW/Source/` | Game modules: objects, world rendering, UI, networking, database, glue screens |
| `WoW/Common/` | Code shared by the game modules |
| `Data/custom.MPQ/Interface/` | Interface files, including the reworked video options |
| `Tools/` | Build and deploy scripts, the `WowClient` target, the x64 movie decoder |
| `ThirdParty/` | Libraries linked from their upstream sources |
| `docs/` | Design notes |

## Credits

- [SatyPardus/wowdecomp](https://github.com/SatyPardus/wowdecomp) and its contributors, for the decompilation this
  fork is built on
- [binana](https://github.com/thunderbrewhq/binana)
- [thunderbrew](https://github.com/thunderbrewhq/thunderbrew)
- [whoa](https://github.com/whoahq/whoa)
- TOMRUS, for the initial IDB
- The WoW modding community
