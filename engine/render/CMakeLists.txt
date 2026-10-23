find_package(SDL3 REQUIRED CONFIG)
find_package(glm REQUIRED CONFIG)
find_package(PkgConfig REQUIRED)
pkg_check_modules(EPOXY REQUIRED IMPORTED_TARGET epoxy) # GL loader (libepoxy)

add_library(bb_render STATIC src/renderer.cpp src/camera.cpp src/bmp.cpp src/zones.cpp src/water_waves.cpp)
target_include_directories(bb_render PUBLIC include)
target_link_libraries(bb_render PUBLIC bb_core SDL3::SDL3 glm::glm PRIVATE PkgConfig::EPOXY)

foreach(t test_projection test_bmp test_zones test_water_waves)
  add_executable(${t} tests/${t}.cpp)
  target_link_libraries(${t} PRIVATE bb_render)
  add_test(NAME render.${t} COMMAND ${t})
endforeach()
