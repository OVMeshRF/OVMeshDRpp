// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string>

namespace ovmesh {
// Throws unless the path passes the same local-file policy as session storage.
// Does not open/create the file or resolve aliases. Call again when writing.
void validate_local_file_path(const std::string& path);
}
