// SPDX-License-Identifier: GPL-3.0-or-later
#include "ovmesh/gps_discovery.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <string_view>

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <setupapi.h>
#else
#include <filesystem>
#include <fstream>
#endif

namespace ovmesh {
namespace {

constexpr std::size_t maximum_devices = 256;
constexpr std::size_t maximum_metadata = 256;
constexpr std::size_t maximum_path = 1024;

bool bounded_text(std::string_view value, std::size_t limit) {
    return value.size() <= limit && value.find('\0') == std::string_view::npos;
}

std::string label_text(std::string_view value) {
    // USB/registry labels are untrusted. Render printable ASCII only; keep raw
    // bounded identity bytes separately so sanitization cannot merge identities.
    std::string result;
    result.reserve(value.size());
    for (const unsigned char c : value) result += c >= 32 && c < 127 ? char(c) : '?';
    return result;
}

std::string hex_bytes(std::string_view value) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(value.size() * 2);
    for (const unsigned char c : value) {
        result += digits[c >> 4];
        result += digits[c & 15];
    }
    return result;
}

std::string hex_id(std::uint16_t value) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result(4, '0');
    for (int i = 3; i >= 0; --i) { result[static_cast<std::size_t>(i)] = digits[value & 15]; value >>= 4; }
    return result;
}

bool local_serial_path(std::string_view path) {
    if (!bounded_text(path, maximum_path) || path.empty()) return false;
    if (path.starts_with("COM") && path.size() <= 8 && path.size() > 3 && path[3] != '0')
        return std::all_of(path.begin() + 3, path.end(), [](char c) { return c >= '0' && c <= '9'; });
    if (!path.starts_with("/dev/") || path.size() == 5) return false;
    return std::all_of(path.begin() + 5, path.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
    }) && path.substr(5) != "." && path.substr(5) != "..";
}

void add_device(GpsDiscovery& result, const SerialDeviceMetadata& metadata) {
    if (const auto device = classify_gps_serial_device(metadata)) result.devices.push_back(*device);
}

#if defined(__APPLE__)

std::string registry_string(io_registry_entry_t entry, CFStringRef key, std::size_t limit = maximum_metadata) {
    CFTypeRef value = IORegistryEntryCreateCFProperty(entry, key, kCFAllocatorDefault, 0);
    if (!value) return {};
    std::string result;
    if (CFGetTypeID(value) == CFStringGetTypeID()) {
        const auto string = static_cast<CFStringRef>(value);
        if (CFStringGetLength(string) <= static_cast<CFIndex>(limit)) {
            std::array<char, maximum_path + 1> buffer{};
            if (CFStringGetCString(string, buffer.data(), static_cast<CFIndex>(limit + 1), kCFStringEncodingUTF8))
                result = buffer.data();
        }
    }
    CFRelease(value);
    return result;
}

std::optional<std::uint32_t> registry_number(io_registry_entry_t entry, CFStringRef key) {
    CFTypeRef value = IORegistryEntryCreateCFProperty(entry, key, kCFAllocatorDefault, 0);
    if (!value) return std::nullopt;
    std::int64_t number = -1;
    const bool valid = CFGetTypeID(value) == CFNumberGetTypeID() &&
        CFNumberGetValue(static_cast<CFNumberRef>(value), kCFNumberSInt64Type, &number) &&
        number >= 0 && number <= 0xffffffffLL;
    CFRelease(value);
    if (!valid) return std::nullopt;
    return static_cast<std::uint32_t>(number);
}

GpsDiscovery enumerate_devices() {
    GpsDiscovery result;
    io_iterator_t iterator = IO_OBJECT_NULL;
    // Registry matching only: no IOServiceOpen, device interface, or USB request.
    if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("IOSerialBSDClient"), &iterator) != KERN_SUCCESS) {
        result.error = "Serial device metadata is unavailable.";
        return result;
    }
    std::size_t examined = 0;
    for (io_object_t service; (service = IOIteratorNext(iterator));) {
        if (++examined > maximum_devices) {
            IOObjectRelease(service);
            result.error = "Serial device list exceeded its limit; automatic selection is disabled.";
            break;
        }
        SerialDeviceMetadata metadata;
        metadata.path = registry_string(service, CFSTR("IOCalloutDevice"), maximum_path);
        io_registry_entry_t current = service;
        bool usb = false;
        for (unsigned depth = 0; depth < 32; ++depth) {
            if (IOObjectConformsTo(current, "IOUSBHostDevice") || IOObjectConformsTo(current, "IOUSBDevice")) {
                const auto vendor = registry_number(current, CFSTR("idVendor"));
                const auto product = registry_number(current, CFSTR("idProduct"));
                if (vendor && product && *vendor <= 65535 && *product <= 65535) {
                    metadata.vendor_id = static_cast<std::uint16_t>(*vendor);
                    metadata.product_id = static_cast<std::uint16_t>(*product);
                    metadata.product = registry_string(current, CFSTR("USB Product Name"));
                    metadata.manufacturer = registry_string(current, CFSTR("USB Vendor Name"));
                    metadata.serial = registry_string(current, CFSTR("USB Serial Number"));
                    if (metadata.serial.empty()) metadata.serial = registry_string(current, CFSTR("kUSBSerialNumberString"));
                    if (const auto location = registry_number(current, CFSTR("locationID")))
                        metadata.location = "mac-usb-location:" + std::to_string(*location);
                    usb = true;
                }
                break;
            }
            io_registry_entry_t parent = IO_OBJECT_NULL;
            if (IORegistryEntryGetParentEntry(current, kIOServicePlane, &parent) != KERN_SUCCESS) break;
            if (current != service) IOObjectRelease(current);
            current = parent;
        }
        if (current != service) IOObjectRelease(current);
        IOObjectRelease(service);
        if (usb) add_device(result, metadata);
    }
    IOObjectRelease(iterator);
    return result;
}

