#
# Copyright (c) 2023, NVIDIA CORPORATION. All rights reserved.
# 
# NVIDIA CORPORATION and its licensors retain all intellectual property
# and proprietary rights in and to this software, related documentation
# and any modifications thereto. Any use, reproduction, disclosure or
# distribution of this software and related documentation without an express
# license agreement from NVIDIA CORPORATION is strictly prohibited.

##########################################################################
################################ UTIL ####################################
##########################################################################
function(util_get_shader_profile_from_name FILE_NAME DXC_PROFILE)
    get_filename_component(EXTENSION ${FILE_NAME} EXT)
    if ("${EXTENSION}" STREQUAL ".cs.hlsl")
        set(DXC_PROFILE "cs" PARENT_SCOPE)
    endif()
    if ("${EXTENSION}" STREQUAL ".vs.hlsl")
        set(DXC_PROFILE "vs" PARENT_SCOPE)
    endif()
    if ("${EXTENSION}" STREQUAL ".gs.hlsl")
        set(DXC_PROFILE "gs" PARENT_SCOPE)
    endif()
    if ("${EXTENSION}" STREQUAL ".ps.hlsl")
        set(DXC_PROFILE "ps" PARENT_SCOPE)
    endif()
endfunction()

function(util_generate_shader_config_file OUT_FILE_NAME DIR DEFINES)
    file(GLOB_RECURSE HLSL_FILES "${CMAKE_CURRENT_SOURCE_DIR}/${DIR}/*.hlsl")

    set(out_content "")
    foreach(FILE_NAME ${HLSL_FILES})
        get_filename_component(NAME_ONLY ${FILE_NAME} NAME)
        set(DXC_PROFILE "")
        util_get_shader_profile_from_name(${FILE_NAME} DXC_PROFILE)
        set(out_content "${out_content}${DIR}/${NAME_ONLY} -T ${DXC_PROFILE} -E main ${DEFINES}\n")
    endforeach()

    file(WRITE ${OUT_FILE_NAME} ${out_content})
endfunction()

##########################################################################
########################### RayTracingDenoiser ###########################
##########################################################################

# see https://github.com/NVIDIAGameWorks/RayTracingDenoiser#cmake-options
# set(NRD_DXC_CUSTOM_PATH ${DXC_CUSTOM_PATH})
set(NRD_DXC_PATH "${DXC_DXIL_EXECUTABLE}" CACHE STRING "DXC shader compiler path for NRD")
set(NRD_SHADER_OUTPUT_PATH "${SHADER_OUTPUT_PATH}" CACHE STRING "")
set(NRD_PROJECT_FOLDER "External/RayTracingDenoiser")
set(NRD_NORMAL_ENCODING "2" CACHE STRING "Normal encoding variant (0-4, matches nrd::NormalEncoding)")
set(NRD_ROUGHNESS_ENCODING "1" CACHE STRING "Roughness encoding variant (0-2, matches nrd::RoughnessEncoding)")
set(NRD_IS_SUBMODULE ON)
# NRD uses a custom output path for some reason
set(GLOBAL_BIN_OUTPUT_PATH ${CMAKE_BINARY_DIR} CACHE STRING "")
set(NRD_SHADERS_PATH "${CMAKE_BINARY_DIR}/NRDShaders" CACHE STRING "")
option(NRD_DISABLE_SHADER_COMPILATION "" ON)
option(NRD_USE_PRECOMPILED_SHADERS "" OFF)

add_subdirectory("RayTracingDenoiser")

# Let CMake generate the nrd_shaders.cfg file 
util_generate_shader_config_file(
    "nrd.cfg" 
    "RayTracingDenoiser/Shaders/Source"
    "-D NRD_COMPILER_DXC=1 -D NRD_NORMAL_ENCODING=${NRD_NORMAL_ENCODING} -D NRD_ROUGHNESS_ENCODING=${NRD_ROUGHNESS_ENCODING}"
)

# ShaderMake general arguments
set (
    SHADERMAKE_GENERAL_ARGS
    "-I ${CMAKE_CURRENT_SOURCE_DIR}/RayTracingDenoiser/External/MathLib -I ${CMAKE_CURRENT_SOURCE_DIR}/RayTracingDenoiser/Shaders/Include -I ${CMAKE_CURRENT_SOURCE_DIR}/RayTracingDenoiser/Shaders/Resources"
)

# Compile all shaders in nrd_shaders.cfg 
donut_compile_shaders(
    TARGET nrd_shaders
    CONFIG ${CMAKE_CURRENT_SOURCE_DIR}/nrd.cfg
    SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/RayTracingDenoiser/Shaders/Include
    FOLDER "NRD"
    DXIL ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/shaders/nrd/dxil
    SPIRV_DXC ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/shaders/nrd/spirv
    CFLAGS "-WX -O3 -enable-16bit-types -all_resources_bound"
    COMPILER_OPTIONS_DXIL ${SHADERMAKE_GENERAL_ARGS}
    COMPILER_OPTIONS_SPIRV ${SHADERMAKE_GENERAL_ARGS}
    #INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/RayTracingDenoiser/External/MathLib INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/RayTracingDenoiser/Shaders/Include ${CMAKE_CURRENT_SOURCE_DIR}/RayTracingDenoiser/Shaders/Resources
)

set(RTXDI_SKIP_SHADER_VALIDATION ON CACHE STRING "Skip RTXDI shader validation")