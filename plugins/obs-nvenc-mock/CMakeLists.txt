cmake_minimum_required(VERSION 3.28...3.30)

option(ENABLE_NVENC_MOCK "Build Mock NVENC Plugin (VRAM Measurement)" OFF)

if(NOT ENABLE_NVENC_MOCK)
  target_disable_feature(obs-nvenc-mock "Mock NVENC Encoder (VRAM Test)")
  target_disable(obs-nvenc-mock)
  return()
endif()

add_library(obs-nvenc-mock MODULE)
add_library(OBS::nvenc-mock ALIAS obs-nvenc-mock)

target_sources(
  obs-nvenc-mock
  PRIVATE
    nvenc-mock.c
)

target_link_libraries(
  obs-nvenc-mock
  PRIVATE OBS::libobs
)

set_target_properties_obs(obs-nvenc-mock PROPERTIES FOLDER plugins/obs-nvenc-mock PREFIX "")