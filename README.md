# wowdecomp

A C++ decompilation of the World of Warcraft 3.3.5a client (`Wow.exe`, build 12340, June 2010).

Join the discord for other clients and to show off some cool stuff you build.

https://discord.gg/DsZMGUD8aT

Every function is reconstructed from the original binary and keeps the address it came from in an
`// OFFSET:` tag, so each one can be compared against the original. The source tree follows the layout
of the original client (`Engine/Source/<Module>`, `WoW/Source/<Module>`, original file names).

x64 is the main target. Win32 still builds and is used to check struct layouts against the 32-bit
original.

## Progress

The percentages below measure how much of the original binary has a counterpart in this source tree. They do
**not** say how much of the game works, and the missing part is not missing gameplay: the client builds into a
complete executable and the game is 100% playable. Bugs could still be hiding, though; a ported function can behave differently
from the original.

Measured against the 12340 binary on 2026-10-15. A function counts as ported when its address is
tagged in the source.

| | Ported | Total | Share of the original |
|---|---:|---:|---:|
| Functions | 24,515 | 27,157 | 90.3% |
| Code bytes | 5,270,186 | 5,656,573 | 93.2% |

The total leaves out 658 functions that IDA identifies as compiler library code and 132 import thunks.
Most of the untagged remainder is code the build gets elsewhere: compiler-generated code (static
initializers, template instances) that our compiler emits on its own, C runtime helpers, and third-party
libraries that are linked from their upstream sources instead of being decompiled (FreeType, zlib, PCRE,
parts of FMOD Ex).

## Requirements

- Windows 10 or 11
- Visual Studio 2022 with the **Desktop development with C++** workload (MSVC v143 and a Windows SDK)
- CMake 3.10 or newer (the one that ships with Visual Studio works)
- For Win32 builds only: [NASM](https://www.nasm.us/), on `PATH` or passed as `-DNASM_EXECUTABLE=<path>`, and
  `DivxDecoder.dll` from your 3.3.5a client copied to `ThirdParty/DivxDecoder/` (the build copies it next to the
  executables; the x64 build uses its own decoder instead)
- A World of Warcraft **3.3.5a (12340)** client. No game data is included in this repository.

## Building

From a Developer PowerShell or any shell with `cmake` on `PATH`. The wrapper builds and **fails loudly** — a
raw piped `cmake --build` swallows the error lines and the exit code, so a broken build looks green:

```powershell
git clone https://github.com/SatyPardus/wowdecomp.git
cd wowdecomp

# x64 (main target)
cmake -S . -B build/x64 -G "Visual Studio 17 2022" -A x64
powershell -NoProfile -ExecutionPolicy Bypass -File Tools/build.ps1

# Win32 (optional, needs NASM)
cmake -S . -B build/win32 -G "Visual Studio 17 2022" -A Win32
powershell -NoProfile -ExecutionPolicy Bypass -File Tools/build.ps1 -Target WowClient -BuildDir build/win32
```

`Tools/build.ps1` defaults to WowClient / Release / `build/x64`, tees the full log to
`<buildDir>/<Target>-<Config>-build.log`, scans it for the failure markers above and exits non-zero on any
error or a missing binary (`0` built, `1` cmake failed, `2` errors in the log, `3` no binary / unknown target).
Without the wrapper, run cmake directly and check `$LASTEXITCODE` **before** any pipe. See AGENTS.md.

In-source builds are refused, so keep the build directory separate (`build/` above).

The client is written to `bin/Win/`:

| Target | x64 | Win32 |
|---|---|---|
| `WowClient` | `WowClient64.exe` | `WowClient32.exe` |
| `Wow` | `Wow64.exe` | `Wow32.exe` |

`WowClient` is the client with a crash logger: it opens a console window and writes
`WowClient_crash.log` when the client crashes. `Wow` is the same client without it. You can also turn the
logger off in `WowClient` with `-DWOWCLIENT_CRASH_LOG=OFF`.

### Options

| CMake option | Default | Effect |
|---|---|---|
| `WOWDECOMP_DISABLE_SIGNATURE_CHECKS` | `OFF` | Skip the MPQ file and `.toc.sig` signature checks, so modified game data and interface files load |
| `WOWCLIENT_CRASH_LOG` | `ON` | Build `WowClient` with the crash logger |
| `WOWCLIENT_OUTPUT_OVERRIDE` | empty | Name the `WowClient` executable something other than `WowClient64` / `WowClient32` |

## Running

1. Copy the `Data` folder of your 3.3.5a client into `bin/Win/`, so you have `bin/Win/Data/`.
2. Point `Data/<locale>/realmlist.wtf` at the server you want to connect to.
3. Start `bin/Win/WowClient64.exe` (or `Wow64.exe`).

### Diagnosing stutters

Frame instrumentation is built in and off by default. Turn it on either way:

```powershell
# environment: value is the spike threshold in milliseconds
$env:WOWLOG_SPIKES = 50; .\bin\Win\WowClient64.exe
```

```text
# or at runtime, from the console
perf on 50      # threshold in ms (default 50)
perf off        # stop; the running window is summarised first
perf dump       # write the rolling summary now
```

Output is `Logs\spikes.<pid>.log`:

- one line per frame at or over the threshold, with the worst phase or event, the worst measured span
  (`async-wait`, `present-d3d`, `fps-cap`, `m2-gc`, `tex-evict`, ...) and the frame's counters;
- a rolling summary every 5 s: frame p50/p95/p99/max, average engine work, spike count, per-span totals
  and the counters (`tex-evicted`, `tex-budget-full`, `m2-gc`, ...).

Read it top-down: `frame` is the wall-clock gap between ticks, `work` is how much of it the engine spent
running. A spike with `work` close to `frame` is the engine; a spike with `work` small is the process
waiting (driver, disk, or the Windows message pump).

`asyncLoadBudget` (MiB, `0` = original 4 MiB) raises the async texture upload budget if the log shows
`tex-budget-full` next to long `async-wait` spans.

## Layout

| Path | Contents |
|---|---|
| `Engine/Source/` | Engine modules: Storm, Gx (Direct3D 9 / OpenGL), Model2, Frame/FrameXML UI, SoundEngine, Lua, ... |
| `WoW/Source/` | Game modules: objects, world rendering, UI, networking, database, glue screens |
| `WoW/Common/` | Code shared by the game modules |
| `ThirdParty/` | Libraries linked from their upstream sources |
| `Tools/WowClient/` | The `WowClient` executable target |
| `Tools/XvidDecoder/` | x64 movie decoder that replaces the original 32-bit `DivxDecoder.dll` |

## Special thanks

- [binana](https://github.com/thunderbrewhq/binana)
- [thunderbrew](https://github.com/thunderbrewhq/thunderbrew)
- [whoa](https://github.com/whoahq/whoa)
- TOMRUS, for the initial IDB
- Everyone who has reported bugs so far
- The WoW modding community
