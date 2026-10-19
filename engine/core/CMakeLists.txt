find_package(OpenSSL REQUIRED)

add_library(bb_core STATIC
  src/bze.cpp
  src/lzss.cpp
  src/levelscript.cpp
  src/manifest.cpp
)
target_include_directories(bb_core PUBLIC include)
target_link_libraries(bb_core PRIVATE OpenSSL::Crypto)
