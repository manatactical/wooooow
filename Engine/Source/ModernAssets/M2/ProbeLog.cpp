#include <cstdint>
#include <cstdio>

#include "Engine/Source/ModernAssets/ModernAssetsLog.h"

void ProbeLogMount(const char* message)
{
    if (message) MODERN_ASSETS_LOG_INFO("%s", message);
}
