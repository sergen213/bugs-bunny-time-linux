find_package(OpenAL REQUIRED)

add_library(bb_audio STATIC src/codec.cpp src/device.cpp)
target_include_directories(bb_audio PUBLIC include)
target_link_libraries(bb_audio PUBLIC bb_core PRIVATE OpenAL::OpenAL)

# Dev tool: xa2wav IN.xa OUT.wav (C++ twin of tools/re/xa2wav.py).
add_executable(xa2wav tools/xa2wav.cpp)
target_link_libraries(xa2wav PRIVATE bb_audio)

# assert()-based tests: keep asserts in every build type.
foreach(t codec realdata)
  add_executable(test_audio_${t} tests/test_${t}.cpp)
  target_link_libraries(test_audio_${t} PRIVATE bb_audio)
  target_compile_options(test_audio_${t} PRIVATE -UNDEBUG)
  add_test(NAME audio.${t} COMMAND test_audio_${t})
endforeach()
# Needs BBTIME_DATA=<dir containing lit/ and tb/>; skipped (77) otherwise.
set_tests_properties(audio.realdata PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600)
