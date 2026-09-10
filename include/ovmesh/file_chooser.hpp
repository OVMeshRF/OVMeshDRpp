// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace ovmesh {

enum class FileChoiceKind { Survey, Csv, GeoJson, Png, Html };
struct FileBrowserEntry {
    std::string name;
    bool directory;
};
struct FileBrowserListing {
    std::vector<FileBrowserEntry> entries;
    bool truncated = false;
    std::string error;
};

// Paths are UTF-8. These helpers never create files or resolve path aliases.
// Local-path checks are shared with the final storage writer, which remains
// responsible for exclusive creation and rechecking the chosen path.
std::string file_browser_home();
std::vector<std::string> file_browser_roots();
std::string file_browser_parent(const std::string& directory);
std::string file_browser_join(const std::string& directory, const std::string& name);
FileBrowserListing list_local_directory(const std::string& directory, size_t limit = 2000);
std::string choose_local_file(const std::string& directory, const std::string& filename,
                             FileChoiceKind kind, bool must_exist);

} // namespace ovmesh
