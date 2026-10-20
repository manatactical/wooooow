file(GLOB_RECURSE PRIVATE_SOURCES CONFIGURE_DEPENDS "*.cpp")

add_library(AntiAlias STATIC
    ${PRIVATE_SOURCES}
)

target_include_directories(AntiAlias
    PUBLIC
        ${CMAKE_SOURCE_DIR}
)

target_link_libraries(AntiAlias
    PRIVATE
        d3dcompiler.lib
)