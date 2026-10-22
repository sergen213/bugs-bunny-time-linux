# tools/re (python prototypes) and tools/ghidra are deliberately not built.
add_executable(bbimport bbimport.cpp)
target_link_libraries(bbimport PRIVATE bb_core)

add_executable(bzedump bzedump.cpp)
target_link_libraries(bzedump PRIVATE bb_core)

# Mod kit CLI (docs/MODDING.md): asset export in the override layout, mod validation, new mod folders.
add_executable(bbtime-modkit modkit.cpp)
target_link_libraries(bbtime-modkit PRIVATE bb_game bb_settings)
