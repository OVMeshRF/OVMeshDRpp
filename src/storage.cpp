// SPDX-License-Identifier: GPL-3.0-or-later
#include "storage.hpp"
#include "bursts.hpp"
#include "ovmesh/local_paths.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cctype>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <locale>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <variant>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <sddl.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/mount.h>
#elif defined(__linux__)
#include <sys/vfs.h>
#endif
#endif

namespace ovmesh {
namespace {
constexpr int application_id = 0x4f564d44;
using Blob = std::vector<uint8_t>;
using Value = std::variant<std::nullptr_t, int64_t, double, std::string, Blob>;
bool valid_utf8(const std::string& text) {
    for (size_t i=0;i<text.size();) {
        const auto first=static_cast<unsigned char>(text[i++]);
        if(first<0x80)continue;
        unsigned value=0,remaining=0,minimum=0;
        if(first>=0xc2 && first<=0xdf){value=first&0x1f;remaining=1;minimum=0x80;}
        else if(first>=0xe0 && first<=0xef){value=first&0x0f;remaining=2;minimum=0x800;}
        else if(first>=0xf0 && first<=0xf4){value=first&7;remaining=3;minimum=0x10000;}
        else return false;
        if(i+remaining>text.size())return false;
        for(unsigned n=0;n<remaining;++n){const auto next=static_cast<unsigned char>(text[i++]);if((next&0xc0)!=0x80)return false;value=(value<<6)|(next&0x3f);}
        if(value<minimum || value>0x10ffff || (value>=0xd800 && value<=0xdfff))return false;
    }
    return true;
}
void check(int rc) {
    if (rc != SQLITE_OK && rc != SQLITE_DONE && rc != SQLITE_ROW)
        throw std::runtime_error("Session database operation failed (code " + std::to_string(rc) + ")");
}
class ReadSnapshot {
public:
    explicit ReadSnapshot(sqlite3* db) : db_(db) {
        if (!db_) throw std::runtime_error("No session database is open");
        // Reuse an existing transaction without taking ownership of its writes.
        if (sqlite3_get_autocommit(db_)) {
            check(sqlite3_exec(db_, "BEGIN", nullptr, nullptr, nullptr));
            owned_ = true;
        }
    }
    ~ReadSnapshot() {
        if (owned_) sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
    }
    ReadSnapshot(const ReadSnapshot&) = delete;
    ReadSnapshot& operator=(const ReadSnapshot&) = delete;
private:
    sqlite3* db_;
    bool owned_ = false;
};
struct Statement {
    sqlite3_stmt* s = nullptr;
    bool owns = true;
    Statement(sqlite3* db, const std::string& sql) { check(sqlite3_prepare_v2(db, sql.c_str(), -1, &s, nullptr)); }
    explicit Statement(sqlite3_stmt* prepared) : s(prepared), owns(false) {check(sqlite3_reset(s));check(sqlite3_clear_bindings(s));}
    ~Statement() { if(owns)sqlite3_finalize(s);else sqlite3_reset(s); }
    void bind(int n, const Value& v) {
        int rc = SQLITE_OK;
        if (std::holds_alternative<std::nullptr_t>(v)) rc = sqlite3_bind_null(s,n);
        else if (auto p=std::get_if<int64_t>(&v)) rc=sqlite3_bind_int64(s,n,*p);
        else if (auto real_value=std::get_if<double>(&v)) {
            if (!std::isfinite(*real_value)) throw std::runtime_error("Non-finite survey measurement rejected");
            rc=sqlite3_bind_double(s,n,*real_value);
        } else if(const auto* bytes=std::get_if<Blob>(&v)) {
            if(bytes->empty() || bytes->size()>65537)throw std::runtime_error("Invalid spectrum blob length");
            rc=sqlite3_bind_blob(s,n,bytes->data(),static_cast<int>(bytes->size()),SQLITE_TRANSIENT);
        } else { const auto& str=std::get<std::string>(v); if(str.size()>8192 || !valid_utf8(str)) throw std::runtime_error("Invalid or oversized survey text field"); rc=sqlite3_bind_text(s,n,str.c_str(),static_cast<int>(str.size()),SQLITE_TRANSIENT); }
        check(rc);
    }
    void values(const std::vector<Value>& values) { int n=1; for(const auto& v:values) bind(n++,v); }
    int step() {
        // Bound work for each row of a potentially untrusted database without
        // imposing a lifetime budget on long, legitimate survey sessions.
        int work = 0;
        auto* db = sqlite3_db_handle(s);
        sqlite3_progress_handler(db, 1000, [](void* value) {
            return ++*static_cast<int*>(value) > 10000 ? 1 : 0;
        }, &work);
        const int result = sqlite3_step(s);
        sqlite3_progress_handler(db, 0, nullptr, nullptr);
        check(result);
        return result;
    }
    void run() { if (step() != SQLITE_DONE) throw std::runtime_error("Unexpected database result"); }
    bool row() { return step()==SQLITE_ROW; }
    int64_t integer(int n) const {
        if (sqlite3_column_type(s,n) != SQLITE_INTEGER) throw std::runtime_error("Invalid saved integer");
        return sqlite3_column_int64(s,n);
    }
    double real(int n) const {
        const int type = sqlite3_column_type(s,n);
        if (type != SQLITE_FLOAT && type != SQLITE_INTEGER) throw std::runtime_error("Invalid saved measurement type");
        double x=sqlite3_column_double(s,n); if(!std::isfinite(x)) throw std::runtime_error("Invalid saved measurement"); return x;
    }
    bool null(int n) const { return sqlite3_column_type(s,n)==SQLITE_NULL; }
    Blob blob(int n,size_t maximum) const {
        if(sqlite3_column_type(s,n)!=SQLITE_BLOB)throw std::runtime_error("Invalid spectrum blob type");
        const int length=sqlite3_column_bytes(s,n);
        if(length<=0 || static_cast<size_t>(length)>maximum)throw std::runtime_error("Invalid spectrum blob size");
        const auto* bytes=static_cast<const uint8_t*>(sqlite3_column_blob(s,n));return {bytes,bytes+length};
    }
    std::string text(int n) const {
        if (null(n)) return {};
        if (sqlite3_column_type(s,n) != SQLITE_TEXT) throw std::runtime_error("Invalid saved text field");
        int len=sqlite3_column_bytes(s,n); if(len>8192) throw std::runtime_error("Saved field exceeds supported size");
        const auto* p=sqlite3_column_text(s,n);auto value=p?std::string(reinterpret_cast<const char*>(p),static_cast<size_t>(len)):std::string{};
        if(!valid_utf8(value))throw std::runtime_error("Saved text is not valid UTF-8");return value;
    }
};
Value number(const std::optional<double>& v) { return v ? Value(*v) : Value(nullptr); }
Value integer(const std::optional<uint32_t>& v) { return v ? Value(static_cast<int64_t>(*v)) : Value(nullptr); }
void insert(sqlite3* db,const std::string& table,const std::string& columns,const std::vector<Value>& values) {
    std::string q="INSERT INTO "+table+"("+columns+") VALUES(";
    for(size_t n=0;n<values.size();++n) { if(n)q+=',';q+='?'; } q+=')';
    Statement s(db,q);s.values(values);s.run();
}
void ensure_local_path(const std::string& path) {
    if (path.empty() || path.find('\0') != std::string::npos || path.rfind("//",0)==0 ||
        path.rfind("\\\\",0)==0 || path.rfind("file:",0)==0)
        throw std::runtime_error("Use an absolute local file path");
    const auto file = std::filesystem::path(std::u8string(path.begin(),path.end()));
    if (!file.is_absolute() || file.filename().empty()) throw std::runtime_error("Use an absolute local file path");
#ifdef _WIN32
    if (GetDriveTypeW(file.root_path().c_str()) != DRIVE_FIXED)
        throw std::runtime_error("Choose a local fixed-disk survey directory");
#endif
    // Reject aliases rather than resolving them: resolution can itself traverse
    // an unapproved mounted/network target. No parent directories are created.
    std::filesystem::path prefix;
    for (const auto& component : file) {
        if (component == "." || component == "..") throw std::runtime_error("File paths must not contain dot components");
        prefix /= component;
        if (prefix == "/Volumes" || prefix == "/net" || prefix == "/Network")
            throw std::runtime_error("Network/mounted volumes are not supported");
        std::error_code error;
        const auto status = std::filesystem::symlink_status(prefix, error);
        if (!error && std::filesystem::is_symlink(status)) throw std::runtime_error("Symlink paths are not supported for survey files");
        if (error && prefix != file) throw std::runtime_error("Survey parent directory is unavailable");
    }
#ifdef _WIN32
    // Reject junctions/reparse points too; the C++ symlink check does not cover
    // every Windows redirection mechanism.
    prefix.clear();
    for (const auto& component : file) {
        prefix /= component;
        const DWORD attributes = GetFileAttributesW(prefix.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
            throw std::runtime_error("Reparse-point paths are not supported for survey files");
    }
#elif defined(__APPLE__)
    struct statfs filesystem{};
    if (::statfs(file.parent_path().c_str(), &filesystem) != 0 || !(filesystem.f_flags & MNT_LOCAL))
        throw std::runtime_error("Choose a local survey directory");
#elif defined(__linux__)
    struct statfs filesystem{};
    if (::statfs(file.parent_path().c_str(), &filesystem) != 0)
        throw std::runtime_error("Survey parent directory is unavailable");
    // Known network filesystems and FUSE (whose backend may be remote) are
    // rejected. This is not a guarantee against a privileged remount race.
    const auto type = static_cast<unsigned long>(filesystem.f_type);
    for (const auto remote : {0x6969UL, 0x517bUL, 0xff534d42UL, 0x01021997UL, 0x5346414fUL, 0x65735546UL})
        if (type == remote) throw std::runtime_error("Network/FUSE survey paths are not supported");
#endif
}

class PrivateFile {
public:
    explicit PrivateFile(const std::string& path) : path_(path) {
        ensure_local_path(path);
    if(path.empty()) throw std::runtime_error("Choose a session file path");
#ifdef _WIN32
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;OW)", SDDL_REVISION_1, &descriptor, nullptr))
            throw std::runtime_error("Cannot prepare private file permissions");
        SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), descriptor, FALSE};
        handle_ = CreateFileW(std::filesystem::path(std::u8string(path.begin(),path.end())).c_str(), GENERIC_WRITE | DELETE, 0, &security, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        LocalFree(descriptor);
        if (handle_ == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot create file; choose a new local filename");
#else
        descriptor_ = ::open(path.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
        if(descriptor_<0) throw std::runtime_error("Cannot create file; choose a new local filename");
#endif
    }
    ~PrivateFile() { close(); }
    PrivateFile(const PrivateFile&) = delete;
    PrivateFile& operator=(const PrivateFile&) = delete;
    void write(const std::string& data) {
        size_t offset = 0;
        while (offset < data.size()) {
#ifdef _WIN32
            DWORD written = 0;
            if (!WriteFile(handle_, data.data() + offset, static_cast<DWORD>(std::min<size_t>(data.size() - offset, 65536)), &written, nullptr) || written == 0)
                throw std::runtime_error("Export write failed; partial file left for inspection");
#else
            const auto written = ::write(descriptor_, data.data() + offset, data.size() - offset);
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) throw std::runtime_error("Export write failed; partial file left for inspection");
#endif
            offset += static_cast<size_t>(written);
        }
    }
    void sync() {
#ifdef _WIN32
        if (!FlushFileBuffers(handle_)) throw std::runtime_error("Export flush failed");
#else
        if (::fsync(descriptor_) != 0) throw std::runtime_error("Export flush failed");
#endif
    }
    // Delete only the file this object created, never an unrelated replacement.
    // Called for a failed new report; historical recordings are never touched.
    void discard() noexcept {
#ifdef _WIN32
        FILE_DISPOSITION_INFO disposition{TRUE};
        if(handle_!=INVALID_HANDLE_VALUE)SetFileInformationByHandle(handle_,FileDispositionInfo,&disposition,sizeof(disposition));
#else
        struct stat held{},named{};
        if(descriptor_>=0 && ::fstat(descriptor_,&held)==0 && ::lstat(path_.c_str(),&named)==0 &&
           held.st_dev==named.st_dev && held.st_ino==named.st_ino)::unlink(path_.c_str());
#endif
        close();
    }
    void close() {
#ifdef _WIN32
        if (handle_ != INVALID_HANDLE_VALUE) { CloseHandle(handle_); handle_ = INVALID_HANDLE_VALUE; }
#else
        if (descriptor_ >= 0) { ::close(descriptor_); descriptor_ = -1; }
#endif
    }
private:
    std::string path_;
#ifdef _WIN32
    HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
    int descriptor_ = -1;
#endif
};

void nonnegative(double value, const char* field) {
    if (!std::isfinite(value) || value < 0) throw std::runtime_error(std::string("Invalid ") + field);
}
void coordinates(double latitude, double longitude) {
    if (!std::isfinite(latitude) || !std::isfinite(longitude) || std::abs(latitude)>90 || std::abs(longitude)>180)
        throw std::runtime_error("Invalid geographic coordinates");
}
void optional_finite(const std::optional<double>& value) {
    if (value && !std::isfinite(*value)) throw std::runtime_error("Non-finite optional measurement");
}
void validate_fix(const PositionFix& fix) {
    coordinates(fix.latitude, fix.longitude);
    nonnegative(fix.utc_seconds, "GPS timestamp");
    nonnegative(fix.monotonic_seconds, "GPS monotonic timestamp");
    optional_finite(fix.altitude_m);
    if (fix.hdop) nonnegative(*fix.hdop, "GPS HDOP");
    if (fix.satellites > 1000 || fix.source.size() > 512) throw std::runtime_error("Invalid GPS metadata");
}
std::string canonical_classification(const std::string& value) {
    if(value=="likely Meshtastic" || value=="possible Meshtastic" || value=="likely LoRaWAN")return value;
    return "unknown LoRa";
}
void validate_reception(const Reception& r, size_t forward_route_limit = 32) {
    if (r.id > static_cast<uint64_t>(INT64_MAX) || r.frequency_hz == 0 || r.frequency_hz > 6000000000ULL ||
        r.bandwidth_hz == 0 || r.bandwidth_hz > 2000000 || r.spreading_factor < 5 || r.spreading_factor > 12 ||
        r.coding_rate < 5 || r.coding_rate > 8 || r.lane_label.size() > 160)
        throw std::runtime_error("Invalid reception metadata");
    nonnegative(r.utc_seconds, "reception time"); nonnegative(r.elapsed_seconds, "reception elapsed time");
    nonnegative(r.duration_seconds, "reception duration");
    if (!std::isfinite(r.snr_db) || !std::isfinite(r.frequency_error_hz)) throw std::runtime_error("Invalid RF estimate");
    if (r.decoded.status < protocol::Status::decoded || r.decoded.status > protocol::Status::crypto_error)
        throw std::runtime_error("Invalid decode status");
    if(r.decoded.authentication!="not authenticated")throw std::runtime_error("Unsupported authentication claim at persistence boundary");
    const auto* a = r.decoded.authorized ? &*r.decoded.authorized : nullptr;
    if ((r.decoded.status == protocol::Status::decoded) != (a != nullptr) ||
        (a && (!r.crc_valid || !r.header_valid))) throw std::runtime_error("Content rejected at persistence boundary");
    if (r.decoded.evidence) {
        const auto& evidence = *r.decoded.evidence;
        if (!r.crc_valid || !r.header_valid || evidence.port == 0 || evidence.port > 65535 ||
            (a ? evidence.port != a->port || evidence.signature_present != a->signature_present
               : r.decoded.status != protocol::Status::unsupported_payload || r.decoded.classification != "possible Meshtastic"))
            throw std::runtime_error("Envelope evidence eligibility is inconsistent");
    } else if (r.decoded.classification == "possible Meshtastic") {
        throw std::runtime_error("Possible Meshtastic classification requires envelope evidence");
    }
    if (a) {
        const auto& c = a->content;
        const std::array<std::string_view,7> supported_kinds{"text","position","node","device telemetry","environment telemetry","routing","traceroute"};
        if(canonical_classification(r.decoded.classification)!="likely Meshtastic" ||
           std::find(supported_kinds.begin(),supported_kinds.end(),c.kind)==supported_kinds.end())
            throw std::runtime_error("Unsupported authorized protocol/content kind");
        if (a->profile_id.empty() || a->profile_id.size() > 160 || c.kind.empty() || c.kind.size() > 64 ||
            c.text.size() > 2048 || c.node_id.size() > 128 || c.long_name.size() > 256 || c.short_name.size() > 64 ||
            c.route.size() > forward_route_limit || c.route_back.size() > 32 || c.snr_towards.size() > 32 ||
            c.snr_back.size() > 32 || a->hop_limit > 7 || a->hop_start > 7)
            throw std::runtime_error("Content exceeds approved bounds");
        if (!c.routing_variant.empty() && (a->port != 5 || c.kind != "routing" ||
            (c.routing_variant != "request" && c.routing_variant != "reply" && c.routing_variant != "error")))
            throw std::runtime_error("Invalid routing variant");
        if (c.latitude.has_value() != c.longitude.has_value()) throw std::runtime_error("Incomplete sender-reported position");
        if (c.latitude) coordinates(*c.latitude, *c.longitude);
        for (const auto* value : {&c.altitude, &c.voltage, &c.temperature, &c.humidity, &c.battery_percent, &c.channel_utilization, &c.air_util_tx}) optional_finite(*value);
    }
    if (r.receiver_position && r.receiver_position->valid) validate_fix(*r.receiver_position);
}

void validate_bin(const FrequencySummary& bin) {
    if (bin.center_hz == 0 || bin.center_hz > 6000000000ULL || bin.width_hz == 0 || bin.width_hz > 20000000 ||
        !std::isfinite(bin.mean_dbfs) || !std::isfinite(bin.peak_dbfs)) throw std::runtime_error("Invalid frequency-bin metadata");
    nonnegative(bin.observed_seconds, "observed time"); nonnegative(bin.active_seconds, "active time");
    if (bin.active_seconds > bin.observed_seconds + 1e-9) throw std::runtime_error("Activity exceeds observed time");
}

void validate_window(const SurveyWindow& window) {
    if(window.id==0 || window.id>static_cast<uint64_t>(INT64_MAX) || window.frequencies.empty() || window.frequencies.size()>8192)
        throw std::runtime_error("Invalid survey-window identity or bin count");
    nonnegative(window.utc_start_seconds,"survey-window UTC start");nonnegative(window.utc_end_seconds,"survey-window UTC end");
    nonnegative(window.elapsed_start_seconds,"survey-window elapsed start");nonnegative(window.elapsed_end_seconds,"survey-window elapsed end");
    if(window.utc_end_seconds<=window.utc_start_seconds || window.elapsed_end_seconds<=window.elapsed_start_seconds)
        throw std::runtime_error("Survey-window end must follow its start");
    std::set<uint64_t> centers;
    for(const auto& bin:window.frequencies) {
        validate_bin(bin);
        if(!centers.insert(bin.center_hz).second)throw std::runtime_error("Duplicate frequency bin in survey window");
    }
    if(window.receiver_position && window.receiver_position->valid)validate_fix(*window.receiver_position);
}
std::string json_string(const std::string& input) {
    std::ostringstream out;out<<'"';
    for(char character:input) {
        const auto c=static_cast<unsigned char>(character);
        if(c=='"'||c=='\\')out<<'\\'<<static_cast<char>(c);
        else if(c<32)out<<"\\u"<<std::hex<<std::setw(4)<<std::setfill('0')<<static_cast<unsigned>(c)<<std::dec;
        else out<<static_cast<char>(c);
    }out<<'"';return out.str();
}
constexpr const char* reception_columns="id,utc,elapsed,frequency,bw,sf,cr,duration,snr,cfo,header_ok,crc_ok,lane,status,classification,authentication,profile,origin,destination,packet_id,port,hop_limit,hop_start,channel_hash,next_hop,relay_node,want_ack,via_mqtt,want_response,kind,text,node_id,long_name,short_name,latitude,longitude,altitude,voltage,temperature,humidity,battery,channel_utilization,air_util_tx,reported_time,hardware_model,role,routing_error,receiver_latitude,receiver_longitude,receiver_altitude,receiver_manual,receiver_utc,receiver_monotonic,receiver_source,receiver_hdop,receiver_satellites";
std::string reception_columns_for(int version) {
    return std::string(reception_columns) + (version >= 3 ?
        ",evidence_port,evidence_signature_present,request_id,reply_id,signature_present,routing_variant" : "");
}
uint64_t unsigned_value(const Statement& s, int column, uint64_t maximum = UINT32_MAX) {
    const auto value = s.integer(column);
    if (value < 0 || static_cast<uint64_t>(value) > maximum) throw std::runtime_error("Saved integer is outside its allowed range");
    return static_cast<uint64_t>(value);
}
bool boolean(const Statement& s, int column) { return unsigned_value(s, column, 1) != 0; }
Reception reception_from(Statement& s, int version) {
    Reception r;r.id=unsigned_value(s,0,INT64_MAX);r.utc_seconds=s.real(1);r.elapsed_seconds=s.real(2);r.frequency_hz=unsigned_value(s,3,6000000000ULL);r.bandwidth_hz=static_cast<uint32_t>(unsigned_value(s,4));r.spreading_factor=static_cast<unsigned>(unsigned_value(s,5,12));r.coding_rate=static_cast<unsigned>(unsigned_value(s,6,8));r.duration_seconds=s.real(7);r.snr_db=s.real(8);r.frequency_error_hz=s.real(9);r.header_valid=boolean(s,10);r.crc_valid=boolean(s,11);r.lane_label=s.text(12);
    auto status=s.integer(13);if(status<0||status>static_cast<int>(protocol::Status::crypto_error))throw std::runtime_error("Unsupported saved decode status");
    r.decoded.status=static_cast<protocol::Status>(status);r.decoded.classification=canonical_classification(s.text(14));r.decoded.authentication=s.text(15);
    if(!s.null(16)) {
        protocol::AuthorizedContent a;a.profile_id=s.text(16);a.from=static_cast<uint32_t>(unsigned_value(s,17));a.to=static_cast<uint32_t>(unsigned_value(s,18));a.packet_id=static_cast<uint32_t>(unsigned_value(s,19));a.port=static_cast<uint32_t>(unsigned_value(s,20));a.hop_limit=static_cast<uint8_t>(unsigned_value(s,21,7));a.hop_start=static_cast<uint8_t>(unsigned_value(s,22,7));a.channel_hash=static_cast<uint8_t>(unsigned_value(s,23,255));a.next_hop=static_cast<uint8_t>(unsigned_value(s,24,255));a.relay_node=static_cast<uint8_t>(unsigned_value(s,25,255));a.want_ack=boolean(s,26);a.via_mqtt=boolean(s,27);a.want_response=boolean(s,28);
        auto& c=a.content;c.kind=s.text(29);c.text=s.text(30);c.node_id=s.text(31);c.long_name=s.text(32);c.short_name=s.text(33);
        std::optional<double>* fields[]={&c.latitude,&c.longitude,&c.altitude,&c.voltage,&c.temperature,&c.humidity,&c.battery_percent,&c.channel_utilization,&c.air_util_tx};
        for(int n=0;n<9;++n)if(!s.null(34+n))*fields[n]=s.real(34+n);
        std::optional<uint32_t>* ints[]={&c.reported_time,&c.hardware_model,&c.role,&c.routing_error};
        for(int n=0;n<4;++n)if(!s.null(43+n))*ints[n]=static_cast<uint32_t>(unsigned_value(s,43+n));
        if(!r.header_valid||!r.crc_valid||r.decoded.status!=protocol::Status::decoded)throw std::runtime_error("Saved content eligibility is inconsistent");
        r.decoded.authorized=std::move(a);
    } else for (int column=17;column<47;++column) if(!s.null(column)) throw std::runtime_error("Unauthorised content fields in saved record");
    if (!s.null(47) || !s.null(48)) {
        PositionFix f;f.latitude=s.real(47);f.longitude=s.real(48);if(!s.null(49))f.altitude_m=s.real(49);
        f.manual=boolean(s,50);f.valid=true;f.utc_seconds=s.real(51);f.monotonic_seconds=s.real(52);f.source=s.text(53);
        if(!s.null(54))f.hdop=s.real(54);f.satellites=static_cast<unsigned>(unsigned_value(s,55,1000));r.receiver_position=f;
    } else for (int column=49;column<56;++column) if(!s.null(column)) throw std::runtime_error("Incomplete receiver position in saved record");
    if (version >= 3) {
        if (!s.null(56) || !s.null(57))
            r.decoded.evidence=protocol::EnvelopeEvidence{static_cast<uint32_t>(unsigned_value(s,56)),boolean(s,57)};
        if (r.decoded.authorized) {
            auto& a=*r.decoded.authorized;
            a.request_id=static_cast<uint32_t>(unsigned_value(s,58));a.reply_id=static_cast<uint32_t>(unsigned_value(s,59));
            a.signature_present=boolean(s,60);
            if(s.null(61))throw std::runtime_error("Missing saved routing variant");
            a.content.routing_variant=s.text(61);
        } else for (int column=58;column<62;++column) if(!s.null(column))
            throw std::runtime_error("Unauthorized correlation or routing fields in saved record");
    }
    validate_reception(r,version>=3?32:64);
    return r;
}

void read_route(sqlite3* db, Reception& reception, int version) {
    Statement route(db,"SELECT step,node FROM routes WHERE reception=? ORDER BY step");
    route.bind(1,int64_t(reception.id));
    size_t expected = 0;
    while(route.row()) {
        const size_t limit=version>=3?32:64;
        if(!reception.decoded.authorized || expected >= limit || unsigned_value(route,0,limit-1) != expected++)
            throw std::runtime_error("Invalid saved authorized route");
        reception.decoded.authorized->content.route.push_back(static_cast<uint32_t>(unsigned_value(route,1)));
    }
    if(version>=3) {
        Statement details(db,"SELECT kind,step,value FROM route_details WHERE reception=? ORDER BY kind,step");
        details.bind(1,int64_t(reception.id));
        while(details.row()) {
            if(!reception.decoded.authorized)throw std::runtime_error("Unauthorized saved route details");
            auto& c=reception.decoded.authorized->content;
            const auto kind=details.text(0);
            if(kind=="route_back") {
                if(c.route_back.size()>=32 || unsigned_value(details,1,31)!=c.route_back.size())throw std::runtime_error("Invalid saved return route");
                c.route_back.push_back(static_cast<uint32_t>(unsigned_value(details,2)));
            } else if(kind=="snr_towards" || kind=="snr_back") {
                auto& snr=kind=="snr_towards"?c.snr_towards:c.snr_back;
                const auto value=details.integer(2);
                if(snr.size()>=32 || unsigned_value(details,1,31)!=snr.size() || value<INT32_MIN || value>INT32_MAX)
                    throw std::runtime_error("Invalid saved route SNR");
                snr.push_back(static_cast<int32_t>(value));
            } else throw std::runtime_error("Unsupported saved route detail");
        }
    }
    validate_reception(reception,version>=3?32:64);
}

constexpr const char* tile_columns="id,first_sample,end_sample,utc_start,utc_end,elapsed_start,elapsed_end,first_center,bin_width,fft_size,frame_count,bin_count,background,clipped,quality,mean_cdb,peak_cdb,activity,start_lat,start_lon,start_alt,start_manual,start_utc,start_monotonic,start_source,start_hdop,start_satellites,end_lat,end_lon,end_alt,end_manual,end_utc,end_monotonic,end_source,end_hdop,end_satellites";
constexpr const char* compact_positions_ddl="CREATE TABLE positions(utc REAL,latitude REAL,longitude REAL,altitude REAL,manual INTEGER,source TEXT,hdop REAL,satellites INTEGER,monotonic REAL,id INTEGER PRIMARY KEY)";
constexpr const char* compact_tile_columns="id,first_sample,end_sample,utc_start,utc_end,elapsed_start,elapsed_end,first_center,bin_width,fft_size,frame_count,bin_count,background,clipped,quality,activity,power_id,start_fix,end_fix";
constexpr const char* power_columns="id,first_sample,end_sample,utc_start,utc_end,elapsed_start,elapsed_end,first_center,bin_width,fft_size,frame_count,bin_count,mean_cdb,peak_cdb";
constexpr const char* compact_tile_ddl="CREATE TABLE spectrum_tiles(id INTEGER PRIMARY KEY,first_sample INTEGER,end_sample INTEGER,utc_start REAL,utc_end REAL,elapsed_start REAL,elapsed_end REAL,first_center REAL,bin_width REAL,fft_size INTEGER,frame_count INTEGER,bin_count INTEGER,background REAL,clipped INTEGER,quality INTEGER,activity BLOB,power_id INTEGER REFERENCES power_blocks(id),start_fix INTEGER REFERENCES positions(id),end_fix INTEGER REFERENCES positions(id))";
constexpr const char* power_ddl="CREATE TABLE power_blocks(id INTEGER PRIMARY KEY,first_sample INTEGER,end_sample INTEGER,utc_start REAL,utc_end REAL,elapsed_start REAL,elapsed_end REAL,first_center REAL,bin_width REAL,fft_size INTEGER,frame_count INTEGER,bin_count INTEGER,mean_cdb BLOB,peak_cdb BLOB)";
// LEFT JOIN keeps malformed/missing references visible to the typed validator.
// The first 36 columns have the legacy logical tile layout; aggregate bounds
// and provenance follow. No missing reference silently removes an observation.
std::string tiles_sql(int version,const char* order="ORDER BY t.id") {
    if(version<6)return std::string("SELECT t.* FROM spectrum_tiles t ")+order;
    return std::string("SELECT t.id,t.first_sample,t.end_sample,t.utc_start,t.utc_end,t.elapsed_start,t.elapsed_end,t.first_center,t.bin_width,t.fft_size,t.frame_count,t.bin_count,t.background,t.clipped,t.quality,p.mean_cdb,p.peak_cdb,t.activity,")+
        "a.latitude,a.longitude,a.altitude,a.manual,a.utc,a.monotonic,a.source,a.hdop,a.satellites,"
        "b.latitude,b.longitude,b.altitude,b.manual,b.utc,b.monotonic,b.source,b.hdop,b.satellites,"
        "p.elapsed_start,p.elapsed_end,p.first_sample,p.end_sample,p.first_center,p.bin_width,p.fft_size,p.frame_count,p.bin_count,t.start_fix,t.end_fix,a.rowid,b.rowid,p.id,p.utc_start,p.utc_end "
        "FROM spectrum_tiles t LEFT JOIN power_blocks p ON p.id=t.power_id "
        "LEFT JOIN positions a ON a.rowid=t.start_fix LEFT JOIN positions b ON b.rowid=t.end_fix "+order;
}
constexpr const char* event_columns="id,first_sample,end_sample,utc_start,utc_end,elapsed_start,elapsed_end,lower_hz,upper_hz,active,mean,peak,quality,start_lat,start_lon,start_alt,start_manual,start_utc,start_monotonic,start_source,start_hdop,start_satellites,end_lat,end_lon,end_alt,end_manual,end_utc,end_monotonic,end_source,end_hdop,end_satellites";
constexpr const char* gap_columns="id,missing_samples,utc_start,utc_end,elapsed_start,elapsed_end,reason";
constexpr const char* metrology_columns="fft_size,hop_size,window,bin_width,enbw,normalization,activity_rule,position_association,time_association,antenna,receiver,notes,encoding";
constexpr const char* waveform_columns="id,center_hz,bandwidth_hz,spreading_factor,first_observed_elapsed,delimiter_elapsed,delimiter_utc,up_match,down_match,contributing_subbands,complete_in_requested_range,association_ambiguous,receiver_lat,receiver_lon,receiver_alt,receiver_manual,receiver_utc,receiver_monotonic,receiver_source,receiver_hdop,receiver_satellites";
constexpr const char* discovery_status_columns="id,enabled,finished,failed,method,fault,accepted_input_samples,rejected_input_samples,channelized_input_samples,abandoned_input_samples,source_queue_drops,stream_resets,result_overflows,gap_overflows,observations";
constexpr const char* discovery_band_columns="subband_index,center_hz,processed_samples,abandoned_samples,source_gap_input_samples,candidate_limit_hits,track_limit_hits";
constexpr const char* discovery_gap_columns="id,first_input_sample,end_input_sample,subband_index,reason";
// Exact DDL is deliberately part of schema 5: reject hidden/generated columns,
// constraints, triggers or substituted virtual tables in untrusted recordings.
const std::map<std::string,std::string>& discovery_tables() {
    static const std::map<std::string,std::string> tables{
        {"waveform_observations","CREATE TABLE waveform_observations(id INTEGER PRIMARY KEY,center_hz REAL,bandwidth_hz INTEGER,spreading_factor INTEGER,first_observed_elapsed REAL,delimiter_elapsed REAL,delimiter_utc REAL,up_match REAL,down_match REAL,contributing_subbands INTEGER,complete_in_requested_range INTEGER,association_ambiguous INTEGER,receiver_lat REAL,receiver_lon REAL,receiver_alt REAL,receiver_manual INTEGER,receiver_utc REAL,receiver_monotonic REAL,receiver_source TEXT,receiver_hdop REAL,receiver_satellites INTEGER)"},
        {"discovery_status","CREATE TABLE discovery_status(id INTEGER PRIMARY KEY,enabled INTEGER,finished INTEGER,failed INTEGER,method TEXT,fault TEXT,accepted_input_samples INTEGER,rejected_input_samples INTEGER,channelized_input_samples INTEGER,abandoned_input_samples INTEGER,source_queue_drops INTEGER,stream_resets INTEGER,result_overflows INTEGER,gap_overflows INTEGER,observations INTEGER)"},
        {"discovery_bands","CREATE TABLE discovery_bands(subband_index INTEGER PRIMARY KEY,center_hz REAL,processed_samples INTEGER,abandoned_samples INTEGER,source_gap_input_samples INTEGER,candidate_limit_hits INTEGER,track_limit_hits INTEGER)"},
        {"discovery_gaps","CREATE TABLE discovery_gaps(id INTEGER PRIMARY KEY,first_input_sample INTEGER,end_input_sample INTEGER,subband_index INTEGER,reason TEXT)"}};
    return tables;
}
constexpr unsigned discovery_band_limit=32;
constexpr uint64_t discovery_sample_limit=uint64_t{1}<<53;
constexpr const char* waveform_interpretation="LoRa waveform observation; inferred modem settings; no packet decode, protocol identity or authentication";
constexpr const char* waveform_time_association="observed preamble through delimiter only; not packet airtime";
constexpr const char* waveform_position_association="receiver fix associated with delimiter time; no transmitter position";
constexpr const char* detailed_encoding="little-endian signed int16 centidB; activity codec 0=bytes,1=u16le count+byte; frame-major LSB-first";
constexpr const char* compact_encoding="little-endian signed int16 centidB; activity codec 0=bytes,1=u16le count+byte; frame-major LSB-first; compact power: frame-weighted linear mean and per-bin maximum over <=1s contiguous blocks; tile activity unchanged; endpoint fixes reference positions.rowid";
constexpr const char* normalization="integrated bin power = |FFT|^2/(N*sum(window^2)); uncalibrated dBFS/bin";
constexpr const char* position_association="receiver-end fix; start/end endpoints retained; no transmitter location or interpolated route";
void fix_values(std::vector<Value>& values,const std::optional<PositionFix>& position) {
    if(position && position->valid) {validate_fix(*position);const auto& f=*position;values.insert(values.end(),{f.latitude,f.longitude,number(f.altitude_m),int64_t(f.manual),f.utc_seconds,f.monotonic_seconds,f.source,number(f.hdop),int64_t(f.satellites)});}
    else values.insert(values.end(),9,nullptr);
}
std::optional<PositionFix> read_fix(const Statement& s,int offset) {
    if(s.null(offset)) {for(int i=1;i<9;++i)if(!s.null(offset+i))throw std::runtime_error("Incomplete spectrum receiver position");return {};}
    PositionFix f;f.latitude=s.real(offset);f.longitude=s.real(offset+1);if(!s.null(offset+2))f.altitude_m=s.real(offset+2);
    f.manual=boolean(s,offset+3);f.utc_seconds=s.real(offset+4);f.monotonic_seconds=s.real(offset+5);f.source=s.text(offset+6);
    if(!s.null(offset+7))f.hdop=s.real(offset+7);f.satellites=static_cast<unsigned>(unsigned_value(s,offset+8,1000));f.valid=true;validate_fix(f);return f;
}
void level(double value) {if(!std::isfinite(value)||value< -300||value>100)throw std::runtime_error("Spectrum power outside supported range");}
void interval(uint64_t id,double utc_start,double utc_end,double elapsed_start,double elapsed_end) {
    if(!id || id>INT64_MAX)throw std::runtime_error("Invalid spectrum record identity");
    for(double value:{utc_start,utc_end,elapsed_start,elapsed_end})nonnegative(value,"spectrum record time");
    if(utc_end<=utc_start || elapsed_end<=elapsed_start || elapsed_end>1e10 ||
       std::abs((utc_end-utc_start)-(elapsed_end-elapsed_start))>0.002)
        throw std::runtime_error("Inconsistent spectrum time interval");
}
void validate_tile(const SpectrumTile& t,const ReceiverConfig& c) {
    interval(t.id,t.utc_start_seconds,t.utc_end_seconds,t.elapsed_start_seconds,t.elapsed_end_seconds);
    const size_t bins=t.mean_dbfs.size();const double width=double(c.sample_rate)/4096;
    if(t.fft_size!=4096 || t.frame_count==0 || t.frame_count>128 || bins==0 || bins>4096 || t.peak_dbfs.size()!=bins ||
       t.activity.size()!=((bins+7)/8)*t.frame_count || t.first_sample>INT64_MAX || t.end_sample>INT64_MAX ||
       t.end_sample<=t.first_sample || t.end_sample-t.first_sample!=uint64_t(t.frame_count)*4096 ||
       !std::isfinite(t.first_center_hz)||!std::isfinite(t.bin_width_hz)||std::abs(t.bin_width_hz-width)>1e-7 ||
       std::abs((t.elapsed_end_seconds-t.elapsed_start_seconds)-double(t.end_sample-t.first_sample)/c.sample_rate)>1e-7 ||
       t.clipped_samples>t.end_sample-t.first_sample || t.quality>1023)
        throw std::runtime_error("Invalid spectrum tile dimensions, timing, or quality");
    const double index=(t.first_center_hz-(double(c.center_hz)-double(c.sample_rate)/2))/width;
    if(std::abs(index-std::round(index))>1e-6 || index<0 || index+double(bins)>4096 ||
       t.first_center_hz-width/2<double(c.center_hz)-double(c.survey_span_hz)/2-1e-5 ||
       t.first_center_hz+(double(bins)-0.5)*width>double(c.center_hz)+double(c.survey_span_hz)/2+1e-5)
        throw std::runtime_error("Spectrum grid is outside the recorded usable range");
    level(t.background_dbfs);
    for(size_t i=0;i<bins;++i){level(t.mean_dbfs[i]);level(t.peak_dbfs[i]);if(t.mean_dbfs[i]>t.peak_dbfs[i]+0.011)throw std::runtime_error("Spectrum mean exceeds peak");}
    if(bins%8)for(size_t f=0;f<t.frame_count;++f)if(t.activity[(f+1)*((bins+7)/8)-1] & uint8_t(0xffu<<(bins%8)))throw std::runtime_error("Nonzero spectrum bitmap padding");
    if(t.receiver_start)validate_fix(*t.receiver_start);if(t.receiver_end)validate_fix(*t.receiver_end);
}
Blob quantize(const std::vector<float>& values) {
    Blob bytes;bytes.reserve(values.size()*2);for(float value:values){level(value);const auto q=static_cast<uint16_t>(static_cast<int16_t>(std::lround(value*100)));bytes.push_back(uint8_t(q&255));bytes.push_back(uint8_t(q>>8));}return bytes;
}
std::vector<float> dequantize(const Blob& bytes,size_t count) {
    if(bytes.size()!=count*2)throw std::runtime_error("Invalid quantized spectrum length");
    std::vector<float> values;values.reserve(count);for(size_t i=0;i<count;++i){const unsigned u=unsigned(bytes[2*i])|(unsigned(bytes[2*i+1])<<8);const int q=u>=32768?int(u)-65536:int(u);const float value=float(q)/100;level(value);values.push_back(value);}return values;
}
Blob encode_activity(const Blob& source) {
    Blob out{1};for(size_t i=0;i<source.size();){size_t end=i+1;while(end<source.size() && source[end]==source[i] && end-i<65535)++end;
        out.push_back(uint8_t((end-i)&255));out.push_back(uint8_t((end-i)>>8));out.push_back(source[i]);i=end;
        if(out.size()>=source.size()+1){out.assign(1,0);out.insert(out.end(),source.begin(),source.end());return out;}}
    return out;
}
Blob decode_activity(const Blob& encoded,size_t count) {
    if(count==0||count>65536||encoded.empty())throw std::runtime_error("Invalid spectrum mask dimensions");
    if(encoded[0]==0){if(encoded.size()!=count+1)throw std::runtime_error("Invalid raw measurement mask length");return {encoded.begin()+1,encoded.end()};}
    if(encoded[0]!=1 || (encoded.size()-1)%3)throw std::runtime_error("Invalid spectrum mask encoding");
    Blob out;out.reserve(count);for(size_t i=1;i<encoded.size();i+=3){const size_t run=size_t(encoded[i])+(size_t(encoded[i+1])<<8);if(!run||run>count-out.size())throw std::runtime_error("Invalid spectrum mask run length");out.insert(out.end(),run,encoded[i+2]);}
    if(out.size()!=count)throw std::runtime_error("Truncated spectrum mask");return out;
}
SpectrumTile tile_from(const Statement& s,const ReceiverConfig& c,int version=5) {
    SpectrumTile t;t.id=unsigned_value(s,0,INT64_MAX);t.first_sample=unsigned_value(s,1,INT64_MAX);t.end_sample=unsigned_value(s,2,INT64_MAX);
    t.utc_start_seconds=s.real(3);t.utc_end_seconds=s.real(4);t.elapsed_start_seconds=s.real(5);t.elapsed_end_seconds=s.real(6);
    t.first_center_hz=s.real(7);t.bin_width_hz=s.real(8);t.fft_size=static_cast<uint32_t>(unsigned_value(s,9,4096));t.frame_count=static_cast<uint32_t>(unsigned_value(s,10,128));
    const size_t bins=static_cast<size_t>(unsigned_value(s,11,4096));if(!bins||!t.frame_count)throw std::runtime_error("Empty spectrum tile");
    t.background_dbfs=static_cast<float>(s.real(12));t.clipped_samples=unsigned_value(s,13,INT64_MAX);t.quality=static_cast<uint32_t>(unsigned_value(s,14,511));
    t.mean_dbfs=dequantize(s.blob(15,8192),bins);t.peak_dbfs=dequantize(s.blob(16,8192),bins);t.activity=decode_activity(s.blob(17,65537),((bins+7)/8)*t.frame_count);
    t.receiver_start=read_fix(s,18);t.receiver_end=read_fix(s,27);
    t.power_elapsed_start=t.elapsed_start_seconds;t.power_elapsed_end=t.elapsed_end_seconds;
    if(version>=6) {
        t.power_elapsed_start=s.real(36);t.power_elapsed_end=s.real(37);
        const auto first=unsigned_value(s,38,INT64_MAX),last=unsigned_value(s,39,INT64_MAX);
        const auto frames=unsigned_value(s,43,5000),count=unsigned_value(s,44,4096);
        interval(unsigned_value(s,49,INT64_MAX),s.real(50),s.real(51),t.power_elapsed_start,t.power_elapsed_end);
        if(t.power_elapsed_start>t.elapsed_start_seconds+1e-8 || t.power_elapsed_end<t.elapsed_end_seconds-1e-8 ||
           t.power_elapsed_end-t.power_elapsed_start>1.0000001 || first>t.first_sample || last<t.end_sample ||
           last-first!=frames*4096 || count!=bins || s.real(40)!=t.first_center_hz || s.real(41)!=t.bin_width_hz || unsigned_value(s,42)!=4096 ||
           std::abs(t.power_elapsed_end-t.power_elapsed_start-double(last-first)/c.sample_rate)>1e-7 ||
           (!s.null(45)&&(s.null(47)||s.integer(45)!=s.integer(47))) || (!s.null(46)&&(s.null(48)||s.integer(46)!=s.integer(48))))
            throw std::runtime_error("Invalid compact power block or receiver reference");
        t.quality|=SurveyPowerAggregated;
    }
    validate_tile(t,c);return t;
}
void validate_event(const SpectrumEvent& e,const ReceiverConfig& c) {
    interval(e.id,e.utc_start_seconds,e.utc_end_seconds,e.elapsed_start_seconds,e.elapsed_end_seconds);level(e.mean_dbfs);level(e.peak_dbfs);
    if(e.end_sample<=e.first_sample||e.end_sample>INT64_MAX || !std::isfinite(e.lower_hz)||!std::isfinite(e.upper_hz)||
       e.lower_hz<double(c.center_hz)-double(c.survey_span_hz)/2-1e-5 || e.upper_hz>double(c.center_hz)+double(c.survey_span_hz)/2+1e-5 ||
       e.upper_hz<=e.lower_hz || !std::isfinite(e.active_seconds)|| e.active_seconds<0 ||
       e.active_seconds>e.elapsed_end_seconds-e.elapsed_start_seconds+1e-7 || e.mean_dbfs>e.peak_dbfs+0.011 || e.quality>511)
        throw std::runtime_error("Invalid spectrum event");
    if(e.receiver_start)validate_fix(*e.receiver_start);if(e.receiver_end)validate_fix(*e.receiver_end);
}
SpectrumEvent event_from(const Statement& s,const ReceiverConfig& c) {
    SpectrumEvent e;e.id=unsigned_value(s,0,INT64_MAX);e.first_sample=unsigned_value(s,1,INT64_MAX);e.end_sample=unsigned_value(s,2,INT64_MAX);
    e.utc_start_seconds=s.real(3);e.utc_end_seconds=s.real(4);e.elapsed_start_seconds=s.real(5);e.elapsed_end_seconds=s.real(6);e.lower_hz=s.real(7);e.upper_hz=s.real(8);
    e.active_seconds=s.real(9);e.mean_dbfs=static_cast<float>(s.real(10));e.peak_dbfs=static_cast<float>(s.real(11));e.quality=static_cast<uint32_t>(unsigned_value(s,12,511));e.receiver_start=read_fix(s,13);e.receiver_end=read_fix(s,22);validate_event(e,c);return e;
}
void validate_gap(const CoverageGap& g) {interval(g.id,g.utc_start_seconds,g.utc_end_seconds,g.elapsed_start_seconds,g.elapsed_end_seconds);if(g.missing_samples>INT64_MAX||g.reason.empty()||g.reason.size()>512||!valid_utf8(g.reason))throw std::runtime_error("Invalid coverage gap");}
CoverageGap gap_from(const Statement& s) {CoverageGap g;g.id=unsigned_value(s,0,INT64_MAX);g.missing_samples=unsigned_value(s,1,INT64_MAX);g.utc_start_seconds=s.real(2);g.utc_end_seconds=s.real(3);g.elapsed_start_seconds=s.real(4);g.elapsed_end_seconds=s.real(5);g.reason=s.text(6);validate_gap(g);return g;}
void discovery_counter(uint64_t value) {
    if(value>INT64_MAX)throw std::runtime_error("Discovery counter exceeds saved integer range");
}
void validate_waveform(const WaveformObservation& w,const ReceiverConfig& c) {
    const double lower=double(c.center_hz)-c.survey_span_hz/2.;
    const double upper=double(c.center_hz)+c.survey_span_hz/2.;
    if(!c.discover_lora || w.id==0 || w.id>INT64_MAX ||
       !std::isfinite(w.center_hz) || w.center_hz<=0 || w.center_hz>6000000000. ||
       (w.bandwidth_hz!=125000 && w.bandwidth_hz!=250000 && w.bandwidth_hz!=500000) ||
       w.spreading_factor<7 || w.spreading_factor>12 ||
       w.contributing_subbands==0 || w.contributing_subbands>discovery_band_limit ||
       w.center_hz+w.bandwidth_hz/2.<=lower || w.center_hz-w.bandwidth_hz/2.>=upper ||
       (w.complete_in_requested_range && (w.center_hz-w.bandwidth_hz/2.<lower-1e-5 || w.center_hz+w.bandwidth_hz/2.>upper+1e-5)))
        throw std::runtime_error("Invalid waveform identity, inferred profile or frequency extent");
    for(double value:{w.first_observed_elapsed,w.delimiter_elapsed,w.delimiter_utc}) {
        nonnegative(value,"waveform evidence time");
        if(value>1e10)throw std::runtime_error("Waveform evidence time exceeds supported range");
    }
    if(w.delimiter_elapsed<=w.first_observed_elapsed)
        throw std::runtime_error("Waveform delimiter must follow observed preamble evidence");
    for(double value:{w.up_match,w.down_match})
        if(!std::isfinite(value)||value<=0||value>1.00001)
            throw std::runtime_error("Invalid waveform match fraction");
    if(w.receiver_position) {
        if(!w.receiver_position->valid)throw std::runtime_error("Invalid waveform receiver position");
        validate_fix(*w.receiver_position);
    }
}
WaveformObservation waveform_from(const Statement& row,const ReceiverConfig& c) {
    WaveformObservation w;w.id=unsigned_value(row,0,INT64_MAX);w.center_hz=row.real(1);
    w.bandwidth_hz=static_cast<uint32_t>(unsigned_value(row,2,500000));
    w.spreading_factor=static_cast<unsigned>(unsigned_value(row,3,12));
    w.first_observed_elapsed=row.real(4);w.delimiter_elapsed=row.real(5);w.delimiter_utc=row.real(6);
    w.up_match=row.real(7);w.down_match=row.real(8);
    w.contributing_subbands=static_cast<unsigned>(unsigned_value(row,9,discovery_band_limit));
    w.complete_in_requested_range=boolean(row,10);w.association_ambiguous=boolean(row,11);
    w.receiver_position=read_fix(row,12);validate_waveform(w,c);return w;
}
void validate_discovery_band(const DiscoveryBandCoverage& b) {
    if(b.subband_index>=discovery_band_limit || !std::isfinite(b.center_hz) || b.center_hz<=0 || b.center_hz>6000000000.)
        throw std::runtime_error("Invalid discovery subband");
    for(uint64_t value:{b.processed_samples,b.abandoned_samples,b.source_gap_input_samples,b.candidate_limit_hits,b.track_limit_hits})discovery_counter(value);
}
void validate_discovery_status(const DiscoveryStatus& d,bool enabled) {
    if(d.enabled!=enabled || d.method!="lora-preamble-v1" || d.fault.size()>160 ||
       !valid_utf8(d.fault) || d.fault.find('\0')!=std::string::npos || d.bands.size()>discovery_band_limit)
        throw std::runtime_error("Invalid discovery status or unsupported method");
    for(uint64_t value:{d.accepted_input_samples,d.rejected_input_samples,d.channelized_input_samples,d.abandoned_input_samples,
        d.source_queue_drops,d.stream_resets,d.result_overflows,d.gap_overflows,d.observations})discovery_counter(value);
    // Counters describe different stages/units. Do not add them or force their
    // instantaneous snapshots to sum to input coverage while work is queued.
    std::set<unsigned> indexes;
    for(const auto& b:d.bands) {
        validate_discovery_band(b);
        if(!indexes.insert(b.subband_index).second)throw std::runtime_error("Duplicate discovery subband");
    }
}
std::vector<Value> discovery_status_values(const DiscoveryStatus& d) {
    return {int64_t(1),int64_t(d.enabled),int64_t(d.finished),int64_t(d.failed),d.method,d.fault,
        int64_t(d.accepted_input_samples),int64_t(d.rejected_input_samples),int64_t(d.channelized_input_samples),int64_t(d.abandoned_input_samples),
        int64_t(d.source_queue_drops),int64_t(d.stream_resets),int64_t(d.result_overflows),int64_t(d.gap_overflows),int64_t(d.observations)};
}
DiscoveryStatus discovery_status_from(const Statement& row) {
    if(unsigned_value(row,0,1)!=1)throw std::runtime_error("Invalid discovery status identity");
    if(row.null(4)||row.null(5))throw std::runtime_error("Missing discovery method or fault field");
    DiscoveryStatus d;d.enabled=boolean(row,1);d.finished=boolean(row,2);d.failed=boolean(row,3);d.method=row.text(4);d.fault=row.text(5);
    d.accepted_input_samples=unsigned_value(row,6,INT64_MAX);d.rejected_input_samples=unsigned_value(row,7,INT64_MAX);
    d.channelized_input_samples=unsigned_value(row,8,INT64_MAX);d.abandoned_input_samples=unsigned_value(row,9,INT64_MAX);
    d.source_queue_drops=unsigned_value(row,10,INT64_MAX);d.stream_resets=unsigned_value(row,11,INT64_MAX);
    d.result_overflows=unsigned_value(row,12,INT64_MAX);d.gap_overflows=unsigned_value(row,13,INT64_MAX);d.observations=unsigned_value(row,14,INT64_MAX);
    return d;
}
DiscoveryBandCoverage discovery_band_from(const Statement& row) {
    DiscoveryBandCoverage b;b.subband_index=static_cast<unsigned>(unsigned_value(row,0,discovery_band_limit-1));b.center_hz=row.real(1);
    b.processed_samples=unsigned_value(row,2,INT64_MAX);b.abandoned_samples=unsigned_value(row,3,INT64_MAX);
    b.source_gap_input_samples=unsigned_value(row,4,INT64_MAX);b.candidate_limit_hits=unsigned_value(row,5,INT64_MAX);b.track_limit_hits=unsigned_value(row,6,INT64_MAX);
    validate_discovery_band(b);return b;
}
void validate_discovery_gap(const DiscoveryGap& g) {
    static constexpr std::array reasons{"source_queue_full","input_discontinuity","input_too_large","invalid_input_order","invalid_sample_coordinate","processing_failure"};
    if(g.id==0 || g.id>INT64_MAX || g.end_input_sample<=g.first_input_sample || g.end_input_sample>discovery_sample_limit ||
       g.subband_index< -1 || g.subband_index>=static_cast<int>(discovery_band_limit) ||
       std::find(reasons.begin(),reasons.end(),g.reason)==reasons.end())
        throw std::runtime_error("Invalid discovery gap");
}
DiscoveryGap discovery_gap_from(const Statement& row) {
    DiscoveryGap g;g.id=unsigned_value(row,0,INT64_MAX);g.first_input_sample=unsigned_value(row,1,discovery_sample_limit);
    g.end_input_sample=unsigned_value(row,2,discovery_sample_limit);const auto band=row.integer(3);
    if(band< -1 || band>=discovery_band_limit)throw std::runtime_error("Invalid saved discovery-gap subband");
    g.subband_index=static_cast<int>(band);g.reason=row.text(4);validate_discovery_gap(g);return g;
}
// RTL uses an explicit extension and a non-boolean source discriminator. Earlier
// readers reject this table (and source=2) rather than reporting RTL as HackRF.
constexpr const char* receiver_setup_ddl="CREATE TABLE receiver_setup(id INTEGER PRIMARY KEY,hardware TEXT NOT NULL,gain_tenths_db INTEGER NOT NULL,auto_gain INTEGER NOT NULL)";
void read_receiver_setup(sqlite3* db, ReceiverConfig& config) {
    Statement receiver(db,"SELECT id,hardware,gain_tenths_db,auto_gain FROM receiver_setup LIMIT 2");
    if(!receiver.row() || receiver.integer(0)!=1 || receiver.text(1)!="rtl_sdr")
        throw std::runtime_error("Missing or invalid RTL-SDR receiver metadata");
    const auto gain=receiver.integer(2);
    if(gain< -100||gain>600)throw std::runtime_error("Invalid saved RTL-SDR tuner gain");
    config.rtl_gain_tenths_db=static_cast<int>(gain);config.rtl_auto_gain=boolean(receiver,3);
    if(receiver.row())throw std::runtime_error("Multiple RTL-SDR receiver metadata records");
}
void validate_schema(sqlite3* db, int version) {
    std::map<std::string,std::string> layouts{
        {"session","id,title,version,synthetic,center,sample_rate,span,lna,vga,amp,threshold,complete,elapsed,input_seconds,measurement_seconds,delivered,dropped,receptions,authorized"},
        {"lanes","label,channel,protocol,frequency,bw,sf,cr,enabled"},
        {"frequencies","center,width,mean,peak,observed,active"},
        {"receptions",reception_columns},{"routes","reception,step,node"},
        {"positions","utc,latitude,longitude,altitude,manual,source,hdop,satellites,monotonic"},
        {"coverage","utc,elapsed,input_seconds,measurement_seconds,dropped,state"},
        {"lane_health","label,frequency,processed,frames,decoded,crc_failures,resets,state"},
        {"survey_windows","id,utc_start,utc_end,elapsed_start,elapsed_end,associated_at,receiver_latitude,receiver_longitude,receiver_altitude,receiver_manual,receiver_utc,receiver_monotonic,receiver_source,receiver_hdop,receiver_satellites"},
        {"window_bins","window_id,center,width,mean,peak,observed,active"}};
    if(version>=2)layouts.at("session")+=",tuning_offset_hz";
    if(version>=3) {layouts.at("receptions")=reception_columns_for(version);layouts.emplace("route_details","reception,kind,step,value");}
    if(version>=4){layouts.emplace("spectrum_tiles",tile_columns);layouts.emplace("spectrum_events",event_columns);layouts.emplace("coverage_gaps",gap_columns);layouts.emplace("survey_metrology",metrology_columns);}
    if(version>=5){layouts.at("session")+=",discover_lora";layouts.emplace("waveform_observations",waveform_columns);layouts.emplace("discovery_status",discovery_status_columns);layouts.emplace("discovery_bands",discovery_band_columns);layouts.emplace("discovery_gaps",discovery_gap_columns);}
    if(version>=6){layouts.at("spectrum_tiles")=compact_tile_columns;layouts.emplace("power_blocks",power_columns);layouts.at("positions")+=",id";}
    std::set<std::string> found;
    Statement objects(db,"SELECT type,name,sql FROM sqlite_schema ORDER BY name");
    while(objects.row()) {
        const auto type=objects.text(0), name=objects.text(1), sql=objects.text(2);
        if(type=="index" && (name=="sqlite_autoindex_session_1" || name=="sqlite_autoindex_routes_1" || name=="sqlite_autoindex_window_bins_1") && sql.empty()) continue;
        if(version>=3 && type=="index" && name=="sqlite_autoindex_route_details_1" && sql.empty())continue;
        if(version>=6 && type=="index" && name=="position_identity" && sql=="CREATE INDEX position_identity ON positions(utc,monotonic,source)")continue;
        if(type=="index" && name=="reception_time" && sql=="CREATE INDEX reception_time ON receptions(utc)") continue;
        if(version>=5 && name=="receiver_setup" && type=="table" && sql==receiver_setup_ddl)
            layouts.emplace("receiver_setup","id,hardware,gain_tenths_db,auto_gain");
        const auto expected=layouts.find(name);
        if(type!="table" || expected==layouts.end() || sql.rfind("CREATE TABLE "+name+"(",0)!=0)
            throw std::runtime_error("Unexpected or executable objects in session schema");
        if(version>=6 && ((name=="spectrum_tiles"&&sql!=compact_tile_ddl)||(name=="power_blocks"&&sql!=power_ddl)||(name=="positions"&&sql!=compact_positions_ddl)))
            throw std::runtime_error("Unsupported compact measurement table definition");
        const auto discovery_table=discovery_tables().find(name);
        if(discovery_table!=discovery_tables().end() && sql!=discovery_table->second)
            throw std::runtime_error("Unsupported discovery table definition");
        Statement columns(db,"PRAGMA table_xinfo("+name+")");
        std::string names;
        while(columns.row()) { if(columns.integer(6)!=0)throw std::runtime_error("Hidden or generated session column");if(!names.empty()) names+=',';names+=columns.text(1); }
        if(names!=expected->second) throw std::runtime_error("Unsupported session table layout");
        found.insert(name);
    }
    if(found.size()!=layouts.size()) throw std::runtime_error("Incomplete session schema");
    Statement rows(db,"SELECT count(*) FROM session");
    if(!rows.row()||rows.integer(0)!=1)throw std::runtime_error("Session must have exactly one metadata record");
    Statement source(db,"SELECT synthetic FROM session");
    if(!source.row())throw std::runtime_error("Session has no receiver source");
    const auto discriminator=unsigned_value(source,0,version>=5?2:1);
    if((discriminator==2)!=found.contains("receiver_setup"))
        throw std::runtime_error("Receiver source and RTL-SDR metadata extension disagree");
    if(discriminator==2) {ReceiverConfig receiver;read_receiver_setup(db,receiver);}
}
}

void validate_local_file_path(const std::string& path) {
    if (!valid_utf8(path)) throw std::runtime_error("File path is not valid UTF-8");
    ensure_local_path(path);
}

SessionStore::~SessionStore(){if(tile_insert_)sqlite3_finalize(tile_insert_);if(db_){if(pending_)sqlite3_exec(db_,"COMMIT;",nullptr,nullptr,nullptr);sqlite3_close_v2(db_);}}
void SessionStore::execute(const char* sql) const { check(sqlite3_exec(db_,sql,nullptr,nullptr,nullptr)); }
void SessionStore::begin_batch() {
    if(!db_||readonly_||(schema_version_!=5&&schema_version_!=6))throw std::runtime_error("Survey is read-only");
    if(!pending_){execute("BEGIN IMMEDIATE;");pending_=true;}
}
void SessionStore::configure(bool readonly) {
    sqlite3_busy_timeout(db_,1500);sqlite3_limit(db_,SQLITE_LIMIT_LENGTH,1024*1024);sqlite3_limit(db_,SQLITE_LIMIT_SQL_LENGTH,32768);
    sqlite3_limit(db_,SQLITE_LIMIT_ATTACHED,0);sqlite3_limit(db_,SQLITE_LIMIT_EXPR_DEPTH,64);
    sqlite3_limit(db_,SQLITE_LIMIT_TRIGGER_DEPTH,0);
    sqlite3_db_config(db_,SQLITE_DBCONFIG_ENABLE_LOAD_EXTENSION,0,nullptr);
    sqlite3_db_config(db_,SQLITE_DBCONFIG_DEFENSIVE,1,nullptr);
    execute("PRAGMA trusted_schema=OFF; PRAGMA foreign_keys=ON; PRAGMA temp_store=MEMORY;");
    if(readonly)execute("PRAGMA query_only=ON;");
    if(!readonly)execute("PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL; PRAGMA wal_autocheckpoint=128;");
}
void SessionStore::create(const std::string& path,const ReceiverConfig& c,const std::string& id) {
    ensure_local_path(path);if(db_)throw std::runtime_error("Session already open");
    // Validate nominal and corrected tuner frequencies before creating a file.
    (void)tuned_center_hz(c);
    if(c.sample_rate<1000000 || c.sample_rate>20000000 || !c.survey_span_hz || c.survey_span_hz>c.sample_rate ||
       c.antenna_description.size()>512||c.receiver_description.size()>512||c.survey_notes.size()>2048)
        throw std::runtime_error("Invalid survey configuration or notes");
    const bool rtl=!c.synthetic&&c.hardware_receiver==HardwareReceiver::RtlSdr;
    if(c.hardware_receiver!=HardwareReceiver::HackRf&&c.hardware_receiver!=HardwareReceiver::RtlSdr)
        throw std::runtime_error("Unsupported survey receiver");
    if(rtl&&((c.sample_rate!=1000000&&c.sample_rate!=2000000)||c.amplifier||
        c.survey_span_hz<500000||c.survey_span_hz>c.sample_rate*4/5||
        (c.discover_lora&&(c.sample_rate!=2000000||c.survey_span_hz>1500000))||
        c.rtl_gain_tenths_db< -100||c.rtl_gain_tenths_db>600))
        throw std::runtime_error("Invalid RTL-SDR recording configuration");
    PrivateFile created(path);created.close();
    try {
    check(sqlite3_open_v2(path.c_str(),&db_,SQLITE_OPEN_READWRITE|SQLITE_OPEN_NOMUTEX|SQLITE_OPEN_NOFOLLOW,nullptr));configure(false);
    execute("PRAGMA application_id=1331055940; PRAGMA user_version=5; BEGIN IMMEDIATE;"
        "CREATE TABLE session(id TEXT PRIMARY KEY,title TEXT,version TEXT,synthetic INTEGER,center INTEGER,sample_rate INTEGER,span INTEGER,lna INTEGER,vga INTEGER,amp INTEGER,threshold REAL,complete INTEGER DEFAULT 0,elapsed REAL DEFAULT 0,input_seconds REAL DEFAULT 0,measurement_seconds REAL DEFAULT 0,delivered INTEGER DEFAULT 0,dropped INTEGER DEFAULT 0,receptions INTEGER DEFAULT 0,authorized INTEGER DEFAULT 0,tuning_offset_hz INTEGER NOT NULL DEFAULT 0,discover_lora INTEGER NOT NULL DEFAULT 0);"
        "CREATE TABLE lanes(label TEXT,channel TEXT,protocol TEXT,frequency INTEGER,bw INTEGER,sf INTEGER,cr INTEGER,enabled INTEGER);"
        "CREATE TABLE frequencies(center INTEGER PRIMARY KEY,width INTEGER,mean REAL,peak REAL,observed REAL,active REAL);"
        "CREATE TABLE receptions(id INTEGER PRIMARY KEY,utc REAL,elapsed REAL,frequency INTEGER,bw INTEGER,sf INTEGER,cr INTEGER,duration REAL,snr REAL,cfo REAL,header_ok INTEGER,crc_ok INTEGER,lane TEXT,status INTEGER,classification TEXT,authentication TEXT,profile TEXT,origin INTEGER,destination INTEGER,packet_id INTEGER,port INTEGER,hop_limit INTEGER,hop_start INTEGER,channel_hash INTEGER,next_hop INTEGER,relay_node INTEGER,want_ack INTEGER,via_mqtt INTEGER,want_response INTEGER,kind TEXT,text TEXT,node_id TEXT,long_name TEXT,short_name TEXT,latitude REAL,longitude REAL,altitude REAL,voltage REAL,temperature REAL,humidity REAL,battery REAL,channel_utilization REAL,air_util_tx REAL,reported_time INTEGER,hardware_model INTEGER,role INTEGER,routing_error INTEGER,receiver_latitude REAL,receiver_longitude REAL,receiver_altitude REAL,receiver_manual INTEGER,receiver_utc REAL,receiver_monotonic REAL,receiver_source TEXT,receiver_hdop REAL,receiver_satellites INTEGER,evidence_port INTEGER,evidence_signature_present INTEGER,request_id INTEGER,reply_id INTEGER,signature_present INTEGER,routing_variant TEXT);"
        "CREATE TABLE routes(reception INTEGER REFERENCES receptions(id),step INTEGER,node INTEGER,PRIMARY KEY(reception,step));"
        "CREATE TABLE route_details(reception INTEGER REFERENCES receptions(id),kind TEXT,step INTEGER,value INTEGER,PRIMARY KEY(reception,kind,step));"
        "CREATE TABLE positions(utc REAL,latitude REAL,longitude REAL,altitude REAL,manual INTEGER,source TEXT,hdop REAL,satellites INTEGER,monotonic REAL);"
        "CREATE TABLE coverage(utc REAL,elapsed REAL,input_seconds REAL,measurement_seconds REAL,dropped INTEGER,state TEXT);"
        "CREATE TABLE lane_health(label TEXT,frequency INTEGER,processed REAL,frames INTEGER,decoded INTEGER,crc_failures INTEGER,resets INTEGER,state TEXT);"
        "CREATE TABLE survey_windows(id INTEGER PRIMARY KEY,utc_start REAL,utc_end REAL,elapsed_start REAL,elapsed_end REAL,associated_at TEXT,receiver_latitude REAL,receiver_longitude REAL,receiver_altitude REAL,receiver_manual INTEGER,receiver_utc REAL,receiver_monotonic REAL,receiver_source TEXT,receiver_hdop REAL,receiver_satellites INTEGER);"
        "CREATE TABLE window_bins(window_id INTEGER REFERENCES survey_windows(id),center INTEGER,width INTEGER,mean REAL,peak REAL,observed REAL,active REAL,PRIMARY KEY(window_id,center));"
        "CREATE TABLE spectrum_tiles(id INTEGER PRIMARY KEY,first_sample INTEGER,end_sample INTEGER,utc_start REAL,utc_end REAL,elapsed_start REAL,elapsed_end REAL,first_center REAL,bin_width REAL,fft_size INTEGER,frame_count INTEGER,bin_count INTEGER,background REAL,clipped INTEGER,quality INTEGER,mean_cdb BLOB,peak_cdb BLOB,activity BLOB,start_lat REAL,start_lon REAL,start_alt REAL,start_manual INTEGER,start_utc REAL,start_monotonic REAL,start_source TEXT,start_hdop REAL,start_satellites INTEGER,end_lat REAL,end_lon REAL,end_alt REAL,end_manual INTEGER,end_utc REAL,end_monotonic REAL,end_source TEXT,end_hdop REAL,end_satellites INTEGER);"
        "CREATE TABLE spectrum_events(id INTEGER PRIMARY KEY,first_sample INTEGER,end_sample INTEGER,utc_start REAL,utc_end REAL,elapsed_start REAL,elapsed_end REAL,lower_hz REAL,upper_hz REAL,active REAL,mean REAL,peak REAL,quality INTEGER,start_lat REAL,start_lon REAL,start_alt REAL,start_manual INTEGER,start_utc REAL,start_monotonic REAL,start_source TEXT,start_hdop REAL,start_satellites INTEGER,end_lat REAL,end_lon REAL,end_alt REAL,end_manual INTEGER,end_utc REAL,end_monotonic REAL,end_source TEXT,end_hdop REAL,end_satellites INTEGER);"
        "CREATE TABLE coverage_gaps(id INTEGER PRIMARY KEY,missing_samples INTEGER,utc_start REAL,utc_end REAL,elapsed_start REAL,elapsed_end REAL,reason TEXT);"
        "CREATE TABLE survey_metrology(fft_size INTEGER,hop_size INTEGER,window TEXT,bin_width REAL,enbw REAL,normalization TEXT,activity_rule TEXT,position_association TEXT,time_association TEXT,antenna TEXT,receiver TEXT,notes TEXT,encoding TEXT);"
        "CREATE INDEX reception_time ON receptions(utc); COMMIT;");
    for(const auto& [name,ddl]:discovery_tables()){(void)name;execute(ddl.c_str());}
    insert(db_,"session","id,title,version,synthetic,center,sample_rate,span,lna,vga,amp,threshold,tuning_offset_hz,discover_lora",{id,c.session_title,Engine::version(),int64_t(rtl?2:c.synthetic?1:0),int64_t(c.center_hz),int64_t(c.sample_rate),int64_t(c.survey_span_hz),int64_t(c.lna_gain),int64_t(c.vga_gain),int64_t(c.amplifier),double(c.activity_threshold_dbfs),c.tuning_offset_hz,int64_t(c.discover_lora)});
    if(rtl) {
        execute(receiver_setup_ddl);
        insert(db_,"receiver_setup","id,hardware,gain_tenths_db,auto_gain",
            {int64_t(1),std::string("rtl_sdr"),int64_t(c.rtl_gain_tenths_db),int64_t(c.rtl_auto_gain)});
    }
    DiscoveryStatus initial_discovery;initial_discovery.enabled=c.discover_lora;
    insert(db_,"discovery_status",discovery_status_columns,discovery_status_values(initial_discovery));
    for(const auto& l:c.lanes)insert(db_,"lanes","label,channel,protocol,frequency,bw,sf,cr,enabled",{l.label,l.channel_name,l.protocol,int64_t(l.frequency_hz),int64_t(l.bandwidth_hz),int64_t(l.spreading_factor),int64_t(l.coding_rate),int64_t(l.enabled)});
    insert(db_,"survey_metrology",metrology_columns,{int64_t(4096),int64_t(4096),std::string("periodic Hann"),double(c.sample_rate)/4096,double(c.sample_rate)*1.5/4096,std::string(normalization),std::string("per FFT bin power >= fixed session threshold; union across selected bins"),std::string(position_association),std::string("host UTC anchor plus contiguous sample progress; application gaps explicit; upstream loss unknown"),c.antenna_description,c.receiver_description,c.survey_notes,std::string(c.compact_recording?compact_encoding:detailed_encoding)});
    schema_version_=c.compact_recording?6:5;readonly_=false;recorded_config_=c;
    if(c.compact_recording) {
        execute("DROP TABLE spectrum_tiles; DROP TABLE positions;");execute(compact_positions_ddl);execute(power_ddl);execute(compact_tile_ddl);execute("CREATE INDEX position_identity ON positions(utc,monotonic,source); PRAGMA user_version=6;");
    }
    std::string query="INSERT INTO spectrum_tiles VALUES(";for(int i=0;i<(c.compact_recording?19:36);++i){if(i)query+=',';query+='?';}query+=')';check(sqlite3_prepare_v2(db_,query.c_str(),-1,&tile_insert_,nullptr));
    } catch(...) { if(db_)sqlite3_close_v2(db_);db_=nullptr;schema_version_=0;throw; }
}
void SessionStore::open_readonly(const std::string& path) {
    ensure_local_path(path);if(db_)throw std::runtime_error("Session already open");
    try {
    check(sqlite3_open_v2(path.c_str(),&db_,SQLITE_OPEN_READONLY|SQLITE_OPEN_NOMUTEX|SQLITE_OPEN_NOFOLLOW,nullptr));configure(true);
    Statement a(db_,"PRAGMA application_id"),v(db_,"PRAGMA user_version");
    if(!a.row()||a.integer(0)!=application_id||!v.row())throw std::runtime_error("Unsupported OVMeshDRpp session format");
    const auto version=v.integer(0);
    if(version<1 || version>6)throw std::runtime_error("Unsupported OVMeshDRpp session format");
    validate_schema(db_,static_cast<int>(version));
    schema_version_=static_cast<int>(version);
    } catch(...) { if(db_)sqlite3_close_v2(db_);db_=nullptr;schema_version_=0;throw; }
}
void SessionStore::save_copy(const std::string& path) const {
    if(!db_||!readonly_)throw std::runtime_error("Open the source session read-only before making a copy");
    validate_local_file_path(path);
    // Validate the source before reserving the destination. ReadSnapshot pins
    // all copied pages to one committed revision even if capture is advancing.
    ReadSnapshot snapshot(db_);(void)read();
    PrivateFile reserved(path);reserved.close();
    sqlite3* destination=nullptr;
    sqlite3_backup* backup=nullptr;
    try {
        check(sqlite3_open_v2(path.c_str(),&destination,SQLITE_OPEN_READWRITE|SQLITE_OPEN_NOMUTEX|SQLITE_OPEN_NOFOLLOW,nullptr));
        sqlite3_busy_timeout(destination,1500);
        sqlite3_db_config(destination,SQLITE_DBCONFIG_ENABLE_LOAD_EXTENSION,0,nullptr);
        sqlite3_db_config(destination,SQLITE_DBCONFIG_DEFENSIVE,1,nullptr);
        check(sqlite3_exec(destination,"PRAGMA trusted_schema=OFF; PRAGMA synchronous=FULL; PRAGMA journal_mode=DELETE;",nullptr,nullptr,nullptr));
        backup=sqlite3_backup_init(destination,"main",db_,"main");
        if(!backup)throw std::runtime_error("Cannot initialize the session copy");
        int result;
        do { result=sqlite3_backup_step(backup,256); } while(result==SQLITE_OK);
        const int finished=sqlite3_backup_finish(backup);backup=nullptr;
        check(result);check(finished);
        if(result!=SQLITE_DONE)throw std::runtime_error("Session copy did not complete");
        // Emit a standalone database, not a copy that depends on WAL sidecars.
        check(sqlite3_exec(destination,"PRAGMA journal_mode=DELETE;",nullptr,nullptr,nullptr));
        check(sqlite3_close(destination));destination=nullptr;
        SessionStore verified;verified.open_readonly(path);(void)verified.read();
    } catch(...) {
        if(backup)sqlite3_backup_finish(backup);
        if(destination)sqlite3_close_v2(destination);
        // Never remove/rewrite an existing filename after releasing the initial
        // private-file handle. An incomplete new file remains inspectable.
        throw std::runtime_error("Could not finish the new session copy; an incomplete destination may remain. Choose a new local filename and retry");
    }
}
void SessionStore::append(const Reception& r) {
    validate_reception(r);
    begin_batch();
    const auto* a=r.decoded.authorized?&*r.decoded.authorized:nullptr;
    if(a&&(!r.crc_valid||!r.header_valid||r.decoded.status!=protocol::Status::decoded))throw std::runtime_error("Content rejected at persistence boundary");
    std::vector<Value> v={int64_t(r.id),r.utc_seconds,r.elapsed_seconds,int64_t(r.frequency_hz),int64_t(r.bandwidth_hz),int64_t(r.spreading_factor),int64_t(r.coding_rate),r.duration_seconds,r.snr_db,r.frequency_error_hz,int64_t(r.header_valid),int64_t(r.crc_valid),r.lane_label,int64_t(r.decoded.status),canonical_classification(r.decoded.classification),std::string("not authenticated")};
    if(a) {
        const auto& c=a->content;
        std::vector<Value> content={a->profile_id,int64_t(a->from),int64_t(a->to),int64_t(a->packet_id),int64_t(a->port),int64_t(a->hop_limit),int64_t(a->hop_start),int64_t(a->channel_hash),int64_t(a->next_hop),int64_t(a->relay_node),int64_t(a->want_ack),int64_t(a->via_mqtt),int64_t(a->want_response),c.kind,c.text,c.node_id,c.long_name,c.short_name,number(c.latitude),number(c.longitude),number(c.altitude),number(c.voltage),number(c.temperature),number(c.humidity),number(c.battery_percent),number(c.channel_utilization),number(c.air_util_tx),integer(c.reported_time),integer(c.hardware_model),integer(c.role),integer(c.routing_error)};
        v.insert(v.end(),content.begin(),content.end());
    } else v.insert(v.end(),31,nullptr);
    if(r.receiver_position&&r.receiver_position->valid) {const auto& f=*r.receiver_position;v.insert(v.end(),{f.latitude,f.longitude,number(f.altitude_m),int64_t(f.manual),f.utc_seconds,f.monotonic_seconds,f.source,number(f.hdop),int64_t(f.satellites)});} else v.insert(v.end(),9,nullptr);
    if(r.decoded.evidence) {const auto& e=*r.decoded.evidence;v.insert(v.end(),{int64_t(e.port),int64_t(e.signature_present)});}
    else v.insert(v.end(),2,nullptr);
    if(a)v.insert(v.end(),{int64_t(a->request_id),int64_t(a->reply_id),int64_t(a->signature_present),a->content.routing_variant});
    else v.insert(v.end(),4,nullptr);
    execute("SAVEPOINT reception;");
    try {
        insert(db_,"receptions",reception_columns_for(3),v);
        if(a) {
            for(size_t i=0;i<a->content.route.size();++i)insert(db_,"routes","reception,step,node",{int64_t(r.id),int64_t(i),int64_t(a->content.route[i])});
            const auto details=[&](const char* kind,const auto& values) {
                for(size_t i=0;i<values.size();++i)insert(db_,"route_details","reception,kind,step,value",{int64_t(r.id),std::string(kind),int64_t(i),int64_t(values[i])});
            };
            details("route_back",a->content.route_back);details("snr_towards",a->content.snr_towards);details("snr_back",a->content.snr_back);
        }
        execute("RELEASE reception;");
    }
    catch(...){sqlite3_exec(db_,"ROLLBACK TO reception; RELEASE reception;",nullptr,nullptr,nullptr);throw;}
}
int64_t SessionStore::ensure_fix(const PositionFix& f) {
    validate_fix(f);
    const auto same=[&](const PositionFix& a) {
        return a.latitude==f.latitude&&a.longitude==f.longitude&&a.altitude_m==f.altitude_m&&
            a.utc_seconds==f.utc_seconds&&a.monotonic_seconds==f.monotonic_seconds&&a.manual==f.manual&&
            a.source==f.source&&a.hdop==f.hdop&&a.satellites==f.satellites;
    };
    for(const auto& saved:recent_fixes_)if(same(saved.fix))return saved.id;
    Statement existing(db_,"SELECT rowid FROM positions WHERE utc=? AND monotonic=? AND source=? AND latitude=? AND longitude=? AND altitude IS ? AND manual=? AND hdop IS ? AND satellites=? LIMIT 1");
    existing.values({f.utc_seconds,f.monotonic_seconds,f.source,f.latitude,f.longitude,number(f.altitude_m),int64_t(f.manual),number(f.hdop),int64_t(f.satellites)});
    int64_t id=0;
    if(existing.row())id=existing.integer(0);
    else {
        insert(db_,"positions","utc,latitude,longitude,altitude,manual,source,hdop,satellites,monotonic",{f.utc_seconds,f.latitude,f.longitude,number(f.altitude_m),int64_t(f.manual),f.source,number(f.hdop),int64_t(f.satellites),f.monotonic_seconds});
        id=sqlite3_last_insert_rowid(db_);
    }recent_fixes_.push_back({f,id});
    if(recent_fixes_.size()>128)recent_fixes_.pop_front();
    return id;
}
void SessionStore::append(const PositionFix& f) {
    if(!f.valid)return;validate_fix(f);begin_batch();
    if(schema_version_>=6){(void)ensure_fix(f);return;}
    insert(db_,"positions","utc,latitude,longitude,altitude,manual,source,hdop,satellites,monotonic",{f.utc_seconds,f.latitude,f.longitude,number(f.altitude_m),int64_t(f.manual),f.source,number(f.hdop),int64_t(f.satellites),f.monotonic_seconds});
}
void SessionStore::append(const SurveyWindow& window) {
    validate_window(window);
    // Full-session bins and fine activity already preserve these measurements.
    // The old five-second per-bin copies are intentionally absent in compact mode.
    if(schema_version_>=6){if(readonly_)throw std::runtime_error("Survey is read-only");return;}
    begin_batch();
    std::vector<Value> values={int64_t(window.id),window.utc_start_seconds,window.utc_end_seconds,window.elapsed_start_seconds,window.elapsed_end_seconds,std::string("window-end")};
    if(window.receiver_position&&window.receiver_position->valid) {
        const auto& f=*window.receiver_position;
        values.insert(values.end(),{f.latitude,f.longitude,number(f.altitude_m),int64_t(f.manual),f.utc_seconds,f.monotonic_seconds,f.source,number(f.hdop),int64_t(f.satellites)});
    } else values.insert(values.end(),9,nullptr);
    execute("SAVEPOINT survey_window;");
    try {
        insert(db_,"survey_windows","id,utc_start,utc_end,elapsed_start,elapsed_end,associated_at,receiver_latitude,receiver_longitude,receiver_altitude,receiver_manual,receiver_utc,receiver_monotonic,receiver_source,receiver_hdop,receiver_satellites",values);
        for(const auto& bin:window.frequencies)
            insert(db_,"window_bins","window_id,center,width,mean,peak,observed,active",{int64_t(window.id),int64_t(bin.center_hz),int64_t(bin.width_hz),bin.mean_dbfs,bin.peak_dbfs,bin.observed_seconds,bin.active_seconds});
        execute("RELEASE survey_window;");
    } catch(...) {sqlite3_exec(db_,"ROLLBACK TO survey_window; RELEASE survey_window;",nullptr,nullptr,nullptr);throw;}
}
void SessionStore::save_power_block() {
    auto& b=power_block_;if(!b.id||!b.dirty)return;
    std::vector<float> mean;mean.reserve(b.power_sum.size());
    for(double sum:b.power_sum)mean.push_back(static_cast<float>(10*std::log10(sum/double(b.frames))));
    std::vector<Value> values{int64_t(b.id),int64_t(b.first_sample),int64_t(b.end_sample),b.utc_start,b.utc_end,b.elapsed_start,b.elapsed_end,
        b.first_center,b.bin_width,int64_t(4096),int64_t(b.frames),int64_t(mean.size()),quantize(mean),quantize(b.peak)};
    std::string sql=std::string("INSERT INTO power_blocks(")+power_columns+") VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?) ON CONFLICT(id) DO UPDATE SET ";
    std::istringstream columns(power_columns);std::string column;std::getline(columns,column,',');bool comma=false;
    while(std::getline(columns,column,',')){if(comma)sql+=',';comma=true;sql+=column+"=excluded."+column;}
    Statement statement(db_,sql);statement.values(values);statement.run();b.dirty=false;
}
void SessionStore::append(const SpectrumTile& tile) {
    validate_tile(tile,recorded_config_);
    if(tile.quality&SurveyPowerAggregated)throw std::runtime_error("Already aggregated power cannot be recorded as a fine input tile");
    if((schema_version_>=6&&tile.id<=previous_tile_id_) || tile.first_sample<previous_tile_end_ || tile.elapsed_start_seconds<previous_tile_elapsed_end_-1e-8)
        throw std::runtime_error("Overlapping or unordered spectrum tiles");
    std::vector<Value> values={int64_t(tile.id),int64_t(tile.first_sample),int64_t(tile.end_sample),tile.utc_start_seconds,tile.utc_end_seconds,tile.elapsed_start_seconds,tile.elapsed_end_seconds,tile.first_center_hz,tile.bin_width_hz,int64_t(tile.fft_size),int64_t(tile.frame_count),int64_t(tile.mean_dbfs.size()),double(tile.background_dbfs),int64_t(tile.clipped_samples),int64_t(tile.quality)};
    begin_batch();
    if(schema_version_>=6) {
        auto& b=power_block_;
        if(!b.id || !b.frames || b.end_sample!=tile.first_sample || std::abs(b.elapsed_end-tile.elapsed_start_seconds)>1e-8 ||
           b.first_center!=tile.first_center_hz || b.bin_width!=tile.bin_width_hz || b.peak.size()!=tile.mean_dbfs.size() ||
           tile.elapsed_end_seconds-b.elapsed_start>1.0+1e-8) {
            save_power_block();const auto next=b.id+1;b=PowerBlock{};b.id=next;
            b.first_sample=tile.first_sample;b.utc_start=tile.utc_start_seconds;b.elapsed_start=tile.elapsed_start_seconds;
            b.first_center=tile.first_center_hz;b.bin_width=tile.bin_width_hz;
            b.power_sum.resize(tile.mean_dbfs.size());b.peak.assign(tile.mean_dbfs.size(),-300);
        }
        b.end_sample=tile.end_sample;b.utc_end=tile.utc_end_seconds;b.elapsed_end=tile.elapsed_end_seconds;b.frames+=tile.frame_count;
        for(size_t i=0;i<tile.mean_dbfs.size();++i){b.power_sum[i]+=std::pow(10.,tile.mean_dbfs[i]/10.)*tile.frame_count;b.peak[i]=std::max(b.peak[i],tile.peak_dbfs[i]);}
        b.dirty=true;
        // Updating the same bounded row keeps even an in-transaction analysis
        // self-consistent. Checkpoints never reset or shorten the aggregate.
        save_power_block();values.push_back(encode_activity(tile.activity));values.push_back(int64_t(b.id));
        for(const auto* fix:{&tile.receiver_start,&tile.receiver_end})
            if(*fix&&(*fix)->valid)values.push_back(ensure_fix(**fix));else values.push_back(nullptr);
    } else {
        values.push_back(quantize(tile.mean_dbfs));values.push_back(quantize(tile.peak_dbfs));values.push_back(encode_activity(tile.activity));
        fix_values(values,tile.receiver_start);fix_values(values,tile.receiver_end);
    }
    Statement statement(tile_insert_);statement.values(values);statement.run();previous_tile_id_=tile.id;previous_tile_end_=tile.end_sample;previous_tile_elapsed_end_=tile.elapsed_end_seconds;
}
void SessionStore::append(const SpectrumEvent& event) {
    validate_event(event,recorded_config_);std::vector<Value> values={int64_t(event.id),int64_t(event.first_sample),int64_t(event.end_sample),event.utc_start_seconds,event.utc_end_seconds,event.elapsed_start_seconds,event.elapsed_end_seconds,event.lower_hz,event.upper_hz,event.active_seconds,double(event.mean_dbfs),double(event.peak_dbfs),int64_t(event.quality)};
    fix_values(values,event.receiver_start);fix_values(values,event.receiver_end);begin_batch();insert(db_,"spectrum_events",event_columns,values);
}
void SessionStore::append(const CoverageGap& gap) {
    validate_gap(gap);begin_batch();
    if(schema_version_>=6){save_power_block();const auto id=power_block_.id;power_block_=PowerBlock{};power_block_.id=id;}
    insert(db_,"coverage_gaps",gap_columns,{int64_t(gap.id),int64_t(gap.missing_samples),gap.utc_start_seconds,gap.utc_end_seconds,gap.elapsed_start_seconds,gap.elapsed_end_seconds,gap.reason});
}
void SessionStore::append(const WaveformObservation& w) {
    validate_waveform(w,recorded_config_);
    begin_batch();
    Statement previous(db_,"SELECT bandwidth_hz,spreading_factor FROM waveform_observations WHERE id=?");previous.bind(1,int64_t(w.id));
    if(previous.row() && (unsigned_value(previous,0)!=w.bandwidth_hz || unsigned_value(previous,1)!=w.spreading_factor))
        throw std::runtime_error("Waveform update changed the inferred profile for an existing identity");
    std::vector<Value> values{int64_t(w.id),w.center_hz,int64_t(w.bandwidth_hz),int64_t(w.spreading_factor),
        w.first_observed_elapsed,w.delimiter_elapsed,w.delimiter_utc,w.up_match,w.down_match,int64_t(w.contributing_subbands),
        int64_t(w.complete_in_requested_range),int64_t(w.association_ambiguous)};
    fix_values(values,w.receiver_position);
    std::string sql=std::string("INSERT INTO waveform_observations(")+waveform_columns+") VALUES(";
    for(size_t i=0;i<values.size();++i){if(i)sql+=',';sql+='?';}
    sql+=") ON CONFLICT(id) DO UPDATE SET ";
    std::istringstream columns(waveform_columns);std::string column;std::getline(columns,column,',');
    bool comma=false;while(std::getline(columns,column,',')){if(comma)sql+=',';comma=true;sql+=column+"=excluded."+column;}
    Statement insert_or_update(db_,sql);insert_or_update.values(values);insert_or_update.run();
}
void SessionStore::append(const DiscoveryGap& gap) {
    validate_discovery_gap(gap);
    if(!recorded_config_.discover_lora)throw std::runtime_error("Discovery is disabled for this recording");
    begin_batch();insert(db_,"discovery_gaps",discovery_gap_columns,
        {int64_t(gap.id),int64_t(gap.first_input_sample),int64_t(gap.end_input_sample),int64_t(gap.subband_index),gap.reason});
}
void SessionStore::update(const Snapshot& s,bool final) {
    nonnegative(s.elapsed_seconds,"session elapsed time");nonnegative(s.input_seconds,"session input time");nonnegative(s.measurement_seconds,"session measurement time");
    validate_discovery_status(s.discovery,recorded_config_.discover_lora);
    begin_batch();
    try {
        Statement q(db_,"UPDATE session SET complete=?,elapsed=?,input_seconds=?,measurement_seconds=?,delivered=?,dropped=?,receptions=?,authorized=?");
        q.values({int64_t(final&&!s.incomplete),s.elapsed_seconds,s.input_seconds,s.measurement_seconds,int64_t(s.delivered_samples),int64_t(s.dropped_samples),int64_t(s.total_receptions),int64_t(s.authorized_messages)});q.run();
        Statement f(db_,"INSERT INTO frequencies VALUES(?,?,?,?,?,?) ON CONFLICT(center) DO UPDATE SET width=excluded.width,mean=excluded.mean,peak=excluded.peak,observed=excluded.observed,active=excluded.active");
        for(const auto& b:s.frequencies){validate_bin(b);sqlite3_reset(f.s);f.values({int64_t(b.center_hz),int64_t(b.width_hz),b.mean_dbfs,b.peak_dbfs,b.observed_seconds,b.active_seconds});f.run();}
        execute("DELETE FROM lane_health;");
        for(const auto& l:s.lane_health) {
            nonnegative(l.processed_seconds,"decoder observed time");
            insert(db_,"lane_health","label,frequency,processed,frames,decoded,crc_failures,resets,state",{l.label,int64_t(l.frequency_hz),l.processed_seconds,int64_t(l.frames),int64_t(l.decoded),int64_t(l.crc_failures),int64_t(l.resets),l.state});
        }
        execute("DELETE FROM discovery_status; DELETE FROM discovery_bands;");
        insert(db_,"discovery_status",discovery_status_columns,discovery_status_values(s.discovery));
        for(const auto& band:s.discovery.bands)
            insert(db_,"discovery_bands",discovery_band_columns,{int64_t(band.subband_index),band.center_hz,
                int64_t(band.processed_samples),int64_t(band.abandoned_samples),int64_t(band.source_gap_input_samples),
                int64_t(band.candidate_limit_hits),int64_t(band.track_limit_hits)});
        insert(db_,"coverage","utc,elapsed,input_seconds,measurement_seconds,dropped,state",{0.0,s.elapsed_seconds,s.input_seconds,s.measurement_seconds,int64_t(s.dropped_samples),s.state});
        execute("COMMIT;");pending_=false;
        if(final)sqlite3_wal_checkpoint_v2(db_,nullptr,SQLITE_CHECKPOINT_TRUNCATE,nullptr,nullptr);
    } catch(...){sqlite3_exec(db_,"ROLLBACK;",nullptr,nullptr,nullptr);pending_=false;throw;}
}
Snapshot SessionStore::read() const {
    ReadSnapshot snapshot(db_);
    Snapshot out;out.historical=true;out.state="Saved survey";
    Statement s(db_,"SELECT * FROM session LIMIT 2");
    if(!s.row())throw std::runtime_error("Session has no metadata");
    auto& c=out.config;
    out.session_id=s.text(0);c.session_title=s.text(1);
    const auto source=unsigned_value(s,3,schema_version_>=5?2:1);
    c.synthetic=source==1;c.hardware_receiver=source==2?HardwareReceiver::RtlSdr:HardwareReceiver::HackRf;
    if(source==2)read_receiver_setup(db_,c);
    c.center_hz=unsigned_value(s,4,6000000000ULL);
    c.sample_rate=static_cast<uint32_t>(unsigned_value(s,5,20000000));
    c.survey_span_hz=static_cast<uint32_t>(unsigned_value(s,6,20000000));
    c.lna_gain=static_cast<unsigned>(unsigned_value(s,7,40));
    c.vga_gain=static_cast<unsigned>(unsigned_value(s,8,62));c.amplifier=boolean(s,9);
    c.activity_threshold_dbfs=static_cast<float>(s.real(10));out.incomplete=!boolean(s,11);
    out.elapsed_seconds=s.real(12);out.input_seconds=s.real(13);out.measurement_seconds=s.real(14);
    out.delivered_samples=unsigned_value(s,15,INT64_MAX);out.dropped_samples=unsigned_value(s,16,INT64_MAX);
    out.total_receptions=unsigned_value(s,17,INT64_MAX);out.authorized_messages=unsigned_value(s,18,INT64_MAX);
    c.tuning_offset_hz=schema_version_>=2?s.integer(19):0;
    c.discover_lora=schema_version_>=5?boolean(s,20):false;
    c.compact_recording=schema_version_>=6;
    (void)tuned_center_hz(c);
    if(s.row())throw std::runtime_error("Multiple session metadata records");
    if(c.center_hz==0 || c.sample_rate==0 || c.survey_span_hz==0 || c.survey_span_hz>c.sample_rate ||
       c.lna_gain%8!=0 || c.vga_gain%2!=0 || out.session_id.empty() || out.session_id.size()>160 || c.session_title.size()>160)
       throw std::runtime_error("Invalid saved receiver configuration");
    if(source==2&&((c.sample_rate!=1000000&&c.sample_rate!=2000000)||c.amplifier||
        c.survey_span_hz<500000||c.survey_span_hz>c.sample_rate*4/5||
        (c.discover_lora&&(c.sample_rate!=2000000||c.survey_span_hz>1500000))))
        throw std::runtime_error("Invalid saved RTL-SDR receiver configuration");
    nonnegative(out.elapsed_seconds,"saved elapsed time");nonnegative(out.input_seconds,"saved input time");nonnegative(out.measurement_seconds,"saved measurement time");
    c.lanes.clear();
    Statement lanes(db_,"SELECT * FROM lanes LIMIT 9");
    while(lanes.row()) {
        if(c.lanes.size()>=8)throw std::runtime_error("Too many saved decode profiles");
        LaneConfig l;l.label=lanes.text(0);l.channel_name=lanes.text(1);l.protocol=lanes.text(2);
        l.frequency_hz=unsigned_value(lanes,3,6000000000ULL);l.bandwidth_hz=static_cast<uint32_t>(unsigned_value(lanes,4,2000000));
        l.spreading_factor=static_cast<uint8_t>(unsigned_value(lanes,5,12));l.coding_rate=static_cast<uint8_t>(unsigned_value(lanes,6,8));l.enabled=boolean(lanes,7);
        if(l.label.size()>160 || l.channel_name.size()>160 || l.frequency_hz==0 || l.bandwidth_hz==0 || l.spreading_factor<5 || l.coding_rate<5 || (l.protocol!="Meshtastic" && l.protocol!="MeshCore"))
            throw std::runtime_error("Invalid saved decoder profile");
        c.lanes.push_back(l);
    }
    Statement freq(db_,"SELECT * FROM frequencies ORDER BY center LIMIT 8193");
    while(freq.row()) {
        if(out.frequencies.size()>=8192)throw std::runtime_error("Too many saved frequency bins");
        FrequencySummary bin{unsigned_value(freq,0,6000000000ULL),static_cast<uint32_t>(unsigned_value(freq,1)),freq.real(2),freq.real(3),freq.real(4),freq.real(5)};
        validate_bin(bin);out.frequencies.push_back(bin);
    }
    Statement packets(db_,std::string("SELECT ")+reception_columns_for(schema_version_)+" FROM receptions ORDER BY id DESC LIMIT 512");
    while(packets.row()) {auto r=reception_from(packets,schema_version_);read_route(db_,r,schema_version_);out.receptions.push_back(std::move(r));}
    // The UI is bounded, but saved counts and exports cover the whole database.
    Statement counts(db_,"SELECT count(*),count(profile) FROM receptions");
    if(counts.row()){out.total_receptions=unsigned_value(counts,0,INT64_MAX);out.authorized_messages=unsigned_value(counts,1,INT64_MAX);}
    Statement positions(db_,"SELECT * FROM (SELECT rowid,* FROM positions ORDER BY rowid DESC LIMIT 12000) ORDER BY rowid");
    while(positions.row()) {
        PositionFix f;f.utc_seconds=positions.real(1);f.latitude=positions.real(2);f.longitude=positions.real(3);
        if(!positions.null(4))f.altitude_m=positions.real(4);f.manual=boolean(positions,5);f.source=positions.text(6);
        if(!positions.null(7))f.hdop=positions.real(7);f.satellites=static_cast<unsigned>(unsigned_value(positions,8,1000));
        f.monotonic_seconds=positions.real(9);f.valid=true;validate_fix(f);out.track.push_back(f);
    }
    Statement health(db_,"SELECT * FROM lane_health LIMIT 9");
    while(health.row()) {
        if(out.lane_health.size()>=8)throw std::runtime_error("Too many saved lane-health records");
        LaneHealth l;l.label=health.text(0);l.frequency_hz=unsigned_value(health,1,6000000000ULL);l.processed_seconds=health.real(2);
        l.frames=unsigned_value(health,3,INT64_MAX);l.decoded=unsigned_value(health,4,INT64_MAX);l.crc_failures=unsigned_value(health,5,INT64_MAX);l.resets=unsigned_value(health,6,INT64_MAX);l.state=health.text(7);
        nonnegative(l.processed_seconds,"saved decoder observation time");out.lane_health.push_back(l);
    }
    if(!out.track.empty())out.gps_status="Saved receiver positions (historical)";
    if(schema_version_>=4) {
        if(c.sample_rate<1000000)throw std::runtime_error("Unsupported spectrum sample rate");
        Statement m(db_,"SELECT * FROM survey_metrology LIMIT 2");
        if(!m.row() || unsigned_value(m,0,4096)!=4096 || unsigned_value(m,1,4096)!=4096 || m.text(2)!="periodic Hann" ||
           std::abs(m.real(3)-double(c.sample_rate)/4096)>1e-7 || std::abs(m.real(4)-double(c.sample_rate)*1.5/4096)>1e-7 ||
           m.text(5)!=normalization || m.text(6)!="per FFT bin power >= fixed session threshold; union across selected bins" || m.text(7)!=position_association ||
           m.text(8)!="host UTC anchor plus contiguous sample progress; application gaps explicit; upstream loss unknown" ||
           m.text(12)!=(schema_version_>=6?compact_encoding:detailed_encoding))
            throw std::runtime_error("Unsupported or inconsistent survey metrology");
        out.spectrum_fft_size=4096;out.spectrum_bin_width_hz=m.real(3);out.spectrum_enbw_hz=m.real(4);
        c.antenna_description=m.text(9);c.receiver_description=m.text(10);c.survey_notes=m.text(11);
        if(c.antenna_description.size()>512||c.receiver_description.size()>512||c.survey_notes.size()>2048||m.row())throw std::runtime_error("Invalid survey metrology metadata");
        Statement count(db_,"SELECT count(*),coalesce(sum(clipped),0) FROM spectrum_tiles");if(count.row()){out.spectrum_tiles=unsigned_value(count,0,INT64_MAX);out.clipped_samples=unsigned_value(count,1,INT64_MAX);}
        Statement events_count(db_,"SELECT count(*) FROM spectrum_events");if(events_count.row())out.spectrum_events=unsigned_value(events_count,0,INT64_MAX);
        Statement latest(db_,tiles_sql(schema_version_,"ORDER BY t.id DESC LIMIT 1"));if(latest.row())out.background_dbfs=tile_from(latest,c,schema_version_).background_dbfs;
        Statement events(db_,"SELECT * FROM spectrum_events ORDER BY id DESC LIMIT 200");while(events.row())out.recent_spectrum_events.push_back(event_from(events,c));
    }
    if(schema_version_>=5) {
        Statement status(db_,"SELECT * FROM discovery_status LIMIT 2");
        if(!status.row())throw std::runtime_error("Missing discovery status");
        out.discovery=discovery_status_from(status);
        if(status.row())throw std::runtime_error("Multiple discovery status records");
        Statement bands(db_,"SELECT * FROM discovery_bands ORDER BY subband_index LIMIT 33");
        while(bands.row()) {
            if(out.discovery.bands.size()>=discovery_band_limit)throw std::runtime_error("Too many discovery subbands");
            out.discovery.bands.push_back(discovery_band_from(bands));
        }
        validate_discovery_status(out.discovery,c.discover_lora);
        Statement waveforms(db_,"SELECT * FROM waveform_observations ORDER BY id DESC LIMIT 256");
        while(waveforms.row())out.waveforms.push_back(waveform_from(waveforms,c));
        // Gap rows are exported from durable history. Snapshot is deliberately
        // bounded and exposes cumulative status, not an unbounded interval list.
    }
    return out;
}

SurveyAnalysis SessionStore::analyze(const SurveyQuery& query) const {
    ReadSnapshot snapshot(db_);
    SurveyAnalysis result;const auto config=read().config;if(schema_version_<4)return result;result.detailed_available=true;
    for(double value:{query.lower_hz,query.upper_hz,query.elapsed_start,query.elapsed_end})nonnegative(value,"survey query bound");
    if((query.upper_hz>0 && query.upper_hz<=query.lower_hz)||(query.elapsed_end>0 && query.elapsed_end<=query.elapsed_start)||
       !std::isfinite(query.time_bucket_seconds)||query.time_bucket_seconds<0.001||query.time_bucket_seconds>1e10 ||
       query.max_observations>2000 || query.max_events>2000)throw std::runtime_error("Invalid survey query interval or display bound");
    if(query.geographic_filter){coordinates(query.south,query.west);coordinates(query.north,query.east);if(query.south>query.north||query.west>query.east)throw std::runtime_error("Invalid geographic rectangle");}
    const double lower=query.lower_hz>0?query.lower_hz:double(config.center_hz)-double(config.survey_span_hz)/2;
    const double upper=query.upper_hz>0?query.upper_hz:double(config.center_hz)+double(config.survey_span_hz)/2;
    // Resolve the entire recorded interval before choosing display buckets. A
    // single read transaction keeps a growing live file's extent and rows in sync.
    double recorded_start=1e10,recorded_end=0;
    const auto include_extent=[&](double from,double to) {
        nonnegative(from,"recorded elapsed start");nonnegative(to,"recorded elapsed end");
        if(to<=from || to>1e10)throw std::runtime_error("Invalid recorded elapsed extent");
        recorded_start=std::min(recorded_start,from);recorded_end=std::max(recorded_end,to);
    };
    for(const char* sql:{"SELECT elapsed_start,elapsed_end FROM spectrum_tiles ORDER BY id LIMIT 1",
                        "SELECT elapsed_start,elapsed_end FROM spectrum_tiles ORDER BY id DESC LIMIT 1",
                        "SELECT min(elapsed_start),max(elapsed_end) FROM coverage_gaps"}) {
        Statement extent(db_,sql);
        if(extent.row() && !extent.null(0))include_extent(extent.real(0),extent.real(1));
    }
    result.effective_time_bucket_seconds=query.time_bucket_seconds;
    if(recorded_end>0) {
        result.resolved_elapsed_start=std::clamp(query.elapsed_start,recorded_start,recorded_end);
        result.resolved_elapsed_end=std::clamp(query.elapsed_end>0?query.elapsed_end:recorded_end,
                                              result.resolved_elapsed_start,recorded_end);
        const double span=result.resolved_elapsed_end-result.resolved_elapsed_start;
        if(query.max_observations && span/query.time_bucket_seconds>double(query.max_observations)) {
            result.effective_time_bucket_seconds=std::nextafter(span/double(query.max_observations),
                                                                 std::numeric_limits<double>::infinity());
            result.observations_coarsened=true;
        }
    }
    const double end=result.resolved_elapsed_end;
    const double event_query_end=query.elapsed_end>0?query.elapsed_end:1e10;
    const auto in_region=[&](const std::optional<PositionFix>& position){return !query.geographic_filter || (position&&position->valid&&position->latitude>=query.south&&position->latitude<=query.north&&position->longitude>=query.west&&position->longitude<=query.east);};
    struct Accumulator {SurveyObservation value;double power=0,background=0,peak=0;};
    std::map<uint64_t,Accumulator> buckets;
    std::vector<double> powers,peaks;
    std::vector<uint8_t> center_masks;
    uint64_t previous_sample=0;double previous_elapsed=0,grid_first=0;size_t grid_count=0,first=0,last=0;
    Statement tiles(db_,tiles_sql(schema_version_));
    BurstGrouper bursts(double(config.center_hz));
    const auto on_burst=[&](SpectrumBurst burst) {
        ++result.burst_count;
        if(result.bursts.size()<query.max_events)result.bursts.push_back(std::move(burst));
    };
    while(tiles.row()) {
        const auto tile=tile_from(tiles,config,schema_version_);const size_t count=tile.mean_dbfs.size();const double width=tile.bin_width_hz;
        if(tile.first_sample<previous_sample || tile.elapsed_start_seconds<previous_elapsed-1e-8)throw std::runtime_error("Overlapping or unordered saved spectrum tiles");
        previous_sample=tile.end_sample;previous_elapsed=tile.elapsed_end_seconds;
        if(!grid_count) {
            grid_count=count;grid_first=tile.first_center_hz;
            while(first<count && tile.first_center_hz+(double(first)+0.5)*width<=lower)++first;
            last=first;while(last<count && tile.first_center_hz+(double(last)-0.5)*width<upper)++last;
            if(last>first){result.covered_lower_hz=tile.first_center_hz+(double(first)-0.5)*width;result.covered_upper_hz=tile.first_center_hz+(double(last)-0.5)*width;result.bin_width_hz=width;
                center_masks.resize((count+7)/8);
                for(size_t b=first;b<last;++b){SurveyBin bin;bin.center_hz=tile.first_center_hz+double(b)*width;bin.width_hz=width;result.bins.push_back(bin);
                    if(std::abs(bin.center_hz-double(config.center_hz))<=2*width+1e-5) {
                        center_masks[b/8]|=static_cast<uint8_t>(1u<<(b%8));
                        if(!result.center_guard_bin_count)result.center_guard_lower_hz=bin.center_hz-width/2;
                        result.center_guard_upper_hz=bin.center_hz+width/2;++result.center_guard_bin_count;
                    }
                }
                result.outside_center_bin_count=last-first-result.center_guard_bin_count;
                powers.resize(last-first);peaks.resize(last-first);}
        } else if(count!=grid_count || std::abs(grid_first-tile.first_center_hz)>1e-5)throw std::runtime_error("Saved spectrum grid changes within session");
        const double from=std::max(result.resolved_elapsed_start,tile.elapsed_start_seconds),to=std::min(end,tile.elapsed_end_seconds);
        if(last==first||to<=from||!in_region(tile.receiver_end))continue;
        bursts.consume(tile,on_burst,first,last,from,to);
        ++result.tile_count;const double duration=to-from;result.observed_seconds+=duration;
        if(!tile.receiver_start)result.missing_start_position_seconds+=duration;
        if(!tile.receiver_end)result.missing_end_position_seconds+=duration;
        uint32_t quality=tile.quality;if(!tile.receiver_end)quality|=SurveyPositionMissing;
        if(from>tile.elapsed_start_seconds || to<tile.elapsed_end_seconds)quality|=SurveyBoundary;
        if((tile.quality&SurveyPowerAggregated) && (query.geographic_filter ||
           result.resolved_elapsed_start>tile.power_elapsed_start || end<tile.power_elapsed_end))quality|=SurveyBoundary;
        result.quality|=quality;
        double integrated_mean=0,integrated_peak=0;
        for(size_t b=first;b<last;++b){const auto index=b-first;const double mean=std::pow(10.0,tile.mean_dbfs[b]/10.0),peak=std::pow(10.0,tile.peak_dbfs[b]/10.0);
            integrated_mean+=mean;integrated_peak+=peak;powers[index]+=mean*duration;peaks[index]=std::max(peaks[index],peak);result.bins[index].observed_seconds+=duration;}
        const size_t stride=(count+7)/8;const double frame_seconds=double(tile.fft_size)/config.sample_rate;
        for(size_t frame=0;frame<tile.frame_count;++frame) {
            const double frame_from=std::max(from,tile.elapsed_start_seconds+double(frame)*frame_seconds),frame_to=std::min(to,tile.elapsed_start_seconds+double(frame+1)*frame_seconds);
            if(frame_to<=frame_from)continue;bool busy=false,center_busy=false,outside_busy=false;
            for(size_t byte=first/8;byte<=(last-1)/8;++byte) {
                unsigned mask=tile.activity[frame*stride+byte];
                if(byte==first/8)mask&=0xffu<<(first%8);
                if(byte==(last-1)/8 && last%8)mask&=(1u<<(last%8))-1;
                if(mask)busy=true;
                if(mask&center_masks[byte])center_busy=true;
                if(mask&~unsigned(center_masks[byte]))outside_busy=true;
                while(mask){const unsigned bit=static_cast<unsigned>(std::countr_zero(mask));result.bins[byte*8+bit-first].active_seconds+=frame_to-frame_from;mask&=mask-1;}
            }
            if(busy)result.busy_seconds+=frame_to-frame_from;
            if(center_busy)result.center_busy_seconds+=frame_to-frame_from;
            if(outside_busy)result.outside_center_busy_seconds+=frame_to-frame_from;
            if(!query.max_observations){result.observations_truncated=true;continue;}
            for(double time=frame_from;time<frame_to;) {
                const double width_seconds=result.effective_time_bucket_seconds;
                const auto edge=[&](uint64_t index){return std::fma(double(index),width_seconds,result.resolved_elapsed_start);};
                auto bucket=static_cast<uint64_t>(std::floor((time-result.resolved_elapsed_start)/width_seconds));
                bucket=std::min(bucket,static_cast<uint64_t>(query.max_observations-1));
                // Correct representational rounding on either side of an edge.
                while(bucket && edge(bucket)>time)--bucket;
                while(bucket+1<query.max_observations && edge(bucket+1)<=time)++bucket;
                const double bucket_end=bucket+1==query.max_observations?end:std::min(end,edge(bucket+1));
                const double boundary=std::min(frame_to,bucket_end);
                if(boundary<=time)throw std::runtime_error("Unrepresentable survey time bucket");
                auto found=buckets.find(bucket);
                if(found==buckets.end()) {Accumulator value;value.value.elapsed_start=time;value.value.receiver_position=tile.receiver_end;found=buckets.emplace(bucket,std::move(value)).first;}
                {auto& a=found->second;const double seconds=boundary-time;a.value.elapsed_end=boundary;a.value.observed_seconds+=seconds;if(busy)a.value.busy_seconds+=seconds;
                    if(center_busy)a.value.center_busy_seconds+=seconds;
                    if(outside_busy)a.value.outside_center_busy_seconds+=seconds;
                    if(a.value.receiver_position && tile.receiver_end && (a.value.receiver_position->latitude!=tile.receiver_end->latitude || a.value.receiver_position->longitude!=tile.receiver_end->longitude))a.value.quality|=SurveyMerged;
                    a.value.quality|=quality;
                    if(edge(bucket)>tile.power_elapsed_start || bucket_end<tile.power_elapsed_end)a.value.quality|=SurveyBoundary;
                    a.power+=integrated_mean*seconds;a.background+=std::pow(10.0,tile.background_dbfs/10.0)*double(last-first)*seconds;a.peak=std::max(a.peak,integrated_peak);a.value.receiver_position=tile.receiver_end;}
                time=boundary;
            }
        }
    }
    for(size_t i=0;i<result.bins.size();++i)if(result.bins[i].observed_seconds>0){result.bins[i].mean_dbfs=10*std::log10(powers[i]/result.bins[i].observed_seconds);result.bins[i].peak_dbfs=10*std::log10(peaks[i]);}
    for(auto& [id,a]:buckets){(void)id;if(a.value.observed_seconds>0){a.value.mean_dbfs=10*std::log10(a.power/a.value.observed_seconds);a.value.peak_dbfs=10*std::log10(a.peak);a.value.background_dbfs=10*std::log10(a.background/a.value.observed_seconds);}result.observations.push_back(std::move(a.value));}
    Statement events(db_,"SELECT * FROM spectrum_events ORDER BY id");while(events.row()) {
        auto event=event_from(events,config);
        const double event_lower=result.bins.empty()?lower:result.covered_lower_hz,event_upper=result.bins.empty()?upper:result.covered_upper_hz;
        if(event.upper_hz<=event_lower||event.lower_hz>=event_upper||event.elapsed_end_seconds<=query.elapsed_start||event.elapsed_start_seconds>=event_query_end||!in_region(event.receiver_end))continue;
        ++result.event_count;if(result.events.size()<query.max_events)result.events.push_back(std::move(event));else result.events_truncated=true;
    }
    bursts.finish(on_burst);
    if(schema_version_>=5) {
        Statement waveforms(db_,"SELECT * FROM waveform_observations ORDER BY id DESC");
        while(waveforms.row()) {
            auto w=waveform_from(waveforms,config);
            // Inferred RF footprint intersects the requested frequency range;
            // delimiter time selects a half-open interval. It is not airtime.
            if(w.center_hz+w.bandwidth_hz/2.<=lower || w.center_hz-w.bandwidth_hz/2.>=upper ||
               w.delimiter_elapsed<query.elapsed_start || w.delimiter_elapsed>=event_query_end || !in_region(w.receiver_position))continue;
            ++result.waveform_count;
            if(result.waveforms.size()<query.max_events)result.waveforms.push_back(std::move(w));
            else result.waveforms_truncated=true;
        }
    }
    Statement gaps(db_,"SELECT * FROM coverage_gaps ORDER BY id");while(gaps.row()) {
        auto gap=gap_from(gaps);if(gap.elapsed_end_seconds<=query.elapsed_start||gap.elapsed_start_seconds>=event_query_end)continue;
        // Gaps have no position; under a rectangle their applicability is unknown.
        if(query.geographic_filter)continue;
        if(result.gaps.size()>=100000)throw std::runtime_error("Too many coverage gaps for analysis");result.gaps.push_back(std::move(gap));
    }
    return result;
}

std::string csv_text(const std::string& input) {
    std::string value=input;
    // Spreadsheet importers can trim whitespace before interpreting a formula.
    size_t first=0;while(first<value.size() && static_cast<unsigned char>(value[first])<=0x20)++first;
    if(first>=value.size())first=std::string::npos;
    if((first!=std::string::npos && (value[first]=='=' || value[first]=='+' || value[first]=='-' || value[first]=='@')) ||
       (!value.empty() && (value[0]=='\t' || value[0]=='\r' || value[0]=='\n')))
        value.insert(value.begin(),'\'');
    std::string out="\"";for(char c:value){if(c=='"')out+='"';out+=c;}return out+'"';
}

namespace {
const std::vector<std::string>& export_columns() {
    static const std::vector<std::string> columns=[] {
        std::vector<std::string> result;
        std::istringstream input("record_type,session_id,utc_seconds,elapsed_seconds,frequency_hz,bandwidth_hz,spreading_factor,coding_rate,duration_seconds,snr_db,frequency_error_hz,header_valid,crc_valid,decode_status,classification,authentication,observed_seconds,active_seconds,occupancy_fraction,mean_dbfs,peak_dbfs,input_seconds,measurement_seconds,delivered_samples,dropped_samples,total_receptions,authorized_records,incomplete,profile_id,content_kind,text,origin,destination,packet_id,port,hop_limit,hop_start,channel_hash,next_hop,relay_node,want_ack,via_mqtt,want_response,node_id,long_name,short_name,sender_latitude,sender_longitude,sender_altitude,voltage,temperature,humidity,battery_percent,channel_utilization,air_util_tx,reported_time,hardware_model,role,routing_error,route,receiver_latitude,receiver_longitude,receiver_altitude,receiver_utc_seconds,receiver_monotonic_seconds,receiver_manual,receiver_source,receiver_hdop,receiver_satellites,window_id,window_utc_start_seconds,window_utc_end_seconds,window_elapsed_start_seconds,window_elapsed_end_seconds,position_association,activity_threshold_dbfs,sample_rate,survey_span_hz,source,rf_level_unit,application_version,lna_gain_db,vga_gain_db,rf_amplifier_enabled,tuning_offset_hz,tuner_command_hz,evidence_port,evidence_signature_present,request_id,reply_id,signature_present,routing_variant,route_back,snr_towards_db_x4,snr_back_db_x4");
        std::string column;while(std::getline(input,column,','))result.push_back(column);
        std::istringstream spectrum("spectrum_id,first_sample,end_sample,first_center_hz,bin_width_hz,fft_size,hop_size,frame_count,bin_count,background_dbfs,clipped_samples,quality_flags,mean_centidb_le_hex,peak_centidb_le_hex,activity_frame_start,activity_frame_count,activity_mask_hex,lower_hz,upper_hz,missing_samples,gap_reason,window_function,enbw_hz,normalization,time_association,power_time_association,peak_interpretation,antenna_description,receiver_description,survey_notes");
        while(std::getline(spectrum,column,','))result.push_back(column);
        std::istringstream discovery("session_schema_version,waveform_id,inferred_bandwidth_hz,inferred_spreading_factor,evidence_first_elapsed_seconds,delimiter_elapsed_seconds,delimiter_utc_seconds,waveform_up_match_fraction,waveform_down_match_fraction,contributing_subbands,complete_in_requested_range,association_ambiguous,discovery_method,discovery_enabled,discovery_finished,discovery_failed,discovery_fault,discovery_accepted_input_samples,discovery_rejected_input_samples,discovery_channelized_input_samples,discovery_abandoned_input_samples,discovery_source_queue_drops,discovery_stream_resets,discovery_result_overflows,discovery_gap_overflows,discovery_observations,discovery_subband_index,discovery_output_sample_rate,discovery_processed_output_samples,discovery_abandoned_output_samples,discovery_source_gap_input_samples,discovery_candidate_limit_hits,discovery_track_limit_hits,discovery_gap_id,discovery_gap_first_input_sample,discovery_gap_end_input_sample,discovery_coverage_interpretation");
        while(std::getline(discovery,column,','))result.push_back(column);
        result.push_back("power_elapsed_start_seconds");result.push_back("power_elapsed_end_seconds");
        result.push_back("rtl_tuner_gain_db");result.push_back("rtl_auto_gain");
        return result;
    }();
    return columns;
}
struct CsvRecord {
    struct Cell {std::string value;bool numeric=false;};
    std::vector<Cell> fields=std::vector<Cell>(export_columns().size());
    void put(const char* name,std::string value,bool numeric=false) {
        const auto& columns=export_columns();const auto found=std::find(columns.begin(),columns.end(),name);
        if(found==columns.end())throw std::logic_error("Unknown CSV field");
        fields[static_cast<size_t>(found-columns.begin())]={std::move(value),numeric};
    }
    void text(const char* name,const std::string& value){put(name,value);}
    template<typename T> void number(const char* name,T value) {
        std::ostringstream out;out.imbue(std::locale::classic());out<<std::setprecision(17)<<value;put(name,out.str(),true);
    }
    void optional(const char* name,const std::optional<double>& value){if(value)number(name,*value);}
    void optional(const char* name,const std::optional<uint32_t>& value){if(value)number(name,*value);}
    void coordinate(const char* name,double value,unsigned decimals) {
        std::ostringstream out;out.imbue(std::locale::classic());out<<std::fixed<<std::setprecision(static_cast<int>(decimals))<<value;put(name,out.str(),true);
    }
    void position(const PositionFix& fix,const ExportOptions& options) {
        validate_fix(fix);coordinate("receiver_latitude",fix.latitude,options.coordinate_decimals);
        coordinate("receiver_longitude",fix.longitude,options.coordinate_decimals);optional("receiver_altitude",fix.altitude_m);
        number("receiver_utc_seconds",fix.utc_seconds);number("receiver_monotonic_seconds",fix.monotonic_seconds);
        number("receiver_manual",int(fix.manual));text("receiver_source",fix.source);optional("receiver_hdop",fix.hdop);number("receiver_satellites",fix.satellites);
    }
    void write(PrivateFile& output) const {
        std::string line;
        for(size_t i=0;i<fields.size();++i){if(i)line+=',';const auto& cell=fields[i];line+=cell.numeric?cell.value:csv_text(cell.value);}
        output.write(line+'\n');
    }
    std::string json() const {
        std::string out="{";bool comma=false;
        for(size_t i=0;i<fields.size();++i) {
            const auto& cell=fields[i];if(cell.value.empty())continue;
            if(comma)out+=',';comma=true;out+=json_string(export_columns()[i])+':';
            out+=cell.numeric?cell.value:json_string(cell.value);
        }
        return out+'}';
    }
};
// Shared evidence serialization prevents the geographic export from losing
// unlocated observations, subband loss, rejected work, or overflow diagnostics.
template<typename Emit>
void export_discovery(sqlite3* db,const Snapshot& summary,const ExportOptions& opt,Emit emit) {
    const auto& d=summary.discovery;
    CsvRecord status;status.text("record_type","discovery_status");status.text("session_id",summary.session_id);
    status.text("discovery_method",d.method);status.number("discovery_enabled",int(d.enabled));status.number("discovery_finished",int(d.finished));
    status.number("discovery_failed",int(d.failed));status.text("discovery_fault",d.fault);
    status.number("discovery_accepted_input_samples",d.accepted_input_samples);status.number("discovery_rejected_input_samples",d.rejected_input_samples);
    status.number("discovery_channelized_input_samples",d.channelized_input_samples);status.number("discovery_abandoned_input_samples",d.abandoned_input_samples);
    status.number("discovery_source_queue_drops",d.source_queue_drops);status.number("discovery_stream_resets",d.stream_resets);
    status.number("discovery_result_overflows",d.result_overflows);status.number("discovery_gap_overflows",d.gap_overflows);status.number("discovery_observations",d.observations);
    status.number("sample_rate",summary.config.sample_rate);
    status.text("discovery_coverage_interpretation","discovery is separate from spectrum coverage; accepted is not processed; rejected, abandoned and overflow are distinct; counters are not additive airtime");
    emit(status,std::optional<PositionFix>{});
    for(const auto& band:d.bands) {
        CsvRecord row;row.text("record_type","discovery_band_coverage");row.text("session_id",summary.session_id);row.text("discovery_method",d.method);
        row.number("discovery_subband_index",band.subband_index);row.number("frequency_hz",band.center_hz);row.number("discovery_output_sample_rate",2000000);
        row.number("discovery_processed_output_samples",band.processed_samples);row.number("discovery_abandoned_output_samples",band.abandoned_samples);
        row.number("discovery_source_gap_input_samples",band.source_gap_input_samples);row.number("discovery_candidate_limit_hits",band.candidate_limit_hits);row.number("discovery_track_limit_hits",band.track_limit_hits);
        row.number("sample_rate",summary.config.sample_rate);
        row.text("discovery_coverage_interpretation","processed/abandoned use 2 MS/s subband output samples; source gaps use receiver input samples; overlapping subbands are not additive coverage");
        emit(row,std::optional<PositionFix>{});
    }
    Statement waveforms(db,"SELECT * FROM waveform_observations ORDER BY id");
    while(waveforms.row()) {
        const auto w=waveform_from(waveforms,summary.config);CsvRecord row;row.text("record_type","waveform_observation");row.text("session_id",summary.session_id);
        row.number("waveform_id",w.id);row.number("frequency_hz",w.center_hz);row.number("inferred_bandwidth_hz",w.bandwidth_hz);row.number("inferred_spreading_factor",w.spreading_factor);
        row.number("evidence_first_elapsed_seconds",w.first_observed_elapsed);row.number("delimiter_elapsed_seconds",w.delimiter_elapsed);row.number("delimiter_utc_seconds",w.delimiter_utc);
        row.number("waveform_up_match_fraction",w.up_match);row.number("waveform_down_match_fraction",w.down_match);row.number("contributing_subbands",w.contributing_subbands);
        row.number("complete_in_requested_range",int(w.complete_in_requested_range));row.number("association_ambiguous",int(w.association_ambiguous));
        row.text("discovery_method",d.method);row.text("classification",waveform_interpretation);row.text("time_association",waveform_time_association);
        row.text("position_association",waveform_position_association);
        if(opt.include_receiver_positions&&w.receiver_position)row.position(*w.receiver_position,opt);
        emit(row,opt.include_receiver_positions?w.receiver_position:std::optional<PositionFix>{});
    }
    Statement gaps(db,"SELECT * FROM discovery_gaps ORDER BY id");
    while(gaps.row()) {
        const auto g=discovery_gap_from(gaps);CsvRecord row;row.text("record_type","discovery_gap");row.text("session_id",summary.session_id);
        row.number("discovery_gap_id",g.id);row.number("discovery_gap_first_input_sample",g.first_input_sample);row.number("discovery_gap_end_input_sample",g.end_input_sample);
        row.number("discovery_subband_index",g.subband_index);row.text("gap_reason",g.reason);row.number("sample_rate",summary.config.sample_rate);
        row.text("discovery_coverage_interpretation","half-open input-sample coordinates; subband -1 means all; invalid_input_order is invalid submission, not additional RF time; overlapping gaps are not additive");
        emit(row,std::optional<PositionFix>{});
    }
}
PositionFix saved_position(const Statement& row) {
    PositionFix fix;fix.utc_seconds=row.real(0);fix.latitude=row.real(1);fix.longitude=row.real(2);
    if(!row.null(3))fix.altitude_m=row.real(3);fix.manual=boolean(row,4);fix.source=row.text(5);
    if(!row.null(6))fix.hdop=row.real(6);fix.satellites=static_cast<unsigned>(unsigned_value(row,7,1000));
    fix.monotonic_seconds=row.real(8);fix.valid=true;validate_fix(fix);return fix;
}
std::string hex_bytes(const uint8_t* data,size_t count) {static constexpr char digits[]="0123456789abcdef";std::string out;out.reserve(count*2);for(size_t i=0;i<count;++i){out+=digits[data[i]>>4];out+=digits[data[i]&15];}return out;}
}
void SessionStore::with_read_snapshot(const std::function<void()>& action) const {
    ReadSnapshot snapshot(db_);action();
}
void SessionStore::write_report_file(const std::string& path,
    const std::function<void(const std::function<void(std::string_view)>&)>& generator) const {
    ReadSnapshot snapshot(db_);PrivateFile output(path);
    try {generator([&](std::string_view data){output.write(std::string(data));});output.sync();}
    catch(...) {output.discard();throw;}
}
void SessionStore::visit_tiles(const std::function<void(const SpectrumTile&)>& visitor) const {
    ReadSnapshot snapshot(db_);if(schema_version_<4)throw std::runtime_error("This legacy recording has no fine frequency activity history");
    const auto config=read().config;Statement rows(db_,tiles_sql(schema_version_));
    uint64_t previous=0;double elapsed=0;size_t bins=0;double first=0;
    while(rows.row()) {
        const auto tile=tile_from(rows,config,schema_version_);
        if(tile.first_sample<previous || tile.elapsed_start_seconds<elapsed-1e-8 ||
           (bins&&(bins!=tile.mean_dbfs.size()||first!=tile.first_center_hz)))
            throw std::runtime_error("Unordered or inconsistent saved spectrum tiles");
        previous=tile.end_sample;elapsed=tile.elapsed_end_seconds;bins=tile.mean_dbfs.size();first=tile.first_center_hz;
        visitor(tile);
    }
}
void SessionStore::visit_positions(const std::function<void(const PositionFix&)>& visitor) const {
    ReadSnapshot snapshot(db_);Statement rows(db_,"SELECT * FROM positions ORDER BY rowid");
    while(rows.row())visitor(saved_position(rows));
}
void SessionStore::visit_waveforms(const std::function<void(const WaveformObservation&)>& visitor) const {
    ReadSnapshot snapshot(db_);if(schema_version_<5)return;const auto config=read().config;
    Statement rows(db_,"SELECT * FROM waveform_observations ORDER BY id");while(rows.row())visitor(waveform_from(rows,config));
}
void SessionStore::visit_receptions(const std::function<void(const Reception&)>& visitor) const {
    ReadSnapshot snapshot(db_);Statement rows(db_,std::string("SELECT ")+reception_columns_for(schema_version_)+" FROM receptions ORDER BY id");
    while(rows.row()){auto reception=reception_from(rows,schema_version_);read_route(db_,reception,schema_version_);visitor(reception);}
}
void SessionStore::visit_gaps(const std::function<void(const CoverageGap&)>& visitor) const {
    ReadSnapshot snapshot(db_);if(schema_version_<4)throw std::runtime_error("This legacy recording has no fine coverage-gap history");
    Statement rows(db_,"SELECT * FROM coverage_gaps ORDER BY id");while(rows.row())visitor(gap_from(rows));
}
void SessionStore::export_csv(const std::string& path,const ExportOptions& opt) const {
    if(opt.coordinate_decimals>7)throw std::runtime_error("Coordinate precision must be 0 through 7");
    ReadSnapshot snapshot(db_);
    const auto summary=read();
    PrivateFile output(path);
    std::string header;for(const auto& column:export_columns()){if(!header.empty())header+=',';header+=column;}output.write(header+'\n');
    CsvRecord session;session.text("record_type","session");session.text("session_id",summary.session_id);
    session.number("elapsed_seconds",summary.elapsed_seconds);session.number("input_seconds",summary.input_seconds);
    session.number("measurement_seconds",summary.measurement_seconds);session.number("delivered_samples",summary.delivered_samples);
    session.number("dropped_samples",summary.dropped_samples);session.number("total_receptions",summary.total_receptions);
    session.number("authorized_records",summary.authorized_messages);session.number("incomplete",int(summary.incomplete));
    session.number("activity_threshold_dbfs",summary.config.activity_threshold_dbfs);session.number("sample_rate",summary.config.sample_rate);
    session.number("survey_span_hz",summary.config.survey_span_hz);session.text("source",summary.config.synthetic?"synthetic":receiver_source_name(summary.config));
    session.text("rf_level_unit","uncalibrated dBFS/bin");session.number("frequency_hz",summary.config.center_hz);
    if(!summary.config.synthetic&&summary.config.hardware_receiver==HardwareReceiver::RtlSdr) {
        session.number("rtl_auto_gain",int(summary.config.rtl_auto_gain));
        if(!summary.config.rtl_auto_gain)session.number("rtl_tuner_gain_db",summary.config.rtl_gain_tenths_db/10.0);
    } else {
        session.number("lna_gain_db",summary.config.lna_gain);session.number("vga_gain_db",summary.config.vga_gain);session.number("rf_amplifier_enabled",int(summary.config.amplifier));
    }
    session.number("tuning_offset_hz",summary.config.tuning_offset_hz);session.number("tuner_command_hz",tuned_center_hz(summary.config));
    session.number("session_schema_version",schema_version_);
    Statement version(db_,"SELECT version FROM session");if(version.row())session.text("application_version",version.text(0));session.write(output);
    if(schema_version_>=5)export_discovery(db_,summary,opt,[&](const CsvRecord& row,const std::optional<PositionFix>&){row.write(output);});
    if(schema_version_>=4) {
        CsvRecord metrology;metrology.text("record_type","spectrum_metrology");metrology.text("session_id",summary.session_id);
        metrology.number("fft_size",4096);metrology.number("hop_size",4096);metrology.text("window_function","periodic Hann");metrology.number("bin_width_hz",summary.spectrum_bin_width_hz);metrology.number("enbw_hz",summary.spectrum_enbw_hz);
        metrology.text("normalization",normalization);metrology.text("position_association",position_association);metrology.text("time_association","host UTC anchor + sample progress; activity frame-major LSB-first, contiguous nonoverlapping FFT frames");
        metrology.text("power_time_association",schema_version_>=6?"frame-weighted linear per-bin mean and maxima over <=1s power block; fine activity unchanged; time/location subsets use coarse block power":"per-bin power mean over entire tile; partial-tile selection assumes tile mean and sets boundary quality");
        metrology.text("peak_interpretation","per-bin maxima over tile; sum across bins is an upper-bound peak envelope, not simultaneous interval peak");metrology.number("activity_threshold_dbfs",summary.config.activity_threshold_dbfs);
        // Free-form notes can contain operational details, so shareable exports omit them.
        if(opt.include_provenance){metrology.text("antenna_description",summary.config.antenna_description);metrology.text("receiver_description",summary.config.receiver_description);metrology.text("survey_notes",summary.config.survey_notes);}metrology.write(output);
        const auto analysis=analyze({});
        for(const auto& bin:analysis.bins){CsvRecord row;row.text("record_type","spectrum_bin");row.text("session_id",summary.session_id);row.number("frequency_hz",bin.center_hz);row.number("bandwidth_hz",bin.width_hz);row.number("observed_seconds",bin.observed_seconds);row.number("active_seconds",bin.active_seconds);if(bin.observed_seconds>0)row.number("occupancy_fraction",bin.active_seconds/bin.observed_seconds);row.number("mean_dbfs",bin.mean_dbfs);row.number("peak_dbfs",bin.peak_dbfs);row.write(output);}
        Statement tiles(db_,tiles_sql(schema_version_));
        while(tiles.row()) {
            const auto t=tile_from(tiles,summary.config,schema_version_);CsvRecord row;row.text("record_type","spectrum_tile");row.text("session_id",summary.session_id);row.number("spectrum_id",t.id);
            row.number("first_sample",t.first_sample);row.number("end_sample",t.end_sample);row.number("window_utc_start_seconds",t.utc_start_seconds);row.number("window_utc_end_seconds",t.utc_end_seconds);row.number("window_elapsed_start_seconds",t.elapsed_start_seconds);row.number("window_elapsed_end_seconds",t.elapsed_end_seconds);
            row.number("first_center_hz",t.first_center_hz);row.number("bin_width_hz",t.bin_width_hz);row.number("fft_size",t.fft_size);row.number("frame_count",t.frame_count);row.number("bin_count",t.mean_dbfs.size());row.number("background_dbfs",t.background_dbfs);row.number("clipped_samples",t.clipped_samples);row.number("quality_flags",t.quality);row.number("power_elapsed_start_seconds",t.power_elapsed_start);row.number("power_elapsed_end_seconds",t.power_elapsed_end);row.text("power_time_association",schema_version_>=6?"coarse block power; exact fine activity":"tile power");
            const auto means=quantize(t.mean_dbfs),peaks=quantize(t.peak_dbfs);row.text("mean_centidb_le_hex",hex_bytes(means.data(),means.size()));row.text("peak_centidb_le_hex",hex_bytes(peaks.data(),peaks.size()));row.text("position_association",position_association);
            if(opt.include_receiver_positions&&t.receiver_end)row.position(*t.receiver_end,opt);row.write(output);
            if(opt.include_receiver_positions&&t.receiver_start){CsvRecord start;start.text("record_type","spectrum_tile_start_position");start.text("session_id",summary.session_id);start.number("spectrum_id",t.id);start.text("position_association","receiver-start fix");start.position(*t.receiver_start,opt);start.write(output);}
            const size_t stride=(t.mean_dbfs.size()+7)/8;
            for(size_t frame=0;frame<t.frame_count;) {size_t end=frame+1;while(end<t.frame_count&&std::equal(t.activity.begin()+static_cast<std::ptrdiff_t>(frame*stride),t.activity.begin()+static_cast<std::ptrdiff_t>((frame+1)*stride),t.activity.begin()+static_cast<std::ptrdiff_t>(end*stride)))++end;
                CsvRecord mask;mask.text("record_type","spectrum_activity_run");mask.text("session_id",summary.session_id);mask.number("spectrum_id",t.id);mask.number("activity_frame_start",frame);mask.number("activity_frame_count",end-frame);mask.text("activity_mask_hex",hex_bytes(t.activity.data()+frame*stride,stride));mask.write(output);frame=end;}
        }
        Statement events(db_,"SELECT * FROM spectrum_events ORDER BY id");while(events.row()) {
            const auto e=event_from(events,summary.config);CsvRecord row;row.text("record_type","spectrum_event");row.text("session_id",summary.session_id);row.number("spectrum_id",e.id);row.number("first_sample",e.first_sample);row.number("end_sample",e.end_sample);
            row.number("window_utc_start_seconds",e.utc_start_seconds);row.number("window_utc_end_seconds",e.utc_end_seconds);row.number("window_elapsed_start_seconds",e.elapsed_start_seconds);row.number("window_elapsed_end_seconds",e.elapsed_end_seconds);
            row.number("lower_hz",e.lower_hz);row.number("upper_hz",e.upper_hz);row.number("active_seconds",e.active_seconds);row.number("mean_dbfs",e.mean_dbfs);row.number("peak_dbfs",e.peak_dbfs);row.number("quality_flags",e.quality);row.text("position_association",position_association);if(opt.include_receiver_positions&&e.receiver_end)row.position(*e.receiver_end,opt);row.write(output);
            if(opt.include_receiver_positions&&e.receiver_start){CsvRecord start;start.text("record_type","spectrum_event_start_position");start.text("session_id",summary.session_id);start.number("spectrum_id",e.id);start.text("position_association","receiver-start fix");start.position(*e.receiver_start,opt);start.write(output);}
        }
        Statement gaps(db_,"SELECT * FROM coverage_gaps ORDER BY id");while(gaps.row()){const auto g=gap_from(gaps);CsvRecord row;row.text("record_type","coverage_gap");row.text("session_id",summary.session_id);row.number("spectrum_id",g.id);row.number("missing_samples",g.missing_samples);row.number("window_utc_start_seconds",g.utc_start_seconds);row.number("window_utc_end_seconds",g.utc_end_seconds);row.number("window_elapsed_start_seconds",g.elapsed_start_seconds);row.number("window_elapsed_end_seconds",g.elapsed_end_seconds);row.text("gap_reason",g.reason);row.write(output);}
    }
    Statement coverage(db_,"SELECT * FROM coverage ORDER BY rowid");while(coverage.row()){CsvRecord row;row.text("record_type","coverage_checkpoint");row.text("session_id",summary.session_id);row.text("time_association","elapsed-only checkpoint; UTC unavailable");for(int column:{1,2,3})nonnegative(coverage.real(column),"coverage time");row.number("elapsed_seconds",coverage.real(1));row.number("input_seconds",coverage.real(2));row.number("measurement_seconds",coverage.real(3));row.number("dropped_samples",unsigned_value(coverage,4,INT64_MAX));row.text("source",coverage.text(5));row.write(output);}
    Statement bins(db_,"SELECT * FROM frequencies ORDER BY center");
    while(bins.row()) {
        const FrequencySummary bin{unsigned_value(bins,0,6000000000ULL),static_cast<uint32_t>(unsigned_value(bins,1)),bins.real(2),bins.real(3),bins.real(4),bins.real(5)};
        validate_bin(bin);CsvRecord row;row.text("record_type","frequency");row.text("session_id",summary.session_id);
        row.number("frequency_hz",bin.center_hz);row.number("bandwidth_hz",bin.width_hz);row.text("classification","sampled RF activity");
        row.number("observed_seconds",bin.observed_seconds);row.number("active_seconds",bin.active_seconds);
        if(bin.observed_seconds>0)row.number("occupancy_fraction",bin.active_seconds/bin.observed_seconds);
        row.number("mean_dbfs",bin.mean_dbfs);row.number("peak_dbfs",bin.peak_dbfs);row.write(output);
    }
    Statement packets(db_,std::string("SELECT ")+reception_columns_for(schema_version_)+" FROM receptions ORDER BY id");
    while(packets.row()) {
        auto r=reception_from(packets,schema_version_);read_route(db_,r,schema_version_);CsvRecord row;
        row.text("record_type","reception");row.text("session_id",summary.session_id);row.number("utc_seconds",r.utc_seconds);
        row.number("elapsed_seconds",r.elapsed_seconds);row.number("frequency_hz",r.frequency_hz);row.number("bandwidth_hz",r.bandwidth_hz);
        row.number("spreading_factor",r.spreading_factor);row.number("coding_rate",r.coding_rate);row.number("duration_seconds",r.duration_seconds);
        row.number("snr_db",r.snr_db);row.number("frequency_error_hz",r.frequency_error_hz);row.number("header_valid",int(r.header_valid));
        row.number("crc_valid",int(r.crc_valid));row.text("decode_status",std::string(protocol::status_name(r.decoded.status)));
        row.text("classification",r.decoded.classification);row.text("authentication",r.decoded.authentication);
        if(r.decoded.evidence) {
            row.number("evidence_port",r.decoded.evidence->port);
            row.number("evidence_signature_present",int(r.decoded.evidence->signature_present));
        }
        if(opt.include_content&&r.decoded.authorized) {
            const auto& a=*r.decoded.authorized;const auto& c=a.content;
            row.text("profile_id",a.profile_id);row.text("content_kind",c.kind);row.text("text",c.text);
            row.number("origin",a.from);row.number("destination",a.to);row.number("packet_id",a.packet_id);row.number("port",a.port);
            row.number("hop_limit",unsigned(a.hop_limit));row.number("hop_start",unsigned(a.hop_start));row.number("channel_hash",unsigned(a.channel_hash));
            row.number("next_hop",unsigned(a.next_hop));row.number("relay_node",unsigned(a.relay_node));row.number("want_ack",int(a.want_ack));
            row.number("via_mqtt",int(a.via_mqtt));row.number("want_response",int(a.want_response));
            if(schema_version_>=3) {
                row.number("request_id",a.request_id);row.number("reply_id",a.reply_id);row.number("signature_present",int(a.signature_present));
                row.text("routing_variant",c.routing_variant);
            }
            row.text("node_id",c.node_id);row.text("long_name",c.long_name);row.text("short_name",c.short_name);
            if(c.latitude)row.coordinate("sender_latitude",*c.latitude,opt.coordinate_decimals);
            if(c.longitude)row.coordinate("sender_longitude",*c.longitude,opt.coordinate_decimals);
            row.optional("sender_altitude",c.altitude);row.optional("voltage",c.voltage);row.optional("temperature",c.temperature);
            row.optional("humidity",c.humidity);row.optional("battery_percent",c.battery_percent);row.optional("channel_utilization",c.channel_utilization);
            row.optional("air_util_tx",c.air_util_tx);row.optional("reported_time",c.reported_time);row.optional("hardware_model",c.hardware_model);
            row.optional("role",c.role);row.optional("routing_error",c.routing_error);
            const auto sequence=[&](const char* name,const auto& values,bool json=false) {
                std::string text;for(auto value:values){if(!text.empty())text+=json?',':';';text+=std::to_string(value);}
                if(json)text='['+text+']';row.text(name,text);
            };
            sequence("route",c.route);
            // JSON integer lists preserve negative raw values without invoking
            // spreadsheet formula syntax or inventing route/SNR alignment.
            if(schema_version_>=3) {
                sequence("route_back",c.route_back);
                sequence("snr_towards_db_x4",c.snr_towards,true);sequence("snr_back_db_x4",c.snr_back,true);
            }
        }
        if(opt.include_receiver_positions&&r.receiver_position)row.position(*r.receiver_position,opt);
        row.write(output);
    }
    Statement windows(db_,"SELECT w.*,b.center,b.width,b.mean,b.peak,b.observed,b.active FROM survey_windows w JOIN window_bins b ON b.window_id=w.id ORDER BY w.id,b.center");
    while(windows.row()) {
        SurveyWindow window;window.id=unsigned_value(windows,0,INT64_MAX);window.utc_start_seconds=windows.real(1);window.utc_end_seconds=windows.real(2);
        window.elapsed_start_seconds=windows.real(3);window.elapsed_end_seconds=windows.real(4);
        if(windows.text(5)!="window-end")throw std::runtime_error("Unsupported survey-window position association");
        if(!windows.null(6)||!windows.null(7)) {
            PositionFix f;f.latitude=windows.real(6);f.longitude=windows.real(7);if(!windows.null(8))f.altitude_m=windows.real(8);
            f.manual=boolean(windows,9);f.utc_seconds=windows.real(10);f.monotonic_seconds=windows.real(11);f.source=windows.text(12);
            if(!windows.null(13))f.hdop=windows.real(13);f.satellites=static_cast<unsigned>(unsigned_value(windows,14,1000));f.valid=true;window.receiver_position=f;
        } else for(int field=8;field<15;++field)if(!windows.null(field))throw std::runtime_error("Incomplete survey-window receiver position");
        const FrequencySummary bin{unsigned_value(windows,15,6000000000ULL),static_cast<uint32_t>(unsigned_value(windows,16)),windows.real(17),windows.real(18),windows.real(19),windows.real(20)};
        window.frequencies.push_back(bin);validate_window(window);
        CsvRecord row;row.text("record_type","survey_window_bin");row.text("session_id",summary.session_id);row.number("window_id",window.id);
        row.number("window_utc_start_seconds",window.utc_start_seconds);row.number("window_utc_end_seconds",window.utc_end_seconds);
        row.number("window_elapsed_start_seconds",window.elapsed_start_seconds);row.number("window_elapsed_end_seconds",window.elapsed_end_seconds);
        row.text("position_association","window-end");row.number("frequency_hz",bin.center_hz);row.number("bandwidth_hz",bin.width_hz);
        row.text("classification","sampled RF activity");row.number("observed_seconds",bin.observed_seconds);row.number("active_seconds",bin.active_seconds);
        if(bin.observed_seconds>0)row.number("occupancy_fraction",bin.active_seconds/bin.observed_seconds);
        row.number("mean_dbfs",bin.mean_dbfs);row.number("peak_dbfs",bin.peak_dbfs);row.number("activity_threshold_dbfs",summary.config.activity_threshold_dbfs);
        if(opt.include_receiver_positions&&window.receiver_position)row.position(*window.receiver_position,opt);row.write(output);
    }
    if(opt.include_receiver_positions) {
        Statement positions(db_,"SELECT * FROM positions ORDER BY rowid");
        while(positions.row()) {CsvRecord row;row.text("record_type","receiver_position");row.text("session_id",summary.session_id);row.position(saved_position(positions),opt);row.write(output);}
    }
    output.sync();
}
void SessionStore::export_geojson(const std::string& path,const ExportOptions& opt) const {
    if(!opt.include_receiver_positions)throw std::runtime_error("Select receiver-position export before creating GeoJSON");
    if(opt.coordinate_decimals>7)throw std::runtime_error("Coordinate precision must be 0 through 7");
    ReadSnapshot snapshot(db_);
    const auto summary=read();
    CsvRecord receiver;receiver.text("source",summary.config.synthetic?"synthetic":receiver_source_name(summary.config));
    receiver.number("sample_rate",summary.config.sample_rate);receiver.number("frequency_hz",summary.config.center_hz);
    receiver.number("survey_span_hz",summary.config.survey_span_hz);receiver.number("tuning_offset_hz",summary.config.tuning_offset_hz);
    if(!summary.config.synthetic&&summary.config.hardware_receiver==HardwareReceiver::RtlSdr) {
        receiver.number("rtl_auto_gain",int(summary.config.rtl_auto_gain));
        if(!summary.config.rtl_auto_gain)receiver.number("rtl_tuner_gain_db",summary.config.rtl_gain_tenths_db/10.0);
    } else {
        receiver.number("lna_gain_db",summary.config.lna_gain);receiver.number("vga_gain_db",summary.config.vga_gain);
        receiver.number("rf_amplifier_enabled",int(summary.config.amplifier));
    }
    PrivateFile output(path);output.write("{\"type\":\"FeatureCollection\",\"receiver\":"+receiver.json()+",\"features\":[");
    bool comma=false;Statement positions(db_,"SELECT * FROM positions ORDER BY rowid");
    while(positions.row()) {
        const auto fix=saved_position(positions);
        std::ostringstream row;row.imbue(std::locale::classic());if(comma)row<<',';comma=true;
        row<<"{\"type\":\"Feature\",\"geometry\":{\"type\":\"Point\",\"coordinates\":["<<std::fixed<<std::setprecision(static_cast<int>(opt.coordinate_decimals))
           <<fix.longitude<<','<<fix.latitude<<"]},\"properties\":{\"utc_seconds\":"<<std::setprecision(3)<<fix.utc_seconds
           <<",\"manual\":"<<(fix.manual?"true":"false")<<",\"source\":"<<json_string(fix.source)<<"}}";
        output.write(row.str());
    }
    if(schema_version_>=4) {
        // One RF feature per located measurement tile, with its own endpoint
        // association. Do not use capped display buckets as an export source.
        Statement tiles(db_,tiles_sql(schema_version_));while(tiles.row()) {
            const auto t=tile_from(tiles,summary.config,schema_version_);if(!t.receiver_end)continue;const auto& fix=*t.receiver_end;
            double mean=0,peak=0;for(float p:t.mean_dbfs)mean+=std::pow(10.0,p/10.0);for(float p:t.peak_dbfs)peak+=std::pow(10.0,p/10.0);
            const size_t stride=(t.mean_dbfs.size()+7)/8;size_t active_frames=0;for(size_t f=0;f<t.frame_count;++f){bool active=false;for(size_t b=0;b<stride;++b)active=active||t.activity[f*stride+b]!=0;if(active)++active_frames;}
            std::ostringstream row;row.imbue(std::locale::classic());if(comma)row<<',';comma=true;
            row<<"{\"type\":\"Feature\",\"geometry\":{\"type\":\"Point\",\"coordinates\":["<<std::fixed<<std::setprecision(static_cast<int>(opt.coordinate_decimals))<<fix.longitude<<','<<fix.latitude
               <<"]},\"properties\":{\"record_type\":\"rf_observation\",\"spectrum_id\":"<<t.id<<std::setprecision(9)<<",\"elapsed_start\":"<<t.elapsed_start_seconds<<",\"elapsed_end\":"<<t.elapsed_end_seconds
               <<",\"utc_start\":"<<t.utc_start_seconds<<",\"utc_end\":"<<t.utc_end_seconds<<",\"lower_hz\":"<<t.first_center_hz-t.bin_width_hz/2<<",\"upper_hz\":"<<t.first_center_hz+(double(t.mean_dbfs.size())-0.5)*t.bin_width_hz
               <<",\"observed_seconds\":"<<t.elapsed_end_seconds-t.elapsed_start_seconds<<",\"busy_seconds\":"<<double(active_frames)*4096/summary.config.sample_rate<<",\"integrated_mean_dbfs\":"<<10*std::log10(mean)
               <<",\"peak_envelope_upper_bound_dbfs\":"<<10*std::log10(peak)<<",\"background_dbfs_per_bin\":"<<t.background_dbfs<<",\"quality_flags\":"<<t.quality
               <<",\"power_elapsed_start_seconds\":"<<t.power_elapsed_start<<",\"power_elapsed_end_seconds\":"<<t.power_elapsed_end<<",\"power_time_association\":"<<json_string(schema_version_>=6?"coarse block power; exact fine activity":"tile power")
               <<",\"position_association\":"<<json_string(position_association)<<",\"receiver_end_fix_utc\":"<<fix.utc_seconds<<",\"receiver_end_hdop\":";
            if(fix.hdop)row<<*fix.hdop;else row<<"null";
            if(t.receiver_start)row<<",\"receiver_start_coordinates\":["<<std::setprecision(static_cast<int>(opt.coordinate_decimals))<<t.receiver_start->longitude<<','<<t.receiver_start->latitude<<"],\"receiver_start_fix_utc\":"<<std::setprecision(9)<<t.receiver_start->utc_seconds;
            row<<"}}";output.write(row.str());
        }
    }
    if(schema_version_>=5)export_discovery(db_,summary,opt,[&](const CsvRecord& record,const std::optional<PositionFix>& fix) {
        std::ostringstream row;row.imbue(std::locale::classic());if(comma)row<<',';comma=true;
        row<<"{\"type\":\"Feature\",\"geometry\":";
        if(fix)row<<"{\"type\":\"Point\",\"coordinates\":["<<std::fixed<<std::setprecision(static_cast<int>(opt.coordinate_decimals))<<fix->longitude<<','<<fix->latitude<<"]}";
        else row<<"null";
        row<<",\"properties\":"<<record.json()<<'}';output.write(row.str());
    });
    output.write("]}\n");output.sync();
}
}
