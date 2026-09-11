// SPDX-License-Identifier: GPL-3.0-or-later
#include "ovmesh/engine.hpp"
#include "rak_process.hpp"
#include <cmath>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <functional>

namespace {
void require(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
void rejects(const std::function<void()>& f){bool failed=false;try{f();}catch(const std::exception&){failed=true;}require(failed,"Unsafe input was accepted");}
}
int main()try {
    using namespace ovmesh;
    ConcentratorConfig c;
    validate_concentrator_config(c,915000000,26000000,0,false);
    c.boards.push_back(c.boards.front());c.boards[1].frequency_hz=908750000;c.boards[1].bandwidth_hz=500000;
    validate_concentrator_config(c,915000000,26000000,0,false);
    rejects([&]{validate_concentrator_config(c,915000000,26000000,900,false);});
    rejects([&]{validate_concentrator_config(c,915000000,26000000,0,true);});
    c.boards[0].device_path="/dev/example-a";c.boards[1].device_path="/dev/example-a";
    rejects([&]{validate_concentrator_config(c,915000000,26000000,0);});
    c.boards[1].device_path="/dev/example-b";
    validate_concentrator_config(c,915000000,26000000,0);
    c.boards.push_back(c.boards.back());rejects([&]{validate_concentrator_config(c,915000000,26000000,0);});c.boards.pop_back();
    c.boards[0].bandwidth_hz=200000;rejects([&]{validate_concentrator_config(c,915000000,26000000,0);});c.boards[0].bandwidth_hz=250000;
    c.boards[0].spreading_factor=13;rejects([&]{validate_concentrator_config(c,915000000,26000000,0);});c.boards[0].spreading_factor=11;
    c.scan_enabled=false;for(auto& b:c.boards)b.packets_enabled=false;
    rejects([&]{validate_concentrator_config(c,915000000,26000000,0);});
    ConcentratorScan scan;scan.counts[0]=10;scan.counts[19]=990;scan.counts[20]=500;scan.counts[32]=500;
    require(concentrator_sample_count(scan)==2000,"Histogram count mismatch");
    require(concentrator_fraction_above(scan,-87)==.5,"4 dB quantized threshold mismatch");
    require(concentrator_fraction_above(scan,-90)==.5,"Partial histogram bin must not be interpolated");
    require(concentrator_fraction_above(scan,-91)==.75,"Adjacent-bin threshold mismatch");
    require(std::isinf(concentrator_bin_lower_dbm(32)),"Underflow bin needs open lower boundary");
    require(parse_rak_message("READY").kind==RakMessage::Kind::Ready,"Missing readiness");
    require(parse_rak_message("HEARTBEAT").kind==RakMessage::Kind::Heartbeat,"Missing heartbeat");
    std::ostringstream line;line<<"SCAN 906875000 100.1 100.12";for(const auto n:scan.counts)line<<' '<<n;
    auto parsed=parse_rak_message(line.str());require(parsed.scan.counts==scan.counts,"Histogram IPC lost counts");
    auto packet=parse_rak_message("PACKET 908750000 500000 11 8 -78.5 7.25 1 4294967295 0001aaff");
    require(packet.payload.size()==4 && packet.payload[2]==0xaa && packet.hardware_timestamp_us==UINT32_MAX,"Packet metadata/payload parser mismatch");
    require(parse_rak_message("PACKET 908750000 500000 11 8 -78.5 7.25 0 1 -").payload.empty(),"Bad CRC retains payload");
    for(const std::string invalid:{"", "READY extra", " READY", "READY ",
        "PACKET 908750000 500000 11 8 nan 7 1 1 aa", "PACKET 908750000 500000 11 8 -78 7 0 1 aa",
        "PACKET 908750000 500000 11 8 -78 7 1 4294967296 aa", "PACKET 908750000 500000 11 8 -78 7 1 1 a",
        "PACKET 908750000 500000 11 8 -78 7 1 1 gg"})rejects([&]{(void)parse_rak_message(invalid);});
    rejects([&]{(void)parse_rak_message(std::string(4096,'x'));});
    std::string erroneous="ERROR ";erroneous+="private-token";
    try{(void)parse_rak_message(erroneous);require(false,"Worker error accepted");}
    catch(const std::runtime_error& e){require(std::string(e.what()).find("private-token")==std::string::npos,"Worker input leaked in diagnostic");}
    Engine engine;ReceiverConfig cfg;cfg.synthetic=false;cfg.hardware_receiver=HardwareReceiver::Rak5146;
    cfg.center_hz=915000000;cfg.survey_span_hz=26000000;cfg.concentrators.boards[0].device_path="/dev/example-a";
    std::string error;
    require(!engine.start(cfg,false,error) && error.find("Explicit permission")!=std::string::npos,"Hardware consent bypass");
    const bool capability=engine.snapshot().rak5146_available;
    require(engine.new_session(error,true),"Fresh session failed after rejected start");
    require(engine.snapshot().rak5146_available==capability,"Fresh session lost capability");
    std::cout<<"RAK configuration, histogram, bounded IPC, privacy and consent checks passed\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
