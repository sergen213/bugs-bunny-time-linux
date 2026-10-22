find_package(Vulkan REQUIRED) # headers + loader (libvulkan)
find_program(GLSLC glslc REQUIRED)

# Shaders -> SPIR-V as comma-separated words, #included into constexpr arrays by src/renderer.cpp.
# Each entry: output name:source:defines (`-` = none). The plain scene shaders stay byte-identical to the
# pre-shadow ones (Original Fidelity); the LIT/RT variants add the Enhanced sun shadows and ray-traced effects.
set(BB_VK_SPV)
foreach(entry scene.vert:scene.vert:- scene.frag:scene.frag:- scene_lit.vert:scene.vert:-DLIT
              scene_map.frag:scene.frag:-DLIT scene_rt.frag:scene.frag:-DLIT,-DRT
              scene_mv.vert:scene.vert:-DLIT,-DMV scene_mv.frag:scene.frag:-DMV scene_map_mv.frag:scene.frag:-DLIT,-DMV
              scene_rt_mv.frag:scene.frag:-DLIT,-DRT,-DMV
              shadow.vert:shadow.vert:- shadow.frag:shadow.frag:- ui.vert:ui.vert:- ui.frag:ui.frag:-)
  string(REPLACE ":" ";" parts ${entry})
  list(GET parts 0 name)
  list(GET parts 1 src)
  list(GET parts 2 defs)
  string(REPLACE "," ";" defs ${defs})
  list(REMOVE_ITEM defs -)
  set(out ${CMAKE_CURRENT_BINARY_DIR}/shaders/${name}.inc)
  add_custom_command(OUTPUT ${out}
    COMMAND ${GLSLC} --target-env=vulkan1.3 -O -Werror -mfmt=num ${defs} -o ${out} ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${src}
    DEPENDS shaders/${src} VERBATIM)
  list(APPEND BB_VK_SPV ${out})
endforeach()

add_library(bb_render_vk STATIC src/renderer.cpp src/fsr.cpp ${BB_VK_SPV})
target_include_directories(bb_render_vk PUBLIC include PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/shaders)
target_link_libraries(bb_render_vk PUBLIC bb_render Vulkan::Vulkan PRIVATE bb_ffx)
# Vulkan create-info idiom `VkFooCreateInfo ci{VK_STRUCTURE_TYPE_...}` value-initialises the rest on purpose.
target_compile_options(bb_render_vk PRIVATE -Wno-missing-field-initializers)

add_executable(test_vk_init tests/test_vk_init.cpp)
target_link_libraries(test_vk_init PRIVATE bb_render_vk)
target_compile_options(test_vk_init PRIVATE -UNDEBUG)
add_test(NAME render_vk.init COMMAND test_vk_init)
set_tests_properties(render_vk.init PROPERTIES SKIP_RETURN_CODE 77)
# Same with ray tracing and FSR forced off: Shadows::Rt / RtAo fall back to the shadow map, reflections / upscaling /
# frame generation to off.
add_test(NAME render_vk.init_no_rt COMMAND test_vk_init)
set_tests_properties(render_vk.init_no_rt PROPERTIES SKIP_RETURN_CODE 77 ENVIRONMENT "BBTIME_VK_NO_RT=1;BBTIME_VK_NO_FSR=1")

# Validation-clean smoke over menus + 3 LiT + 3 TB levels through the real app (needs BBTIME_DATA, else 77).
add_test(NAME render_vk.validation_levels
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/validation_levels.sh $<TARGET_FILE:bbtime> ${CMAKE_CURRENT_BINARY_DIR}/validation)
set_tests_properties(render_vk.validation_levels PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 900)
