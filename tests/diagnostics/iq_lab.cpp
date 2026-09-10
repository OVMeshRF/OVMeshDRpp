// SPDX-License-Identifier: GPL-3.0-or-later
// Opt-in developer experiment. Never linked into the ordinary application.
// Captures temporary signed 8-bit IQ only after BOTH explicit consent flags.
// Replay has no device operations, keys, protocol decoding, or payload output.
#include "ovmesh/engine.hpp"
#include "channelizer.hpp"
#if __has_include(<libhackrf/hackrf.h>)
#include <libhackrf/hackrf.h>
#else
#include <hackrf.h>
#endif
#include <openssl/evp.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <complex>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <unistd.h>

namespace {
constexpr uint32_t rate=8000000;
constexpr uint64_t center=907500000;
constexpr int64_t correction=900;
constexpr uint64_t maximum_bytes=uint64_t(rate)*2*30;
constexpr size_t header_size=96,digest_size=32;
using Header=std::array<uint8_t,header_size>;
template<class T> struct ClearedVector:std::vector<T> {
    using std::vector<T>::vector;
    ~ClearedVector(){OPENSSL_cleanse(this->data(),this->size()*sizeof(T));}
};
volatile std::sig_atomic_t interrupted=0;
void interrupt(int) { interrupted=1; }
void require(bool yes,const char* why) { if(!yes)throw std::runtime_error(why); }
struct File {
    int fd=-1;
    explicit File(int value):fd(value) { require(fd>=0,"Cannot open private capture file"); }
    ~File(){if(fd>=0)::close(fd);}
    File(const File&)=delete;File& operator=(const File&)=delete;
};
void exact_read(int fd,std::span<uint8_t> data) {
    while(!data.empty()) {
        const auto count=::read(fd,data.data(),data.size());
        if(count<0&&errno==EINTR)continue;
        require(count>0,"Truncated or unreadable capture");
        data=data.subspan(static_cast<size_t>(count));
    }
}
void exact_write(int fd,std::span<const uint8_t> data) {
    while(!data.empty()) {
        const auto count=::write(fd,data.data(),data.size());
        if(count<0&&errno==EINTR)continue;
        require(count>0,"Capture write failed");
        data=data.subspan(static_cast<size_t>(count));
    }
}
void put(Header& h,size_t at,uint64_t value,size_t width) {
    for(size_t i=0;i<width;++i)h.at(at+i)=static_cast<uint8_t>(value>>(8*i));
}
uint64_t get(const Header& h,size_t at,size_t width) {
    uint64_t value=0;
    for(size_t i=0;i<width;++i)value|=uint64_t(h.at(at+i))<<(8*i);
    return value;
}
Header make_header(uint64_t bytes,uint64_t utc,uint64_t elapsed,uint64_t callbacks) {
    Header h{};std::memcpy(h.data(),"OVMIQ01\n",8);
    put(h,8,1,4);put(h,12,header_size,4);put(h,16,rate,4);put(h,20,1,4);
    put(h,24,center,8);put(h,32,std::bit_cast<uint64_t>(correction),8);
    put(h,40,bytes,8);put(h,48,utc,8);put(h,56,elapsed,8);
    put(h,64,32,4);put(h,68,48,4);put(h,72,1,4);put(h,76,0,4);
    put(h,80,callbacks,8);return h;
}
uint64_t validate_header(const Header& h,uint64_t file_size) {
    require(std::memcmp(h.data(),"OVMIQ01\n",8)==0&&get(h,8,4)==1&&get(h,12,4)==header_size,"Unsupported capture header");
    require(get(h,16,4)==rate&&get(h,20,4)==1&&get(h,24,8)==center&&
        std::bit_cast<int64_t>(get(h,32,8))==correction,"Capture settings differ from this bounded experiment");
    require(get(h,64,4)==32&&get(h,68,4)==48&&get(h,72,4)==1&&get(h,76,4)==0&&get(h,88,8)==0,"Invalid capture gain, bias, or reserved fields");
    const auto bytes=get(h,40,8);
    require(bytes>0&&bytes<=maximum_bytes&&bytes%2==0,"Invalid capture sample length");
    require(file_size==header_size+bytes+digest_size,"Capture size does not match header");
    require(get(h,56,8)<=35000000&&get(h,80,8)>0,"Invalid capture duration or callback count");
    return bytes;
}
struct Digest {
    std::unique_ptr<EVP_MD_CTX,decltype(&EVP_MD_CTX_free)> ctx{EVP_MD_CTX_new(),EVP_MD_CTX_free};
    Digest(){require(ctx&&EVP_DigestInit_ex(ctx.get(),EVP_sha256(),nullptr)==1,"SHA256 initialization failed");}
    void add(std::span<const uint8_t> bytes){require(EVP_DigestUpdate(ctx.get(),bytes.data(),bytes.size())==1,"SHA256 update failed");}
    std::array<uint8_t,digest_size> finish(){std::array<uint8_t,digest_size> out{};unsigned n=0;require(EVP_DigestFinal_ex(ctx.get(),out.data(),&n)==1&&n==out.size(),"SHA256 finalization failed");return out;}
};
void check_local_private_parent(const std::string& text) {
    const std::filesystem::path path(text);
    require(path.is_absolute()&&!path.filename().empty(),"Use an absolute local capture path");
    std::filesystem::path prefix;
    for(const auto& part:path) {
        require(part!="."&&part!="..","Dot components are not allowed");prefix/=part;
        require(prefix!="/Volumes"&&prefix!="/Network"&&prefix!="/net","Network volumes are not allowed");
        struct stat s{};
        if(::lstat(prefix.c_str(),&s)==0)require(!S_ISLNK(s.st_mode),"Symlink paths are not allowed");
        else require(prefix==path&&errno==ENOENT,"Capture parent is unavailable");
    }
    struct stat s{};require(::stat(path.parent_path().c_str(),&s)==0&&S_ISDIR(s.st_mode)&&s.st_uid==::geteuid()&&(s.st_mode&0077)==0,"Capture parent must be an owner-only directory (0700)");
    struct statfs fs{};require(::statfs(path.parent_path().c_str(),&fs)==0&&(fs.f_flags&MNT_LOCAL),"Capture filesystem must be local");
}
struct Radio {
    hackrf_device* device=nullptr;bool initialized=false;
    ~Radio(){stop();}
    void stop(){if(device){hackrf_stop_rx(device);hackrf_close(device);device=nullptr;}if(initialized){hackrf_exit();initialized=false;}}
};
void radio_ok(int result){if(result!=HACKRF_SUCCESS)throw std::runtime_error(std::string("HackRF: ")+hackrf_error_name(static_cast<hackrf_error>(result)));}
struct Capture {
    std::vector<uint8_t> bytes;
    std::atomic<size_t> used{0};
    std::atomic<uint64_t> callbacks{0};
    std::atomic<bool> invalid{false},full{false};
    explicit Capture(size_t size):bytes(size){}
    ~Capture(){OPENSSL_cleanse(bytes.data(),bytes.size());}
    static int receive(hackrf_transfer* transfer) noexcept {
        auto* state=static_cast<Capture*>(transfer->rx_ctx);
        if(!transfer->buffer||transfer->valid_length<0||transfer->valid_length>transfer->buffer_length||transfer->valid_length%2){state->invalid=true;return -1;}
        const size_t count=static_cast<size_t>(transfer->valid_length),used=state->used.load(std::memory_order_relaxed);
        const size_t keep=std::min(count,state->bytes.size()-used);
        std::memcpy(state->bytes.data()+used,transfer->buffer,keep);
        ++state->callbacks;state->used.store(used+keep,std::memory_order_release);
        if(used+keep==state->bytes.size()){state->full=true;return -1;}
        return 0;
    }
};
void capture(const std::string& path,unsigned seconds) {
    check_local_private_parent(path);
    File output(::open(path.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600));
    bool complete=false;
    try {
        Capture data(static_cast<size_t>(rate)*2*seconds);Radio radio;
        radio_ok(hackrf_init());radio.initialized=true;radio_ok(hackrf_open(&radio.device));
        radio_ok(hackrf_set_sample_rate(radio.device,rate));
        radio_ok(hackrf_set_baseband_filter_bandwidth(radio.device,hackrf_compute_baseband_filter_bw_round_down_lt(rate+1)));
        radio_ok(hackrf_set_freq(radio.device,center+static_cast<uint64_t>(correction)));
        radio_ok(hackrf_set_lna_gain(radio.device,32));radio_ok(hackrf_set_vga_gain(radio.device,48));
        radio_ok(hackrf_set_amp_enable(radio.device,1));radio_ok(hackrf_set_antenna_enable(radio.device,0));
        const auto start=std::chrono::steady_clock::now();
        const auto utc=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        radio_ok(hackrf_start_rx(radio.device,Capture::receive,&data));
        std::cout<<"CAPTURE RUNNING receive_only=1 seconds="<<seconds<<" sample_rate="<<rate<<" nominal_center_hz="<<center<<" tuning_offset_hz="<<correction<<" lna=32 vga=48 amp=1 bias=0 max_iq_bytes="<<data.bytes.size()<<std::endl;
        const auto deadline=start+std::chrono::seconds(seconds);
        while(!interrupted&&!data.full&&!data.invalid&&std::chrono::steady_clock::now()<deadline) {
            const bool streaming=hackrf_is_streaming(radio.device)==HACKRF_TRUE;
            require(streaming||data.full||data.invalid,"Receiver stopped before capture deadline");
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        radio.stop();
        const auto elapsed=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-start).count();
        require(!interrupted&&!data.invalid,"Capture interrupted or callback invalid; discard partial recording");
        const auto used=data.used.load();require(used>0,"No samples received");
        const auto header=make_header(used,static_cast<uint64_t>(utc),static_cast<uint64_t>(elapsed),data.callbacks.load());
        (void)validate_header(header,header_size+used+digest_size);
        Digest digest;digest.add(header);digest.add(std::span(data.bytes).first(used));
        exact_write(output.fd,header);exact_write(output.fd,std::span(data.bytes).first(used));exact_write(output.fd,digest.finish());
        require(::fsync(output.fd)==0,"Capture sync failed");complete=true;
        std::cout<<"CAPTURE STOPPED iq_bytes="<<used<<" delivered_seconds="<<double(used)/(2*rate)<<" elapsed_seconds="<<double(elapsed)/1e6<<" callbacks="<<data.callbacks<<" upstream_loss_unknown=1 integrity=sha256 raw_output=none\n";
    } catch(...) {if(!complete)::unlink(path.c_str());throw;}
}
void replay(const std::string& path) {
    check_local_private_parent(path);File file(::open(path.c_str(),O_RDONLY|O_NOFOLLOW|O_CLOEXEC));
    struct stat info{};require(::fstat(file.fd,&info)==0&&S_ISREG(info.st_mode)&&info.st_uid==::geteuid()&&(info.st_mode&0077)==0&&info.st_nlink==1&&info.st_size>=0,"Capture must be a private regular owner file");
    Header header{};exact_read(file.fd,header);const auto bytes=validate_header(header,static_cast<uint64_t>(info.st_size));
    // Authenticate no party: this digest only detects accidental corruption.
    Digest digest;digest.add(header);ClearedVector<uint8_t> raw(262144);
    for(uint64_t offset=0;offset<bytes;){const auto size=static_cast<size_t>(std::min<uint64_t>(raw.size(),bytes-offset));auto block=std::span(raw).first(size);exact_read(file.fd,block);digest.add(block);offset+=size;}
    std::array<uint8_t,digest_size> stored{};exact_read(file.fd,stored);require(stored==digest.finish(),"Capture digest mismatch");
    require(::lseek(file.fd,header_size,SEEK_SET)==static_cast<off_t>(header_size),"Cannot rewind capture");
    std::array<ovmesh::PhyConfig,2> configs{{{250000,11,5,0x2b},{500000,11,8,0x2b}}};
    std::array<uint64_t,2> frequencies{906875000,908750000};
    std::array<ovmesh::Downconverter,2> lanes{{{rate,250000,-625000},{rate,500000,1250000}}};
    std::array<ovmesh::LoRaReceiver,2> receivers{ovmesh::LoRaReceiver(configs[0]),ovmesh::LoRaReceiver(configs[1])};
    std::array<uint64_t,2> crc_ok{},crc_bad{};
    ClearedVector<std::complex<float>> samples(raw.size()/2),filtered;
    // The fixed sample allocation is never shrunk. Reserve the largest profile's
    // output before use, then wipe before feed() clears/reuses that allocation.
    filtered.reserve(samples.size()/(rate/500000)+1);
    Digest replay_digest;replay_digest.add(header);
    for(uint64_t offset=0;offset<bytes;){
        const auto size=static_cast<size_t>(std::min<uint64_t>(raw.size(),bytes-offset));auto block=std::span(raw).first(size);exact_read(file.fd,block);
        replay_digest.add(block);
        const auto active=std::span(samples).first(size/2);
        for(size_t i=0;i<active.size();++i)active[i]={float(std::bit_cast<int8_t>(raw[2*i]))/128,float(std::bit_cast<int8_t>(raw[2*i+1]))/128};
        for(size_t lane=0;lane<2;++lane){OPENSSL_cleanse(filtered.data(),filtered.size()*sizeof(filtered[0]));lanes[lane].feed(active,filtered);receivers[lane].feed(filtered,[&](ovmesh::PhyFrame&& frame){if(frame.payload_crc_valid)++crc_ok[lane];else ++crc_bad[lane];OPENSSL_cleanse(frame.bytes.data(),frame.bytes.size());});}
        offset+=size;
    }
    require(stored==replay_digest.finish(),"Capture changed during replay; no result accepted");
    for(size_t lane=0;lane<2;++lane){const auto p=receivers[lane].diagnostics();
        std::cout<<"REPLAY lane="<<lane+1<<" frequency_hz="<<frequencies[lane]<<" input_seconds="<<double(bytes)/(2*rate)<<" preamble="<<p.preamble_candidates<<" sync="<<p.sync_matches<<" rejected="<<p.sync_rejections<<" first_mismatch="<<p.sync_first_mismatch<<" second_mismatch="<<p.sync_second_mismatch<<" low_ratio="<<p.sync_low_ratio<<" timeout="<<p.sync_timeout<<" headers_valid="<<p.headers_valid<<" headers_failed="<<p.headers_failed<<" completed="<<p.completed_frames<<" crc_valid="<<crc_ok[lane]<<" crc_failed="<<crc_bad[lane]<<'\n';}
}
void self_test() {
    const auto valid=make_header(16000,1000,1000,1);require(validate_header(valid,header_size+16000+digest_size)==16000,"Valid header rejected");
    for(const size_t position:{size_t(0),size_t(8),size_t(12),size_t(16),size_t(20),size_t(24),size_t(32),size_t(64),size_t(76),size_t(88)}){
        auto bad=valid;bad[position]^=0x01;bool rejected=false;try{(void)validate_header(bad,header_size+16000+digest_size);}catch(const std::exception&){rejected=true;}require(rejected,"Malformed header accepted");}
    for(uint64_t bytes:{uint64_t(0),uint64_t(1),maximum_bytes+2}){auto bad=valid;put(bad,40,bytes,8);bool rejected=false;try{(void)validate_header(bad,header_size+bytes+digest_size);}catch(const std::exception&){rejected=true;}require(rejected,"Invalid length accepted");}
    Capture data(8);std::array<uint8_t,12> input{};hackrf_transfer transfer{};transfer.rx_ctx=&data;transfer.buffer=input.data();transfer.buffer_length=12;transfer.valid_length=12;
    require(Capture::receive(&transfer)==-1&&data.full&&data.used==8&&data.callbacks==1,"Callback sample cap failed");
    Capture invalid(8);transfer.rx_ctx=&invalid;transfer.valid_length=3;
    require(Capture::receive(&transfer)==-1&&invalid.invalid&&invalid.used==0,"Malformed callback accepted");
    std::cout<<"IQ lab header and callback boundary tests passed; no hardware or capture file opened\n";
}
}
int main(int argc,char** argv) {
    try {
        std::string mode,path;bool radio_consent=false,iq_consent=false;unsigned seconds=0;
        for(int i=1;i<argc;++i){const std::string arg=argv[i];auto value=[&](){require(++i<argc,"Missing argument value");return std::string(argv[i]);};
            if(arg=="--capture"||arg=="--replay"){require(mode.empty(),"Choose exactly one mode");mode=arg;path=value();}
            else if(arg=="--self-test"){require(mode.empty(),"Choose exactly one mode");mode=arg;}
            else if(arg=="--confirm-radio-access")radio_consent=true;
            else if(arg=="--confirm-temporary-iq")iq_consent=true;
            else if(arg=="--seconds"){const auto text=value();const auto parsed=std::from_chars(text.data(),text.data()+text.size(),seconds);require(parsed.ec==std::errc{}&&parsed.ptr==text.data()+text.size(),"Invalid duration");}
            else if(arg=="--help"){std::cout<<"Developer-only temporary IQ lab (Mac). No device access by default.\n--capture ABS_NEW.iq8 --seconds 1..30 --confirm-radio-access --confirm-temporary-iq\n--replay ABS_EXISTING.iq8 (no radio, no keys, aggregate output only)\n--self-test\nFixed 8MS/s, nominal907.5MHz,+900Hz,LNA32,VGA48,amp on,bias off; max480MB IQ.\nPrivate0700 local parent, exclusive0600 file, delete temporary captures after investigation.\n";return 0;}
            else throw std::runtime_error("Unknown option");}
        if(mode=="--capture"){
            require(radio_consent&&iq_consent,"Capture requires explicit radio and temporary-IQ consent; no device opened");
            require(seconds>=1&&seconds<=30,"Capture duration must be 1 through 30 seconds");
            std::signal(SIGINT,interrupt);std::signal(SIGTERM,interrupt);capture(path,seconds);
        }else{
            require(!radio_consent&&!iq_consent&&seconds==0,"Capture options cannot be used in passive modes");
            if(mode=="--replay")replay(path);else if(mode=="--self-test")self_test();else throw std::runtime_error("Choose capture, replay, self-test, or help; no device opened");
        }return 0;
    }catch(const std::exception& e){std::cerr<<"IQ lab: "<<e.what()<<'\n';return 1;}
}
