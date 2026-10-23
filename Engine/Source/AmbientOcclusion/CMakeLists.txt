file(GLOB_RECURSE PRIVATE_SOURCES CONFIGURE_DEPENDS "*.cpp")

add_library(AmbientOcclusion STATIC
    ${PRIVATE_SOURCES}
)

target_include_directories(AmbientOcclusion
    PUBLIC
        ${CMAKE_SOURCE_DIR}
)

target_link_libraries(AmbientOcclusion
    PRIVATE
        d3dcompiler.lib
)