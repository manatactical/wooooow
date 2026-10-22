file(GLOB_RECURSE PRIVATE_SOURCES CONFIGURE_DEPENDS "*.cpp")

add_library(LiquidSystem STATIC
    ${PRIVATE_SOURCES}
)

target_include_directories(LiquidSystem
    PUBLIC
        ${CMAKE_SOURCE_DIR}
)

# The modern water pass replaces this module's shading, so the liquid materials ask it whether they are being drawn
# through it (see CMaterialProcWater::Render's depth-write decision).
target_link_libraries(LiquidSystem
    PRIVATE
        ModernWater
)
