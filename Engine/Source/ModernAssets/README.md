# Modern assets

Reads modern-client content the stock 3.3.5a client cannot: `.db2` database tables and, once that
lands, the modern M2 model format. Everything here is behind one CMake option, **off by default**, so a
stock build is exactly the code it was before.

```
cmake -S . -B build/x64 -DWOWDECOMP_MODERN_ASSETS=ON
```

## Why

Modern content stopped naming most assets by path. A texture or a model is a number — a FileDataID —
and a database table maps that number to the file it actually is. The stock client has no such table
and no reader for the format those tables ship in, so a modern asset is not something it rejects
politely; it is a file the client cannot open at all.

This module reads those tables directly, through the client's own already-mounted archive set. Nothing
is converted on the host, nothing is written to disk, and the archives are the only authority — the
same ones the client loads every other asset from.

## What is here now

| Piece | State |
|---|---|
| WDC1 / WDC2 / WDC3 tables (`Db2/Db2Decode.h`, `Db2/Db2File.h`) | done |
| WDC5 declarative tables (`Db2/Wdc5.h`, `Db2/Db2Table.h`) | done |
| FileDataID → path resolution (`FdidResolver.h`) | done |
| Lighting tables on top of WDC5 (`Db2/LightTables.h`) | done |
| Modern M2 (MD21) load (`M2/M2Modern.h`) | done |
| Modern skin contract rebuild (`M2/M2SkinContract.h`) | done |
| Add-on DBC rows (`Dbc/DbcMerge.h`) | done |

## Verified against a running client

Not only offline: models were loaded through the client's real `CreateModel` path with the archives
mounted, and the results logged.

```
modernm2: 'Creature\ancientmount\ancientmount.m2'             repacked: skinBatches=9 sections=9 loaded=yes
modernm2: 'Creature\alliancewolfmount2\alliancewolfmount2.m2' repacked: skinBatches=3 sections=3 loaded=yes
modernm2: 'Creature\ardenwealdstagmount\ardenwealdstagmount.m2' stock load: skinBatches=4 sections=2
```

The third is a plain MD20 and was correctly left alone. Note `visible=0` on all three: a freshly
created model has every geoset hidden, so the visibility-counting accessors report zero for a perfectly
good model — the skin's own batch count is the figure that means anything.

`Logs\modern-assets.log` from the same session:

```
db2: loaded TextureFilePath.db2 (867690 rows, id 116922..8514911)
db2: loaded ModelFilePath.db2 (602631 rows, id 106679..8514101)
fdid: texture paths=867690 model paths=602631 (model stems=601532)
```

## Add-on DBC rows

A content pack does not replace the client's tables, it *adds* to them: it ships
`CreatureModelData.dbc1-mana` beside `CreatureModelData.dbc`, and the client layers the extra rows on
top. Without that merge the rows simply do not exist, so a creature points at a display id the client
has no model name for -- and the pack's own models, however well they convert, are never asked for.

`Dbc/DbcMerge.h` does the merge at table-open time, then hands the stock loader an `SFileImpl` that
reads the merged bytes from memory, so nothing below it changes. On the reference pack:

```
CreatureDisplayInfo.dbc   merged 77 appended,   0 replaced
CreatureModelData.dbc     merged 77 appended,   0 replaced
CreatureSoundData.dbc     merged  0 appended, 1306 replaced
Item.dbc                  merged 78 appended,   0 replaced
ItemDisplayInfo.dbc       merged 75 appended,   0 replaced
Spell.dbc                 merged 76 appended,   3 replaced
SpellIcon.dbc             merged 27671 appended, 0 replaced
```

A continuation must match the base table's field count and row size or it is skipped with a reason in
the log, and a row whose id already exists replaces it (later tiers win).

## Verified offline

| Table | Family | Result |
|---|---|---|
| `TextureFilePath.db2` | WDC1 | 867,690 rows |
| `ModelFilePath.db2` | WDC1 | 602,631 rows |
| `Light.db2` | WDC5 | 5,355 rows |
| `LightData.db2` | WDC5 | 19,866 rows |
| `LightParams.db2` | WDC5 | 3,738 rows (27 of 32 columns palletised or bitpacked) |
| 70 creature models | MD20/MD21 | 33 modern repacked, 37 stock untouched, 0 invalid |
| 120 corrupted models | MD21 | 0 crashes; 83 refused, 37 still valid |

`Light.db2` decodes byte-for-byte to what the raw file holds: light 2 comes out as
`(-10666.7, 64.0, 0.0)` with falloff `399.474..466.978`, matching a direct read of the record.

