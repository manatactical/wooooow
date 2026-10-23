file(GLOB_RECURSE PRIVATE_SOURCES CONFIGURE_DEPENDS "*.cpp")

add_library(UnderwaterFx STATIC
    ${PRIVATE_SOURCES}
)

target_include_directories(UnderwaterFx
    PUBLIC
        ${CMAKE_SOURCE_DIR}
)

target_link_libraries(UnderwaterFx
    PRIVATE
        d3dcompiler.lib
)
