# bb_game: LiT 30 Hz simulation (GAMEPLAY.md): pad words, heightfield collision, player controller, camera,
# collectibles. Headless (no SDL/GL) so tests can replay input deterministically.
add_library(bb_game STATIC
  src/heightfield.cpp
  src/pad.cpp
  src/lit_world.cpp
)
target_include_directories(bb_game PUBLIC include)
target_link_libraries(bb_game PUBLIC bb_core)

add_executable(test_game tests/test_game.cpp)
target_link_libraries(test_game PRIVATE bb_game)
target_compile_options(test_game PRIVATE -UNDEBUG)
add_test(NAME game.lit COMMAND test_game)
# Real-data part needs BBTIME_DATA=<dir containing lit/>; the synthetic part always runs (exit 77 only if both skip).
set_tests_properties(game.lit PROPERTIES TIMEOUT 300)
