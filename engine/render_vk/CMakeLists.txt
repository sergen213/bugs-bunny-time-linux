find_package(Vulkan REQUIRED) # headers + loader (libvulkan)
find_program(GLSLC glslc REQUIRED)

# Shaders -> SPIR-V as comma-separated words, #included into constexpr arrays by src/renderer.cpp.
set(BB_VK_SPV)
foreach(s scene.vert scene.frag ui.vert ui.frag)
  set(out ${CMAKE_CURRENT_BINARY_DIR}/shaders/${s}.inc)
  add_custom_command(OUTPUT ${out}
    COMMAND ${GLSLC} --target-env=vulkan1.3 -O -Werror -mfmt=num -o ${out} ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${s}
    DEPENDS shaders/${s} VERBATIM)
  list(APPEND BB_VK_SPV ${out})
endforeach()

add_library(bb_render_vk STATIC src/renderer.cpp ${BB_VK_SPV})
target_include_directories(bb_render_vk PUBLIC include PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/shaders)
target_link_libraries(bb_render_vk PUBLIC bb_render Vulkan::Vulkan)
# Vulkan create-info idiom `VkFooCreateInfo ci{VK_STRUCTURE_TYPE_...}` value-initialises the rest on purpose.
target_compile_options(bb_render_vk PRIVATE -Wno-missing-field-initializers)

add_executable(test_vk_init tests/test_vk_init.cpp)
target_link_libraries(test_vk_init PRIVATE bb_render_vk)
target_compile_options(test_vk_init PRIVATE -UNDEBUG)
add_test(NAME render_vk.init COMMAND test_vk_init)
set_tests_properties(render_vk.init PROPERTIES SKIP_RETURN_CODE 77)

# Validation-clean smoke over menus + 3 LiT + 3 TB levels through the real app (needs BBTIME_DATA, else 77).
add_test(NAME render_vk.validation_levels
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/validation_levels.sh $<TARGET_FILE:bbtime> ${CMAKE_CURRENT_BINARY_DIR}/validation)
set_tests_properties(render_vk.validation_levels PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 900)
