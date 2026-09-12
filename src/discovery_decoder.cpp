// SPDX-License-Identifier: GPL-3.0-or-later
#include "discovery_decoder.hpp"
#include "channelizer.hpp"
#include "discovery_iq_wipe.hpp"
#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>
#include <stdexcept>
#include <utility>

namespace ovmesh {
namespace {
using Complex = std::complex<float>;
constexpr uint32_t rate = DiscoveryChannelizer::output_sample_rate;
constexpr uint64_t maximum_coordinate = uint64_t{1} << 53;
void wipe(std::vector<uint8_t>& bytes) noexcept {
    volatile uint8_t* output = bytes.data();
    for (size_t i = 0; i < bytes.size(); ++i) output[i] = 0;
    bytes.clear();
}
void increment(uint64_t& counter) {
    if (counter != std::numeric_limits<uint64_t>::max()) ++counter;
}
void add_diagnostics(PhyDiagnostics& destination, const PhyDiagnostics& source) {
#define SUM(field) destination.field += std::min(source.field, std::numeric_limits<uint64_t>::max() - destination.field)
    SUM(preamble_candidates); SUM(sync_matches); SUM(sync_rejections); SUM(sync_low_ratio);
    SUM(sync_timeout); SUM(sync_first_mismatch); SUM(sync_second_mismatch);
    SUM(headers_valid); SUM(headers_failed); SUM(completed_frames);
#undef SUM
}
}

DiscoveryDecodedFrame::~DiscoveryDecodedFrame() { wipe(frame.bytes); }
DiscoveryDecodedFrame::DiscoveryDecodedFrame(DiscoveryDecodedFrame&& other) noexcept { *this = std::move(other); }
DiscoveryDecodedFrame& DiscoveryDecodedFrame::operator=(DiscoveryDecodedFrame&& other) noexcept {
    if (this != &other) {
        wipe(frame.bytes);
        subband_index = other.subband_index; segment_id = other.segment_id;
        center_hz = other.center_hz; bandwidth_hz = other.bandwidth_hz;
        spreading_factor = other.spreading_factor;
        first_input_sample = other.first_input_sample; end_input_sample = other.end_input_sample;
        delimiter_input_sample = other.delimiter_input_sample;
        frame = std::move(other.frame);
    }
    return *this;
}

struct DiscoveryDecoder::Impl {
    struct Active {
        LoRaDiscovery waveform;
        uint64_t replay_start = 0, deadline = 0;
        Downconverter convert;
        LoRaReceiver receiver;
        bool completed = false;
        Active(const LoRaDiscovery& found, uint64_t start, uint64_t end, double subband_center)
            : waveform(found), replay_start(start), deadline(end),
              convert(rate, found.bandwidth_hz, static_cast<int64_t>(std::llround(found.center_hz - subband_center))),
              receiver(PhyConfig{found.bandwidth_hz, static_cast<uint8_t>(found.spreading_factor), 5, 0x2b}) {}
        ~Active() { convert.reset(); receiver.reset(); }
    };
    size_t index;
    std::vector<DiscoveryChannelizer::Subband> bands;
    double lower, upper;
    DiscoveryDecoderOptions options;
    std::vector<Complex> history, scratch, converted;
    uint64_t begin = 0, end = 0, anchor = 0, segment = 0;
    uint32_t stride = 0;
    bool initialized = false;
    std::vector<std::unique_ptr<Active>> active;
    std::deque<LoRaDiscovery> recent;
    DiscoveryDecoderStats counters;

