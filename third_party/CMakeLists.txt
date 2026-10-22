# AMD FidelityFX SDK v1.1.4 subset (third_party/fidelityfx-sdk, README.md there): FSR 3.1.4 upscaler, frame
# interpolation and optical flow for the Vulkan backend, vendored and built as in /mnt/1st4TB/etiyami (same files,
# same local patches, same glue under yami/). Only the permutations yami_ffx_clamp_capabilities
# lets the SDK request are compiled (FP32, wave32, render-resolution unjittered motion vectors, standard depth);
# tools/embed_fsr_shaders.py checks the set and generates the blob accessor.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
# glslangValidator -Os (as etiyami) keeps the resource names the SDK binds by (OpName). glslc's optimiser strips them
# unless -g keeps debug info (much larger blobs), and unoptimised glslc keeps unused samplers the SDK's binding tables
# reject, so the glslc fallback is -O -g. The AppImage toolchain image installs glslang-tools.
find_program(GLSLANG_VALIDATOR glslangValidator HINTS "$ENV{VULKAN_SDK}/bin")
set(BB_FFX_DIR "${CMAKE_CURRENT_SOURCE_DIR}/fidelityfx-sdk")
set(BB_FFX_GPU "${BB_FFX_DIR}/include/FidelityFX/gpu")
file(GLOB_RECURSE BB_FFX_GPU_HEADERS "${BB_FFX_GPU}/*.h")
set(BB_FFX_SPIRV_FILES)
# Compiles src/backends/vk/shaders/<effect>/<stem>.glsl to ffx/<stem><suffix>.spv with the extra defines.
function(bb_ffx_shader effect stem suffix)
    set(source "${BB_FFX_DIR}/src/backends/vk/shaders/${effect}/${stem}.glsl")
    set(spirv "${CMAKE_CURRENT_BINARY_DIR}/ffx/${stem}${suffix}.spv")
    if(GLSLANG_VALIDATOR)
        set(compiler "${GLSLANG_VALIDATOR}" -V --target-env vulkan1.2 -S comp -Os)
    else()
        set(compiler ${GLSLC} --target-env=vulkan1.2 -fshader-stage=compute -O -g)
    endif()
    add_custom_command(OUTPUT "${spirv}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/ffx"
        COMMAND ${compiler} -DFFX_GPU=1 -DFFX_GLSL=1 -DFFX_HALF=0 ${ARGN}
                "-I${BB_FFX_GPU}" "-I${BB_FFX_GPU}/${effect}" -o "${spirv}" "${source}"
        DEPENDS "${source}" ${BB_FFX_GPU_HEADERS}
        VERBATIM)
    set(BB_FFX_SPIRV_FILES ${BB_FFX_SPIRV_FILES} "${spirv}" PARENT_SCOPE)
endfunction()
# Base defines from the SDK's CMakeCompile{FSR3Upscaler,Frameinterpolation,Opticalflow}Shaders.txt.
set(BB_FSR3_DEFINES
    -DFFX_FSR3UPSCALER_OPTION_UPSAMPLE_SAMPLERS_USE_DATA_HALF=0 -DFFX_FSR3UPSCALER_OPTION_ACCUMULATE_SAMPLERS_USE_DATA_HALF=0
    -DFFX_FSR3UPSCALER_OPTION_REPROJECT_SAMPLERS_USE_DATA_HALF=1 -DFFX_FSR3UPSCALER_OPTION_POSTPROCESSLOCKSTATUS_SAMPLERS_USE_DATA_HALF=0
    -DFFX_FSR3UPSCALER_OPTION_UPSAMPLE_USE_LANCZOS_TYPE=2 -DFFX_FSR3UPSCALER_OPTION_REPROJECT_USE_LANCZOS_TYPE=0
    -DFFX_FSR3UPSCALER_OPTION_LOW_RESOLUTION_MOTION_VECTORS=1 -DFFX_FSR3UPSCALER_OPTION_JITTERED_MOTION_VECTORS=0
    -DFFX_FSR3UPSCALER_OPTION_INVERTED_DEPTH=0)
foreach(pass IN ITEMS prepare_inputs luma_pyramid shading_change_pyramid shading_change prepare_reactivity
        luma_instability accumulate rcas debug_view autogen_reactive)
    foreach(sharpen 0 1)
        foreach(hdr 0 1)
            bb_ffx_shader(fsr3upscaler ffx_fsr3upscaler_${pass}_pass .h${hdr}s${sharpen} ${BB_FSR3_DEFINES}
                -DFFX_FSR3UPSCALER_OPTION_HDR_COLOR_INPUT=${hdr} -DFFX_FSR3UPSCALER_OPTION_APPLY_SHARPENING=${sharpen})
        endforeach()
    endforeach()
