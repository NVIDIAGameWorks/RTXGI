# Copyright (c) 2023-2025, NVIDIA CORPORATION.  All rights reserved.
#
# NVIDIA CORPORATION and its licensors retain all intellectual property
# and proprietary rights in and to this software, related documentation
# and any modifications thereto.  Any use, reproduction, disclosure or
# distribution of this software and related documentation without an express
# license agreement from NVIDIA CORPORATION is strictly prohibited.

cmake_minimum_required (VERSION 3.19)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../External/Donut/compileshaders.cmake)

file(GLOB shaders "*.hlsl")
file(GLOB sources "*.cpp" "*.h")

set(project Pathtracer)
set(folder "Samples/Pathtracer")

option (NRD_EMBEDS_SPIRV_SHADERS "NRD embeds SPIRV shaders" OFF)
option (NRD_EMBEDS_DXIL_SHADERS "NRD embeds DXIL shaders" OFF)
option (NRD_EMBEDS_DXBC_SHADERS "NRD embeds DXBC shaders" OFF)

set(NRC_LIB_DIR ${NRC_DIR}/Lib)

# Is the NRC package a dev package (internal), or production
if(EXISTS ${NRC_LIB_DIR}/NRC_D3D12_relwithdebinfo.lib)
    set (NRC_DEV_BUILD true)
else()
    set (NRC_DEV_BUILD false)
endif()

if(NRC_DEV_BUILD)
    set(NRC_LIBRARY_NAME_D3D12_DEBUG NRC_D3D12_debug)
    set(NRC_LIBRARY_NAME_D3D12_RELEASE NRC_D3D12_relwithdebinfo)

    if(DONUT_WITH_VULKAN AND SETUP_NRC_WITH_VULKAN)
        set(NRC_LIBRARY_NAME_VULKAN_DEBUG NRC_Vulkan_debug)
        set(NRC_LIBRARY_NAME_VULKAN_RELEASE NRC_Vulkan_relwithdebinfo)
    endif()
else()
    set(NRC_LIBRARY_NAME_D3D12_DEBUG NRC_D3D12)
    set(NRC_LIBRARY_NAME_D3D12_RELEASE NRC_D3D12)

    if(DONUT_WITH_VULKAN AND SETUP_NRC_WITH_VULKAN)
        set(NRC_LIBRARY_NAME_VULKAN_DEBUG NRC_Vulkan)
        set(NRC_LIBRARY_NAME_VULKAN_RELEASE NRC_Vulkan)
    endif()
endif()

# Set SHARC includes
set(SHARC_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../Libraries)

# Shader Model 6_6 requires Agility SDK on Windows 10.
if(DONUT_WITH_VULKAN)
    set (SHADERMAKE_GENERAL_ARGS_SPIRV "--shaderModel 6_6 -I ${NRC_DIR}/include -I ${SHARC_INCLUDE_DIR}/Sharc/include --vulkanMemoryLayout dx")
endif()
set (SHADERMAKE_GENERAL_ARGS_DXIL "--shaderModel 6_6 --useAPI --WX --PDB -I ${NRC_DIR}/include -I ${SHARC_INCLUDE_DIR}/Sharc/include")

donut_compile_shaders(
        TARGET ${project}_shaders
        CONFIG ${CMAKE_CURRENT_SOURCE_DIR}/shaders.cfg
        SOURCES ${shaders}
        FOLDER ${folder}
        SPIRV_DXC ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/shaders/${project}/spirv
        DXIL ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/shaders/${project}/dxil
        SHADERMAKE_OPTIONS_SPIRV ${SHADERMAKE_GENERAL_ARGS_SPIRV}
        SHADERMAKE_OPTIONS_DXIL ${SHADERMAKE_GENERAL_ARGS_DXIL}
)

add_executable(${project} WIN32 ${sources})
target_link_libraries(${project} donut_render donut_app donut_engine NRD)
add_dependencies(${project} ${project}_shaders nrd_shaders)
set_target_properties(${project} PROPERTIES FOLDER ${folder})

