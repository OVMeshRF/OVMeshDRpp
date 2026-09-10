# SPDX-License-Identifier: GPL-3.0-or-later
# Never download or fall back to an unrelated system crypto installation.
set(_ovmesh_openssl_help
  "Build the reviewed local dependency from the repository root:\n  python3 tools/bootstrap_openssl.py --download\nOr use its --archive PATH option for an already downloaded archive.\nThen configure again. See docs/security/openssl-intake.md.\nOPENSSL_ROOT_DIR must identify that staged prefix, not the openssl executable.\nIf changing an existing build's dependency prefix, use a fresh build directory.")

if(NOT DEFINED OPENSSL_ROOT_DIR OR OPENSSL_ROOT_DIR STREQUAL "")
  if(DEFINED ENV{OPENSSL_ROOT_DIR} AND NOT "$ENV{OPENSSL_ROOT_DIR}" STREQUAL "")
    set(OPENSSL_ROOT_DIR "$ENV{OPENSSL_ROOT_DIR}" CACHE PATH "Reviewed static OpenSSL prefix" FORCE)
  else()
    set(OPENSSL_ROOT_DIR "${PROJECT_SOURCE_DIR}/build/deps/openssl-3.5.8-local"
      CACHE PATH "Reviewed static OpenSSL prefix" FORCE)
  endif()
endif()
get_filename_component(_ovmesh_openssl_root "${OPENSSL_ROOT_DIR}" ABSOLUTE BASE_DIR "${PROJECT_SOURCE_DIR}")
if(NOT IS_DIRECTORY "${_ovmesh_openssl_root}")
  message(FATAL_ERROR "Reviewed OpenSSL 3.5.8 prefix is missing: ${_ovmesh_openssl_root}\n${_ovmesh_openssl_help}")
endif()
file(REAL_PATH "${_ovmesh_openssl_root}" _ovmesh_openssl_root)

function(_ovmesh_openssl_require_inside path label)
  if(NOT EXISTS "${path}")
    message(FATAL_ERROR "Reviewed OpenSSL ${label} is missing: ${path}\n${_ovmesh_openssl_help}")
  endif()
  file(REAL_PATH "${path}" _resolved)
  cmake_path(IS_PREFIX _ovmesh_openssl_root "${_resolved}" NORMALIZE _inside)
  if(NOT _inside)
    message(FATAL_ERROR "Mixed OpenSSL configuration: ${label} is outside OPENSSL_ROOT_DIR.\nSelected: ${path}\nPrefix: ${_ovmesh_openssl_root}\n${_ovmesh_openssl_help}")
  endif()
endfunction()

# Reject stale overrides rather than silently replacing a user's cached paths.
foreach(_variable OPENSSL_INCLUDE_DIR OPENSSL_CRYPTO_LIBRARY LIB_EAY LIB_EAY_DEBUG LIB_EAY_RELEASE)
  if(DEFINED ${_variable} AND NOT "${${_variable}}" STREQUAL "" AND NOT "${${_variable}}" MATCHES "-NOTFOUND$")
    _ovmesh_openssl_require_inside("${${_variable}}" "${_variable}")
  endif()
endforeach()
if(NOT OPENSSL_INCLUDE_DIR)
  set(OPENSSL_INCLUDE_DIR "${_ovmesh_openssl_root}/include" CACHE PATH "Reviewed OpenSSL headers" FORCE)
endif()
foreach(_header crypto.h opensslv.h opensslconf.h configuration.h)
  _ovmesh_openssl_require_inside("${OPENSSL_INCLUDE_DIR}/openssl/${_header}" "header ${_header}")
endforeach()

# Restrict the archive search to the selected prefix. FindOpenSSL's ROOT_DIR
# alone is merely a hint and can otherwise select system headers/libraries.
set(_ovmesh_saved_library_suffixes "${CMAKE_FIND_LIBRARY_SUFFIXES}")
if(WIN32 AND MSVC)
  set(CMAKE_FIND_LIBRARY_SUFFIXES .lib)
else()
  set(CMAKE_FIND_LIBRARY_SUFFIXES .a)
