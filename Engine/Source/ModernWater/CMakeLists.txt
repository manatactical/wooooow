# Modern water: the engine-side counterpart of the WXL extension module wxl-modern-water (Modern Water).
# See ModernWater.h for what it does and why it lives here instead of in a DLL.

# fxc is needed at build time only; the bytecode is baked into generated headers.
include(${CMAKE_CURRENT_SOURCE_DIR}/shaders.cmake)

file(GLOB PRIVATE_SOURCES CONFIGURE_DEPENDS "*.cpp")

add_library(ModernWater STATIC
    ${PRIVATE_SOURCES}
)

target_include_directories(ModernWater
    PUBLIC
        ${CMAKE_SOURCE_DIR}
    PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}
)

target_link_libraries(ModernWater
    PRIVATE
        Gx
        Model2
        Tempest
)

add_dependencies(ModernWater ModernWaterShaders)
