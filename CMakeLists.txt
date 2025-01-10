# Copyright (c) 2023-2025, NVIDIA CORPORATION.  All rights reserved.
#
# NVIDIA CORPORATION and its licensors retain all intellectual property
# and proprietary rights in and to this software, related documentation
# and any modifications thereto.  Any use, reproduction, disclosure or
# distribution of this software and related documentation without an express
# license agreement from NVIDIA CORPORATION is strictly prohibited.

cmake_minimum_required (VERSION 3.19)

project(
    rtxgi2_samples
    DESCRIPTION "Samples showcasing NRC and SHARC features"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

# Set MSVC debug level
if(MSVC)
    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} /D_ITERATOR_DEBUG_LEVEL=1")
    set (CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# Warnings as errors
set(CMAKE_COMPILE_WARNING_AS_ERROR ON)

# -----------------------------------------------------------------------------
# Other packages downloaded from zips
# -----------------------------------------------------------------------------

# Helper to download and unzip a package from a URL
# Uses a zero-length file to identify the version of the package
function(CheckAndDownloadPackage NAME VERSION LOCAL_PATH URL ARCHIVE_TYPE)
    # Do we already have the correct version?
    if(NOT EXISTS ${LOCAL_PATH}/${VERSION}.ver)
        # Was there a previous version that we need to delete?
        if(EXISTS ${LOCAL_PATH})
            message(STATUS "Deleting old " ${NAME})
            file(REMOVE_RECURSE ${LOCAL_PATH})
        endif()
        message(STATUS "Obtaining " ${NAME} " " ${VERSION})
        file(DOWNLOAD ${URL} ${LOCAL_PATH}.${ARCHIVE_TYPE})
        message(STATUS "Extracting " ${NAME})
        file(ARCHIVE_EXTRACT INPUT ${LOCAL_PATH}.${ARCHIVE_TYPE} DESTINATION ${LOCAL_PATH})
        file(REMOVE ${LOCAL_PATH}.${ARCHIVE_TYPE})
        # Create an empty file so we know which version we have
        file(WRITE ${LOCAL_PATH}/${VERSION}.ver)
    endif()
endfunction()

CheckAndDownloadPackage("Agility SDK" "v1.610.4" ${CMAKE_CURRENT_SOURCE_DIR}/external/AgilitySDK https://www.nuget.org/api/v2/package/Microsoft.Direct3D.D3D12/1.610.4 "zip")
CheckAndDownloadPackage("DXC" "v1.7.2308" ${CMAKE_CURRENT_SOURCE_DIR}/external/dxc https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.7.2308/dxc_2023_08_14.zip "zip")

# -----------------------------------------------------------------------------
# Configure options
# -----------------------------------------------------------------------------

# Setup shader compiler 
set(DXC_DXIL_EXECUTABLE "${CMAKE_CURRENT_SOURCE_DIR}/external/dxc/bin/x64/dxc.exe" CACHE STRING "DXC shader compiler path")
set(DXC_PATH "${CMAKE_CURRENT_SOURCE_DIR}/external/dxc/bin/x64/dxc.exe")
set(DXC_SPIRV_PATH "${CMAKE_CURRENT_SOURCE_DIR}/external/dxc/bin/x64/dxc.exe")

# Setup denoiser support (NRD)
option(USE_RAY_TRACING_DENOISER "Include NRD as part of the sample." ON)
set(NRD_PATH "${CMAKE_CURRENT_SOURCE_DIR}/external/RayTracingDenoiser")

# Setup asset importer support
option(DONUT_WITH_ASSIMP "" OFF)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/bin")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
set(DONUT_SHADERS_OUTPUT_DIR "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/shaders/framework")


# Setup option for local NRC library development
set(CUSTOM_NRC_DISTRO_PATH "" CACHE PATH "Enter the path to NRC distro directory containing lib, include, and bin.")

# Options for NRC DLL signing verification
option(NRC_ENABLE_DLL_CHECK "Enable NRC DLL signing verification." ON)
if(NRC_ENABLE_DLL_CHECK)
  add_compile_definitions(NRC_ENABLE_DLL_CHECK)
endif()

# Check if the custom directory path is provided.
if(NOT CUSTOM_NRC_DISTRO_PATH)
    message(STATUS "Custom NRC distro path is not provided.")
    set(NRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/sdk-libraries/nrc)
else()
    set(NRC_DIR ${CUSTOM_NRC_DISTRO_PATH})  
endif()

if(EXISTS ${NRC_DIR}/bin/NRC_Vulkan.dll)
    set(SETUP_NRC_WITH_VULKAN TRUE) #TODO: maybe not required...
    add_compile_definitions(NRC_WITH_VULKAN)   
endif()

unset(DONUT_WITH_VULKAN CACHE)
unset(NVRHI_WITH_VULKAN CACHE)

# Force Vulkan support
option(DONUT_WITH_VULKAN "Enable the Vulkan version of Donut" ON)

# Add projects
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/external/donut)

message(STATUS "NVRHI Vulkan support " "${NVRHI_WITH_VULKAN}")
message(STATUS "Donut Vulkan support " "${DONUT_WITH_VULKAN}")

if(NVRHI_WITH_VULKAN OR NVRHI_WITH_DX12)
    add_subdirectory(samples/pathtracer)
    # Set Pathtracer sample project as a startup project
	set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT pathtracer)
endif()


# Add SHARC includes to the solution
set(SHARC_LIBRARY_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/sdk-libraries/sharc")
file(GLOB_RECURSE INCLUDE_SHARC "${SHARC_LIBRARY_ROOT}/*.h")

# Add NRC includes to the solution
file(GLOB_RECURSE INCLUDE_NRC
    "${NRC_DIR}/include/*.hlsli"
    "${NRC_DIR}/include/*.h"
)

add_library(sdk-libraries ${INCLUDE_SHARC} ${INCLUDE_NRC})
set_target_properties(sdk-libraries PROPERTIES LINKER_LANGUAGE CXX)
set(VS_FOLDER_NAME "sdk-libraries")
set_target_properties(sdk-libraries PROPERTIES FOLDER ${VS_FOLDER_NAME})

source_group("sharc" FILES ${INCLUDE_SHARC})
source_group("nrc"   FILES ${INCLUDE_NRC})

# Include external
add_subdirectory(external)
