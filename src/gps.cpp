// SPDX-License-Identifier: GPL-3.0-or-later
// NMEA field layout reference (reviewed 2026-09-09):
// https://gpsd.gitlab.io/gpsd/NMEA.html#_rmc_recommended_minimum_navigation_information
// No GPSD dependency or external service is used.
#include "gps.hpp"
#include <mutex>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <thread>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace ovmesh {
double monotonic_now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
double utc_now() {
    return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
}
std::optional<PositionFix> position_at(std::span<const PositionFix> history,double target) {
    if(!std::isfinite(target)||target<0)return {};
    for(auto it=history.rbegin();it!=history.rend();++it) {
        if(it->monotonic_seconds>target)continue;
        if(!it->valid)return {};
        if(it->manual||target-it->monotonic_seconds<=2.0)return *it;
        return {};
    }
    return {};
}
namespace {
constexpr std::size_t maximum_sentence=128;
bool digits(std::string_view value) {
    return !value.empty() && std::all_of(value.begin(),value.end(),[](char c){return c>='0'&&c<='9';});
}
std::optional<unsigned> integer(std::string_view value,unsigned maximum) {
    if(!digits(value)||value.size()>9) return {};
    unsigned result=0;
    for(char c:value) {
        result=result*10+static_cast<unsigned>(c-'0');
        if(result>maximum) return {};
    }
    return result;
}
std::optional<double> decimal(std::string_view value,bool signed_value=false) {
    if(value.empty()||value.size()>24) return {};
    double sign=1;
    if(value.front()=='-'&&signed_value) { sign=-1; value.remove_prefix(1); }
    const auto point=value.find('.');
    const auto whole=value.substr(0,point);
    if(!digits(whole)||whole.size()>9) return {};
    double result=0;
    for(char c:whole) result=result*10+(c-'0');
    if(point!=std::string_view::npos) {
        const auto fraction=value.substr(point+1);
        if(!digits(fraction)||fraction.size()>12) return {};
        double scale=0.1;
        for(char c:fraction) { result+=(c-'0')*scale; scale*=0.1; }
    }
    return result*sign;
}
std::optional<double> clock_seconds(std::string_view value) {
    if(value.size()<6 || !digits(value.substr(0,6)) || (value.size()>6&&value[6]!='.')) return {};
    auto hour=integer(value.substr(0,2),23),minute=integer(value.substr(2,2),59);
    auto second=decimal(value.substr(4));
    // A leap-second label is not representable as an unambiguous POSIX timestamp.
    if(!hour||!minute||!second||*second>=60) return {};
    return *hour*3600+*minute*60+*second;
}
std::optional<double> date_seconds(std::string_view date) {
    if(date.size()!=6 || !digits(date)) return {};
    auto day=integer(date.substr(0,2),31),month=integer(date.substr(2,2),12),year=integer(date.substr(4,2),99);
    if(!day||!month||!year||!*day||!*month) return {};
    // Conventional NMEA two-digit-year pivot, explicit rather than locale dependent.
    const int full_year=*year>=80?1900+static_cast<int>(*year):2000+static_cast<int>(*year);
    const std::chrono::year_month_day ymd{std::chrono::year{full_year},std::chrono::month{*month},std::chrono::day{*day}};
    if(!ymd.ok()) return {};
    return std::chrono::duration<double>(std::chrono::sys_days{ymd}.time_since_epoch()).count();
}
std::optional<double> coordinate(std::string_view value,std::string_view direction,bool latitude) {
    const unsigned degree_digits=latitude?2:3;
    if(value.size()<degree_digits+2 || (value.size()>degree_digits+2&&value[degree_digits+2]!='.')) return {};
    const auto degree=integer(value.substr(0,degree_digits),latitude?90:180);
    const auto minute=decimal(value.substr(degree_digits));
    if(!degree||!minute||*minute>=60||(*degree==(latitude?90U:180U)&&*minute!=0)) return {};
    double result=*degree+*minute/60;
    if(direction==(latitude?"S":"W")) result=-result;
    else if(direction!=(latitude?"N":"E")) return {};
    return result;
}
int hex(char c) {
    if(c>='0'&&c<='9') return c-'0';
    if(c>='A'&&c<='F') return c-'A'+10;
    if(c>='a'&&c<='f') return c-'a'+10;
    return -1;
}
void invalidate(PositionFix& fix,std::string_view reason) {
    fix.valid=false; fix.source+="; "; fix.source+=reason;
}
}

