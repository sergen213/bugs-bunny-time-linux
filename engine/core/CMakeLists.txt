find_package(OpenSSL REQUIRED)
find_package(nlohmann_json REQUIRED)  # mod.json + glTF (mods.cpp, mods_gltf.cpp)

add_library(bb_core STATIC
  src/bze.cpp
  src/lzss.cpp
  src/levelscript.cpp
  src/manifest.cpp
  src/tim.cpp
  src/level.cpp
  src/tb_level.cpp
  src/actor.cpp
  src/tb_actor.cpp
  src/mods.cpp
  src/mods_gltf.cpp
)
target_include_directories(bb_core PUBLIC include)
target_link_libraries(bb_core PRIVATE OpenSSL::Crypto nlohmann_json::nlohmann_json bb_stb)
