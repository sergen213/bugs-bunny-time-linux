add_executable(bbtime main.cpp level_hook.cpp music.cpp play.cpp)
target_link_libraries(bbtime PRIVATE bb_render bb_render_vk bb_audio bb_settings bb_game)

add_executable(test_fixed_step test_fixed_step.cpp)
target_compile_options(test_fixed_step PRIVATE -UNDEBUG)
add_test(NAME bbtime.fixed_step COMMAND test_fixed_step)
