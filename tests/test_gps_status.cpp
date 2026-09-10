// SPDX-License-Identifier: GPL-3.0-or-later
#include "ovmesh/engine.hpp"
#include "../src/gps.hpp"
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#ifdef __APPLE__
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {
using ovmesh::GpsConnectionState;
void require(bool value,const std::string& message) {if(!value)throw std::runtime_error(message);}
bool until(const std::function<bool()>& predicate,double seconds=3) {
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::duration<double>(seconds);
    while(std::chrono::steady_clock::now()<deadline) {
        if(predicate())return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return predicate();
}
void disconnected_and_manual() {
    ovmesh::Engine engine;
    require(engine.gps_connection_status().state==GpsConnectionState::Disconnected,
        "New engine distinguishes disconnected GPS from a satellite fix failure");
    ovmesh::ReceiverConfig config;config.sample_rate=8000000;config.survey_span_hz=5000000;config.lanes.clear();
    std::string error;
    require(engine.start(config,false,error),error);
    require(until([&]{return engine.snapshot().input_seconds>.15;}),"Synthetic RF started");
    require(engine.snapshot().gps_status=="GPS disconnected",
        "Observations without GPS must not invent a stale connected receiver");
    engine.stop();
    engine.set_fixed_position(0,0);
    require(engine.snapshot().gps_status=="Manual fixed receiver position"&&
        engine.gps_connection_status().state==GpsConnectionState::Disconnected,
        "Manual fixed position is distinct from serial transport");
    const auto prior_track=engine.snapshot().track.size();
    engine.disconnect_gps();
    require(engine.snapshot().gps_status=="Manual fixed receiver position"&&
        engine.snapshot().track.size()==prior_track,"Disconnecting absent serial GPS preserves manual position");
    engine.clear_position();
    require(engine.snapshot().gps_status=="GPS disconnected"&&engine.snapshot().track.size()==prior_track,
        "Clearing future position preserves previous track");
}

#ifdef __APPLE__
// Allocates a test-owned pseudo-terminal. No USB device is enumerated or opened.
class PseudoGps {
public:
    int master=-1;
    std::string path;
    PseudoGps() {
        master=posix_openpt(O_RDWR|O_NOCTTY|O_CLOEXEC);
        require(master>=0,"Create test-owned pseudo-terminal");
        if(grantpt(master)!=0||unlockpt(master)!=0) {
            close(master);master=-1;throw std::runtime_error("Prepare test pseudo-terminal");
        }
        const char* slave=ptsname(master);
        if(!slave) {close(master);master=-1;throw std::runtime_error("Read test pseudo-terminal path");}
        path=slave;
        require(path.starts_with("/dev/tty"),"Only test-owned Mac terminal reaches serial GPS reader");
    }
    ~PseudoGps(){disconnect();}
    void disconnect(){if(master>=0){close(master);master=-1;}}
    void send(const std::string& message) {
        require(master>=0&&write(master,message.data(),message.size())==static_cast<ssize_t>(message.size()),
            "Write synthetic NMEA to test-owned pseudo-terminal");
    }
};
std::string sentence(std::string body) {
    unsigned check=0;for(char c:body)check^=static_cast<unsigned char>(c);
    constexpr char hex[]="0123456789ABCDEF";
    return "$"+body+"*"+hex[check>>4]+hex[check&15]+"\r\n";
}
std::string current_rmc(bool valid=true) {
    const auto now=std::time(nullptr);std::tm utc{};gmtime_r(&now,&utc);
    char time[20],date[20];
    std::strftime(time,sizeof(time),"%H%M%S",&utc);std::strftime(date,sizeof(date),"%d%m%y",&utc);
    return sentence(std::string("GPRMC,")+time+(valid?",A,0000.000,N,00000.000,E,0,0,":",V,,,,,,,")+date+(valid?",,,A":",,,N"));
}
void serial_lifecycle() {
    PseudoGps source;ovmesh::Engine engine;std::string error;
    engine.set_fixed_position(1,2);
    require(engine.connect_gps(source.path,9600,error),error);
    require(engine.gps_connection_status().state==GpsConnectionState::WaitingForFix&&
        engine.gps_connection_status().device_path==source.path&&
        engine.snapshot().gps_status=="GPS connected; waiting for a valid fix",
        "Successful serial connection clears old manual association and waits for a fix");
    require(!engine.connect_gps(source.path,9600,error)&&
        engine.gps_connection_status().state==GpsConnectionState::WaitingForFix,
        "Duplicate connection attempt preserves the active receiver state");
    source.send(current_rmc(false));
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    require(engine.gps_connection_status().state==GpsConnectionState::WaitingForFix,
        "Connected serial receiver with no satellites remains waiting for fix");
    const auto fixed_sentence=current_rmc();source.send(fixed_sentence);
    require(until([&]{return engine.gps_connection_status().state==GpsConnectionState::ValidFix&&
        engine.snapshot().track.size()>1;}),"Synthetic NMEA establishes a fresh receiver fix");
    ovmesh::ReceiverConfig config;config.sample_rate=8000000;config.survey_span_hz=5000000;config.lanes.clear();
    require(engine.start(config,false,error),error);
    require(until([&]{return engine.snapshot().input_seconds>.15;}),"Synthetic RF started with serial GPS");
    require(engine.gps_connection_status().state==GpsConnectionState::ValidFix&&
        engine.snapshot().gps_status=="GPS fix valid"&&!engine.snapshot().track.empty(),
        "Starting RF preserves live GPS source and fix");
    engine.stop();
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    source.send(fixed_sentence);
    require(until([&]{return engine.gps_connection_status().state==GpsConnectionState::StaleFix;}),
        "Repeated NMEA epoch cannot refresh an aging receiver fix");
    require(engine.snapshot().gps_status=="GPS fix stale; waiting for a fresh fix",
        "Freshness state updates even while RF reception is stopped");
    source.send(current_rmc());
    require(until([&]{return engine.gps_connection_status().state==GpsConnectionState::ValidFix;}),
        "New epoch restores valid fix");
    require(engine.start(config,false,error),error);
    require(until([&]{return engine.snapshot().input_seconds>.15;}),"RF resumed before simulated serial loss");
    source.disconnect();
    require(until([&]{return engine.gps_connection_status().state==GpsConnectionState::ReadError;}),
        "Serial hangup reports read error rather than satellite fix loss");
    require(engine.snapshot().gps_status=="GPS serial device disconnected or read failed",
        "Transport failure remains visible to the desktop snapshot");
    const auto before_disconnect=engine.snapshot().track.size();engine.disconnect_gps();
    require(engine.gps_connection_status().state==GpsConnectionState::Disconnected&&
        engine.snapshot().track.size()==before_disconnect&&engine.snapshot().gps_status=="GPS disconnected",
        "Explicit disconnect invalidates future GPS association but preserves prior track");
    require(engine.snapshot().running,"GPS disconnect leaves an active RF survey running");
    engine.stop();
    require(engine.start(config,false,error),error);
    require(until([&]{return engine.snapshot().input_seconds>.15;}),"Synthetic RF restarted after GPS disconnect");
    require(engine.snapshot().track.empty()&&engine.snapshot().gps_status=="GPS disconnected",
        "New survey cannot inherit the disconnected receiver's last fix");
    engine.stop();
}
#endif
}

int main() {
    try {
        disconnected_and_manual();
#ifdef __APPLE__
        serial_lifecycle();
        std::cout<<"GPS lifecycle and Mac pseudo-terminal tests passed; no USB devices opened\n";
#else
        std::cout<<"GPS disconnected/manual lifecycle tests passed; native serial integration pending on this platform\n";
#endif
        return EXIT_SUCCESS;
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return EXIT_FAILURE;}
}
