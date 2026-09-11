# Offline drift check. The manifest is reviewed in Git; it is not a signature.
if(NOT DEFINED OVMESH_SOURCE_DIR)
  get_filename_component(OVMESH_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
endif()
file(READ "${OVMESH_SOURCE_DIR}/third_party/integrity.json" manifest)
string(JSON count LENGTH "${manifest}" files)
math(EXPR last "${count} - 1")
set(expected)
foreach(index RANGE 0 ${last})
  string(JSON path GET "${manifest}" files ${index} path)
  string(JSON expected_hash GET "${manifest}" files ${index} sha256)
  if(path MATCHES "(^/|\\.\\.|\\\\)" OR NOT path MATCHES "^third_party/")
    message(FATAL_ERROR "Invalid source manifest path")
  endif()
  if(NOT EXISTS "${OVMESH_SOURCE_DIR}/${path}" OR IS_SYMLINK "${OVMESH_SOURCE_DIR}/${path}")
    message(FATAL_ERROR "Missing or symbolic vendored source: ${path}")
  endif()
  file(SHA256 "${OVMESH_SOURCE_DIR}/${path}" actual_hash)
  if(NOT actual_hash STREQUAL expected_hash)
    message(FATAL_ERROR "Vendored source changed: ${path}. Review the change and its provenance before updating the manifest.")
  endif()
  list(APPEND expected "${path}")
endforeach()
file(GLOB_RECURSE actual LIST_DIRECTORIES FALSE RELATIVE "${OVMESH_SOURCE_DIR}"
  "${OVMESH_SOURCE_DIR}/third_party/LIBUSB_COPYING.txt"
  "${OVMESH_SOURCE_DIR}/third_party/SQLITE_NOTICE.txt"
  "${OVMESH_SOURCE_DIR}/third_party/UI_ADDITIONAL_NOTICES.txt"
  "${OVMESH_SOURCE_DIR}/third_party/libhackrf-NOTICE.txt"
  "${OVMESH_SOURCE_DIR}/third_party/imgui/*"
  "${OVMESH_SOURCE_DIR}/third_party/glfw/*"
  "${OVMESH_SOURCE_DIR}/third_party/sqlite/*"
  "${OVMESH_SOURCE_DIR}/third_party/openssl/*"
  "${OVMESH_SOURCE_DIR}/third_party/nanopb/*"
  "${OVMESH_SOURCE_DIR}/third_party/meshtastic/*"
  "${OVMESH_SOURCE_DIR}/third_party/sdrangel/*"
  "${OVMESH_SOURCE_DIR}/third_party/rtlsdr/*"
  "${OVMESH_SOURCE_DIR}/third_party/sx1302_hal/*")
list(SORT expected)
list(SORT actual)
if(NOT actual STREQUAL expected)
  message(FATAL_ERROR "Vendored source inventory changed. Review added/removed files before updating the manifest.")
endif()
message(STATUS "Verified ${count} vendored files against the reviewed local manifest (offline)")
