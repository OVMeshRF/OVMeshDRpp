# SPDX-License-Identifier: GPL-3.0-or-later
# Embed local reviewed notices; no generator package, fetch or runtime file IO.
set(OVMESH_LICENSE_FILES
  "NOTICE|About OVMeshDRpp"
  "LICENSE|GNU GPL version 3"
  "THIRD_PARTY_NOTICES.md|Third-party inventory and source references"
  "third_party/sdrangel/PROVENANCE.md|SDRangel attribution and modifications"
  "third_party/sdrangel/LICENSE|SDRangel GPL license"
  "third_party/imgui/LICENSE.txt|Dear ImGui MIT license"
  "third_party/UI_ADDITIONAL_NOTICES.txt|Embedded fonts, stb and OpenGL loader"
  "third_party/glfw/LICENSE.md|GLFW license"
  "third_party/SQLITE_NOTICE.txt|SQLite public-domain dedication"
  "third_party/openssl/LICENSE.txt|OpenSSL Apache license 2.0"
  "third_party/nanopb/LICENSE.txt|Nanopb license"
  "third_party/meshtastic/LICENSE|Meshtastic schema GPL license"
  "third_party/meshtastic/PROVENANCE.md|Meshtastic schema provenance"
  "third_party/meshtastic/PROTOBUF-TOOLING-LICENSE|Protobuf generation tools (not runtime)"
  "third_party/libhackrf-NOTICE.txt|libhackrf public header notice"
  "third_party/LIBUSB_COPYING.txt|libusb LGPL version 2.1"
)
set(OVMESH_NOTICE_CPP "// Generated from checked-in license texts. Do not edit.\nnamespace ovmesh {\nstatic constexpr LicenseNotice bundled_notices[] = {\n")
set(OVMESH_NOTICE_TEXT "")
foreach(entry IN LISTS OVMESH_LICENSE_FILES)
  string(REPLACE "|" ";" fields "${entry}")
  list(GET fields 0 path)
  list(GET fields 1 title)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${path}")
  file(READ "${CMAKE_CURRENT_SOURCE_DIR}/${path}" content)
  string(FIND "${content}" ")OVMESH_LIC\"" delimiter)
  if(NOT delimiter EQUAL -1)
    message(FATAL_ERROR "License text collides with C++ delimiter: ${path}")
  endif()
  string(APPEND OVMESH_NOTICE_CPP "{\"${title}\", \"${path}\", R\"OVMESH_LIC(${content})OVMESH_LIC\"},\n")
  string(APPEND OVMESH_NOTICE_TEXT "\n===== ${title} (${path}) =====\n\n${content}\n")
  get_filename_component(parent "${path}" DIRECTORY)
  install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/${path}" DESTINATION "share/OVMeshDRpp/${parent}")
endforeach()
string(APPEND OVMESH_NOTICE_CPP "};\n}\n")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/license_notices.inc" CONTENT "${OVMESH_NOTICE_CPP}" @ONLY)
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/LICENSES.txt" CONTENT "${OVMESH_NOTICE_TEXT}" @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/generated/LICENSES.txt" DESTINATION share/OVMeshDRpp)