## Two pre-existing crashes modern models exposed

Both were already in the decompiled client and neither is in this module; modern content is simply the
first thing to reach them.

| Where | What |
|---|---|
| `CM2Shared::SubstituteSimpleShaders` | `uint32_t ops[2]` indexed by a batch's texture count. A batch with more than two textures wrote past it — caught as stack corruption in Debug, silent in Release. Sized to 8 and the loop bounded. |
| `CM2Shared::GetEffect` | A packed `shaderId` whose low 15 bits exceed the three effects the switch knows left both shader names null, and the following `strcpy` faulted. Now returns null, which the caller already tolerates. |

Both were found by running the client, not by reading the code. They are why the modern path is worth
testing against real content rather than only synthetic fixtures.

### The decoder

The store is not a flat row array. A field may be inline, bitpacked, a per-id override of a default,
a pallet index, or a pallet index that expands to an array, and a table may be split across sections.
Each record is reconstructed into a native-width layout with natural C alignment,

```
[id u32] [each field in file order at its native width] [relationship id u32, if any]
```

so a row struct mirrors the table fields exactly: a `uint16` field stays 2 bytes, an array stays
consecutive elements, and a string stays an offset into the rebuilt string table (`DB2File::Str`).

The upstream reference decoder this was ported from reads those offsets and pallet indices on trust.
Every one of them is bounds-checked here instead: a truncated, short or hostile file fails the decode
and is logged, and never reads outside the buffer it was handed. `Tools/ModernAssetsTest` builds a
synthetic table per storage type and feeds it, a truncated copy of it, and junk, and asserts the
outcomes — so a regression is caught without a game install.

### WDC5, and why it needs a definition

The later revision has no self-describing records: the row is a bitpacked blob, and a column may be
inline, bitpacked, common-data backed, a pallet index, or a pallet index that expands to an array.
Nothing in the file says which field is which, so the caller declares the layout:

```cpp
constexpr modern::db2::Field kFields[] = {
    { "GameCoords", 3 }, { "GameFalloffStart" }, { "GameFalloffEnd" },
    { "ContinentID" }, { "LightParamsID", 8 },
};

modern::db2::Definition def{ "light", "light.db2", 0, std::vector<modern::db2::Field>(std::begin(kFields), std::end(kFields)) };
modern::db2::Table table;
if (table.Load(def, &error))
    uint32_t continent = table.Value(row, "ContinentID");
```

Every physical field must be declared, including ones nothing reads yet: the reader cross-checks the
declared count against the file, and a missing column would silently shift every field after it. A
population `layoutHash` can be passed instead of `0` to refuse a file whose layout has moved on.

`Db2/LightTables.h` is the worked example: three real retail tables (5, 47 and 32 fields) declared and
exposed with typed accessors, including the Light → LightData relationship. It is the first consumer of
the declarative layer, and it is what the tables above were verified through.

### FileDataID resolution

Backed by `TextureFilePath.db2`, `ModelFilePath.db2` and `TextureFileData.db2`: an id is looked up in
the texture table, then the model table (one FileDataID space covers both). Resolved paths are cached
and stay valid for the process lifetime, which is what lets one be stored straight into a model's
texture record.

`TextureFileData` also maps a `MaterialResourcesID` to a path, which is how a modern material — which
is itself just an id — becomes a `.blp` the client can open.

## Using it

Two console commands are registered when the option is on, so the readers can be exercised in a
running client rather than only through whatever consumes them first:

```
moderninfo            what loaded, and how many rows each table has
modernpath <id>       resolve one FileDataID to the path the client would open
modernlight [id]      report the lighting tables, or one light point and its day curve
```

Offline, without a mounted archive set:

```
ModernAssetsTest                                  self-check (synthetic WDC1/WDC3/WDC5, hostile input)
ModernAssetsTest <file.db2>                       decode one table
ModernAssetsTest lighttables <light> <data> <params>
                                                  decode the three lighting tables from loose files
```

Details go to `Logs\modern-assets.log`.

From C++:

```cpp
#include "Engine/Source/ModernAssets/FdidResolver.h"

if (const char* path = modern::db2::fdid::ResolveTexture(fileDataId))
    /* that is the client path for this id */;
```

## Provenance

The DB2 decoder and the FileDataID resolver are ported from
[WarcraftXL](https://github.com/WarcraftXL) (`wxl-db2`), GPL-3.0-or-later, Copyright (C) 2026
WarcraftXL. See `ThirdParty/README-modern-assets.md`. They are adapted to read through this client's
own `SFile` seam and to validate every offset the original trusted.
