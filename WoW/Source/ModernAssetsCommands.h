#ifndef WOW_SOURCE_MODERNASSETS_MODERNASSETSCOMMANDS_H
#define WOW_SOURCE_MODERNASSETS_MODERNASSETSCOMMANDS_H

// Console commands for the modern-asset readers, registered only when the build enables
// WOWDECOMP_MODERN_ASSETS. They exist so the readers can be exercised and observed in a running
// client instead of only through whatever consumes them first: `moderninfo` reports what loaded and
// `modernpath <fileDataId>` resolves one id to the path the client would open for it.
void ModernAssetsRegisterCommands();

#endif
