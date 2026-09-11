// SPDX-License-Identifier: GPL-3.0-or-later
#include "rak_process.hpp"
#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <sstream>
#include <locale>
#include <openssl/crypto.h>
#ifdef OVMESH_HAVE_RAK5146
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#endif

namespace ovmesh {
namespace {
[[noreturn]] void malformed() {throw std::runtime_error("Invalid or oversized RAK worker record; reception stopped");}
template<class T> T numeric(std::string_view s) {
    T value{};
    if constexpr(std::is_floating_point_v<T>) {
        if(s.size()>40)malformed();
        // Apple libc++ versions supported by this project lack floating-point
        // from_chars. The classic locale makes this independent of OS locale.
        std::istringstream input{std::string(s)};input.imbue(std::locale::classic());
        input>>std::noskipws>>value;
        if(!input || input.peek()!=std::char_traits<char>::eof() || !std::isfinite(value))malformed();
    } else {
        const auto r=std::from_chars(s.data(),s.data()+s.size(),value);
        if(r.ec!=std::errc{} || r.ptr!=s.data()+s.size())malformed();
    }
    return value;
}
void wipe(std::string& s) {if(!s.empty())OPENSSL_cleanse(s.data(),s.size());s.clear();}
}
RakMessage::~RakMessage() {if(!payload.empty())OPENSSL_cleanse(payload.data(),payload.size());}
RakMessage parse_rak_message(std::string_view line) {
    if(line.empty() || line.size()>2048)malformed();
    std::array<std::string_view,48> fields{};size_t n=0;
    while(!line.empty()) {
        if(n==fields.size())malformed();
        const auto end=line.find(' ');fields[n++]=line.substr(0,end);
        if(fields[n-1].empty())malformed();
        if(end==std::string_view::npos)break;
        line.remove_prefix(end+1);
        if(line.empty())malformed();
    }
    RakMessage m;
    if(fields[0]=="ERROR") {
        // Only compiled-in diagnostic identifiers are surfaced. Never echo
        // arbitrary child text, USB identity or possible packet bytes.
        for(const auto code:{"operation-timeout","receive-timeout","scan-timeout","initialization-timeout",
            "invalid-arguments","device-unavailable","initialization-failed","receive-failed",
            "usb-ack-header-timeout","usb-ack-body-timeout","usb-request-timeout",
            "scan-start-failed","scan-tune-failed","scan-command-failed","scan-status-failed","scan-read-failed","scan-count-mismatch",
            "scan-incomplete","record-overflow","scanner-close-failed","receiver-close-failed","device-close-failed"})
            if(n==2 && fields[1]==code)throw std::runtime_error(std::string("RAK receiver failure: ")+code);
        throw std::runtime_error("RAK worker reported a USB or receiver failure; stop and reconnect the selected concentrator");
    }
    if(fields[0]=="READY" && n==1)m.kind=RakMessage::Kind::Ready;
    else if(fields[0]=="HEARTBEAT" && n==1)m.kind=RakMessage::Kind::Heartbeat;
    else if(fields[0]=="END" && n==1)m.kind=RakMessage::Kind::End;
    else if(fields[0]=="SCAN" && n==37) {
        m.kind=RakMessage::Kind::Scan;m.scan.frequency_hz=numeric<uint64_t>(fields[1]);
        m.scan_monotonic_start=numeric<double>(fields[2]);m.scan_monotonic_end=numeric<double>(fields[3]);
        if(m.scan.frequency_hz<902000000 || m.scan.frequency_hz>928000000 ||
           m.scan_monotonic_start<0 || m.scan_monotonic_end<=m.scan_monotonic_start ||
           m.scan_monotonic_end-m.scan_monotonic_start>5)malformed();
        for(size_t i=0;i<33;++i)m.scan.counts[i]=numeric<uint32_t>(fields[i+4]);
        if(concentrator_sample_count(m.scan)!=2000)malformed();
    } else if(fields[0]=="PACKET" && n==10) {
        m.kind=RakMessage::Kind::Packet;m.frequency_hz=numeric<uint64_t>(fields[1]);
        m.bandwidth_hz=numeric<unsigned>(fields[2]);m.spreading_factor=numeric<unsigned>(fields[3]);
        m.coding_rate=numeric<unsigned>(fields[4]);m.rssi_dbm=numeric<double>(fields[5]);m.snr_db=numeric<double>(fields[6]);
        const auto crc=numeric<unsigned>(fields[7]);m.crc_valid=crc==1;m.hardware_timestamp_us=numeric<uint32_t>(fields[8]);
        if(m.frequency_hz<902000000 || m.frequency_hz>928000000 ||
           (m.bandwidth_hz!=125000 && m.bandwidth_hz!=250000 && m.bandwidth_hz!=500000) ||
           m.spreading_factor<7 || m.spreading_factor>12 || m.coding_rate<5 || m.coding_rate>8 ||
           crc>1 || m.rssi_dbm< -250 || m.rssi_dbm>100 || m.snr_db< -100 || m.snr_db>100)malformed();
        const auto hex=fields[9];
        if(hex!="-") {
            if(!m.crc_valid || hex.size()>510 || hex.size()%2)malformed();
            auto digit=[](char c)->unsigned {if(c>='0'&&c<='9')return unsigned(c-'0');if(c>='a'&&c<='f')return unsigned(c-'a'+10);malformed();};
            m.payload.resize(hex.size()/2);
            for(size_t i=0;i<m.payload.size();++i)m.payload[i]=static_cast<uint8_t>(digit(hex[2*i])*16+digit(hex[2*i+1]));
        }
    } else malformed();
    return m;
}

struct RakProcess::Impl {
    std::string pending;
    Impl(){pending.reserve(2048);} // Avoid freed, uncleansed reallocation fragments.
#ifdef OVMESH_HAVE_RAK5146
    int fd=-1;pid_t pid=-1;
#endif
    bool ended=false, clean_stop=true, saw_end=false, failed=false, eof=false;
};
RakProcess::RakProcess():impl_(std::make_unique<Impl>()){}
RakProcess::~RakProcess(){stop();}
bool RakProcess::available() noexcept {
#ifdef OVMESH_HAVE_RAK5146
    return true;
#else
    return false;
#endif
}
double rak_monotonic_now() {
#ifdef OVMESH_HAVE_RAK5146
    timespec t{};if(clock_gettime(CLOCK_MONOTONIC,&t))throw std::runtime_error("Cannot read receiver timing clock");
    return double(t.tv_sec)+double(t.tv_nsec)/1e9;
#else
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
#endif
}
void RakProcess::start(const ConcentratorBoardConfig& board,int64_t offset,
                       uint64_t lower,uint64_t upper,uint32_t step,unsigned samples,unsigned index) {
    stop();impl_->ended=false;impl_->clean_stop=true;
    impl_->saw_end=false;impl_->failed=false;impl_->eof=false;
#ifdef OVMESH_HAVE_RAK5146
    std::array<char,8192> path{};
#ifdef __APPLE__
    uint32_t size=static_cast<uint32_t>(path.size());
    if(_NSGetExecutablePath(path.data(),&size))throw std::runtime_error("Cannot locate RAK receiver worker");
#else
    const auto length=readlink("/proc/self/exe",path.data(),path.size()-1);
    if(length<=0 || size_t(length)>=path.size()-1)throw std::runtime_error("Cannot locate RAK receiver worker");
    path[static_cast<size_t>(length)]='\0';
#endif
    const auto executable=std::filesystem::canonical(path.data()).parent_path()/"ovmesh-rak-worker";
    if(!std::filesystem::is_regular_file(executable) || access(executable.c_str(),X_OK))
        throw std::runtime_error("RAK receiver worker is missing beside the application; rebuild or reinstall this application");
    std::vector<std::string> args{executable.string(),"--device",board.device_path,
        "--frequency",std::to_string(board.packets_enabled?int64_t(board.frequency_hz)+offset:0),
        "--bandwidth",std::to_string(board.bandwidth_hz),"--sf",std::to_string(board.spreading_factor),
        "--sync",std::to_string(board.sync_word),
        "--scan-lower",std::to_string(lower?int64_t(lower)+offset:0),
        "--scan-upper",std::to_string(upper?int64_t(upper)+offset:0),
        "--scan-step",std::to_string(step),"--scan-samples",std::to_string(samples),"--board-index",std::to_string(index)};
    std::vector<char*> argv;for(auto& s:args)argv.push_back(s.data());argv.push_back(nullptr);
    // Do not propagate loader/search-path overrides or arbitrary user environment
    // into a hardware helper. No shell, PATH search, network or log file is used.
    char locale_env[]="LC_ALL=C";char* env[]={locale_env,nullptr};
    int sockets[2];if(socketpair(AF_UNIX,SOCK_STREAM,0,sockets))throw std::runtime_error("Cannot create local RAK worker channel");
    for(auto& fd:sockets) {
        if(fd<3) {
            const int replacement=fcntl(fd,F_DUPFD_CLOEXEC,3);
            if(replacement<0){close(sockets[0]);close(sockets[1]);throw std::runtime_error("Cannot isolate RAK worker descriptors");}
            close(fd);fd=replacement;
        }
        if(fcntl(fd,F_SETFD,FD_CLOEXEC)){close(sockets[0]);close(sockets[1]);throw std::runtime_error("Cannot isolate RAK worker descriptors");}
    }
#ifdef __APPLE__
    int no_sigpipe=1;setsockopt(sockets[0],SOL_SOCKET,SO_NOSIGPIPE,&no_sigpipe,sizeof(no_sigpipe));
#endif
    posix_spawn_file_actions_t actions;posix_spawnattr_t attr;
    int result=posix_spawn_file_actions_init(&actions);
    if(result){close(sockets[0]);close(sockets[1]);throw std::runtime_error("Cannot configure RAK worker");}
    bool attr_ready=posix_spawnattr_init(&attr)==0;
    result=posix_spawn_file_actions_adddup2(&actions,sockets[1],STDIN_FILENO);
    result|=posix_spawn_file_actions_adddup2(&actions,sockets[1],STDOUT_FILENO);
    result|=posix_spawn_file_actions_addopen(&actions,STDERR_FILENO,"/dev/null",O_WRONLY,0);
    result|=posix_spawn_file_actions_addclose(&actions,sockets[0]);
    result|=posix_spawn_file_actions_addclose(&actions,sockets[1]);
#if defined(__APPLE__)
    if(attr_ready)result|=posix_spawnattr_setflags(&attr,POSIX_SPAWN_CLOEXEC_DEFAULT);
#else
    result|=posix_spawn_file_actions_addclosefrom_np(&actions,3);
#endif
    pid_t child=-1;
    if(!result && attr_ready)result=posix_spawn(&child,executable.c_str(),&actions,&attr,argv.data(),env);
    else result=1;
    posix_spawn_file_actions_destroy(&actions);if(attr_ready)posix_spawnattr_destroy(&attr);close(sockets[1]);
    if(result){close(sockets[0]);throw std::runtime_error("Could not start the RAK receiver worker");}
    impl_->pid=child;impl_->fd=sockets[0];
    const int flags=fcntl(impl_->fd,F_GETFL);
    if(flags<0 || fcntl(impl_->fd,F_SETFL,flags|O_NONBLOCK)<0){stop();throw std::runtime_error("Cannot bound RAK worker channel reads");}
#else
    (void)board;(void)offset;(void)lower;(void)upper;(void)step;(void)samples;(void)index;
    throw std::runtime_error("RAK5146 USB support is not available in this build");
#endif
}
std::vector<RakMessage> RakProcess::poll() {
    std::vector<RakMessage> messages;
#ifdef OVMESH_HAVE_RAK5146
    if(impl_->fd<0 || impl_->eof)return messages;
    std::array<char,4096> bytes{};
    struct CleanRead {
        std::array<char,4096>& bytes;
        ~CleanRead(){OPENSSL_cleanse(bytes.data(),bytes.size());}
    } clean{bytes};
    const auto terminal=[&](const char* reason) {
        RakMessage error;error.kind=RakMessage::Kind::Error;error.error=reason;
        impl_->failed=true;impl_->ended=true;wipe(impl_->pending);
        messages.push_back(std::move(error));
    };
    const auto count=recv(impl_->fd,bytes.data(),bytes.size(),0);
    if(count==0) {
        impl_->eof=true;impl_->ended=true;
        if(!impl_->failed && (!impl_->saw_end || !impl_->pending.empty()))
            terminal("RAK worker closed without a complete END record; receiver may have disconnected");
        return messages;
    }
    if(count<0) {
        if(errno==EAGAIN || errno==EWOULDBLOCK || errno==EINTR)return messages;
        if(!impl_->failed)terminal("RAK worker communication failed");
        return messages;
    }
    // Once a record failed validation, only drain/wipe the remaining transport
    // bytes. Later records cannot repair the failed protocol stream.
    if(impl_->failed)return messages;
    try {
        for(size_t i=0;i<static_cast<size_t>(count);++i) {
            if(impl_->saw_end)malformed();
            if(bytes[i]=='\n') {
                auto message=parse_rak_message(impl_->pending);wipe(impl_->pending);
                if(message.kind==RakMessage::Kind::End){impl_->saw_end=true;impl_->ended=true;}
                messages.push_back(std::move(message));
            } else {
                if(impl_->pending.size()>=2048 || bytes[i]<32 || bytes[i]>126)malformed();
                impl_->pending.push_back(bytes[i]);
            }
        }
    }catch(const std::runtime_error& e) {
        // The parser only emits fixed/whitelisted messages. Preserve the valid
        // prefix from this read and deliver the failure after that prefix.
        terminal(e.what());
    }catch(...) {wipe(impl_->pending);throw;}
#endif
    return messages;
}
bool RakProcess::alive()const {
#ifdef OVMESH_HAVE_RAK5146
    return impl_->pid>0 && !impl_->ended;
#else
    return false;
#endif
}
bool RakProcess::stop(const std::function<void(RakMessage&)>& consume) noexcept {
#ifdef OVMESH_HAVE_RAK5146
    bool delivered=true;
    if(impl_->fd>=0) {
        if(!impl_->saw_end) {
#ifdef MSG_NOSIGNAL
            const auto sent=send(impl_->fd,"STOP\n",5,MSG_NOSIGNAL);
#else
            const auto sent=send(impl_->fd,"STOP\n",5,0);
#endif
            if(sent!=5)delivered=false;
        }
        shutdown(impl_->fd,SHUT_WR);
    }
    if(impl_->pid>0) {
        int status=0;bool reaped=false,status_known=false;
        // Continue the normal parser and consumer after STOP, including bytes
        // queued before the child exits. Exit status alone is not a data-drain
        // barrier. Only this exact owned child may be signalled.
        for(unsigned i=0;i<150;++i) {
            for(unsigned reads=0;reads<16 && !impl_->eof;++reads) {
                try {
                    auto messages=poll();
                    if(messages.empty())break;
                    for(auto& message:messages) {
                        if(message.kind==RakMessage::Kind::Error)delivered=false;
                        if(consume) {
                            try {consume(message);}catch(...){delivered=false;}
                        } else if(message.kind==RakMessage::Kind::Scan || message.kind==RakMessage::Kind::Packet)delivered=false;
                    }
                }catch(...) {delivered=false;impl_->failed=true;wipe(impl_->pending);break;}
            }
            if(!reaped) {
                const auto result=waitpid(impl_->pid,&status,WNOHANG);
                if(result==impl_->pid){reaped=true;status_known=true;}
                else if(result<0 && errno==ECHILD)reaped=true;
            }
            if(reaped && impl_->eof)break;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if(!reaped){kill(impl_->pid,SIGKILL);while(waitpid(impl_->pid,&status,0)<0 && errno==EINTR){}}
        impl_->clean_stop=delivered && !impl_->failed && impl_->saw_end && impl_->eof &&
            reaped && status_known && WIFEXITED(status) && WEXITSTATUS(status)==0;
        impl_->pid=-1;
    }
    if(impl_->fd>=0){close(impl_->fd);impl_->fd=-1;}
#else
    (void)consume;
#endif
    wipe(impl_->pending);impl_->ended=true;
    return impl_->clean_stop;
}
}