#elif defined(_WIN32)

std::string utf8(std::wstring_view value) {
    if (value.empty() || value.size() > maximum_path) return {};
    const int required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (required <= 0 || required > static_cast<int>(maximum_path)) return {};
    std::string result(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        result.data(), required, nullptr, nullptr) != required) return {};
    return result;
}

std::string property(HDEVINFO devices, SP_DEVINFO_DATA& device, DWORD property_id) {
    std::array<wchar_t, maximum_path + 1> buffer{};
    DWORD type = 0, bytes = 0;
    if (!SetupDiGetDeviceRegistryPropertyW(devices, &device, property_id, &type,
        reinterpret_cast<BYTE*>(buffer.data()), static_cast<DWORD>(sizeof(buffer)), &bytes) ||
        (type != REG_SZ && type != REG_MULTI_SZ) || bytes < sizeof(wchar_t) ||
        bytes > sizeof(buffer) || bytes % sizeof(wchar_t)) return {};
    const auto limit = buffer.begin() + bytes / sizeof(wchar_t);
    const auto end = std::find(buffer.begin(), limit, L'\0');
    if (end == limit) return {};
    return utf8({buffer.data(), static_cast<std::size_t>(end - buffer.begin())});
}

std::optional<std::uint16_t> usb_id(const std::string& id, std::string_view prefix) {
    const auto at = id.find(prefix);
    if (at == std::string::npos || at + prefix.size() + 4 > id.size()) return std::nullopt;
    const auto start = id.data() + at + prefix.size();
    unsigned value = 0;
    const auto parsed = std::from_chars(start, start + 4, value, 16);
    if (parsed.ec != std::errc{} || parsed.ptr != start + 4) return std::nullopt;
    const auto after = at + prefix.size() + 4;
    if (after < id.size() && id[after] != '&' && id[after] != '\\') return std::nullopt;
    return static_cast<std::uint16_t>(value);
}