endif()
find_library(OPENSSL_CRYPTO_LIBRARY NAMES crypto libcrypto_static libcrypto
  PATHS "${_ovmesh_openssl_root}/lib" "${_ovmesh_openssl_root}/lib64" NO_DEFAULT_PATH)
set(CMAKE_FIND_LIBRARY_SUFFIXES "${_ovmesh_saved_library_suffixes}")
if(NOT OPENSSL_CRYPTO_LIBRARY)
  message(FATAL_ERROR "Reviewed OpenSSL static libcrypto archive is missing under ${_ovmesh_openssl_root}.\n${_ovmesh_openssl_help}")
endif()
_ovmesh_openssl_require_inside("${OPENSSL_CRYPTO_LIBRARY}" "crypto archive")
if((WIN32 AND MSVC AND NOT OPENSSL_CRYPTO_LIBRARY MATCHES "\\.lib$") OR
   (NOT (WIN32 AND MSVC) AND NOT OPENSSL_CRYPTO_LIBRARY MATCHES "\\.a$"))
  message(FATAL_ERROR "Reviewed OpenSSL requires a static crypto archive, not ${OPENSSL_CRYPTO_LIBRARY}.\n${_ovmesh_openssl_help}")
endif()
if(WIN32)
  # FindOpenSSL uses these instead of OPENSSL_CRYPTO_LIBRARY on Windows.
  set(LIB_EAY "${OPENSSL_CRYPTO_LIBRARY}")
  set(LIB_EAY_DEBUG "${OPENSSL_CRYPTO_LIBRARY}")
  set(LIB_EAY_RELEASE "${OPENSSL_CRYPTO_LIBRARY}")
endif()
set(OPENSSL_USE_STATIC_LIBS TRUE)
find_package(OpenSSL 3.5.8 EXACT QUIET COMPONENTS Crypto)
if(NOT OpenSSL_FOUND OR NOT OPENSSL_VERSION STREQUAL "3.5.8")
  message(FATAL_ERROR "Reviewed OpenSSL 3.5.8 is required; the selected headers/archive do not match that version.\n${_ovmesh_openssl_help}")
endif()
_ovmesh_openssl_require_inside("${OPENSSL_INCLUDE_DIR}" "selected headers")
_ovmesh_openssl_require_inside("${OPENSSL_CRYPTO_LIBRARY}" "selected crypto archive")

# Always compile and link the existing intake source, even when BUILD_TESTING
# is OFF. This checks the header configuration and linkability without running
# target code, opening devices or preventing cross compilation. CTest still
# runs the NIST vectors on native builds; configure is not a runtime test.
function(_ovmesh_openssl_check_configuration)
  file(READ "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../third_party/openssl/intake_smoke.c" _smoke)
  set(_source "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/ovmesh-openssl-check.c")
  file(WRITE "${_source}" "#include <openssl/opensslv.h>\n#if OPENSSL_VERSION_MAJOR != 3 || OPENSSL_VERSION_MINOR != 5 || OPENSSL_VERSION_PATCH != 8\n#error Reviewed OpenSSL header version must be 3.5.8\n#endif\n${_smoke}")
  set(CMAKE_TRY_COMPILE_TARGET_TYPE EXECUTABLE)
  unset(_ovmesh_openssl_compiles CACHE)
  try_compile(_ovmesh_openssl_compiles "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/ovmesh-openssl-check"
    SOURCES "${_source}" LINK_LIBRARIES OpenSSL::Crypto Threads::Threads
    OUTPUT_VARIABLE _compile_output)
  set(_checked "${_ovmesh_openssl_compiles}")
  unset(_ovmesh_openssl_compiles CACHE)
  if(NOT _checked)
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/ovmesh-openssl-check.log" "${_compile_output}")
    message(FATAL_ERROR "Reviewed OpenSSL configuration or link check failed. A stock system OpenSSL package does not supply the required no-autoload-config/no-dso/no-engine/no-sock/no-http build.\nCompiler details: ${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/ovmesh-openssl-check.log\n${_ovmesh_openssl_help}")
  endif()
endfunction()
_ovmesh_openssl_check_configuration()
message(STATUS "Reviewed static OpenSSL 3.5.8 configuration and link check passed")
