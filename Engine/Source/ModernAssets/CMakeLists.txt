file(GLOB_RECURSE PRIVATE_SOURCES CONFIGURE_DEPENDS "*.cpp")

add_library(ModernAssets STATIC
    ${PRIVATE_SOURCES}
)

target_include_directories(ModernAssets
    PUBLIC
        ${CMAKE_SOURCE_DIR}
)

# Reads files through the client's own archive set, so it needs BlizzardCore's SFile seam, and it
# reports through Storm's allocator-backed log helpers. Event carries DiagnosticLogsEnabled(), the
# WOWLOG_QUIET switch that decides whether the file sink opens.
target_link_libraries(ModernAssets
    PUBLIC
        BlizzardCore
        Storm
        Event
)
