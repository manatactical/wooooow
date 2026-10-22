add_executable(ModernAssetsTest main.cpp)

target_include_directories(ModernAssetsTest PRIVATE ${CMAKE_SOURCE_DIR})
# Event for DiagnosticLogsEnabled(), which ModernAssetsLog.cpp reads to decide whether to open its file.
# Event pulls PerfLog.cpp, which needs OsGetAsyncTimeMs() from Os, so that comes along too.
target_link_libraries(ModernAssetsTest PRIVATE ModernAssets Event Os)