endforeach()
foreach(pass IN ITEMS reconstruct_and_dilate_pass setup_pass reconstruct_previous_depth_pass game_motion_vector_field_pass
        optical_flow_vector_field_pass disocclusion_mask_pass pass compute_inpainting_pyramid_pass inpainting_pass
        compute_game_vector_field_inpainting_pyramid_pass debug_view_pass)
    bb_ffx_shader(frameinterpolation ffx_frameinterpolation_${pass} ""
        -DFFX_FRAMEINTERPOLATION_OPTION_UPSAMPLE_SAMPLERS_USE_DATA_HALF=0 -DFFX_FRAMEINTERPOLATION_OPTION_ACCUMULATE_SAMPLERS_USE_DATA_HALF=0
        -DFFX_FRAMEINTERPOLATION_OPTION_REPROJECT_SAMPLERS_USE_DATA_HALF=1 -DFFX_FRAMEINTERPOLATION_OPTION_POSTPROCESSLOCKSTATUS_SAMPLERS_USE_DATA_HALF=0
        -DFFX_FRAMEINTERPOLATION_OPTION_UPSAMPLE_USE_LANCZOS_TYPE=2 -DFFX_FRAMEINTERPOLATION_OPTION_LOW_RES_MOTION_VECTORS=1
        -DFFX_FRAMEINTERPOLATION_OPTION_JITTER_MOTION_VECTORS=0 -DFFX_FRAMEINTERPOLATION_OPTION_INVERTED_DEPTH=0)
endforeach()
foreach(pass IN ITEMS prepare_luma_pass compute_luminance_pyramid_pass generate_scd_histogram_pass compute_scd_divergence_pass
        compute_optical_flow_advanced_pass_v5 filter_optical_flow_pass_v5 scale_optical_flow_advanced_pass_v5)
    bb_ffx_shader(opticalflow ffx_opticalflow_${pass} "" -DFFX_OPTICALFLOW_OPTION_HDR_COLOR_INPUT=0)
endforeach()
set(BB_FSR_BLOBS "${CMAKE_CURRENT_BINARY_DIR}/ffx/ffx_shader_blobs.cpp")
add_custom_command(OUTPUT "${BB_FSR_BLOBS}"
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/embed_fsr_shaders.py" --output "${BB_FSR_BLOBS}" ${BB_FFX_SPIRV_FILES}
    DEPENDS ${BB_FFX_SPIRV_FILES} "${PROJECT_SOURCE_DIR}/tools/embed_fsr_shaders.py"
    VERBATIM)
set(BB_FFX_UPSTREAM_SOURCES
    "${BB_FFX_DIR}/src/components/fsr3upscaler/ffx_fsr3upscaler.cpp"
    "${BB_FFX_DIR}/src/components/frameinterpolation/ffx_frameinterpolation.cpp"
    "${BB_FFX_DIR}/src/components/opticalflow/ffx_opticalflow.cpp" "${BB_FFX_DIR}/src/backends/vk/ffx_vk.cpp"
    "${BB_FFX_DIR}/src/shared/ffx_assert.cpp" "${BB_FFX_DIR}/src/shared/ffx_message.cpp"
    "${BB_FFX_DIR}/src/shared/ffx_breadcrumbs_list.cpp" "${BB_FFX_DIR}/src/shared/ffx_object_management.cpp")
add_library(bb_ffx STATIC ${BB_FFX_UPSTREAM_SOURCES} "${BB_FFX_DIR}/yami/yami_ffx_vulkan.cpp" "${BB_FSR_BLOBS}")
set_target_properties(bb_ffx PROPERTIES CXX_STANDARD 17)
# Vulkan entry points are resolved at runtime (yami_ffx_load_vulkan); the library references no vk* symbols.
target_compile_definitions(bb_ffx PRIVATE VK_NO_PROTOTYPES)
# yami/ first: it shadows FidelityFX/host/ffx_types.h to size the opaque contexts for 4-byte wchar_t.
target_include_directories(bb_ffx SYSTEM PUBLIC "${BB_FFX_DIR}/yami" "${BB_FFX_DIR}/include")
target_include_directories(bb_ffx PRIVATE "${BB_FFX_DIR}/src/shared" "${BB_FFX_DIR}/src/backends/shared" "${BB_FFX_DIR}/src/components")
target_link_libraries(bb_ffx PUBLIC Vulkan::Headers)
target_compile_options(bb_ffx PRIVATE "-include${BB_FFX_DIR}/yami/ffx_compat.h")
set_source_files_properties(${BB_FFX_UPSTREAM_SOURCES} "${BB_FSR_BLOBS}" PROPERTIES COMPILE_OPTIONS -w)

# stb (public domain / MIT, third_party/stb/LICENSE): PNG for mod textures (bb_core), Ogg Vorbis for mod music (bb_audio).
add_library(bb_stb STATIC stb/stb_impl.c stb/stb_vorbis.c)
target_include_directories(bb_stb PUBLIC stb)
target_compile_options(bb_stb PRIVATE -w)
set_target_properties(bb_stb PROPERTIES POSITION_INDEPENDENT_CODE ON)
