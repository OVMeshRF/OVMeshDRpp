// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <pb.h>
#include <pb_common.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <span>

#if defined(PB_ENABLE_MALLOC) || !defined(PB_VALIDATE_UTF8) || !defined(PB_BUFFER_ONLY) || !defined(PB_NO_ERRMSG)
#error Unsupported Nanopb build configuration
#endif
#if PB_MESSAGE_NESTING_MAX != 8
#error Nanopb nesting limit must match the reviewed wire guard
#endif

namespace ovmesh::protocol::detail {
using WireBytes = std::span<const std::uint8_t>;

// An additional bounded wire-policy check, driven by generated descriptors.
// Nanopb owns field decoding/defaults/arrays/unknown-field skipping. This guard
// rejects ambiguous singular/oneof encodings and noncanonical scalar encodings
// before unauthenticated channel plaintext is considered plausible evidence.
class WireReader {
public:
    explicit WireReader(WireBytes input) : input_(input) {}
    struct Field { std::uint32_t tag{}, wire{}; std::uint64_t value{}; WireBytes bytes; };
    bool next(Field& field) {
        if (done()) return false;
        field = {};
        std::uint64_t tag{};
        if (!varint(tag) || tag > 0xffffffffULL || !(tag >> 3)) return fail();
        field.tag = static_cast<std::uint32_t>(tag >> 3);
        field.wire = static_cast<std::uint32_t>(tag & 7);
        if (field.wire == 0) return varint(field.value);
        if (field.wire == 1 || field.wire == 5) {
            const auto count = field.wire == 1 ? 8u : 4u;
            if (input_.size() - position_ < count) return fail();
            for (unsigned i = 0; i < count; ++i) field.value |= std::uint64_t(input_[position_++]) << (8 * i);
            return true;
        }
        if (field.wire == 2) {
            std::uint64_t count{};
            if (!varint(count) || count > input_.size() - position_) return fail();
            field.bytes = input_.subspan(position_, static_cast<std::size_t>(count));
            position_ += static_cast<std::size_t>(count);
            return true;
        }
        return fail(); // No deprecated groups/reserved wire types in selected schemas.
    }
    bool varint(std::uint64_t& value) {
        value = 0;
        for (unsigned i = 0; i < 10; ++i) {
            if (done()) return fail();
            const auto byte = input_[position_++];
            if (i == 9 && byte > 1) return fail();
            value |= std::uint64_t(byte & 127) << (7 * i);
            if (!(byte & 128)) return i == 0 || byte != 0 ? true : fail();
        }
        return fail();
    }
    bool done() const { return position_ == input_.size(); }
    bool good() const { return good_ && done(); }
private:
    bool fail() { good_ = false; position_ = input_.size(); return false; }
    WireBytes input_;
    std::size_t position_{};
    bool good_{true};
};

inline bool scalar_fits(const pb_field_iter_t& field, std::uint64_t value) {
    const auto kind = PB_LTYPE(field.type);
    if (kind == PB_LTYPE_BOOL) return value <= 1;
    if (field.data_size == 0 || field.data_size > 8) return false;
    if (field.data_size == 8) return true;
    const auto bits = static_cast<unsigned>(field.data_size * 8);
    if (kind == PB_LTYPE_VARINT) {
        const auto positive = (std::uint64_t{1} << (bits - 1)) - 1;
        return value <= positive || value >= ~positive;
    }
    return value <= ((std::uint64_t{1} << bits) - 1);
}

inline bool validate_wire(WireBytes bytes, const pb_msgdesc_t* schema, unsigned depth = 0) {
    if (!schema || depth > 8 || bytes.size() > 239) return false;
    WireReader reader(bytes);
    WireReader::Field wire;
    std::array<std::uint32_t, 128> singular{};
    std::size_t singular_count = 0;
    // Each selected decoded message has at most one oneof. This is deliberately
    // not a general-purpose protobuf policy for arbitrary runtime schemas.
    bool oneof_seen = false;
    pb_field_iter_t field{};
    const bool has_fields = pb_field_iter_begin(&field, schema, nullptr);
    while (reader.next(wire)) {
        if (!has_fields || !pb_field_iter_find(&field, wire.tag)) continue;
        const auto kind = PB_LTYPE(field.type);
        const bool repeated = PB_HTYPE(field.type) == PB_HTYPE_REPEATED;
        if (!repeated) {
            if (std::find(singular.begin(), singular.begin() + static_cast<std::ptrdiff_t>(singular_count), wire.tag) !=
                singular.begin() + static_cast<std::ptrdiff_t>(singular_count) || singular_count == singular.size()) return false;
            singular[singular_count++] = wire.tag;
        }
        if (PB_HTYPE(field.type) == PB_HTYPE_ONEOF) {
            if (oneof_seen) return false;
            oneof_seen = true;
        }
        switch (kind) {
        case PB_LTYPE_BOOL: case PB_LTYPE_VARINT: case PB_LTYPE_UVARINT: case PB_LTYPE_SVARINT:
            if (wire.wire == 0) { if (!scalar_fits(field, wire.value)) return false; }
            else if (wire.wire == 2 && repeated) {
                WireReader packed(wire.bytes);
                while (!packed.done()) {
                    std::uint64_t value{};
                    if (!packed.varint(value) || !scalar_fits(field, value)) return false;
                }
                if (!packed.good()) return false;
            } else return false;
            break;
        case PB_LTYPE_FIXED32:
            if (wire.wire != 5 && !(repeated && wire.wire == 2 && wire.bytes.size() % 4 == 0)) return false;
            break;
        case PB_LTYPE_FIXED64:
            if (wire.wire != 1 && !(repeated && wire.wire == 2 && wire.bytes.size() % 8 == 0)) return false;
            break;
        case PB_LTYPE_STRING:
            if (wire.wire != 2 || std::find(wire.bytes.begin(), wire.bytes.end(), 0) != wire.bytes.end()) return false;
            break;
        case PB_LTYPE_BYTES: case PB_LTYPE_FIXED_LENGTH_BYTES:
            if (wire.wire != 2) return false;
            break;
        case PB_LTYPE_SUBMESSAGE: case PB_LTYPE_SUBMSG_W_CB:
            if (wire.wire != 2 || (field.submsg_desc && !validate_wire(wire.bytes, field.submsg_desc, depth + 1))) return false;
            break;
        default: return false;
        }
    }
    return reader.good();
}

inline bool has_wire_field(WireBytes bytes, std::uint32_t tag) {
    WireReader reader(bytes); WireReader::Field field;
    while (reader.next(field)) if (field.tag == tag) return true;
    return false;
}
} // namespace ovmesh::protocol::detail