GpsDiscovery enumerate_devices() {
    GpsDiscovery result;
    // Standard Ports class GUID, kept local to avoid an additional uuid library.
    constexpr GUID ports = {0x4d36e978, 0xe325, 0x11ce, {0xbf,0xc1,0x08,0x00,0x2b,0xe1,0x03,0x18}};
    const HDEVINFO devices = SetupDiGetClassDevsW(&ports, nullptr, nullptr, DIGCF_PRESENT);
    if (devices == INVALID_HANDLE_VALUE) { result.error = "Serial device metadata is unavailable."; return result; }
    for (DWORD index = 0;; ++index) {
        SP_DEVINFO_DATA device{};
        device.cbSize = sizeof(device);
        if (!SetupDiEnumDeviceInfo(devices, index, &device)) {
            if (GetLastError() != ERROR_NO_MORE_ITEMS) result.error = "Serial device enumeration was incomplete.";
            break;
        }
        if (index >= maximum_devices) { result.error = "Serial device list exceeded its limit; automatic selection is disabled."; break; }
        const std::string hardware = property(devices, device, SPDRP_HARDWAREID);
        const auto vendor = usb_id(hardware, "VID_");
        const auto product = usb_id(hardware, "PID_");
        if (!vendor || !product) continue;
        SerialDeviceMetadata metadata;
        metadata.vendor_id = *vendor;
        metadata.product_id = *product;
        metadata.product = property(devices, device, SPDRP_FRIENDLYNAME);
        if (metadata.product.empty()) metadata.product = property(devices, device, SPDRP_DEVICEDESC);
        metadata.manufacturer = property(devices, device, SPDRP_MFG);
        const HKEY key = SetupDiOpenDevRegKey(devices, &device, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_QUERY_VALUE);
        if (key != INVALID_HANDLE_VALUE) {
            std::array<wchar_t, 32> port{};
            DWORD type = 0, bytes = sizeof(port);
            if (RegQueryValueExW(key, L"PortName", nullptr, &type, reinterpret_cast<BYTE*>(port.data()), &bytes) == ERROR_SUCCESS &&
                type == REG_SZ && bytes >= sizeof(wchar_t) && bytes <= sizeof(port) && bytes % sizeof(wchar_t) == 0) {
                const auto limit = port.begin() + bytes / sizeof(wchar_t);
                const auto end = std::find(port.begin(), limit, L'\0');
                if (end != limit) metadata.path = utf8({port.data(), static_cast<std::size_t>(end - port.begin())});
            }
            RegCloseKey(key);
        }
        std::array<wchar_t, maximum_path + 1> instance{};
        DWORD required = 0;
        if (SetupDiGetDeviceInstanceIdW(devices, &device, instance.data(), static_cast<DWORD>(instance.size()), &required) &&
            required > 0 && required <= instance.size() && instance[required - 1] == L'\0') {
            const std::string id = utf8({instance.data(), required - 1});
            DWORD capabilities = 0, type = 0, bytes = 0;
            const bool unique = SetupDiGetDeviceRegistryPropertyW(devices, &device, SPDRP_CAPABILITIES, &type,
                reinterpret_cast<BYTE*>(&capabilities), sizeof(capabilities), &bytes) &&
                type == REG_DWORD && bytes == sizeof(capabilities) && (capabilities & 0x10U);
            const auto separator = id.find_last_of('\\');
            const std::string tail = separator == std::string::npos ? "" : id.substr(separator + 1);
            // Only a direct USB instance with a unique, non-location identifier
            // supplies a serial identity. Composite/driver-specific instances remain scoped.
            if (unique && id.starts_with("USB\\VID_") && id.find("&MI_") == std::string::npos &&
                !tail.empty() && tail.find('&') == std::string::npos) metadata.serial = tail;
            else metadata.location = "windows-instance:" + id;
        }
        add_device(result, metadata);
    }
    SetupDiDestroyDeviceInfoList(devices);
    return result;
}

#else

std::string read_attribute(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    std::array<char, maximum_metadata + 2> buffer{};
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const auto size = static_cast<std::size_t>(input.gcount());
    if (size > maximum_metadata + 1) return {};
    std::string result(buffer.data(), size);
    if (!result.empty() && result.back() == '\n') result.pop_back();
    return bounded_text(result, maximum_metadata) ? result : "";
}

std::optional<std::uint16_t> parse_id(std::string_view value) {
    if (value.size() != 4) return std::nullopt;
    unsigned id = 0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), id, 16);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size()) return std::nullopt;
    return static_cast<std::uint16_t>(id);
}

GpsDiscovery enumerate_devices() {
    GpsDiscovery result;
    std::error_code error;
    std::filesystem::directory_iterator entries("/sys/class/tty", error);
    if (error) { result.error = "Serial device metadata is unavailable in /sys/class/tty."; return result; }
    std::size_t examined = 0;
    for (const auto end = std::filesystem::directory_iterator{}; entries != end; entries.increment(error)) {
        if (error) break;
        if (++examined > 4096) { result.error = "Serial metadata enumeration exceeded its limit."; break; }
        const std::string name = entries->path().filename().string();
        if (!name.starts_with("ttyUSB") && !name.starts_with("ttyACM")) continue;
        SerialDeviceMetadata metadata;
        metadata.path = "/dev/" + name;
        auto current = std::filesystem::canonical(entries->path() / "device", error);
        if (error) { error.clear(); continue; }
        if (!current.generic_string().starts_with("/sys/devices/")) continue;
        for (unsigned depth = 0; depth < 32 && current != "/sys/devices"; ++depth, current = current.parent_path()) {
            const auto vendor = parse_id(read_attribute(current / "idVendor"));
            const auto product = parse_id(read_attribute(current / "idProduct"));
            if (!vendor || !product) continue;
            metadata.vendor_id = *vendor;
            metadata.product_id = *product;
            metadata.product = read_attribute(current / "product");
            metadata.manufacturer = read_attribute(current / "manufacturer");
            metadata.serial = read_attribute(current / "serial");
            metadata.location = "linux-sysfs:" + current.generic_string();
            if (result.devices.size() >= maximum_devices) result.error = "Serial device list exceeded its limit; automatic selection is disabled.";
            else add_device(result, metadata);
            break;
        }
        if (!result.error.empty()) break;
    }
    if (error) result.error = "Serial device enumeration was incomplete.";
    return result;
}

