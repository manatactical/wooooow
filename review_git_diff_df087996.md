## Git Diff Review

**2 critical, 3 warnings, 2 suggestions**

🔴 **2 Critical**

- `Engine/Source/BLPFile/blp.cpp:958` - The new cleanup uses CalcLevelCount() on the full header dimensions, but this in-place path is called from PumpBlpTexture() with s_mipBits sized from the possibly bestMip-reduced width/height (Engine/Source/Services/Texture.cpp:1997). For textures larger than the device limit, levelCount here is larger than the allocated MipBits table, so the zeroing loop writes past the table into the scratch buffer (and potentially past the allocation). Size the scratch for the full header chain or bound the loop by the known allocation capacity.
- `Engine/Source/Services/Texture.cpp:893` - The added `&& bits` only guards the cube-map pointer arithmetic. The resulting null texels are still handed to the Gx backend, and CGxDeviceOpenGl::ITexMarkAsUpdated calls ITexDownload without a null check (CGxDeviceOpenGl.cpp:4374/4145), where `bits` is dereferenced in Blit/glTexImage2D. The D3d backend checks `if (texels ...)`, but the OpenGL backend does not, so clearing missing mip levels to null can turn the old dangling-pointer bug into a deterministic null dereference. Add a null check in the OpenGL upload path or avoid emitting null levels.

🟡 **3 Warnings**

- `Engine/Source/BLPFile/blp.cpp:951` - The first loop still writes images[0..15] whenever the file's m_mipSizes entries are nonzero, without bounding by CalcLevelCount(). A malformed header with a small width/height but nonzero entries in all 16 mip-size slots writes past a MipBits table allocated for only levelCount entries; the new cleanup loop does not protect against this.
- `Engine/Source/BLPFile/blp.cpp:960` - The null-fill is capped at 16, but CalcLevelCount() can legitimately return more than 16 levels for large header dimensions. Levels 16..levelCount-1 therefore remain stale, so the dangling-pointer problem this change targets is only partially fixed for extreme (or hostile) dimensions.
- `Engine/Source/Services/Texture.cpp:1906` - Same null-handling issue exists in the TGA path: the guard prevents pointer arithmetic on null, but the null texels are still passed down to backend code that may not tolerate them (notably the OpenGL upload path).

🔵 **2 Suggestions**

- `Engine/Source/Gx/CMakeLists.txt:8` - This exclusion is unconditional and matches a directory that does not currently exist. It silently drops any future CGxDeviceD3d11 sources even after the backend is fixed; prefer an explicit option (e.g. enable/disable the backend) or remove the stale filter.
- `docs/M2-ANIMATIONS.md:202` - The m2seq build target was removed because analysis/m2seq.cpp is absent, but the documentation still lists `analysis/m2seq.cpp` as an available tool. Update the reference so the docs match the removed target.

**Summary:** The BLP changes address a real stale-mip-pointer problem, but the cleanup derives its bound from full header dimensions while the shared scratch may be allocated for smaller bestMip-reduced dimensions, risking out-of-bounds writes. The resulting null mip pointers are also not handled by the OpenGL backend, so the fix can still crash there. The build-file changes are mostly fine but leave stale documentation and an overly broad permanent D3d11 exclusion.
