cmake_minimum_required(VERSION 3.28...3.30)

find_package(Libx265 REQUIRED)

if(NOT TARGET OBS::opts-parser)
  add_subdirectory("${CMAKE_SOURCE_DIR}/shared/opts-parser" "${CMAKE_BINARY_DIR}/shared/opts-parser")
endif()

add_library(obs-x265 MODULE)
add_library(OBS::x265 ALIAS obs-x265)

target_sources(obs-x265 PRIVATE obs-x265.c obs-x265-plugin-main.c)
target_link_libraries(obs-x265 PRIVATE OBS::opts-parser Libx265::Libx265)

if(OS_WINDOWS)
  configure_file(cmake/windows/obs-module.rc.in obs-x265.rc)
  target_sources(obs-x265 PRIVATE obs-x265.rc)
endif()

set_target_properties_obs(obs-x265 PROPERTIES FOLDER plugins/obs-x265 PREFIX "")
