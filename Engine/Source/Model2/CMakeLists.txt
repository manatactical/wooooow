file(GLOB_RECURSE PRIVATE_SOURCES CONFIGURE_DEPENDS "*.cpp")

add_library(Model2 STATIC
    ${PRIVATE_SOURCES}
)

target_include_directories(Model2
    PUBLIC
        ${CMAKE_SOURCE_DIR}
)

# NOT IN BINARY, build-only: the modern-M2 repack lives in its own module, which this library calls at
# load. Conditional so a stock build links exactly the libraries it did before.
if (WOWDECOMP_MODERN_ASSETS)
    target_link_libraries(Model2
        PRIVATE
            ModernAssets
    )
endif ()