std::optional<PositionFix> parse_nmea(std::string_view sentence,double received_monotonic,double received_utc) {
    if(!std::isfinite(received_monotonic)||received_monotonic<0 || !std::isfinite(received_utc) ||
        received_utc<0 || received_utc>4102444800.0 || sentence.size()>maximum_sentence) return {};
    if(sentence.ends_with("\r\n")) sentence.remove_suffix(2);
    else if(sentence.ends_with('\n')) sentence.remove_suffix(1);
    if(sentence.size()<10||sentence.front()!='$') return {};
    const auto star=sentence.find('*');
    if(star==std::string_view::npos || star+3!=sentence.size()) return {};
    const int h=hex(sentence[star+1]),l=hex(sentence[star+2]);
    if(h<0||l<0) return {};
    unsigned checksum=0;
    for(char c:sentence.substr(1,star-1)) {
        if(c<0x20||c>0x7e||c=='$') return {};
        checksum^=static_cast<unsigned char>(c);
    }
    if(checksum!=static_cast<unsigned>((h<<4)|l)) return {};
    std::array<std::string_view,20> fields{};
    unsigned count=0; auto body=sentence.substr(1,star-1);
    while(true) {
        if(count==fields.size()) return {};
        const auto comma=body.find(','); fields[count++]=body.substr(0,comma);
        if(comma==std::string_view::npos) break;
        body.remove_prefix(comma+1);
    }
    if(fields[0].size()!=5 || fields[0][0]<'A'||fields[0][0]>'Z' || fields[0][1]<'A'||fields[0][1]>'Z') return {};
    const auto kind=fields[0].substr(2);
    if(kind!="RMC"&&kind!="GGA") return {};
    if((kind=="RMC"&&(count<12||count>14)) || (kind=="GGA"&&count!=15)) return {};
    PositionFix fix;
    fix.monotonic_seconds=received_monotonic; fix.utc_seconds=received_utc;
    fix.source=std::string("NMEA ")+std::string(kind)+(kind=="RMC"?"; GNSS UTC":"; date inferred from host UTC");
    const auto time=clock_seconds(fields[1]);
    if(!time) return {};
    if(kind=="RMC") {
        const auto date=date_seconds(fields[9]);
        if(!date) return {};
        fix.utc_seconds=*date+*time;
        if(fields[2]!="A"&&fields[2]!="V") return {};
        if(fields[2]=="V") { invalidate(fix,"receiver reports invalid fix"); return fix; }
        if(count>12&&!fields[12].empty()) {
            // Refuse simulated, manually supplied, dead-reckoned, unsafe, unknown modes.
            if(fields[12]!="A"&&fields[12]!="D"&&fields[12]!="F"&&fields[12]!="P"&&fields[12]!="R") {
                invalidate(fix,"unsupported or invalid navigation mode"); return fix;
            }
        }
        if(count>13&&!fields[13].empty()&&fields[13]!="A"&&fields[13]!="D"&&fields[13]!="V") {
            invalidate(fix,"invalid navigation status"); return fix;
        }
        const auto lat=coordinate(fields[3],fields[4],true),lon=coordinate(fields[5],fields[6],false);
        if(!lat||!lon) return {};
        fix.latitude=*lat; fix.longitude=*lon;
    } else {
        const double day=std::floor(received_utc/86400)*86400;
        fix.utc_seconds=day+*time;
        // Around midnight use the nearest date; GGA itself does not contain a date.
        if(fix.utc_seconds-received_utc>43200) fix.utc_seconds-=86400;
        if(received_utc-fix.utc_seconds>43200) fix.utc_seconds+=86400;
        const auto quality=integer(fields[6],8),satellites=integer(fields[7],99);
        if(!quality) return {};
        if(*quality==0) { invalidate(fix,"receiver reports no fix"); return fix; }
        if(*quality>5) { invalidate(fix,"estimated/manual/simulated fix excluded"); return fix; }
        const auto lat=coordinate(fields[2],fields[3],true),lon=coordinate(fields[4],fields[5],false);
        const auto hdop=decimal(fields[8]);
        if(!lat||!lon||!satellites||*satellites==0||!hdop||*hdop>1000) return {};
        fix.latitude=*lat; fix.longitude=*lon; fix.satellites=*satellites; fix.hdop=*hdop;
        if(!fields[9].empty()) {
            const auto altitude=decimal(fields[9],true);
            if(!altitude||*altitude< -12000||*altitude>100000||fields[10]!="M") return {};
            fix.altitude_m=*altitude;
        }
    }
    fix.valid=true;
    // A host/GNSS clock disagreement cannot be silently treated as a current fix.
    // Host UTC is not disciplined by this parser. The reason is retained explicitly.
    if(received_utc-fix.utc_seconds>10 || fix.utc_seconds-received_utc>5)
        invalidate(fix,"stale/future sentence or host clock disagreement");
    return fix;
}

