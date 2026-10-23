file(GLOB_RECURSE PRIVATE_SOURCES CONFIGURE_DEPENDS "*.cpp")

add_library(SunShafts STATIC
    ${PRIVATE_SOURCES}
)

target_include_directories(SunShafts
    PUBLIC
        ${CMAKE_SOURCE_DIR}
)

target_link_libraries(SunShafts
    PRIVATE
        d3dcompiler.lib
)
