add_executable(ModernAssetsTest main.cpp)

target_include_directories(ModernAssetsTest PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(ModernAssetsTest PRIVATE ModernAssets)
