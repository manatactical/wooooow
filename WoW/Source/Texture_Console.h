#ifndef WOW_SOURCE_TEXTURE_CONSOLE_H
#define WOW_SOURCE_TEXTURE_CONSOLE_H

// Console diagnostics for textures: `texstat`, `texlist`, `texdump`, `texaudit`.
//
// They exist to separate the two halves of a texture bug that look identical on screen -- the client
// decoding a file wrong, and correct bytes being uploaded/drawn wrong. See Texture_Console.cpp.
void TextureRegisterCommands();

#endif
