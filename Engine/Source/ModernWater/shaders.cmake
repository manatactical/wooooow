# The modern water shaders. Same compile line the WXL module uses (module.cmake): fxc, /O3, ps_3_0, one
# generated header per pass, included by the water renderer by name.
#
# fxc is a build tool, not a runtime dependency: the D3D9 bytecode ends up in the header as a static array, so
# the client never compiles HLSL at runtime and does not need d3dcompiler for any of this.

file(GLOB MODERNWATER_FXC_CANDIDATES
    "$ENV{ProgramFiles\(x86\)}/Windows Kits/10/bin/10.*/x64/fxc.exe"
    "$ENV{ProgramFiles\(x86\)}/Windows Kits/10/bin/10.*/x86/fxc.exe")
if (MODERNWATER_FXC_CANDIDATES)
    list(SORT MODERNWATER_FXC_CANDIDATES COMPARE NATURAL ORDER DESCENDING)
    list(GET MODERNWATER_FXC_CANDIDATES 0 MODERNWATER_FXC_DEFAULT)
else ()
    set(MODERNWATER_FXC_DEFAULT "")
endif ()
set(MODERNWATER_FXC "${MODERNWATER_FXC_DEFAULT}" CACHE FILEPATH "HLSL compiler (fxc.exe) for the modern water shaders")

if (NOT EXISTS "${MODERNWATER_FXC}")
    message(FATAL_ERROR "ModernWater needs fxc.exe from the Windows SDK; set -DMODERNWATER_FXC=<path>")
endif ()

set(MODERNWATER_SHADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/shaders")
set(MODERNWATER_SHADER_OUT "${CMAKE_CURRENT_BINARY_DIR}/modernwater-shaders")
file(MAKE_DIRECTORY ${MODERNWATER_SHADER_OUT})

set(MODERNWATER_INCLUDES
    ${MODERNWATER_SHADER_DIR}/vf_common.hlsli
    ${MODERNWATER_SHADER_DIR}/vw_constants.hlsli
    ${MODERNWATER_SHADER_DIR}/vw_surface.hlsli
    ${MODERNWATER_SHADER_DIR}/vw_optics.hlsli
    ${MODERNWATER_SHADER_DIR}/vw_reflection.hlsli
    ${MODERNWATER_SHADER_DIR}/vw_fft_common.hlsli)

set(MODERNWATER_SHADER_HEADERS "")

# modemwater_shader(<header name> <source> <profile> [defines...])
function(modemwater_shader name source profile)
    set(out ${MODERNWATER_SHADER_OUT}/${name}.h)
    add_custom_command(
        OUTPUT ${out}
        COMMAND "${MODERNWATER_FXC}" /nologo /O3 /T ${profile} /E main /Vn g_${name} ${ARGN}
                /Fh ${out} /Fc ${MODERNWATER_SHADER_OUT}/${name}.asm ${MODERNWATER_SHADER_DIR}/${source}
        DEPENDS ${MODERNWATER_SHADER_DIR}/${source} ${MODERNWATER_INCLUDES}
        COMMENT "fxc ${name}"
        VERBATIM)
    set(MODERNWATER_SHADER_HEADERS ${MODERNWATER_SHADER_HEADERS} ${out} PARENT_SCOPE)
endfunction()

modemwater_shader(vs_fullscreen vf_fullscreen_vs.hlsl vs_3_0)

# Water shading, one variant per quality level. The only difference is how many steps the screen-space
# reflection march may take, which is compiled in because ps_3_0 has no dynamic loop bound worth the branches.
modemwater_shader(ps_vw_shade_low vw_water.hlsl ps_3_0 /DSSR_STEPS=0)
modemwater_shader(ps_vw_shade_mid vw_water.hlsl ps_3_0 /DSSR_STEPS=8)
modemwater_shader(ps_vw_shade_high vw_water.hlsl ps_3_0 /DSSR_STEPS=16)

# Scene depth to linear eye depth, plain and three-byte packed for the depth readback path.
modemwater_shader(ps_vw_depth vw_linear_depth.hlsl ps_3_0 /DPACKED_DEPTH=0)
modemwater_shader(ps_vw_depth_packed vw_linear_depth.hlsl ps_3_0 /DPACKED_DEPTH=1)

# The FFT wave simulation: evolve the spectrum, a row pass and a column pass per stage, assemble the
# displacement and momentum maps, and derive the foam from the displacement.
modemwater_shader(ps_vw_fft_evolve vw_fft_evolve.hlsl ps_3_0)
modemwater_shader(ps_vw_fft_rows vw_fft_butterfly.hlsl ps_3_0 /DCOLUMNS=0 /DFUSED_STAGES=0 /DASSEMBLE=0)
modemwater_shader(ps_vw_fft_rows_fused vw_fft_butterfly.hlsl ps_3_0 /DCOLUMNS=0 /DFUSED_STAGES=1 /DASSEMBLE=0)
modemwater_shader(ps_vw_fft_columns vw_fft_butterfly.hlsl ps_3_0 /DCOLUMNS=1 /DFUSED_STAGES=0 /DASSEMBLE=0)
modemwater_shader(ps_vw_fft_columns_fused vw_fft_butterfly.hlsl ps_3_0 /DCOLUMNS=1 /DFUSED_STAGES=1 /DASSEMBLE=0)
modemwater_shader(ps_vw_fft_assemble_fused vw_fft_butterfly.hlsl ps_3_0 /DCOLUMNS=1 /DFUSED_STAGES=1 /DASSEMBLE=1)
modemwater_shader(ps_vw_fft_surface_foam vw_fft_surface.hlsl ps_3_0 /DSURFACE=1 /DFOAM=1)
modemwater_shader(ps_vw_fft_surface vw_fft_surface.hlsl ps_3_0 /DSURFACE=1 /DFOAM=0)
modemwater_shader(ps_vw_fft_foam vw_fft_surface.hlsl ps_3_0 /DSURFACE=0 /DFOAM=1)
modemwater_shader(ps_vw_fft_mip vw_fft_mip.hlsl ps_3_0 /DPAIRED=0)
modemwater_shader(ps_vw_fft_mip_pair vw_fft_mip.hlsl ps_3_0 /DPAIRED=1)

# Ripples: one step of the wave equation from the previous two state maps.
modemwater_shader(ps_vw_ripple_step vw_ripple_step.hlsl ps_3_0)

add_custom_target(ModernWaterShaders DEPENDS ${MODERNWATER_SHADER_HEADERS})
