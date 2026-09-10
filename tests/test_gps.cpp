// GPS coordinates below are public protocol examples or invented test values,
// not captured receiver positions or application defaults. See tests/README.md.
#include "../src/gps.hpp"
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>

namespace {
void require(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
double utc(int year,unsigned month,unsigned day,unsigned hour=0,unsigned minute=0,double second=0) {
    return std::chrono::duration<double>(std::chrono::sys_days{
        std::chrono::year{year}/month/day}.time_since_epoch()).count()+hour*3600+minute*60+second;
}
std::string sentence(std::string body) {
    unsigned check=0; for(char c:body) check^=static_cast<unsigned char>(c);
    static const char* hex="0123456789ABCDEF";
    return "$"+body+"*"+hex[check>>4]+hex[check&15]+"\r\n";
}
void golden() {
    const double now=utc(1994,3,23,12,35,19);
    const auto fix=ovmesh::parse_nmea("$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6A",40,now);
    require(fix && fix->valid,"Published RMC checksum example");
    require(std::abs(fix->latitude-48.1173)<1e-8 && std::abs(fix->longitude-11.5166666667)<1e-8,"Coordinates in degrees/minutes");
    require(fix->utc_seconds==now && fix->monotonic_seconds==40,"Explicit UTC date without local timezone");
    require(fix->source.find("GNSS UTC")!=std::string::npos && !fix->manual,"Time provenance");
    const auto modern=ovmesh::parse_nmea("$GNRMC,001031.00,A,4404.13993,N,12118.86023,W,0.146,,100117,,,A*7B",4,utc(2017,1,10,0,10,31));
    require(modern && modern->valid && modern->longitude<0,"GPSD GN talker example");
    const auto gga=ovmesh::parse_nmea("$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47",40,now);
    require(gga&&gga->valid&&gga->satellites==8&&gga->hdop&&gga->altitude_m,"Published GGA example");
    require(std::abs(*gga->altitude_m-545.4)<1e-8&&std::abs(*gga->hdop-0.9)<1e-8,"GGA fields");
    require(gga->source.find("inferred")!=std::string::npos,"GGA date provenance");
}
void invalid_and_hostile() {
    const double now=utc(2026,9,9,12);
    const std::string body="GPRMC,120000,A,4807.038,N,01131.000,E,0,0,090926,,,A";
    require(ovmesh::parse_nmea(sentence(body),1,now)->valid,"Synthetic current fix");
    auto stale=ovmesh::parse_nmea(sentence(body),1,now+11);
    require(stale&&!stale->valid&&stale->source.find("stale")!=std::string::npos,"Old fix marked invalid");
    require(!ovmesh::parse_nmea(sentence(body),1,now-6)->valid,"Future fix not fresh");
    require(!ovmesh::parse_nmea(sentence("GPRMC,120000,V,,,,,,,090926,,,N"),1,now)->valid,"Void fix");
    require(!ovmesh::parse_nmea(sentence("GPGGA,120000,,,,,0,00,,,,,,,"),1,now)->valid,"No fix status");
    for(char mode:{'E','M','N','S','U','Z'}) {
        std::string b=body; b.back()=mode;
        const auto fix=ovmesh::parse_nmea(sentence(b),1,now);
        require(fix&&!fix->valid,"Estimated, manual, simulation and invalid modes excluded");
    }
    for(const auto& b:{
        "GPRMC,126000,A,4807.038,N,01131.000,E,0,0,090926,,,A",
        "GPRMC,120060,A,4807.038,N,01131.000,E,0,0,090926,,,A",
        "GPRMC,120000,A,4860.001,N,01131.000,E,0,0,090926,,,A",
        "GPRMC,120000,A,9000.001,N,01131.000,E,0,0,090926,,,A",
        "GPRMC,120000,A,4807.038,N,18000.001,E,0,0,090926,,,A",
        "GPRMC,120000,A,4807.038,X,01131.000,E,0,0,090926,,,A",
        "GPRMC,120000,A,nan,N,01131.000,E,0,0,090926,,,A",
        "GPRMC,120000,A,4807.038,N,01131.000,E,0,0,310226,,,A",
        "GPRMC,120000,A,4807.038,N,01131.000,E,0,0,290225,,,A",
        "GPGGA,120000,4807.038,N,01131.000,E,1,08,nan,545.4,M,46.9,M,,",
        "GPGGA,120000,4807.038,N,01131.000,E,1,08,0.9,545.4,F,46.9,M,,",
        "GPGGA,120000,4807.038,N,01131.000,E,1,08,0.9,100001,M,46.9,M,,"})
        require(!ovmesh::parse_nmea(sentence(b),1,now),"Invalid numeric/date/coordinate field rejected");
    auto bad_checksum=sentence(body); bad_checksum[4]='X';
    require(!ovmesh::parse_nmea(bad_checksum,1,now),"Bad checksum");
    require(!ovmesh::parse_nmea("$"+body,1,now),"Missing checksum");
    require(!ovmesh::parse_nmea(sentence(body)+"junk",1,now),"Trailing junk");
    require(!ovmesh::parse_nmea(sentence(std::string(300,'x')),1,now),"Long sentence rejected");
    require(!ovmesh::parse_nmea(sentence(body),std::numeric_limits<double>::quiet_NaN(),now),"NaN monotonic rejected");
    require(!ovmesh::parse_nmea(sentence(body),1,std::numeric_limits<double>::infinity()),"Infinite UTC rejected");
    const auto midnight=ovmesh::parse_nmea(sentence("GPGGA,235959,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,"),1,utc(2026,9,10,0,0,1));
    require(midnight&&midnight->valid&&midnight->utc_seconds==utc(2026,9,9,23,59,59),"GGA previous-day rollover");
    std::mt19937 random(42);
    for(unsigned i=0;i<5000;++i) {
        std::string input(random()%150,' ');
        for(auto& c:input) c=static_cast<char>(random()%256);
        const auto fix=ovmesh::parse_nmea(input,1,now);
        require(!fix,"Random non-NMEA input rejected");
    }
}
void no_device_operations() {
    ovmesh::SerialGps reader; std::string error;
    require(reader.status().state==ovmesh::GpsConnectionState::Disconnected,"New reader is explicitly disconnected");
    auto callback=[](ovmesh::PositionFix){};
    require(!reader.start("",9600,callback,error),"Empty path rejected before open");
    require(!reader.start("tcp://example.test:2947",9600,callback,error),"Network source rejected before open");
    require(!reader.start("/dev/ttySynthetic",123,callback,error),"Invalid baud rejected before open");
    require(reader.status().state==ovmesh::GpsConnectionState::ReadError&&reader.status().detail==error,
        "Failed connection has a durable actionable status");
    require(!reader.start("/Volumes/no-device",9600,callback,error),"Volume path rejected before open");
    reader.stop(); reader.stop();
    require(reader.status().state==ovmesh::GpsConnectionState::Disconnected,"Repeated stop remains disconnected");
    const double mono=ovmesh::monotonic_now();
    require(std::isfinite(mono)&&ovmesh::monotonic_now()>=mono&&std::isfinite(ovmesh::utc_now()),"Clock helpers");
}
void historical_positions() {
    ovmesh::PositionFix good;good.valid=true;good.monotonic_seconds=10;good.latitude=1;good.longitude=2;
    ovmesh::PositionFix lost;lost.monotonic_seconds=11;
    const std::array history{good,lost};
    require(ovmesh::position_at(history,10.5).has_value(),"Later GPS loss preserves earlier observation position");
    require(!ovmesh::position_at(history,11.5),"An invalid fix stops position carry-forward");
    require(!ovmesh::position_at(history,9),"No future GPS association");
    const std::array only_good{good};
    require(ovmesh::position_at(only_good,12).has_value()&&!ovmesh::position_at(only_good,12.01),"GPS freshness is evaluated at sample time");
    good.manual=true;const std::array manual{good,lost};
    require(!ovmesh::position_at(manual,40),"Clearing a manual position prevents its reuse");
    const std::array fixed{good};require(ovmesh::position_at(fixed,40).has_value(),"Explicit stationary position remains valid");
}
}
int main() {
    try { golden(); invalid_and_hostile(); no_device_operations(); historical_positions(); std::cout<<"GPS parser and historical association tests passed; no devices opened\n"; return EXIT_SUCCESS; }
    catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return EXIT_FAILURE; }
}