struct SerialGps::Impl {
    std::atomic<bool> running{false};
    std::thread worker;
    std::function<void(PositionFix)> callback;
    std::string pending;
    double last_utc=-1,last_monotonic=0;
    std::optional<PositionFix> last_fix;
    mutable std::mutex status_mutex;
    GpsConnectionStatus connection;
    double fresh_fix_monotonic = 0;
#ifdef _WIN32
    HANDLE device=INVALID_HANDLE_VALUE;
    DCB original{};
    COMMTIMEOUTS original_timeouts{};
#else
    int device=-1;
    termios original{};
#endif
    void deliver(PositionFix fix) {
        if(fix.valid) {
            if(fix.utc_seconds<last_utc-0.0001) invalidate(fix,"out-of-order fix");
            else if(std::abs(fix.utc_seconds-last_utc)<0.0001) {
                // RMC/GGA from one epoch may complement each other but replaying the
                // same epoch must not make its monotonic freshness advance.
                fix.monotonic_seconds=last_monotonic;
                if(last_fix) {
                    if(!fix.altitude_m) fix.altitude_m=last_fix->altitude_m;
                    if(!fix.hdop) fix.hdop=last_fix->hdop;
                    if(fix.satellites==0) fix.satellites=last_fix->satellites;
                }
                fix.source+="; same epoch freshness preserved";
            } else {
                last_utc=fix.utc_seconds; last_monotonic=fix.monotonic_seconds;
            }
            if(fix.valid) last_fix=fix;
        }
        {
            std::lock_guard lock(status_mutex);
            // A serial read failure is stronger evidence than its invalid-fix
            // marker. Never replace it with a generic no-satellite-fix state.
            if(connection.state!=GpsConnectionState::ReadError) {
                if(fix.valid) {
                    connection.state=GpsConnectionState::ValidFix;
                    connection.detail="GPS fix valid";
                    fresh_fix_monotonic=fix.monotonic_seconds;
                } else {
                    connection.state=last_fix?GpsConnectionState::StaleFix:GpsConnectionState::WaitingForFix;
                    connection.detail=last_fix?"GPS fix lost; waiting for a valid fix":"GPS connected; waiting for a valid fix";
                }
            }
        }
        // Never call the engine while holding the serial status mutex.
        try { callback(std::move(fix)); }
        catch(...) {
            running=false;
            std::lock_guard lock(status_mutex);
            connection.state=GpsConnectionState::ReadError;
            connection.detail="GPS receiver callback failed";
        }
    }
    void bytes(std::span<const char> input) {
        for(char c:input) {
            if(c=='$') { pending.clear(); pending.push_back(c); }
            else if(!pending.empty()) {
                if(c=='\n') {
                    pending.push_back(c);
                    if(auto fix=parse_nmea(pending,monotonic_now(),utc_now())) deliver(std::move(*fix));
                    pending.clear();
                } else if(pending.size()>=maximum_sentence-1 || (c!='\r'&&(c<0x20||c>0x7e))) pending.clear();
                else pending.push_back(c);
            }
        }
    }
    void disconnected() {
        if(running.exchange(false)) {
            {
                std::lock_guard lock(status_mutex);
                connection.state=GpsConnectionState::ReadError;
                connection.detail="GPS serial device disconnected or read failed";
            }
            PositionFix fix; fix.monotonic_seconds=monotonic_now(); fix.utc_seconds=utc_now();
            fix.source="Serial GPS disconnected or read failed"; deliver(std::move(fix));
        }
    }
    void read_loop() {
        std::array<char,512> data{};
        while(running) {
#ifdef _WIN32
            DWORD count=0;
            if(!ReadFile(device,data.data(),static_cast<DWORD>(data.size()),&count,nullptr)) {
                disconnected(); break;
            }
            if(count) bytes(std::span(data).first(count));
#else
            pollfd descriptor{device,POLLIN,0};
            const int result=poll(&descriptor,1,100);
            if(result<0) { if(errno==EINTR) continue; disconnected(); break; }
            if(result==0) continue;
            if(descriptor.revents&(POLLERR|POLLHUP|POLLNVAL)) { disconnected(); break; }
            if(descriptor.revents&POLLIN) {
                const auto count=read(device,data.data(),data.size());
                if(count>0) bytes(std::span(data).first(static_cast<std::size_t>(count)));
                else if(count<0&&errno!=EINTR&&errno!=EAGAIN&&errno!=EWOULDBLOCK) { disconnected(); break; }
            }
#endif
        }
        std::fill(data.begin(),data.end(),0);
    }
};

