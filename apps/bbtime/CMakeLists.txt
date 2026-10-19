add_executable(bbtime main.cpp level_hook.cpp)
target_link_libraries(bbtime PRIVATE bb_render)

add_executable(test_fixed_step test_fixed_step.cpp)
add_test(NAME bbtime.fixed_step COMMAND test_fixed_step)
