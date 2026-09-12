# SPDX-License-Identifier: GPL-3.0-or-later
# A single reviewed prefix supplies shared libusb and the static HackRF adapter.
# Configure never fetches dependencies, initializes USB, or enumerates devices.
set(OVMESH_HAVE_HACKRF_LIBRARY FALSE)
if(NOT OVMESH_ENABLE_HACKRF AND NOT OVMESH_ENABLE_RTLSDR)
  return()
endif()

set(_ovmesh_usb_help
  "Build the reviewed local dependencies from the repository root:\n  python3 tools/bootstrap_usb.py --download\nOr provide the reviewed prefix with -DOVMESH_USB_ROOT_DIR=PATH.\nUse a fresh build directory when changing the dependency prefix.\nFor a software-only build, set both OVMESH_ENABLE_HACKRF=OFF and OVMESH_ENABLE_RTLSDR=OFF.")
set(OVMESH_USB_ROOT_DIR "${PROJECT_SOURCE_DIR}/build/deps/usb-1.0.30-local"
  CACHE PATH "Reviewed shared libusb 1.0.30 and static HackRF prefix")
get_filename_component(_ovmesh_usb_root "${OVMESH_USB_ROOT_DIR}" ABSOLUTE BASE_DIR "${PROJECT_SOURCE_DIR}")
if(OVMESH_USB_ROOT_DIR STREQUAL "" OR NOT IS_DIRECTORY "${_ovmesh_usb_root}")
  message(FATAL_ERROR "Reviewed USB prefix is missing: ${_ovmesh_usb_root}\n${_ovmesh_usb_help}")
endif()
file(REAL_PATH "${_ovmesh_usb_root}" _ovmesh_usb_root)

function(_ovmesh_usb_require_inside path label)
  if(NOT EXISTS "${path}")
    message(FATAL_ERROR "Reviewed USB ${label} is missing: ${path}\n${_ovmesh_usb_help}")
  endif()
  file(REAL_PATH "${path}" _resolved)
  cmake_path(IS_PREFIX _ovmesh_usb_root "${_resolved}" NORMALIZE _inside)
  if(NOT _inside)
    message(FATAL_ERROR "Mixed USB configuration: ${label} is outside OVMESH_USB_ROOT_DIR.\nSelected: ${path}\nPrefix: ${_ovmesh_usb_root}\n${_ovmesh_usb_help}")
  endif()
endfunction()

# Cache and command-line paths are not search hints. Reject stale selections,
# including symlinks that leave the chosen prefix, rather than replacing them.
foreach(_variable RTLUSB_INCLUDE_DIR RTLUSB_LIBRARY RTLUSB_RUNTIME_LIBRARY
    HACKRF_INCLUDE_DIR HACKRF_LIBRARY HACKRF_INCLUDE_DIRS HACKRF_LIBRARY_DIRS
    RTLUSB_INCLUDE_DIRS RTLUSB_LIBRARY_DIRS)
  if(DEFINED ${_variable} AND NOT "${${_variable}}" STREQUAL "" AND NOT "${${_variable}}" MATCHES "-NOTFOUND$")
    foreach(_path IN LISTS ${_variable})
      _ovmesh_usb_require_inside("${_path}" "${_variable}")
    endforeach()
  endif()
endforeach()
if(DEFINED OVMESH_RTLUSB_TARGET AND NOT "${OVMESH_RTLUSB_TARGET}" STREQUAL "" AND
    NOT "${OVMESH_RTLUSB_TARGET}" STREQUAL "OVMesh::USB")
  message(FATAL_ERROR "Mixed USB configuration: OVMESH_RTLUSB_TARGET may not override the reviewed target.\n${_ovmesh_usb_help}")
endif()

