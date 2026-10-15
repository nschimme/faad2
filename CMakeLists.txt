# Available CMake versions:
#  - Ubuntu 20.04 LTS    : 3.16.3
#  - Solaris 11.4 SRU 15 : 3.15
cmake_minimum_required(VERSION 3.15)

project(LIBFAAD2 LANGUAGES C)

# Set a default build type if none was specified
set(default_build_type "RelWithDebInfo")
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  message(STATUS "Setting build type to '${default_build_type}' as none was specified.")
  set(CMAKE_BUILD_TYPE "${default_build_type}" CACHE
      STRING "Choose the type of build." FORCE)
  # Set the possible values of build type for cmake-gui
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
    "Debug" "Release" "MinSizeRel" "RelWithDebInfo")
endif()

# If FAAD is being bundled in another project, we don't want to
# install anything. However, we want to let people override this, so
# we'll use the FAAD_BUNDLED_MODE variable to let them do that; just
# set it to OFF in your project before you add_subdirectory(faad2).
get_directory_property(FAAD_PARENT_DIRECTORY PARENT_DIRECTORY)
if(NOT DEFINED FAAD_BUNDLED_MODE)
  # Bundled mode hasn't been set one way or the other, set the default
  # depending on whether or not we are the top-level project.
  if(FAAD_PARENT_DIRECTORY)
    set(FAAD_BUNDLED_MODE ON)
    if(NOT DEFINED BUILD_SHARED_LIBS)
      set(BUILD_SHARED_LIBS OFF)
    endif()
  else()
    set(FAAD_BUNDLED_MODE OFF)
    if(NOT DEFINED BUILD_SHARED_LIBS)
      set(BUILD_SHARED_LIBS ON)
    endif()
  endif()
endif()
mark_as_advanced(FAAD_BUNDLED_MODE)

if(BUILD_SHARED_LIBS)
  if(NOT DEFINED CMAKE_C_VISIBILITY_PRESET)
    set(CMAKE_C_VISIBILITY_PRESET hidden)
  endif()
  if (WIN32 AND NOT DEFINED CMAKE_DLL_NAME_WITH_SOVERSION)
    set(CMAKE_DLL_NAME_WITH_SOVERSION ON)
  endif()
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(FAAD3 REQUIRED IMPORTED_TARGET faad3)

include(GNUInstallDirs)

