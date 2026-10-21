add_executable(ModernAssetsTest main.cpp)

target_include_directories(ModernAssetsTest PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(ModernAssetsTest PRIVATE ModernAssets)

# Throwaway analysis tool (see analysis/). Reads M2 sequence tables through the archive set.
add_executable(m2seq ../../analysis/m2seq.cpp)
target_include_directories(m2seq PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(m2seq PRIVATE ModernAssets)
