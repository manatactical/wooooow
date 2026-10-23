file(GLOB_RECURSE PRIVATE_SOURCES CONFIGURE_DEPENDS "*.cpp")

add_library(DepthFog STATIC
    ${PRIVATE_SOURCES}
)

target_include_directories(DepthFog
    PUBLIC
        ${CMAKE_SOURCE_DIR}
)

target_link_libraries(DepthFog
    PRIVATE
        d3dcompiler.lib
)
