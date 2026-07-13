cmake_minimum_required(VERSION 3.16)

project(audio_lightbar_obs_plugin VERSION 0.1.0 LANGUAGES C)

include(GNUInstallDirs)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBOBS REQUIRED IMPORTED_TARGET libobs)
find_path(SIMDE_INCLUDE_DIR
	NAMES simde/x86/sse2.h
	DOC "SIMDe include directory required by OBS headers"
)

if(NOT SIMDE_INCLUDE_DIR)
	message(FATAL_ERROR "SIMDe headers were not found. Install libsimde-dev or set SIMDE_INCLUDE_DIR.")
endif()

add_library(audio-lightbar MODULE
	src/audio-lightbar.c
)

target_compile_features(audio-lightbar PRIVATE c_std_17)
target_link_libraries(audio-lightbar PRIVATE PkgConfig::LIBOBS)
target_include_directories(audio-lightbar PRIVATE "${SIMDE_INCLUDE_DIR}")

if(UNIX AND NOT APPLE)
	target_link_libraries(audio-lightbar PRIVATE m)
endif()

set_target_properties(audio-lightbar PROPERTIES
	PREFIX ""
	OUTPUT_NAME "audio-lightbar"
)

install(TARGETS audio-lightbar
	LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}/obs-plugins"
)
