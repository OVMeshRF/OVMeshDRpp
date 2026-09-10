// SPDX-License-Identifier: GPL-3.0-or-later
#include "session_disk.hpp"
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace ovmesh;
namespace fs=std::filesystem;
void require(bool b,const char* message){if(!b)throw std::runtime_error(message);}
void file(const fs::path& path,size_t size){std::ofstream out(path,std::ios::binary);out<<std::string(size,'x');}
int main(){
    const auto dir=fs::current_path()/("disk-fixture-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        fs::create_directory(dir);const auto db=dir/"session.sqlite";
        file(db,100);file(dir/"session.sqlite-wal",60);file(dir/"session.sqlite-shm",20);file(dir/"unrelated.sqlite",400);
        auto usage=read_session_disk_usage(db.string());
        require(usage.available&&usage.database_bytes==100&&usage.journal_bytes==80,"Only selected database and sidecars counted");
        file(dir/"session.sqlite-wal",90);usage=read_session_disk_usage(db.string());
        require(usage.journal_bytes==110,"Growing WAL included");
        fs::remove(dir/"session.sqlite-wal");usage=read_session_disk_usage(db.string());
        require(usage.available&&usage.journal_bytes==20,"Checkpoint-removal of WAL is normal");
        require(!read_session_disk_usage((dir/"absent.sqlite").string()).available,"Missing file is unavailable, not zero bytes");
        require(!read_session_disk_usage("").available&&!read_session_disk_usage("relative.sqlite").available,"Memory/relative paths not probed");
        SessionDiskMonitor monitor;monitor.poll(db.string(),0);monitor.pending.wait();monitor.poll(db.string(),1);
        require(monitor.value.available&&!monitor.pending.valid(),"Background result delivered without immediate rescan");
        monitor.poll(db.string(),5);require(monitor.pending.valid(),"Five-second refresh");monitor.pending.wait();
        monitor.poll("",6);require(!monitor.value.available&&monitor.display_path.empty(),"Late result never leaks into replacement session");
#ifndef _WIN32
        fs::create_symlink(db,dir/"alias.sqlite");
        require(!read_session_disk_usage((dir/"alias.sqlite").string()).available,"Symlink is not followed");
#endif
        fs::remove_all(dir);std::cout<<"Session disk metadata and refresh checks passed\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