#endif

} // namespace

std::optional<GpsDevice> classify_gps_serial_device(const SerialDeviceMetadata& metadata) {
    if (!local_serial_path(metadata.path) || !bounded_text(metadata.serial, maximum_metadata) ||
        !bounded_text(metadata.location, maximum_path) || !bounded_text(metadata.product, maximum_metadata) ||
        !bounded_text(metadata.manufacturer, maximum_metadata) || metadata.vendor_id == 0) return std::nullopt;
    GpsDevice result;
    result.path = metadata.path;
    const std::string usb = "usb:" + hex_id(metadata.vendor_id) + ":" + hex_id(metadata.product_id);
    if (!metadata.serial.empty()) result.stable_id = usb + ":serial:" + hex_bytes(metadata.serial);
    else if (!metadata.location.empty()) result.stable_id = usb + ":location:" + hex_bytes(metadata.location);
    else result.stable_id = usb + ":port:" + hex_bytes(metadata.path);
    // u-blox UBX-17058776 R01 (15-Dec-2017), section 2.3, page 5:
    // 1546:01a5/01a6/01a7/01a8 identify GNSS generations 5, 6, 7 and 8/M8.
    // A matching label alone never qualifies an arbitrary USB UART as a GPS.
    result.automatic_candidate = metadata.vendor_id == 0x1546 &&
        metadata.product_id >= 0x01a5 && metadata.product_id <= 0x01a8;
    result.label = label_text(metadata.product.empty() ? metadata.manufacturer : metadata.product);
    if (result.label.empty()) result.label = "USB serial device";
    result.label += " (" + hex_id(metadata.vendor_id) + ":" + hex_id(metadata.product_id) + ")";
    if (metadata.serial.empty()) result.label += " [port-specific identity]";
    return result;
}

GpsDiscovery discover_gps_devices() {
    GpsDiscovery result = enumerate_devices();
    std::sort(result.devices.begin(), result.devices.end(), [](const auto& left, const auto& right) { return left.path < right.path; });
    // An incomplete inventory cannot establish uniqueness. The UI must also
    // avoid remembered selection when error is nonempty.
    if (!result.error.empty()) for (auto& device : result.devices) device.automatic_candidate = false;
    return result;
}

std::optional<std::size_t> select_gps_device(const std::vector<GpsDevice>& devices, const std::string& preferred_id) {
    std::optional<std::size_t> selected;
    for (std::size_t i = 0; i < devices.size(); ++i) {
        const auto& device = devices[i];
        const bool matches = preferred_id.empty() ? device.automatic_candidate : device.stable_id == preferred_id;
        if (!matches) continue;
        if (selected || device.stable_id.empty() || !local_serial_path(device.path)) return std::nullopt;
        selected = i;
    }
    if (!selected) return std::nullopt;
    const auto& chosen = devices[*selected];
    // Duplicate metadata identity or duplicate OS path makes even one candidate ambiguous.
    for (std::size_t i = 0; i < devices.size(); ++i)
        if (i != *selected && (devices[i].stable_id == chosen.stable_id || devices[i].path == chosen.path)) return std::nullopt;
    return selected;
}

std::optional<ConcentratorDevice> classify_concentrator_serial_device(const SerialDeviceMetadata& metadata) {
    if (metadata.vendor_id != 0x0483 || metadata.product_id != 0x5740) return std::nullopt;
    const auto serial = classify_gps_serial_device(metadata);
    if (!serial) return std::nullopt;
    return ConcentratorDevice{serial->path, serial->label, serial->stable_id};
}

ConcentratorDiscovery discover_concentrator_devices() {
    // GPS enumeration already returns all bounded USB serial metadata, while
    // only reviewed GNSS identities are automatic GPS candidates. Reusing its
    // inventory preserves every existing platform and GPS selection behavior.
    const auto serial = discover_gps_devices();
    ConcentratorDiscovery result;
    result.error = serial.error;
    for (const auto& device : serial.devices)
        if (device.stable_id.starts_with("usb:0483:5740:"))
            result.devices.push_back({device.path, device.label, device.stable_id});
    return result;
}

std::optional<std::size_t> select_concentrator_device(const std::vector<ConcentratorDevice>& devices,
                                                    const std::string& preferred_id) {
    if (preferred_id.empty() || !preferred_id.starts_with("usb:0483:5740:")) return std::nullopt;
    std::vector<GpsDevice> serial;
    serial.reserve(devices.size());
    for (const auto& device : devices) serial.push_back({device.path, device.label, device.stable_id, false});
    return select_gps_device(serial, preferred_id);
}

} // namespace ovmesh