find_path(RTLUSB_INCLUDE_DIR NAMES libusb.h
  PATHS "${_ovmesh_usb_root}/include/libusb-1.0" "${_ovmesh_usb_root}/include"
  NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
if(NOT RTLUSB_INCLUDE_DIR)
  message(FATAL_ERROR "Reviewed USB libusb header is missing.\n${_ovmesh_usb_help}")
endif()
_ovmesh_usb_require_inside("${RTLUSB_INCLUDE_DIR}/libusb.h" "libusb header")
set(_ovmesh_saved_library_suffixes "${CMAKE_FIND_LIBRARY_SUFFIXES}")
if(WIN32)
  set(CMAKE_FIND_LIBRARY_SUFFIXES .lib .dll.a)
elseif(APPLE)
  set(CMAKE_FIND_LIBRARY_SUFFIXES .dylib)
else()
  set(CMAKE_FIND_LIBRARY_SUFFIXES .so)
endif()
find_library(RTLUSB_LIBRARY NAMES usb-1.0 libusb-1.0
  PATHS "${_ovmesh_usb_root}/lib" "${_ovmesh_usb_root}/lib64"
  NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
set(CMAKE_FIND_LIBRARY_SUFFIXES "${_ovmesh_saved_library_suffixes}")
if(NOT RTLUSB_LIBRARY)
  message(FATAL_ERROR "Reviewed USB shared libusb library is missing.\n${_ovmesh_usb_help}")
endif()
_ovmesh_usb_require_inside("${RTLUSB_LIBRARY}" "libusb library")
file(REAL_PATH "${RTLUSB_LIBRARY}" _ovmesh_usb_library)
if(WIN32)
  if(NOT RTLUSB_LIBRARY MATCHES "\\.(lib|dll\\.a)$")
    message(FATAL_ERROR "Reviewed USB requires a libusb import library on Windows.\n${_ovmesh_usb_help}")
  endif()
  find_file(RTLUSB_RUNTIME_LIBRARY NAMES libusb-1.0.dll usb-1.0.dll
    PATHS "${_ovmesh_usb_root}/bin" "${_ovmesh_usb_root}/lib"
    NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
  if(NOT RTLUSB_RUNTIME_LIBRARY)
    message(FATAL_ERROR "Reviewed USB libusb runtime DLL is missing.\n${_ovmesh_usb_help}")
  endif()
  _ovmesh_usb_require_inside("${RTLUSB_RUNTIME_LIBRARY}" "libusb runtime DLL")
elseif((APPLE AND NOT _ovmesh_usb_library MATCHES "\\.dylib$") OR
    (NOT APPLE AND NOT _ovmesh_usb_library MATCHES "\\.so(\\.[0-9]+)*$"))
  message(FATAL_ERROR "Reviewed USB requires shared libusb, not ${RTLUSB_LIBRARY}.\n${_ovmesh_usb_help}")
endif()
if(NOT WIN32)
  file(READ "${_ovmesh_usb_library}" _ovmesh_usb_magic LIMIT 8 HEX)
  if(_ovmesh_usb_magic MATCHES "^(213c617263683e0a|213c7468696e3e0a)")
    message(FATAL_ERROR "Reviewed USB requires shared libusb; an archive renamed as a shared library is not accepted.\n${_ovmesh_usb_help}")
  endif()
endif()

if(OVMESH_ENABLE_HACKRF)
  # The application prefers <libhackrf/hackrf.h>. Requiring that installed
  # layout keeps its __has_include branch from selecting an ambient host header
  # when a caller supplies only a flat hackrf.h include directory.
  find_path(HACKRF_INCLUDE_DIR NAMES libhackrf/hackrf.h
    PATHS "${_ovmesh_usb_root}/include" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
  set(_ovmesh_hackrf_header "${HACKRF_INCLUDE_DIR}/libhackrf/hackrf.h")
  _ovmesh_usb_require_inside("${_ovmesh_hackrf_header}" "HackRF header")
  if(WIN32 AND MSVC)
    set(CMAKE_FIND_LIBRARY_SUFFIXES .lib)
  else()
    set(CMAKE_FIND_LIBRARY_SUFFIXES .a)
  endif()
  find_library(HACKRF_LIBRARY NAMES hackrf hackrf_static libhackrf
    PATHS "${_ovmesh_usb_root}/lib" "${_ovmesh_usb_root}/lib64"
    NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
  set(CMAKE_FIND_LIBRARY_SUFFIXES "${_ovmesh_saved_library_suffixes}")
  if(NOT HACKRF_LIBRARY)
    message(FATAL_ERROR "Reviewed USB static HackRF archive is missing.\n${_ovmesh_usb_help}")
  endif()
  _ovmesh_usb_require_inside("${HACKRF_LIBRARY}" "HackRF archive")
  if((WIN32 AND MSVC AND NOT HACKRF_LIBRARY MATCHES "\\.lib$") OR
      (NOT (WIN32 AND MSVC) AND NOT HACKRF_LIBRARY MATCHES "\\.a$"))
    message(FATAL_ERROR "Reviewed USB requires a static HackRF archive, not ${HACKRF_LIBRARY}.\n${_ovmesh_usb_help}")
  endif()
  # Thin archives can reference object files outside the reviewed prefix.
  file(READ "${HACKRF_LIBRARY}" _ovmesh_hackrf_magic LIMIT 8 HEX)
  if(NOT _ovmesh_hackrf_magic STREQUAL "213c617263683e0a")
    message(FATAL_ERROR "Reviewed USB requires a regular static HackRF archive. Thin archives and renamed shared libraries are not accepted.\n${_ovmesh_usb_help}")
  endif()
  if(WIN32 AND MSVC)
    # Both MSVC static libraries and DLL import libraries use .lib archives.
    # An import descriptor would reintroduce a dynamic HackRF dependency.
    file(STRINGS "${HACKRF_LIBRARY}" _ovmesh_hackrf_imports
      REGEX "__IMPORT_DESCRIPTOR_|_NULL_IMPORT_DESCRIPTOR")
    if(_ovmesh_hackrf_imports)
      message(FATAL_ERROR "Reviewed USB requires static HackRF objects, not a Windows DLL import library.\n${_ovmesh_usb_help}")
    endif()
  endif()
endif()

# The helper's receipt binds intake versions to the selected canonical files.
# It detects stale or mixed local builds; it is not a signature or provenance
# proof. LIBUSB_API_VERSION alone is not the installed release version.
_ovmesh_usb_require_inside("${_ovmesh_usb_root}/ovmesh-usb-intake.json" "intake receipt")
file(READ "${_ovmesh_usb_root}/ovmesh-usb-intake.json" _ovmesh_usb_receipt)
foreach(_field version libusb_version)
  string(JSON _value ERROR_VARIABLE _error GET "${_ovmesh_usb_receipt}" "${_field}")
  if(_error OR (_field STREQUAL "version" AND NOT _value STREQUAL "1") OR
      (_field STREQUAL "libusb_version" AND NOT _value STREQUAL "1.0.30"))
    message(FATAL_ERROR "Reviewed USB intake receipt requires libusb 1.0.30 and receipt version 1.\n${_ovmesh_usb_help}")
  endif()
endforeach()
if(OVMESH_ENABLE_HACKRF)
  string(JSON _value ERROR_VARIABLE _error GET "${_ovmesh_usb_receipt}" hackrf_version)
  if(_error OR NOT _value STREQUAL "2024.02.1")
    message(FATAL_ERROR "Reviewed USB intake receipt requires HackRF 2024.02.1.\n${_ovmesh_usb_help}")
  endif()
endif()
function(_ovmesh_usb_check_digest path)
  file(REAL_PATH "${path}" _resolved)
  file(RELATIVE_PATH _relative "${_ovmesh_usb_root}" "${_resolved}")
  string(JSON _expected ERROR_VARIABLE _error GET "${_ovmesh_usb_receipt}" files "${_relative}")
  file(SHA256 "${_resolved}" _actual)
  if(_error OR NOT _actual STREQUAL _expected)
    message(FATAL_ERROR "Reviewed USB intake receipt digest mismatch or missing entry: ${_relative}.\n${_ovmesh_usb_help}")
  endif()
endfunction()
_ovmesh_usb_check_digest("${RTLUSB_INCLUDE_DIR}/libusb.h")
_ovmesh_usb_check_digest("${RTLUSB_LIBRARY}")
if(WIN32)
  _ovmesh_usb_check_digest("${RTLUSB_RUNTIME_LIBRARY}")
endif()
if(OVMESH_ENABLE_HACKRF)
  _ovmesh_usb_check_digest("${_ovmesh_hackrf_header}")
  _ovmesh_usb_check_digest("${HACKRF_LIBRARY}")
endif()

if(TARGET OVMesh::USB OR TARGET hackrf_external)
  message(FATAL_ERROR "Reviewed USB targets already exist; use an isolated configuration.\n${_ovmesh_usb_help}")
endif()
add_library(OVMesh::USB SHARED IMPORTED)
set_target_properties(OVMesh::USB PROPERTIES
  IMPORTED_LOCATION "${_ovmesh_usb_library}"
  INTERFACE_INCLUDE_DIRECTORIES "${RTLUSB_INCLUDE_DIR}")
if(WIN32)
  set_target_properties(OVMesh::USB PROPERTIES IMPORTED_IMPLIB "${_ovmesh_usb_library}"
    IMPORTED_LOCATION "${RTLUSB_RUNTIME_LIBRARY}")
endif()
set(OVMESH_RTLUSB_TARGET OVMesh::USB)
if(OVMESH_ENABLE_HACKRF)
  add_library(hackrf_external STATIC IMPORTED)
  set_target_properties(hackrf_external PROPERTIES IMPORTED_LOCATION "${HACKRF_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${HACKRF_INCLUDE_DIR}"
    INTERFACE_LINK_LIBRARIES "OVMesh::USB;Threads::Threads")
  if(UNIX AND NOT APPLE)
    set_property(TARGET hackrf_external APPEND PROPERTY INTERFACE_LINK_LIBRARIES m)
  endif()
  set(OVMESH_HAVE_HACKRF_LIBRARY TRUE)
endif()

# This probe calls only immutable library-version accessors. There is no USB
# initialization, device enumeration, open, radio reception, or transmission.
function(_ovmesh_usb_check_configuration)
  set(_source "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/ovmesh-usb-check.c")
  set(_code "#include <libusb.h>\n#include <stdio.h>\n")
  set(_links OVMesh::USB)
  if(OVMESH_ENABLE_HACKRF)
    string(APPEND _code "#include \"${_ovmesh_hackrf_header}\"\n")
    list(PREPEND _links hackrf_external)
  endif()
  string(APPEND _code "int main(void) {\n  const struct libusb_version *v = libusb_get_version();\n  if (!v) return 1;\n  printf(\"libusb %u.%u.%u\\n\", v->major, v->minor, v->micro);\n")
  if(OVMESH_ENABLE_HACKRF)
    string(APPEND _code "  if (!hackrf_library_version()) return 2;\n")
  endif()
  string(APPEND _code "  return v->major == 1 && v->minor == 0 && v->micro == 30 ? 0 : 3;\n}\n")
  file(WRITE "${_source}" "${_code}")
  set(CMAKE_TRY_COMPILE_TARGET_TYPE EXECUTABLE)
  unset(_ovmesh_usb_compiles CACHE)
  unset(_ovmesh_usb_runs CACHE)
  if(CMAKE_CROSSCOMPILING)
    try_compile(_ovmesh_usb_compiles "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/ovmesh-usb-check"
      SOURCES "${_source}" LINK_LIBRARIES ${_links} OUTPUT_VARIABLE _output)
  else()
    if(WIN32)
      # The native probe is built in CMakeFiles, away from the selected DLL.
      # Give only this subprocess the prefix's runtime location, then restore.
      set(_saved_path "$ENV{PATH}")
      get_filename_component(_dll_directory "${RTLUSB_RUNTIME_LIBRARY}" DIRECTORY)
      set(ENV{PATH} "${_dll_directory};$ENV{PATH}")
    endif()
    try_run(_ovmesh_usb_runs _ovmesh_usb_compiles "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/ovmesh-usb-check"
      SOURCES "${_source}" LINK_LIBRARIES ${_links}
      COMPILE_OUTPUT_VARIABLE _output RUN_OUTPUT_VARIABLE _run_output)
    if(WIN32)
      set(ENV{PATH} "${_saved_path}")
    endif()
  endif()
  set(_compiled "${_ovmesh_usb_compiles}")
  set(_ran "${_ovmesh_usb_runs}")
  unset(_ovmesh_usb_compiles CACHE)
  unset(_ovmesh_usb_runs CACHE)
  file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/ovmesh-usb-check.log" "${_output}\n${_run_output}")
  if(NOT _compiled OR (NOT CMAKE_CROSSCOMPILING AND NOT _ran STREQUAL "0"))
    message(FATAL_ERROR "Reviewed USB configuration, link, or runtime release check failed.\nCompiler/runtime details: ${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/ovmesh-usb-check.log\n${_ovmesh_usb_help}")
  endif()
endfunction()
_ovmesh_usb_check_configuration()
if(CMAKE_CROSSCOMPILING)
  message(STATUS "Reviewed USB receipt and link check passed; target runtime libusb 1.0.30 qualification remains pending")
else()
  message(STATUS "Reviewed USB receipt, link, and runtime libusb 1.0.30 check passed (no USB initialization)")
endif()
