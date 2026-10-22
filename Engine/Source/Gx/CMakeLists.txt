file(GLOB_RECURSE PRIVATE_SOURCES CONFIGURE_DEPENDS "*.cpp")

if (NOT WIN32)
    list(FILTER PRIVATE_SOURCES EXCLUDE REGEX "/CGxDevice(D3d|D3d9Ex|OpenGL|D3d11)/")
endif ()

# D3d11 is the client's rendering API. The D3d (D3D9), D3d9Ex and OpenGl backends are retired: nothing
# selects them any more -- GxDefaultApi()/GxApiSupported() in Gx.cpp expose D3d11 only -- and they are still
# written against the old flat CGxShader model that CGxShaderPermute replaced. Keeping them would mean
# maintaining three dead implementations of every shader entry point.
list(FILTER PRIVATE_SOURCES EXCLUDE REGEX "/CGxDeviceD3d/")
list(FILTER PRIVATE_SOURCES EXCLUDE REGEX "/CGxDeviceD3d9Ex/")
list(FILTER PRIVATE_SOURCES EXCLUDE REGEX "/CGxDeviceOpenGL/")

add_library(Gx STATIC
    ${PRIVATE_SOURCES}
)

target_include_directories(Gx
    PUBLIC
        ${CMAKE_SOURCE_DIR}
)

target_link_libraries(Gx
    PRIVATE
        version.lib
)

if (WIN32)
    target_link_libraries(Gx
        PRIVATE
            nvapi
            opengl32.lib
            d3d11.lib
            dxgi.lib
            # The D3d11 backend calls D3DPERF_SetOptions (a PIX marker from the D3D9 SDK) and includes d3d9.h,
            # so the import library is still needed even though the D3D9 render backend is gone.
            d3d9.lib
    )
endif ()
