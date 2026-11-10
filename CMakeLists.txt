cmake_minimum_required(VERSION 3.16)
project(ps-stream C)

set(CHIAKI_DIR "" CACHE PATH "chiaki-ng source directory")
option(PSSTREAM_TEST "Add the --demo test mode" OFF)

foreach(opt CHIAKI_ENABLE_TESTS CHIAKI_ENABLE_CLI CHIAKI_ENABLE_GUI CHIAKI_ENABLE_STEAM_SHORTCUT)
	set(${opt} OFF CACHE BOOL "" FORCE)
endforeach()
foreach(opt CHIAKI_ENABLE_SETSU CHIAKI_ENABLE_STEAMDECK_NATIVE CHIAKI_ENABLE_SPEEX CHIAKI_ENABLE_PI_DECODER)
	set(${opt} OFF CACHE STRING "" FORCE)
endforeach()
set(CHIAKI_ENABLE_FFMPEG_DECODER ON CACHE STRING "" FORCE)
set(CHIAKI_USE_SYSTEM_CURL ON CACHE STRING "" FORCE)

add_subdirectory("${CHIAKI_DIR}" chiaki EXCLUDE_FROM_ALL)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
pkg_check_modules(JSONC REQUIRED IMPORTED_TARGET json-c)

add_executable(ps-stream WIN32 src/main.c src/app.rc)
# The exe's version details come from APP_VERSION, so they can't disagree with it
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS src/main.c)
file(STRINGS src/main.c app_version_line REGEX "^#define APP_VERSION \"[0-9]+\.[0-9]+\.[0-9]+\"")
string(REGEX MATCH "([0-9]+)\.([0-9]+)\.([0-9]+)" app_version "${app_version_line}")
if(NOT app_version)
	message(FATAL_ERROR "APP_VERSION not found in src/main.c")
endif()
set_source_files_properties(src/app.rc PROPERTIES COMPILE_DEFINITIONS
	"PSV_MAJOR=${CMAKE_MATCH_1};PSV_MINOR=${CMAKE_MATCH_2};PSV_PATCH=${CMAKE_MATCH_3}")
target_link_libraries(ps-stream chiaki-lib PkgConfig::SDL2 PkgConfig::JSONC comctl32 shell32 ws2_32 iphlpapi wlanapi gdiplus ole32 oleaut32 gdi32 dwmapi advapi32 bcrypt d3d11 dxgi)
target_link_options(ps-stream PRIVATE -municode)
if(PSSTREAM_TEST)
	target_compile_definitions(ps-stream PRIVATE PSSTREAM_TEST)
endif()
