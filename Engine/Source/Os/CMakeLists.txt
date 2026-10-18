file(GLOB_RECURSE PRIVATE_SOURCES CONFIGURE_DEPENDS "*.cpp")

add_library(Os STATIC
    ${PRIVATE_SOURCES}
)

target_include_directories(Os
    PUBLIC
        ${CMAKE_SOURCE_DIR}
)

if(WIN32)
    target_link_libraries(Os
        PUBLIC
            imm32
            wininet
            winmm
            ws2_32
            dinput8
            dxguid
            version
    )
endif()