    Impl(size_t band_index, std::span<const DiscoveryChannelizer::Subband> descriptions,
         double requested_lower, double requested_upper, const DiscoveryDecoderOptions& config, size_t capacity)
        : index(band_index), bands(descriptions.begin(), descriptions.end()),
          lower(requested_lower), upper(requested_upper), options(config) {
        if (index >= bands.size() || !std::isfinite(lower) || !std::isfinite(upper) || lower >= upper ||
            capacity < maximum_block || capacity > maximum_history_samples || options.exclusions.size() > maximum_exclusions)
            throw std::invalid_argument("Invalid bounded automatic decoder configuration");
        for (const auto& band : bands)
            if (!std::isfinite(band.center_hz) || !std::isfinite(band.passband_lower_hz) ||
                !std::isfinite(band.passband_upper_hz) || band.passband_lower_hz >= band.passband_upper_hz)
                throw std::invalid_argument("Invalid automatic decoder subband");
        for (const auto& excluded : options.exclusions)
            if (!std::isfinite(excluded.center_hz) || !excluded.bandwidth_hz ||
                excluded.spreading_factor < 7 || excluded.spreading_factor > 12)
                throw std::invalid_argument("Invalid configured decoder exclusion");
        if (options.enabled) history.resize(capacity);
        active.reserve(maximum_active); scratch.reserve(maximum_block); converted.reserve(maximum_block);
    }
    ~Impl() { reset(); }
    void retire(size_t i) {
        add_diagnostics(counters.phy, active[i]->receiver.diagnostics());
        active.erase(active.begin() + static_cast<std::ptrdiff_t>(i));
    }
    void reset() {
        if (initialized) increment(counters.resets);
        for (const auto& decoder : active) {
            increment(counters.abandoned_decoders);
            add_diagnostics(counters.phy, decoder->receiver.diagnostics());
        }
        active.clear(); recent.clear();
        discovery_detail::wipe_ring(history, begin, end - begin);
        std::fill(scratch.begin(), scratch.end(), Complex{}); scratch.clear();
        std::fill(converted.begin(), converted.end(), Complex{}); converted.clear();
        begin = end = anchor = segment = 0; stride = 0; initialized = false;
    }
    void process(Active& decoder, std::span<const Complex> values, const Callback& callback) {
        decoder.convert.feed(values, converted);
        decoder.receiver.feed(converted, [&](PhyFrame&& phy) {
            // One confirmed delimiter launches one frame attempt. Later
            // preambles must receive their own discovery and bounded slot.
            if (decoder.completed) { wipe(phy.bytes); return; }
            decoder.completed = true;
            DiscoveryDecodedFrame output;
            output.subband_index = index; output.segment_id = segment;
            output.center_hz = decoder.waveform.center_hz;
            output.bandwidth_hz = decoder.waveform.bandwidth_hz;
            output.spreading_factor = decoder.waveform.spreading_factor;
            const double origin = static_cast<double>(decoder.replay_start) + decoder.convert.first_output_seconds() * rate;
            const double sample_stride = static_cast<double>(rate) / output.bandwidth_hz;
            output.first_input_sample = static_cast<double>(anchor) +
                (origin + phy.first_sample * sample_stride) * stride;
            output.end_input_sample = static_cast<double>(anchor) +
                (origin + phy.last_sample * sample_stride) * stride;
            output.delimiter_input_sample = static_cast<double>(anchor) + decoder.waveform.delimiter_sample * stride;
            increment(counters.completed);
            if (phy.payload_crc_present && phy.payload_crc_valid) increment(counters.crc_valid);
            else wipe(phy.bytes);
            output.frame = std::move(phy);
            callback(std::move(output));
        });
        std::fill(converted.begin(), converted.end(), Complex{}); converted.clear();
    }
    void feed(std::span<const Complex> values, uint64_t first, uint64_t input_anchor,
              uint32_t input_stride, uint64_t input_segment, const Callback& callback) {
        if (!options.enabled) return;
        if (values.size() > maximum_block || !input_stride || !input_segment || first > maximum_coordinate - values.size() ||
            input_anchor > maximum_coordinate || first + values.size() > (maximum_coordinate - input_anchor) / input_stride)
            throw std::invalid_argument("Automatic decoder input exceeds bounded coordinates");
        for (const auto sample : values)
            if (!std::isfinite(sample.real()) || !std::isfinite(sample.imag())) {
                reset(); throw std::invalid_argument("Nonfinite automatic decoder input");
            }
        if (initialized && (first != end || anchor != input_anchor || stride != input_stride || segment != input_segment)) reset();
        if (!initialized) {
            begin = end = first; anchor = input_anchor; stride = input_stride; segment = input_segment; initialized = true;
        }
        for (const auto sample : values) { history[end % history.size()] = sample; ++end; }
        if (end - begin > history.size()) begin = end - history.size();
        for (size_t i = 0; i < active.size();) {
            auto& decoder = *active[i];
            const auto count = first >= decoder.deadline ? 0 :
                static_cast<size_t>(std::min<uint64_t>(values.size(), decoder.deadline - first));
            if (count) process(decoder, values.first(count), callback);
            if (decoder.completed) retire(i);
            else if (end >= decoder.deadline) { increment(counters.timeouts); retire(i); }
            else ++i;
        }
    }
    void observe(const LoRaDiscovery& found, const Callback& callback) {
        if (!options.enabled) return;
        increment(counters.candidates);
        if (!initialized || !std::isfinite(found.center_hz) || !std::isfinite(found.delimiter_sample) ||
            !std::isfinite(found.first_observed_upchirp_sample) || found.first_observed_upchirp_sample < 0 ||
            found.delimiter_sample < found.first_observed_upchirp_sample || found.delimiter_sample > static_cast<double>(end) ||
            found.spreading_factor < 7 || found.spreading_factor > 12 ||
            (found.bandwidth_hz != 15625 && found.bandwidth_hz != 62500 && found.bandwidth_hz != 125000 && found.bandwidth_hz != 250000 && found.bandwidth_hz != 500000) ||
            (found.bandwidth_hz == 15625 && found.spreading_factor > 10)) {
            increment(counters.unsupported_candidates); return;
        }
        const double signal_lower = found.center_hz - found.bandwidth_hz / 2.;
        const double signal_upper = found.center_hz + found.bandwidth_hz / 2.;
        size_t owner = bands.size(); double nearest = std::numeric_limits<double>::infinity();
        for (size_t i = 0; i < bands.size(); ++i)
            if (signal_lower >= bands[i].passband_lower_hz && signal_upper <= bands[i].passband_upper_hz &&
                std::abs(found.center_hz - bands[i].center_hz) < nearest) {
                owner = i; nearest = std::abs(found.center_hz - bands[i].center_hz);
            }
        if (signal_lower < lower || signal_upper > upper || owner == bands.size()) {
            increment(counters.outside_range_candidates); return;
        }
        if (owner != index) { increment(counters.duplicate_candidates); return; }
        const double symbol = static_cast<double>(uint64_t{1} << found.spreading_factor) * rate / found.bandwidth_hz;
        const double tolerance = std::max(100., 2. * found.bandwidth_hz / (uint64_t{1} << found.spreading_factor));
        for (const auto& excluded : options.exclusions)
            if (excluded.bandwidth_hz == found.bandwidth_hz && excluded.spreading_factor == found.spreading_factor &&
                std::abs(excluded.center_hz - found.center_hz) <= tolerance) {
                increment(counters.excluded_candidates); return;
            }
        for (const auto& previous : recent)
            if (previous.bandwidth_hz == found.bandwidth_hz && previous.spreading_factor == found.spreading_factor &&
                std::abs(previous.center_hz - found.center_hz) <= tolerance &&
                std::abs(previous.delimiter_sample - found.delimiter_sample) < symbol / 2) {
                increment(counters.duplicate_candidates); return;
            }
        // Twelve symbols bound replay even when an on-air preamble is unusually
        // long. Seven before the delimiter include sync plus enough repeated
        // upchirps for PHY acquisition. Preserve filter warmup before that edge.
        const double start = std::max(0., std::min(found.first_observed_upchirp_sample - symbol,
                                                 found.delimiter_sample - 7 * symbol) - 256.);
        auto replay_start = static_cast<uint64_t>(std::floor(std::max(start, found.delimiter_sample - 12 * symbol - 256.)));
        if (active.size() == maximum_active) { increment(counters.active_limit_hits); return; }
        // Explicit-header payload bound: 255 bytes, CRC present, worst CR4/8.
        const bool low_rate = symbol / rate > .016;
        const double numerator = 8 * 255 - 4 * static_cast<int>(found.spreading_factor) + 28 + 16;
        const double denominator = 4 * (static_cast<int>(found.spreading_factor) - (low_rate ? 2 : 0));
        const double symbols = 8 + std::ceil(numerator / denominator) * 8 + 4;
        const auto deadline = static_cast<uint64_t>(std::ceil(std::min(static_cast<double>(maximum_coordinate),
            found.delimiter_sample + symbols * symbol)));
        if (end >= deadline) { increment(counters.timeouts); return; }
        auto decoder = std::make_unique<Active>(found, replay_start, deadline, bands[index].center_hz);
        // The discovered reset supplies a fractional sample grid. Align the
        // first chip-rate output to it after removing this converter's group
        // delay; arbitrary replay chunk boundaries must not choose the PHY's
        // sampling phase. Rounding is only to the original 2 MS/s sample grid.
        const double chip_stride = static_cast<double>(rate) / found.bandwidth_hz;
        const double phase_origin = found.first_observed_upchirp_sample - decoder->convert.first_output_seconds() * rate;
        replay_start = static_cast<uint64_t>(std::max(0., std::round(phase_origin +
            std::ceil((static_cast<double>(replay_start) - phase_origin) / chip_stride) * chip_stride)));
        decoder->replay_start = replay_start;
        if (replay_start < begin || replay_start > end || end - replay_start > history.size()) {
            increment(counters.history_misses); return;
        }
        increment(counters.started);
        if (recent.size() == recent_capacity) recent.pop_front();
        recent.push_back(found);
        for (uint64_t at = replay_start; at < end && !decoder->completed;) {
            const size_t count = static_cast<size_t>(std::min<uint64_t>(maximum_block, end - at));
            scratch.resize(count);
            for (size_t i = 0; i < count; ++i) scratch[i] = history[(at + i) % history.size()];
            process(*decoder, scratch, callback);
            std::fill(scratch.begin(), scratch.end(), Complex{}); scratch.clear();
            at += count;
        }
        if (decoder->completed) add_diagnostics(counters.phy, decoder->receiver.diagnostics());
        else active.push_back(std::move(decoder));
    }
    DiscoveryDecoderStats stats() const {
        auto result = counters; result.active_decoders = active.size(); result.history_samples = static_cast<size_t>(end - begin);
        for (const auto& decoder : active) add_diagnostics(result.phy, decoder->receiver.diagnostics());
        return result;
    }
};

DiscoveryDecoder::DiscoveryDecoder(size_t index, std::span<const DiscoveryChannelizer::Subband> bands,
    double lower, double upper, const DiscoveryDecoderOptions& options, size_t history_capacity)
    : impl_(std::make_unique<Impl>(index, bands, lower, upper, options, history_capacity)) {}
DiscoveryDecoder::~DiscoveryDecoder() = default;
void DiscoveryDecoder::feed(std::span<const Complex> samples, uint64_t first, uint64_t anchor,
    uint32_t stride, uint64_t segment, const Callback& callback) { impl_->feed(samples, first, anchor, stride, segment, callback); }
void DiscoveryDecoder::observe(const LoRaDiscovery& found, const Callback& callback) { impl_->observe(found, callback); }
void DiscoveryDecoder::reset() { impl_->reset(); }
DiscoveryDecoderStats DiscoveryDecoder::stats() const { return impl_->stats(); }
} // namespace ovmesh
