# Third-party provenance: modern assets

The modern asset readers under `Engine/Source/ModernAssets/` are ported from
[WarcraftXL](https://github.com/WarcraftXL), licensed **GPL-3.0-or-later**, Copyright (C) 2026
WarcraftXL. That is the same license as this repository, so the ported code stays under it; the
original headers are retained in each file.

## What came from where

| Here | Upstream | Notes |
|---|---|---|
| `Db2/Db2Decode.{h,cpp}` | `wxl-db2` `src/decode/Db2Decode.cpp` | Same record model and layout rules. Bounds checks added on every offset, count, and pallet index; string columns can be declared from the row definition. |
| `Db2/Db2File.{h,cpp}` | `wxl-db2` `src/decode/DB2File.{hpp,cpp}` | Same indexing and string resolution. Reads through this client's `SFile` seam instead of the WXL storage interface. |
| `Db2/Wdc5.{h,cpp}` | `wxl-db2` `src/decode/Wdc5.{hpp,cpp}` | Same bitpacked/palletised/common-data decode. Bounds checks added on every offset, section, pallet and record size. |
| `Db2/Db2Table.{h,cpp}` | `wxl-db2` `src/api/Db2.{hpp,cpp}` | Same declarative Definition/relation model. Added a `LoadBytes` entry so the same definitions can be decoded from bytes already in memory. |
| `Db2/LightTables.{h,cpp}` | `wxl-db2` `src/schemas/LightStore.cpp` | The three table declarations and typed accessors. The day-curve evaluation, the per-frame cache and the WXL service glue are **not** ported. |
| `FdidResolver.{h,cpp}` | `wxl-db2` `src/api/FdidResolver.{hpp,cpp}` | Texture/model path resolution only. MaterialResourcesID resolution and the WXL service glue are not ported. |

| `M2/M2Modern.{h,cpp}` | `wxl-modern-m2` `load/*` | MD21 demux and the particle/camera normalisers. Implemented as an in-place repack rather than the binary-side hook set, because this client is source. |
| `M2/M2SkinContract.{h,cpp}` | `wxl-modern-m2` `compat/Skin.cpp` | The batch shaderId decode and lookup-table synthesis. The bone-budget split and draw-time fixups are not ported yet. |
| `Dbc/DbcMerge.{h,cpp}` | `wxl-extended-dbc` `src/*` | The `.dbcN-project` continuation merge, delivered as merged bytes for a memory-backed file rather than a storage transform. |
**Not ported yet:** the appearance / model-data schemas, the WXL service registration, the bone-budget
split, and the draw-time ribbon/emitter and hit-test fixups.

Still to come from the sibling project `wxl-modern-m2`: the MD21 container demux, per-era record
normalization, the offset→pointer walk, and the live-engine fixups (skin material contract rebuild,
bone-budget split, shadow animate gate, ribbon/emitter blending, hit-test ordering).

## Obtaining the originals

- https://github.com/WarcraftXL/wxl-db2
- https://github.com/WarcraftXL/wxl-modern-m2

Both are GPL-3.0-or-later. If this repository's license ever diverges from GPL-3.0-or-later, these
files must be removed or re-licensed with the upstream authors' permission.
