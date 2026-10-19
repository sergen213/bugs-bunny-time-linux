# tools/re (python prototypes) and tools/ghidra are deliberately not built.
add_executable(bbimport bbimport.cpp)
target_link_libraries(bbimport PRIVATE bb_core)

add_executable(bzedump bzedump.cpp)
target_link_libraries(bzedump PRIVATE bb_core)
