// SPDX-License-Identifier: GPL-3.0-or-later
#include "ovmesh/gps_discovery.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }

ovmesh::SerialDeviceMetadata fixture() {
    return {"/dev/cu.usbmodemTest", "u-blox 7 GPS/GNSS Receiver", "u-blox", "TEST-SERIAL", "port-one", 0x1546, 0x01a7};
}

void classification() {
    auto input = fixture();
    const auto gps = ovmesh::classify_gps_serial_device(input);
    require(gps && gps->automatic_candidate, "Known USB GPS is a candidate");
    for (const auto product : {0x01a5, 0x01a6, 0x01a7, 0x01a8}) {
        input.product_id = static_cast<std::uint16_t>(product);
        require(ovmesh::classify_gps_serial_device(input)->automatic_candidate, "Reviewed GPS families recognized");
    }
    input.product_id = 0x01a9;
    require(!ovmesh::classify_gps_serial_device(input)->automatic_candidate, "Unknown future product needs review");
    input.vendor_id = 0x239a; input.product_id = 0x8029;
    require(!ovmesh::classify_gps_serial_device(input)->automatic_candidate, "RAK radio is not a GPS even with a GPS label");
    input.vendor_id = 0x303a; input.product_id = 0x1001;
    require(!ovmesh::classify_gps_serial_device(input)->automatic_candidate, "ESP USB serial is not a GPS");
    input.vendor_id = 0x10c4; input.product_id = 0xea60;
    require(!ovmesh::classify_gps_serial_device(input)->automatic_candidate, "Generic UART is not guessed to be a GPS");
    input.product = "GPS\n\x1b\t%s##bad";
    const auto hostile = ovmesh::classify_gps_serial_device(input);
    require(hostile && hostile->label.find('\n') == std::string::npos && hostile->label.find('\x1b') == std::string::npos,
        "Metadata labels cannot inject controls");
    input = fixture(); input.product = std::string(257, 'x');
    require(!ovmesh::classify_gps_serial_device(input), "Oversize metadata rejected");
    input = fixture(); input.serial = std::string("a\0b", 3);
    require(!ovmesh::classify_gps_serial_device(input), "Embedded null identity rejected");
    for (const auto* path : {"", "\\\\server\\COM3", "/dev/../tmp/tty", "/Volumes/GPS", "COM0", "COM3x", "/dev/.", "/dev/.."}) {
        input = fixture(); input.path = path;
        require(!ovmesh::classify_gps_serial_device(input), "Unsafe or malformed serial path rejected");
    }
    input = fixture(); input.path = "COM123";
    require(ovmesh::classify_gps_serial_device(input).has_value(), "Windows COM ports need no user path syntax");
}

void selection() {
    auto input = fixture();
    auto first = *ovmesh::classify_gps_serial_device(input);
    input.serial = "SECOND"; input.path = "/dev/ttyACM1";
    const auto second = *ovmesh::classify_gps_serial_device(input);
    require(!ovmesh::select_gps_device({}, ""), "No GPS does not select a port");
    require(ovmesh::select_gps_device({first}, "") == 0, "One GPS selects automatically");
    require(!ovmesh::select_gps_device({first, second}, ""), "Two GPS devices require a choice");
    require(ovmesh::select_gps_device({first, second}, second.stable_id) == 1, "Remembered receiver resolves multiple GPS units");
    require(!ovmesh::select_gps_device({first}, second.stable_id), "Missing remembered receiver never silently switches");
    auto moved = first; moved.path = "COM24";
    require(ovmesh::select_gps_device({moved}, first.stable_id) == 0, "Same USB serial is retained across port changes");
    require(!ovmesh::select_gps_device({first, moved}, first.stable_id), "Duplicate serial identity refuses automatic choice");
    auto other = second; other.path = first.path; other.automatic_candidate = false;
    require(!ovmesh::select_gps_device({first, other}, ""), "Duplicate OS path refuses automatic choice");
    auto generic = second; generic.automatic_candidate = false;
    require(!ovmesh::select_gps_device({generic}, ""), "A sole generic USB serial is never opened automatically");
    require(ovmesh::select_gps_device({first, generic}, "") == 0, "Generic serial does not prevent unique known GPS selection");
    require(ovmesh::select_gps_device({generic}, generic.stable_id) == 0, "Explicitly remembered generic GPS can be selected");
    input = fixture(); input.serial.clear();
    const auto unnumbered = *ovmesh::classify_gps_serial_device(input);
    input.location = "port-two";
    const auto changed_port = *ovmesh::classify_gps_serial_device(input);
    require(unnumbered.stable_id.find(":location:") != std::string::npos && unnumbered.stable_id != changed_port.stable_id,
        "No-serial identity is explicitly port scoped");
    require(!ovmesh::select_gps_device({changed_port}, unnumbered.stable_id), "Moving an unnumbered device requires a new choice");
    input.location.clear();
    require(ovmesh::classify_gps_serial_device(input)->stable_id.find(":port:") != std::string::npos,
        "Missing USB location uses explicit serial-port fallback");
    auto raw_a = fixture(); raw_a.serial = "A:B";
    auto raw_b = raw_a; raw_b.serial = "A/B";
    require(ovmesh::classify_gps_serial_device(raw_a)->stable_id != ovmesh::classify_gps_serial_device(raw_b)->stable_id,
        "Identity escaping does not merge distinct serial values");
}
}

int main() {
    try {
        classification(); selection();
        std::cout << "GPS discovery classification and selection tests passed; no devices enumerated or opened\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
