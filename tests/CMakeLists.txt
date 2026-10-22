# assert()-based tests: keep asserts in every build type.
foreach(t lzss bze realdata tim levels tb_levels actor mods)
  add_executable(test_${t} test_${t}.cpp)
  target_link_libraries(test_${t} PRIVATE bb_core)
  target_compile_options(test_${t} PRIVATE -UNDEBUG)
  add_test(NAME core.${t} COMMAND test_${t})
endforeach()
# Needs BBTIME_DATA=<dir containing lit/ and tb/>; skipped (77) otherwise.
set_tests_properties(core.realdata core.levels core.tb_levels core.actor PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600)
target_compile_definitions(test_actor PRIVATE BBTIME_TESTS_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
