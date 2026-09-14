#[=======================================================================[.rst
FindLibx265
-----------

FindModule for Libx265 and associated libraries

.. versionadded:: 30.0
  Initial FindModule for Libx265

Imported Targets
^^^^^^^^^^^^^^^^

This module defines the :prop_tgt:`IMPORTED` target ``Libx265::Libx265``.

Result Variables
^^^^^^^^^^^^^^^^

This module sets the following variables:

``Libx265_FOUND``
  True, if all required components and the core library were found.
``Libx265_VERSION``
  Detected version of found Libx265 libraries.

Cache variables
^^^^^^^^^^^^^^^

The following cache variables may also be set:

``Libx265_LIBRARY``
  Path to the library component of Libx265.
``Libx265_INCLUDE_DIR``
  Directory containing ``x265.h``.

#]=======================================================================]

include(FindPackageHandleStandardArgs)

find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
  pkg_search_module(PC_Libx265 QUIET libx265 x265)
endif()

macro(Libx265_set_soname)
  if(CMAKE_HOST_SYSTEM_NAME MATCHES "Darwin")
    execute_process(
      COMMAND sh -c "otool -D '${Libx265_LIBRARY}' | grep -v '${Libx265_LIBRARY}'"
      OUTPUT_VARIABLE _output
      RESULT_VARIABLE _result
    )
    if(_result EQUAL 0 AND _output MATCHES "^@rpath/")
      set_property(TARGET Libx265::Libx265 PROPERTY IMPORTED_SONAME "${_output}")
    endif()
  elseif(CMAKE_HOST_SYSTEM_NAME MATCHES "Linux|FreeBSD")
    execute_process(
      COMMAND sh -c "objdump -p '${Libx265_LIBRARY}' | grep SONAME"
      OUTPUT_VARIABLE _output
      RESULT_VARIABLE _result
    )
    if(_result EQUAL 0)
      string(REGEX REPLACE "[ \t]+SONAME[ \t]+([^ \t]+)" "\\1" _soname "${_output}")
      set_property(TARGET Libx265::Libx265 PROPERTY IMPORTED_SONAME "${_soname}")
      unset(_soname)
    endif()
  endif()
  unset(_output)
  unset(_result)
endmacro()

macro(Libx265_find_dll)
  if(DEFINED Libx265_IMPLIB AND Libx265_IMPLIB)
    cmake_path(GET Libx265_IMPLIB PARENT_PATH _implib_path)
    cmake_path(SET _bin_path NORMALIZE "${_implib_path}/../bin")
  else()
    set(_bin_path "")
  endif()

  find_program(
    Libx265_DLL
    NAMES libx265.dll x265.dll
    HINTS ${_implib_path} ${_bin_path} ${PC_Libx265_LIBRARY_DIRS}
    DOC "Libx265 DLL location"
  )

  if(Libx265_DLL)
    set(Libx265_LIBRARY "${Libx265_DLL}")
  elseif(DEFINED Libx265_IMPLIB AND Libx265_IMPLIB)
    set(Libx265_LIBRARY "${Libx265_IMPLIB}")
  endif()

  unset(_implib_path)
  unset(_bin_path)
endmacro()

find_path(
  Libx265_INCLUDE_DIR
  NAMES x265.h
  HINTS ${PC_Libx265_INCLUDE_DIRS}
  PATHS /usr/include /usr/local/include
  DOC "Libx265 include directory"
)

if(PC_Libx265_VERSION VERSION_GREATER 0)
  set(Libx265_VERSION ${PC_Libx265_VERSION})
elseif(EXISTS "${Libx265_INCLUDE_DIR}/x265_config.h")
  file(STRINGS "${Libx265_INCLUDE_DIR}/x265_config.h" _VERSION_STRING REGEX "#define[ \t]+X265_BUILD[ \t]+[0-9]+")
  string(REGEX REPLACE ".*#define[ \t]+X265_BUILD[ \t]+([0-9]+).*" "\\1" Libx265_VERSION "${_VERSION_STRING}")
else()
  if(NOT Libx265_FIND_QUIETLY)
    message(AUTHOR_WARNING "Failed to find Libx265 version.")
  endif()
  set(Libx265_VERSION 0.0.0)
endif()

if(CMAKE_HOST_SYSTEM_NAME MATCHES "Windows")
  find_library(Libx265_IMPLIB NAMES x265 libx265 DOC "Libx265 import library location")
  Libx265_find_dll()
else()
  find_library(
    Libx265_LIBRARY
    NAMES x265 libx265
    HINTS ${PC_Libx265_LIBRARY_DIRS}
    PATHS /usr/lib /usr/local/lib
    DOC "Libx265 location"
  )
endif()

if(CMAKE_HOST_SYSTEM_NAME MATCHES "Darwin|Windows")
  set(Libx265_ERROR_REASON "Ensure that obs-deps is provided as part of CMAKE_PREFIX_PATH.")
elseif(CMAKE_HOST_SYSTEM_NAME MATCHES "Linux|FreeBSD")
  set(Libx265_ERROR_REASON "Ensure that x265 is installed on the system.")
endif()

find_package_handle_standard_args(
  Libx265
  REQUIRED_VARS Libx265_LIBRARY Libx265_INCLUDE_DIR
  VERSION_VAR Libx265_VERSION
  REASON_FAILURE_MESSAGE "${Libx265_ERROR_REASON}"
)
mark_as_advanced(Libx265_INCLUDE_DIR Libx265_LIBRARY Libx265_IMPLIB)
unset(Libx265_ERROR_REASON)

if(Libx265_FOUND)
  if(NOT TARGET Libx265::Libx265)
    if(IS_ABSOLUTE "${Libx265_LIBRARY}")
      if(DEFINED Libx265_IMPLIB AND Libx265_IMPLIB)
        if(Libx265_IMPLIB STREQUAL Libx265_LIBRARY)
          add_library(Libx265::Libx265 STATIC IMPORTED)
        else()
          add_library(Libx265::Libx265 SHARED IMPORTED)
          set_property(TARGET Libx265::Libx265 PROPERTY IMPORTED_IMPLIB "${Libx265_IMPLIB}")
        endif()
      else()
        # No import library available; use UNKNOWN so CMake links the DLL directly
        add_library(Libx265::Libx265 UNKNOWN IMPORTED)
      endif()
      set_property(TARGET Libx265::Libx265 PROPERTY IMPORTED_LOCATION "${Libx265_LIBRARY}")
    else()
      add_library(Libx265::Libx265 INTERFACE IMPORTED)
      set_property(TARGET Libx265::Libx265 PROPERTY IMPORTED_LIBNAME "${Libx265_LIBRARY}")
    endif()

    Libx265_set_soname()
    set_target_properties(
      Libx265::Libx265
      PROPERTIES
        INTERFACE_COMPILE_OPTIONS "${PC_Libx265_CFLAGS_OTHER}"
        INTERFACE_INCLUDE_DIRECTORIES "${Libx265_INCLUDE_DIR}"
        VERSION ${Libx265_VERSION}
    )
  endif()
endif()

include(FeatureSummary)
set_package_properties(
  Libx265
  PROPERTIES
    URL "https://x265.readthedocs.io/"
    DESCRIPTION
      "x265 is a free software library and application for encoding video streams into the H.265/HEVC compression format."
)
