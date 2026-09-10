// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "ovmesh/local_paths.hpp"
#include <filesystem>
#include <future>
#include <limits>
#include <string>

namespace ovmesh {
struct SessionDiskUsage {
    bool available = false;
    uintmax_t database_bytes = 0, journal_bytes = 0;
};
// Metadata only for the selected local database and its SQLite sidecars. No
// directory traversal, database reads, checkpoints or changes to the session.
inline SessionDiskUsage read_session_disk_usage(const std::string& path) {
    SessionDiskUsage result;
    if(path.empty())return result;
    try {
        for(const std::string suffix : {"", "-wal", "-shm", "-journal"}) {
            const auto name=path+suffix;
            validate_local_file_path(name);
            const auto file=std::filesystem::path(std::u8string(name.begin(),name.end()));
            std::error_code error;const auto status=std::filesystem::symlink_status(file,error);
            if(error==std::errc::no_such_file_or_directory||(!error&&!std::filesystem::exists(status))) {
                if(suffix.empty())return {}; else continue;
            }
            if(error||!std::filesystem::is_regular_file(status))return {};
            const auto bytes=std::filesystem::file_size(file,error);
            // A sidecar may vanish as SQLite checkpoints between metadata calls.
            if(error==std::errc::no_such_file_or_directory&&!suffix.empty())continue;
            if(error)return {};
            if(suffix.empty())result.database_bytes=bytes;
            else {
                if(bytes>std::numeric_limits<uintmax_t>::max()-result.journal_bytes)return {};
                result.journal_bytes+=bytes;
            }
        }
        if(result.journal_bytes>std::numeric_limits<uintmax_t>::max()-result.database_bytes)return {};
        result.available=true;
    }catch(const std::exception&){return {};}
    return result;
}
struct SessionDiskMonitor {
    std::string display_path,pending_path;
    SessionDiskUsage value;
    std::future<SessionDiskUsage> pending;
    double last_started=-1;
    void poll(const std::string& path,double now) {
        if(path!=display_path){display_path=path;value={};last_started=-1;}
        if(pending.valid()&&pending.wait_for(std::chrono::seconds(0))==std::future_status::ready) {
            auto completed=pending.get();
            if(pending_path==display_path)value=completed;
        }
        if(!path.empty()&&!pending.valid()&&(last_started<0||now-last_started>=5)) {
            pending_path=path;last_started=now;
            pending=std::async(std::launch::async,[path]{return read_session_disk_usage(path);});
        }
    }
};
} // namespace ovmesh
