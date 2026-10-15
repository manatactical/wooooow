#ifndef WOW_SOURCE_UI_VIDEOOPTIONS_H
#define WOW_SOURCE_UI_VIDEOOPTIONS_H

#include <cstddef>
#include <cstdint>

class CGVideoOptions {
    public:
    static void RegisterScriptFunctions();
    static void UnregisterScriptFunctions();
};

// CVars the Advanced video panel expects but this client never registered. See VideoOptions_CVar.cpp.
namespace VideoOptions {
void RegisterClientCVars();
}

#endif