ADD_DEFINITIONS(-DUNICODE)
ADD_DEFINITIONS(-D_UNICODE)

# NRC libs
if(NRC_DEV_BUILD)
    target_link_libraries(${project} debug "${NRC_LIB_DIR}/${NRC_LIBRARY_NAME_D3D12_DEBUG}.lib")
    target_link_libraries(${project} optimized "${NRC_LIB_DIR}/${NRC_LIBRARY_NAME_D3D12_RELEASE}.lib")
else()
    target_link_libraries(${project} "${NRC_LIB_DIR}/${NRC_LIBRARY_NAME_D3D12_RELEASE}.lib")
endif()

if(DONUT_WITH_VULKAN AND SETUP_NRC_WITH_VULKAN)
    if(NRC_DEV_BUILD)
        target_link_libraries(${project} debug "${NRC_LIB_DIR}/${NRC_LIBRARY_NAME_VULKAN_DEBUG}.lib")
        target_link_libraries(${project} optimized "${NRC_LIB_DIR}/${NRC_LIBRARY_NAME_VULKAN_RELEASE}.lib")
    else()
        target_link_libraries(${project} "${NRC_LIB_DIR}/${NRC_LIBRARY_NAME_VULKAN_RELEASE}.lib")
    endif()
endif()

# NRC includes
include_directories("${NRC_DIR}/include" "${NRD_PATH}/Include")

# Copy Agility SDK binaries (if needed) one level below project executable to avoid known issues.
# Details in the section "Known Issues" https://devblogs.microsoft.com/directx/gettingstarted-dx12agility/
add_custom_command(TARGET ${project} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different 
    ${CMAKE_CURRENT_SOURCE_DIR}/../../External/AgilitySDK/build/native/bin/x64/D3D12Core.dll
    ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/D3D12/D3D12Core.dll
)

add_custom_command(TARGET ${project} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${CMAKE_CURRENT_SOURCE_DIR}/../../External/AgilitySDK/build/native/bin/x64/d3d12SDKLayers.dll
    ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/D3D12/d3d12SDKLayers.dll
)

# Copy runtime libraries
file(GLOB NVRTC64_PATH "${NRC_DIR}/Bin/nvrtc64_*")
file(GLOB CUDART64_PATH "${NRC_DIR}/Bin/cudart64_*")
file(GLOB NVRTC_BUILTINS64 "${NRC_DIR}/Bin/nvrtc-builtins64_*")
add_custom_command(
    TARGET ${project} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy "${NVRTC64_PATH}"
    ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}
    COMMAND ${CMAKE_COMMAND} -E copy "${CUDART64_PATH}"
    ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}
    COMMAND ${CMAKE_COMMAND} -E copy "${NVRTC_BUILTINS64}"
    ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}
)

set( copy_nrc_d3d_command
    ${CMAKE_COMMAND} -E copy
    "${NRC_DIR}/Bin/$<$<CONFIG:Debug>:${NRC_LIBRARY_NAME_D3D12_DEBUG}>$<$<CONFIG:Release,RelWithDebInfo>:${NRC_LIBRARY_NAME_D3D12_RELEASE}>.dll"
    $<TARGET_FILE_DIR:${project}>
)
add_custom_command( TARGET ${project} POST_BUILD
    COMMAND "${copy_nrc_d3d_command}"
    COMMAND_EXPAND_LISTS
)

if(DONUT_WITH_VULKAN AND SETUP_NRC_WITH_VULKAN)
    set( copy_nrc_vulkan_command
        ${CMAKE_COMMAND} -E copy
        "${NRC_DIR}/Bin/$<$<CONFIG:Debug>:${NRC_LIBRARY_NAME_VULKAN_DEBUG}>$<$<CONFIG:Release,RelWithDebInfo>:${NRC_LIBRARY_NAME_VULKAN_RELEASE}>.dll"
        $<TARGET_FILE_DIR:${project}>
    )
    add_custom_command( TARGET ${project} POST_BUILD 
        COMMAND "${copy_nrc_vulkan_command}"
        COMMAND_EXPAND_LISTS
    )
endif()

