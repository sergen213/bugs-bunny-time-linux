# bb_settings: settings.ini shared by bbtime and the launcher (source lives with the game, no Qt/SDL).
add_library(bb_settings STATIC ../bbtime/settings.cpp)
target_include_directories(bb_settings PUBLIC ../bbtime)

# bb_updater: signed HTTPS updater (libsodium Ed25519 + SHA-256, libcurl, nlohmann_json). No Qt.
find_package(CURL REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SODIUM REQUIRED IMPORTED_TARGET libsodium)
set(BBTIME_UPDATE_PUBKEY_FILE "${PROJECT_SOURCE_DIR}/tools/release/bbtime-update.pub"
    CACHE FILEPATH "Release signing public key (base64 raw Ed25519), see docs/UPDATES.md")
file(STRINGS "${BBTIME_UPDATE_PUBKEY_FILE}" BBTIME_UPDATE_PUBKEY LIMIT_COUNT 1)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${BBTIME_UPDATE_PUBKEY_FILE}")
add_library(bb_updater STATIC updater.cpp)
target_include_directories(bb_updater PUBLIC .)
target_compile_definitions(bb_updater PRIVATE BBTIME_UPDATE_PUBKEY="${BBTIME_UPDATE_PUBKEY}")
target_link_libraries(bb_updater PUBLIC PkgConfig::SODIUM PRIVATE CURL::libcurl nlohmann_json::nlohmann_json)

add_library(bb_launcher_files STATIC files.cpp)
target_include_directories(bb_launcher_files PUBLIC .)
target_link_libraries(bb_launcher_files PUBLIC bb_core)

# assert()-based tests: keep asserts in every build type.
foreach(t settings updater files)
  add_executable(test_launcher_${t} tests/test_${t}.cpp)
  target_compile_options(test_launcher_${t} PRIVATE -UNDEBUG)
endforeach()
target_link_libraries(test_launcher_settings PRIVATE bb_settings)
target_link_libraries(test_launcher_updater PRIVATE bb_updater)
target_compile_definitions(test_launcher_updater PRIVATE
  BBTIME_HTTPS_SERVER="${CMAKE_CURRENT_SOURCE_DIR}/tests/https_server.py"
  BBTIME_RELEASE_DIR="${PROJECT_SOURCE_DIR}/tools/release")
target_link_libraries(test_launcher_files PRIVATE bb_launcher_files)
add_test(NAME launcher.settings COMMAND test_launcher_settings)
add_test(NAME launcher.updater COMMAND test_launcher_updater)
add_test(NAME launcher.files COMMAND test_launcher_files)
add_test(NAME launcher.install_realdata COMMAND test_launcher_files real)
# updater needs python3 + cryptography for its local HTTPS server; real-data needs BBTIME_DATA. 77 = skipped.
set_tests_properties(launcher.updater launcher.install_realdata PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)

# bbtime-launcher: Qt 6 Widgets front end. Optional so headless/CI builds without Qt still get the game.
find_package(Qt6 6.4 COMPONENTS Widgets Concurrent)
find_package(SDL3 CONFIG)
if(NOT Qt6_FOUND OR NOT SDL3_FOUND)
  message(STATUS "bbtime-launcher disabled (needs Qt6 Widgets/Concurrent and SDL3)")
  return()
endif()
qt_add_executable(bbtime-launcher main.cpp)
qt_add_resources(bbtime-launcher changelog PREFIX "/" BASE "${PROJECT_SOURCE_DIR}" FILES "${PROJECT_SOURCE_DIR}/CHANGELOG.md")
set_target_properties(bbtime-launcher PROPERTIES AUTOMOC ON)
target_compile_definitions(bbtime-launcher PRIVATE BBTIME_VERSION="${PROJECT_VERSION}")
target_link_libraries(bbtime-launcher PRIVATE bb_settings bb_updater bb_launcher_files Qt6::Widgets Qt6::Concurrent SDL3::SDL3)
