# bb_game: LiT and TB 30 Hz simulations (GAMEPLAY.md, GAMEPLAY_TB.md): pads, collision, player controllers, camera,
# collectibles. Headless (no SDL/GL) so tests can replay input deterministically.
add_library(bb_game STATIC
  src/heightfield.cpp
  src/pad.cpp
  src/lit_world.cpp
  src/lit_logic.cpp
  src/lit_actors.cpp
  src/lit_overlay.cpp
  src/lit_hud.cpp
  src/lit_save.cpp
  src/tb_collision.cpp
  src/tb_world.cpp
  src/tb_entities.cpp
  src/tb_hud.cpp
  src/tb_menu.cpp
  src/tb_save.cpp
  src/sfx.cpp
)
target_include_directories(bb_game PUBLIC include)
target_link_libraries(bb_game PUBLIC bb_core bb_audio)

add_executable(test_game tests/test_game.cpp)
target_link_libraries(test_game PRIVATE bb_game)
target_compile_options(test_game PRIVATE -UNDEBUG)
add_test(NAME game.lit COMMAND test_game)
# Real-data part needs BBTIME_DATA=<dir containing lit/>; the synthetic part always runs (exit 77 only if both skip).
set_tests_properties(game.lit PROPERTIES TIMEOUT 300)

add_executable(test_tb tests/test_tb.cpp)
target_link_libraries(test_tb PRIVATE bb_game)
target_compile_options(test_tb PRIVATE -UNDEBUG)
add_test(NAME game.tb COMMAND test_tb)
set_tests_properties(game.tb PROPERTIES TIMEOUT 600)

add_executable(test_lit_save tests/test_lit_save.cpp)
target_link_libraries(test_lit_save PRIVATE bb_game)
target_compile_options(test_lit_save PRIVATE -UNDEBUG)
# Real part: Wine-produced saves in BBTIME_SAVES or $BBTIME_DATA/../local/saves/lit (skipped without them).
add_test(NAME game.lit_save COMMAND test_lit_save)

add_executable(test_lit_overlay tests/test_lit_overlay.cpp)
target_link_libraries(test_lit_overlay PRIVATE bb_game)
target_compile_options(test_lit_overlay PRIVATE -UNDEBUG)
# Real MERLIN data (BBTIME_DATA): the save prompt vs the Wine oracle; 77 = skipped.
add_test(NAME game.lit_overlay COMMAND test_lit_overlay)
set_tests_properties(game.lit_overlay PROPERTIES SKIP_RETURN_CODE 77)

add_executable(test_lit_title tests/test_lit_title.cpp)
target_link_libraries(test_lit_title PRIVATE bb_game)
target_compile_options(test_lit_title PRIVATE -UNDEBUG)
# Real title.bze (BBTIME_DATA): front-end logic vs the Wine oracle (RUNTIME_REFERENCE.md §14); 77 = skipped.
add_test(NAME game.lit_title COMMAND test_lit_title)
set_tests_properties(game.lit_title PROPERTIES SKIP_RETURN_CODE 77)

add_executable(test_tb_menu tests/test_tb_menu.cpp)
target_link_libraries(test_tb_menu PRIVATE bb_game)
target_compile_options(test_tb_menu PRIVATE -UNDEBUG)
# Real Datas/data/menu.men (BBTIME_DATA); the Savedata.dat round trip always runs; 77 = skipped.
add_test(NAME game.tb_menu COMMAND test_tb_menu)
set_tests_properties(game.tb_menu PROPERTIES SKIP_RETURN_CODE 77)

add_executable(test_tb_save tests/test_tb_save.cpp)
target_link_libraries(test_tb_save PRIVATE bb_game)
target_compile_options(test_tb_save PRIVATE -UNDEBUG)
# Real part: Wine-produced saves in BBTIME_SAVES or $BBTIME_DATA/../local/saves/tb (skipped without them).
add_test(NAME game.tb_save COMMAND test_tb_save)

add_executable(test_sfx tests/test_sfx.cpp)
target_link_libraries(test_sfx PRIVATE bb_game)
target_compile_options(test_sfx PRIVATE -UNDEBUG)
# Attenuation / volume / pitch against hand-evaluated exe formulas + the voice machine; real banks via BBTIME_DATA.
add_test(NAME game.sfx COMMAND test_sfx)
