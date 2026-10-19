cmake_minimum_required(VERSION 3.28)
project(bbtime VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(BBTIME_SANITIZE "Build with ASan/UBSan" OFF)
if(BBTIME_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()
add_compile_options(-Wall -Wextra)

enable_testing()

# Ownership: each subdirectory owns its own CMakeLists.txt.
add_subdirectory(engine/core)    # bb::core  — formats, BZE, LZSS, importer lib (no GPU deps)
add_subdirectory(engine/render)  # bb::render — SDL3 + OpenGL renderer
add_subdirectory(apps/bbtime)    # collection executable (game-select menu)
add_subdirectory(tools)          # CLI tools (bbimport, bzedump, ...)
add_subdirectory(tests)