if(${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
  set(CMAKE_MACOS_RPATH TRUE)
  set(CMAKE_INSTALL_NAME_DIR "${CMAKE_INSTALL_PREFIX}/lib")
endif()

# Extract version information

set(CAPTURE_PACKAGE_VERSION "[ \\t]*\"PACKAGE_VERSION\"[ \\t]*:[ \\t]\"(.*)\"")
file(STRINGS "properties.json" _faad_version_line REGEX "${CAPTURE_PACKAGE_VERSION}")
string(REGEX REPLACE "${CAPTURE_PACKAGE_VERSION}" "\\1" FAAD_VERSION "${_faad_version_line}")
string(REPLACE "\." ";" FAAD_VERSION_PARTS ${FAAD_VERSION})
list(GET FAAD_VERSION_PARTS 0 FAAD_VERSION_MAJOR)
list(GET FAAD_VERSION_PARTS 1 FAAD_VERSION_MINOR)
list(GET FAAD_VERSION_PARTS 2 FAAD_VERSION_PATCH)
# Semantic -> library version
# NB(eustas): likely that will be always OK; if not, we could read "overrides" from properties.json
math(EXPR FAAD_ABI_VERSION_CURRENT "${FAAD_VERSION_MAJOR} + ${FAAD_VERSION_MINOR}")
set(FAAD_ABI_VERSION_AGE "${FAAD_VERSION_MINOR}")
set(FAAD_ABI_VERSION_REVISION "${FAAD_VERSION_PATCH}")
set(FAAD_ABI_COMPATIBILITY "${FAAD_VERSION_MAJOR}")

message(STATUS "FAAD PACKAGE_VERSION: ${FAAD_VERSION}")
mark_as_advanced(FAAD_VERSION FAAD_ABI_COMPATIBILITY FAAD_ABI_VERSION_AGE FAAD_ABI_VERSION_REVISION)

file(READ include/faad.h.in FAAD_H_SRC)
string(REGEX REPLACE "@VERSION@" "${FAAD_VERSION}" FAAD_H_SRC ${FAAD_H_SRC})
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/include/faad.h ${FAAD_H_SRC})

# The compatibility library contains only the API shim.
add_library(faad libfaad/shim.c)
set_target_properties(faad PROPERTIES C_VISIBILITY_PRESET hidden)
target_include_directories(faad PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_compile_definitions(faad PRIVATE FAAD2_VERSION="${FAAD_VERSION}")
if(BUILD_SHARED_LIBS)
  target_link_libraries(faad PRIVATE PkgConfig::FAAD3)
  if(WIN32)
    target_sources(faad PRIVATE libfaad2.def)
  endif()
else()
  target_link_libraries(faad PUBLIC PkgConfig::FAAD3)
endif()
if(NOT WIN32)
  target_link_libraries(faad PRIVATE m)
endif()
if(NOT MSVC)
  target_compile_options(faad PRIVATE -Wall -Wextra -pedantic)
endif()

option(FAAD_BUILD_TESTS "Build tests" OFF)
if(FAAD_BUILD_TESTS)
  enable_testing()
  add_executable(shim_test tests/shim_test.c)
  target_link_libraries(shim_test PRIVATE faad)
  if(NOT WIN32)
    target_link_libraries(shim_test PRIVATE m)
  endif()
  add_test(NAME shim_test COMMAND shim_test "${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
endif()

# Compile the libFuzzer entry points without requiring a fuzzing runtime.
option(FAAD_BUILD_FUZZ "Build fuzz entry point object targets" OFF)
if(FAAD_BUILD_FUZZ)
  foreach(name decode config)
    add_library(fuzz_${name} OBJECT fuzz/fuzz_${name}.c)
    target_link_libraries(fuzz_${name} PRIVATE faad)
  endforeach()
endif()

# Generate a pkg-config files

function(generate_pkg_config_path outvar path)
  string(LENGTH "${path}" path_length)

  set(path_args ${ARGV})
  list(REMOVE_AT path_args 0 1)
  list(LENGTH path_args path_args_remaining)

  set("${outvar}" "${path}")

  while(path_args_remaining GREATER 1)
    list(GET path_args 0 name)
    list(GET path_args 1 value)

    get_filename_component(value_full "${value}" ABSOLUTE)
    string(LENGTH "${value}" value_length)

    if(path_length EQUAL value_length AND path STREQUAL value)
      set("${outvar}" "\${${name}}")
      break()
    elseif(path_length GREATER value_length)
      # We might be in a subdirectory of the value, but we have to be
      # careful about a prefix matching but not being a subdirectory
      # (for example, /usr/lib64 is not a subdirectory of /usr/lib).
      # We'll do this by making sure the next character is a directory
      # separator.
      string(SUBSTRING "${path}" ${value_length} 1 sep)
      if(sep STREQUAL "/")
        string(SUBSTRING "${path}" 0 ${value_length} s)
        if(s STREQUAL value)
          string(SUBSTRING "${path}" "${value_length}" -1 suffix)
          set("${outvar}" "\${${name}}${suffix}")
          break()
        endif()
      endif()
    endif()

    list(REMOVE_AT path_args 0 1)
    list(LENGTH path_args path_args_remaining)
  endwhile()

  set("${outvar}" "${${outvar}}" PARENT_SCOPE)
endfunction(generate_pkg_config_path)

function(transform_pc_file INPUT_FILE OUTPUT_FILE VERSION)
  file(READ ${INPUT_FILE} TEXT)

  set(PREFIX "${CMAKE_INSTALL_PREFIX}")
  string(REGEX REPLACE "@prefix@" "${PREFIX}" TEXT ${TEXT})
  string(REGEX REPLACE "@exec_prefix@" "\${prefix}" TEXT ${TEXT})

  generate_pkg_config_path(LIBDIR "${CMAKE_INSTALL_FULL_LIBDIR}" exec_prefix "${PREFIX}")
  string(REGEX REPLACE "@libdir@" "${LIBDIR}" TEXT ${TEXT})

  generate_pkg_config_path(INCLUDEDIR "${CMAKE_INSTALL_FULL_INCLUDEDIR}" prefix "${PREFIX}")
  string(REGEX REPLACE "@includedir@" "${INCLUDEDIR}" TEXT ${TEXT})

  string(REGEX REPLACE "@VERSION@" "${VERSION}" TEXT ${TEXT})

  set(FAAD_PC_LIBS_PRIVATE "")
  if(NOT WIN32)
    set(FAAD_PC_LIBS_PRIVATE "-lm")
  endif()
  string(REPLACE "@FAAD_PC_LIBS_PRIVATE@" "${FAAD_PC_LIBS_PRIVATE}" TEXT "${TEXT}")
  file(WRITE ${OUTPUT_FILE} ${TEXT})
endfunction()

transform_pc_file("libfaad/faad2.pc.in" "${CMAKE_CURRENT_BINARY_DIR}/faad2.pc" "${FAAD_VERSION}")

set_target_properties(faad PROPERTIES
  VERSION "${FAAD_ABI_COMPATIBILITY}.${FAAD_ABI_VERSION_AGE}.${FAAD_ABI_VERSION_REVISION}"
  SOVERSION "${FAAD_ABI_COMPATIBILITY}"
)

# Installation

if(NOT FAAD_BUNDLED_MODE)
  install(FILES "${CMAKE_CURRENT_BINARY_DIR}/faad2.pc"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig")

  install(
    TARGETS faad
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
  )

  # exclude faad.h.in
  install(
    FILES ${CMAKE_CURRENT_BINARY_DIR}/include/faad.h
    FILES ${CMAKE_CURRENT_SOURCE_DIR}/include/neaacdec.h
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
  )

  install(
    FILES docs/libfaad.3
    DESTINATION ${CMAKE_INSTALL_MANDIR}/man3
  )
endif()  # FAAD_BUNDLED_MODE
