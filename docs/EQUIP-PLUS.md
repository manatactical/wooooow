# Equip Plus

3D models on every armour slot, driven by data. A port of the WarcraftXL extension
[wxl-equip-plus](https://github.com/TheDaveCalaz/wxl-equip-plus) (GPL-3.0-or-later) into the client itself.

A stock 3.3.5a client draws a model for the head, the shoulders and held items. Chest, belt, legs, boots, gloves,
bracers, cloaks, shirts and tabards are textures painted on the body. With Equip Plus an `ItemDisplayInfo` row, or a row
in a CSV file, can attach up to two M2 models to any of those slots, including retail "collection" pieces: one model
file that serves a whole set, filtered to the geosets a slot wants and skinned to the character's own skeleton.

Credit: [Morfium-G](https://github.com/Morfium-G/wxl-equip-extension) wrote the original, [Atraxian123](https://github.com/Atraxian123/WXL-Equip-Creature-Extension-CSV-WXL11)
ported it to the 1.1 extension ABI and added weapons, creatures, CSV sidecars and animation sync,
[TheDaveCalaz](https://github.com/TheDaveCalaz/wxl-equip-plus) maintains it, [Furioz420](https://github.com/Furioz420) wrote the CSV sidecar
implementation. The upstream README is the reference for the CSV columns and the `Icon2` format; this page covers only what
differs here.

## Using it

Nothing to install. Put the data in the client and equip the item.

| What | Where |
|---|---|
| Models (`.m2`/`.mdx`) and textures (`.blp`) | `Item\ObjectComponents\<Folder>\` inside any patch MPQ or open-folder patch |
| `ItemDisplayInfo.dbc` rows | `ModelName_1/2`, `ModelTexture_1/2` and `Icon2` as the upstream README describes |
| `WXLItemDisplayModels.csv`, `WXLItemDisplayModelMaterials.csv`, `WXLEquipAnimations.csv`, `WXLHelmOffsets.csv` | `DBFilesClient\` |
| `WXLWeaponModels.csv`, `WXLWeaponTextures.csv` | `DBFilesClient\` |
| `WXLCreatureModels.csv`, `WXLCreatureTextures.csv` | `DBFilesClient\` |

Each CSV is read from the archive the client resolves, from a loose copy beside the executable, and from
`Data\*.MPQ\DBFilesClient\` of every open-folder patch, so several patches can each add rows. A row that repeats an earlier one
adds nothing.

Console:

| | |
|---|---|
| `equipplus` | on or off, and how many rows each CSV gave |
| `equipplus reload` | read the CSV files again. Items already worn change when they are equipped again |
| cvar `equipPlus` | 0 turns the whole system off, 1 (default) on |
| cvar `equipPlusLog` | 1 writes `EquipPlus.log` beside the executable: every model attached, skipped or refused, and why |
| cvar `equipPlusPivots` | 1 moves each part of a retail piece to the matching bone of the client body (Tauren especially) |
| cvar `equipPlusScale` | 1 also sizes a retail piece to the client skeleton |

## How it works here

The extension hooks the 32-bit client and rewrites each model's bytes as they leave the archive. Inside the client's source
none of that is needed. The model API already has per-geoset visibility, texture replacement by type and attachment, so the
code decides what to attach (the same rules as the extension) and attaches it.

- `WoW/Common/ComponentCore/EquipPlus.cpp` is the port. `CCharacterComponent::AddItem` and `RemoveItem` call it, so it covers the
  player, other players, NPCs wearing gear, the character screens and the dress-up window alike.
- `Engine/Source/Model2/M2BoneFollow.cpp` is the one thing the model API lacked: a model whose bones follow another model's. Each
  frame, after the model has animated, its bone matrices are replaced by the matching bone matrices of the model it is attached to.
  Bones are matched by name CRC, then key bone id, then the parent's id table, then a bone with no match takes its parent's.
  Fit options (`equipPlusPivots`, `equipPlusScale`) and a `WXLHelmOffsets.csv` offset are folded into the copied matrix.
  A model can also follow the parent's animation, started when the parent starts a sequence (`WXLEquipAnimations.csv`).
- `CM2Scene::s_resolveModelPath` lets a creature display name a model that is not a file (see below).

Head and shoulders keep the client's own code unless the display opts in (a CSV row for it, a `name:geosets` collection name,
or an `Icon2` setting). A helm Equip Plus draws still hides the hair and face geosets the helm covers.

## Differences from the WarcraftXL extension

Not ported:

- **Live physics** (`src/physics`, the `phy2` data and the `[Physics]` and `[Fit]` ini keys). Plumes, ribbons and chains stand in their
  modelled pose. The fit options are cvars instead of ini keys.
- **The F9 overlay panel.** `equipplus` and `equipplus reload` in the console do its job.
- **Offhand auto-mirror** (`NoAutoMirror`/`AutoMirror`). A weapon's `Model1OffhandPath` is used; a model is never flipped.
- The `[Memory]` and `[EagerPreload]` ini keys. Nothing is baked, so there is no cache to cap.
- The Backport Helper and the converter scripts. They write the same files; use them from the upstream repository.

Changed:

- **Geoset filter** hides everything not listed, including geoset 0, exactly as the extension's code did (its README says 0 is always
  kept; its code does not). Weapon and creature sheets keep 0 implied, as theirs do.
- **Creatures.** `CreatureModelData` names a display's model with its display id appended (`model_33113.mdx`). That name is not a file;
  `CreateModel` maps it to the `CreatureModelPath` of `WXLCreatureModels.csv` when the name before the id matches. The geoset list and
  textures are applied to each model instance, so displays that share a model share its file.
- **Weapons.** A row in `WXLWeaponModels.csv` replaces the weapon the client just attached: `Model1Path` (and `Model2Path` for an effect
  model on the same attachment), geosets per model, textures of any type from `WXLWeaponTextures.csv`. The off hand uses
  `Model1OffhandPath` when given. `ItemModelData.dbc` no longer needs patching to virtual names.
- **Helm offsets** are the model's own transform (a root-bone translation and scale in the extension).

## Known limits

- A plain (non-collection) glove model hangs from the hand attachment, which the client also uses for the weapon, so equipping a weapon
  takes it down. Use a collection piece for gloves, as retail does.
- Not tested in game against retail collection data: this was built and started, but the bone follow, fit options and animation sync
  have not been seen on a character wearing real pieces.

## Trying it without retail data

Put these in `Data\custom.MPQ\DBFilesClient\WXLItemDisplayModels.csv`, with `<id>` the `ItemDisplayInfo` id of a belt you wear (look
the belt up in `ItemDisplayInfo.dbc` by its model texture or icon):

```csv
DisplayID,ItemID,Slot,Layer,Side,Folder,Model,Texture,Geosets,Attach,SuffixPolicy,TexturePolicy,Flags
<id>,,Waist,Base,C,Shoulder,LShoulder_Plate_A_01.mdx,,,53,Exact,Exact,
```

Set `equipPlusLog` to 1, equip the belt, and `EquipPlus.log` says `attached 'Item\ObjectComponents\Shoulder\LShoulder_Plate_A_01.mdx'`
(or why not). A pauldron then hangs from the belt buckle.
