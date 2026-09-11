// SPDX-License-Identifier: GPL-3.0-or-later
// A copied test executable impersonates the sibling worker. It never opens a
// device, discovers USB hardware, loads the HAL or reads operational settings.
#include "rak_process.hpp"
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#ifndef _WIN32
#include <cerrno>
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {
using namespace ovmesh;
unsigned checks=0;
void require(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
#ifndef _WIN32
void write_all(std::string_view text) {
    while(!text.empty()) {
        const auto n=::write(STDOUT_FILENO,text.data(),text.size());
        if(n<0 && errno==EINTR)continue;
        if(n<=0)throw std::runtime_error("Synthetic worker output failed");
        text.remove_prefix(static_cast<size_t>(n));
    }
}
std::string scan_line() {
    std::string line="SCAN 906875000 100.1 100.12 2000";
    for(unsigned i=1;i<33;++i)line+=" 0";
    return line+'\n';
}
const std::string packet_line="PACKET 906875000 250000 11 5 -82 7 1 123 0001aaff\n";
void await_stop() {
    std::string command;char byte=0;
    while(command.size()<5) {
        const auto n=::read(STDIN_FILENO,&byte,1);
        if(n<0 && errno==EINTR)continue;
        if(n!=1)throw std::runtime_error("Synthetic worker did not receive STOP");
        command+=byte;
    }
    if(command!="STOP\n")throw std::runtime_error("Unexpected synthetic worker command");
}
int fake_worker(std::string_view scenario) {
    if(scenario=="prefix-error") {
        write_all("READY\n"+scan_line()+packet_line+"ERROR synthetic-private-marker\n");
        return 1;
    }
    if(scenario=="prefix-malformed") {
        write_all("READY\n"+scan_line()+packet_line+"SCAN invalid\n");
        return 1;
    }
    if(scenario=="partial-eof") {
        write_all("READY\n"+scan_line()+packet_line+"PACK");
        return 0;
    }
    write_all("READY\n");await_stop();
    // Exceed one receive buffer and exit immediately. The parent must drain
    // queued records even when waitpid says the child has already exited.
    const unsigned count=scenario=="stop-tail"?80:1;
    std::string output;
    for(unsigned i=0;i<count;++i)output+=scan_line()+packet_line;
    if(scenario!="missing-end")output+="END\n";
    write_all(output);
    return 0;
}
struct Seen {
    unsigned ready=0,scans=0,packets=0,errors=0,ends=0;
    bool saw_error=false;
    void consume(RakMessage& m) {
        require(!saw_error,"Records appeared after a terminal error");
        switch(m.kind) {
        case RakMessage::Kind::Ready:++ready;break;
        case RakMessage::Kind::Scan:
            ++scans;require(concentrator_sample_count(m.scan)==2000,"Completed scan lost histogram counts");break;
        case RakMessage::Kind::Packet:
            ++packets;require(m.payload==std::vector<uint8_t>({0,1,0xaa,0xff}),"Completed packet lost its temporary bytes");break;
        case RakMessage::Kind::Error:
            ++errors;saw_error=true;
            require(m.error.find("synthetic-private-marker")==std::string::npos,"Terminal diagnostic leaked worker text");
            require(!m.error.empty(),"Terminal error lacks a diagnostic");break;
        case RakMessage::Kind::End:++ends;break;
        case RakMessage::Kind::Heartbeat:break;
        }
    }
};
template<class Predicate>void receive_until(RakProcess& process,Seen& seen,Predicate done) {
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while(!done()) {
        auto messages=process.poll();for(auto& m:messages)seen.consume(m);
        require(std::chrono::steady_clock::now()<deadline,"Synthetic worker response timed out");
        if(!done())std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}
void start(RakProcess& process,const std::string& scenario) {
    ConcentratorBoardConfig b;b.device_path=scenario;
    process.start(b,0,906875000,906875000,200000,2000,0);
}
void cases() {
    for(const auto scenario:{"prefix-error","prefix-malformed","partial-eof"}) {
        RakProcess process;Seen seen;start(process,scenario);
        receive_until(process,seen,[&]{return seen.errors!=0;});
        require(seen.ready==1 && seen.scans==1 && seen.packets==1 && seen.errors==1,
                "A terminal error discarded completed records from its valid prefix");
        require(!process.stop([&](auto& m){seen.consume(m);}),"Failed worker was reported as a clean stop");
        require(!process.alive(),"Failed worker remains alive after stop");
    }
    {
        RakProcess process;Seen seen;start(process,"stop-tail");
        receive_until(process,seen,[&]{return seen.ready!=0;});
        require(process.stop([&](auto& m){seen.consume(m);}),"STOP and END with clean exit should succeed");
        require(seen.scans==80 && seen.packets==80 && seen.ends==1 && seen.errors==0,
                "Final drain discarded or duplicated completed records");
        require(process.stop() && !process.alive(),"Repeated cleanup changed a completed stop");
    }
    {
        RakProcess process;Seen seen;start(process,"missing-end");
        receive_until(process,seen,[&]{return seen.ready!=0;});
        require(!process.stop([&](auto& m){seen.consume(m);}),"Exit zero without END is incomplete");
        require(seen.scans==1 && seen.packets==1 && seen.errors==1 && seen.ends==0,
                "Missing END discarded its valid completed prefix");
    }
    {
        RakProcess process;Seen seen;start(process,"consumer-error");
        receive_until(process,seen,[&]{return seen.ready!=0;});
        require(!process.stop([&](auto& m){seen.consume(m);if(m.kind==RakMessage::Kind::Scan)throw std::runtime_error("Synthetic persistence failure");}),
                "A failed record consumer was reported as clean");
        require(seen.scans==1 && seen.packets==1 && seen.ends==1,"Consumer failure blocked bounded final drain");
    }
}
int isolated_runner(const std::filesystem::path& executable) {
    const auto directory=std::filesystem::current_path()/("rak-process-synthetic-"+std::to_string(::getpid()));
    require(std::filesystem::create_directory(directory),"Synthetic fixture directory already exists");
    std::filesystem::permissions(directory,std::filesystem::perms::owner_all);
    const auto runner=directory/"test-runner";
    std::filesystem::copy_file(executable,runner);
    std::filesystem::copy_file(executable,directory/"ovmesh-rak-worker");
    const auto child=::fork();
    if(child==0) {
        if(::chdir(directory.c_str())!=0)_exit(120);
        ::execl(runner.c_str(),runner.c_str(),"--synthetic-cases",static_cast<char*>(nullptr));
        _exit(121);
    }
    require(child>0,"Could not start isolated synthetic test runner");
    int status=0;bool finished=false;
    for(unsigned i=0;i<1000;++i) {
        const auto result=::waitpid(child,&status,WNOHANG);
        if(result==child){finished=true;break;}
        if(result<0 && errno!=EINTR)break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if(!finished){::kill(child,SIGKILL);while(::waitpid(child,&status,0)<0 && errno==EINTR){}}
    require(finished && WIFEXITED(status) && WEXITSTATUS(status)==0,"Isolated worker lifecycle tests failed");
    std::filesystem::remove_all(directory);
    return 0;
}
#endif
}
int main(int argc,char** argv)try {
#ifndef _WIN32
    if(argc>2 && std::string_view(argv[1])=="--device")return fake_worker(argv[2]);
    if(!RakProcess::available()){std::cout<<"RAK worker unavailable; process integration skipped\n";return 0;}
    if(argc==2 && std::string_view(argv[1])=="--synthetic-cases") {
        cases();std::cout<<checks<<" synthetic RAK process checks passed; no hardware used\n";return 0;
    }
    return isolated_runner(std::filesystem::canonical(argv[0]));
#else
    (void)argc;(void)argv;std::cout<<"RAK process integration unavailable on Windows; skipped\n";return 0;
#endif
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