SerialGps::SerialGps():impl_(std::make_unique<Impl>()) {}
SerialGps::~SerialGps() { stop(); }
bool SerialGps::start(const std::string& path,unsigned baud,std::function<void(PositionFix)> callback,std::string& error) {
    error.clear();
    if(impl_->worker.joinable()) { error="GPS is already connected; disconnect before changing ports"; return false; }
    const auto failed=[&] {
        std::lock_guard lock(impl_->status_mutex);
        impl_->connection={GpsConnectionState::ReadError,path,error};
        return false;
    };
    if(!callback||path.empty()||path.size()>240||path.find('\0')!=std::string::npos) { error="An explicit local serial port is required"; return failed(); }
    if(baud!=4800&&baud!=9600&&baud!=19200&&baud!=38400&&baud!=57600&&baud!=115200) {
        error="Supported GPS baud rates: 4800, 9600, 19200, 38400, 57600, 115200"; return failed();
    }
#ifdef _WIN32
    std::string port=path;
    if(port.starts_with("\\\\.\\")) port.erase(0,4);
    if(!port.starts_with("COM")||!digits(std::string_view(port).substr(3))||port.size()>8) { error="Specify a local COM port such as COM3"; return failed(); }
    port="\\\\.\\"+port;
    impl_->device=CreateFileA(port.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,0,nullptr);
    if(impl_->device==INVALID_HANDLE_VALUE) { error="Unable to open the requested GPS serial port"; return failed(); }
    impl_->original.DCBlength=sizeof(DCB);
    if(!GetCommState(impl_->device,&impl_->original)||!GetCommTimeouts(impl_->device,&impl_->original_timeouts)) {
        CloseHandle(impl_->device); impl_->device=INVALID_HANDLE_VALUE; error="Unable to read serial settings"; return failed();
    }
    DCB settings=impl_->original;
    settings.BaudRate=baud; settings.ByteSize=8; settings.Parity=NOPARITY; settings.StopBits=ONESTOPBIT;
    settings.fBinary=TRUE; settings.fParity=FALSE; settings.fOutxCtsFlow=FALSE; settings.fOutxDsrFlow=FALSE;
    settings.fDtrControl=DTR_CONTROL_DISABLE; settings.fRtsControl=RTS_CONTROL_DISABLE;
    settings.fOutX=FALSE; settings.fInX=FALSE;
    COMMTIMEOUTS timeouts{}; timeouts.ReadIntervalTimeout=MAXDWORD; timeouts.ReadTotalTimeoutConstant=100;
    if(!SetCommState(impl_->device,&settings)||!SetCommTimeouts(impl_->device,&timeouts)) {
        SetCommState(impl_->device,&impl_->original); SetCommTimeouts(impl_->device,&impl_->original_timeouts);
        CloseHandle(impl_->device); impl_->device=INVALID_HANDLE_VALUE; error="Unable to configure GPS serial settings"; return failed();
    }
#else
    if(!(path.starts_with("/dev/tty")||path.starts_with("/dev/cu."))||path.find("/../")!=std::string::npos) {
        error="Specify a local serial device under /dev/tty or /dev/cu."; return failed();
    }
    const speed_t speed=baud==4800?B4800:baud==9600?B9600:baud==19200?B19200:baud==38400?B38400:baud==57600?B57600:B115200;
    impl_->device=open(path.c_str(),O_RDONLY|O_NOCTTY|O_NONBLOCK|O_CLOEXEC);
    if(impl_->device<0) { error="Unable to open the requested GPS serial port: "; error+=std::strerror(errno); return failed(); }
    struct stat info{};
    if(fstat(impl_->device,&info)!=0||!S_ISCHR(info.st_mode)||tcgetattr(impl_->device,&impl_->original)!=0) {
        close(impl_->device); impl_->device=-1; error="Requested path is not a configurable local serial device"; return failed();
    }
    termios settings=impl_->original;
    cfmakeraw(&settings); settings.c_cflag|=CLOCAL|CREAD; settings.c_cflag&=~static_cast<tcflag_t>(CSTOPB|PARENB);
#ifdef CRTSCTS
    settings.c_cflag&=~static_cast<tcflag_t>(CRTSCTS);
#endif
    settings.c_cc[VMIN]=0; settings.c_cc[VTIME]=0;
    if(cfsetispeed(&settings,speed)!=0||cfsetospeed(&settings,speed)!=0||tcsetattr(impl_->device,TCSANOW,&settings)!=0) {
        close(impl_->device); impl_->device=-1; error="Unable to configure GPS serial settings"; return failed();
    }
#endif
    impl_->callback=std::move(callback); impl_->pending.clear(); impl_->last_utc=-1; impl_->last_fix.reset(); impl_->running=true;
    {
        std::lock_guard lock(impl_->status_mutex);
        impl_->connection={GpsConnectionState::WaitingForFix,path,"GPS connected; waiting for a valid fix"};
        impl_->fresh_fix_monotonic=0;
    }
    try { impl_->worker=std::thread([this]{impl_->read_loop();}); }
    catch(...) { error="Unable to start GPS reader"; stop(); return failed(); }
    return true;
}
void SerialGps::stop() {
    if(!impl_) return;
    impl_->running=false;
#ifdef _WIN32
    if(impl_->device!=INVALID_HANDLE_VALUE) CancelIoEx(impl_->device,nullptr);
#endif
    if(impl_->worker.joinable()) impl_->worker.join();
#ifdef _WIN32
    if(impl_->device!=INVALID_HANDLE_VALUE) {
        SetCommState(impl_->device,&impl_->original); SetCommTimeouts(impl_->device,&impl_->original_timeouts);
        CloseHandle(impl_->device); impl_->device=INVALID_HANDLE_VALUE;
    }
#else
    if(impl_->device>=0) { tcsetattr(impl_->device,TCSANOW,&impl_->original); close(impl_->device); impl_->device=-1; }
#endif
    std::fill(impl_->pending.begin(),impl_->pending.end(),0); impl_->pending.clear();
    impl_->last_fix.reset(); impl_->callback={};
    {
        std::lock_guard lock(impl_->status_mutex);
        impl_->connection=GpsConnectionStatus{};
        impl_->fresh_fix_monotonic=0;
    }
}
GpsConnectionStatus SerialGps::status() const {
    std::lock_guard lock(impl_->status_mutex);
    auto result=impl_->connection;
    if(result.state==GpsConnectionState::ValidFix&&monotonic_now()-impl_->fresh_fix_monotonic>2.0) {
        result.state=GpsConnectionState::StaleFix;
        result.detail="GPS fix stale; waiting for a fresh fix";
    }
    return result;
}
} // namespace ovmesh
