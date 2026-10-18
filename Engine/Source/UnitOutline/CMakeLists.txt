file(GLOB_RECURSE PRIVATE_SOURCES CONFIGURE_DEPENDS "*.cpp")

add_library(UnitOutline STATIC
    ${PRIVATE_SOURCES}
)

target_include_directories(UnitOutline
    PUBLIC
        ${CMAKE_SOURCE_DIR}
)

target_link_libraries(UnitOutline
    PRIVATE
        d3dcompiler.lib
)