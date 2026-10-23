file(GLOB_RECURSE PRIVATE_SOURCES CONFIGURE_DEPENDS "*.cpp")

add_library(SunGlow STATIC
    ${PRIVATE_SOURCES}
)

target_include_directories(SunGlow
    PUBLIC
        ${CMAKE_SOURCE_DIR}
)

target_link_libraries(SunGlow
    PRIVATE
        d3dcompiler.lib
)
