cmake_minimum_required(VERSION 3.28...3.30)

add_library(abr-gov OBJECT)
add_library(OBS::abr-gov ALIAS abr-gov)

target_sources(abr-gov PRIVATE abr-gov.c PUBLIC abr-gov.h)

target_include_directories(abr-gov PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(abr-gov PUBLIC OBS::libobs)

set_target_properties(abr-gov PROPERTIES FOLDER deps)
