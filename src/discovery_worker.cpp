// SPDX-License-Identifier: GPL-3.0-or-later
#include "discovery_worker.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <thread>
#ifdef OVMESH_DISCOVERY_SCHEDULER_TEST
#include <functional>
#endif

namespace ovmesh {
namespace {
using Complex = std::complex<float>;
constexpr uint64_t maximum_coordinate = uint64_t{1} << 53;
void add(uint64_t& counter, uint64_t value) {
    counter = value > std::numeric_limits<uint64_t>::max() - counter ?
              std::numeric_limits<uint64_t>::max() : counter + value;
}
} // namespace

#ifdef OVMESH_DISCOVERY_SCHEDULER_TEST
void discovery_scheduler_probe_impl(size_t initial_head,
    const std::function<void(size_t, bool, uint64_t)>& before_process,
    const std::function<void(unsigned)>& checkpoint);
#endif

class DiscoveryWorker::Impl {
public:
    struct SourceBlock {
        std::array<Complex, maximum_input_block> values{};
        uint64_t first = 0;
        size_t count = 0;
    };
    struct Job {
        bool reset = false;
        size_t subband = 0, count = 0;
        uint64_t first_local = 0, anchor = 0, segment = 0;
        uint32_t stride = 0;
        std::array<Complex, detector_batch> values{};
    };
    struct Worker {
        std::array<Job, detector_queue_capacity> jobs;
        // Pending FIFO entries refer to fixed IQ slots. An active job retains
        // its slot until completion, so the 48-slot bound includes active work.
        std::array<size_t, detector_queue_capacity> ready{};
        std::array<bool, detector_queue_capacity> occupied{};
        size_t head = 0, tail = 0, count = 0;
        size_t occupied_count = 0;
        std::thread thread;
    };
    struct Staging {
        std::array<Complex, detector_batch> values{};
        size_t count = 0;
        uint64_t next_local = 0, anchor = 0;
        uint32_t stride = 0;
        bool anchored = false;
    };

    DiscoveryChannelizer bank;
    double requested_lower, requested_upper;
    std::array<SourceBlock, source_capacity> sources;
    size_t source_head = 0, source_tail = 0, source_count = 0;
    std::mutex source_mutex, lifecycle_mutex;
    std::condition_variable source_available;
    std::thread producer;
    std::array<std::unique_ptr<Worker>, detector_workers> workers;
    std::mutex detector_mutex;
    std::condition_variable detector_available, detector_space;
    std::vector<bool> detector_busy;
    size_t next_detector_queue = 0, pending_detector_jobs = 0;
    std::vector<std::unique_ptr<LoRaPreambleDiscovery>> detectors;
    std::vector<std::unique_ptr<DiscoveryDecoder>> decoders;
    std::vector<DiscoveryDecoderStats> decoder_stats;
    std::vector<Staging> staging;
    std::atomic<bool> closed{false}, producer_done{false}, failed{false};
    bool submitted_before = false; // single submitting caller owns these two fields
    uint64_t expected_submission = 0;
    uint64_t segment = 0, expected_source = 0; // PFB producer owns these fields
    bool source_initialized = false;
    mutable std::mutex state_mutex;
    Snapshot view;
    std::array<Result, result_capacity> results;
    size_t result_head = 0, result_count = 0;
    std::array<Gap, gap_capacity> gaps;
    size_t gap_head = 0, gap_count = 0;
    std::array<DiscoveryDecodedFrame, frame_capacity> frames;
    size_t frame_head = 0, frame_count = 0;
#ifdef OVMESH_DISCOVERY_SCHEDULER_TEST
    std::function<void(size_t, bool, uint64_t)> before_process_test;

    // Compiled only into the worker test executable. Exercise the actual
    // enqueue/claim/process/release paths with deterministic test barriers.
    friend void discovery_scheduler_probe_impl(size_t initial_head,
        const std::function<void(size_t, bool, uint64_t)>& before_process,
        const std::function<void(unsigned)>& checkpoint) {
        auto impl = std::make_unique<Impl>(8000000, 907500000, 904300000, 910700000,
                                          DiscoveryDecoderOptions{});
        impl->before_process_test = before_process;
        auto& queue = *impl->workers[0];
        queue.head = queue.tail = initial_head;
        auto append = [&](size_t band, bool reset) {
            auto& stage = impl->staging[band];
            stage.count = reset ? 0 : 1;
            stage.stride = 4;
            impl->enqueue(band, reset);
        };
        append(0, false); // Held active by the first test barrier.
        append(0, true);  // Busy queue front: must not hide band 3's work.
        append(3, false);
        append(3, true);
        for (size_t i = 4; i < detector_queue_capacity; ++i) append(0, false);
        if (queue.count != detector_queue_capacity || queue.tail != initial_head)
            throw std::logic_error("Scheduler test did not fill/wrap its fixed queue");
        impl->producer_done = true;
        impl->workers[0]->thread = std::thread([&] { impl->consume(); });
        checkpoint(0);
        impl->workers[1]->thread = std::thread([&] { impl->consume(); });
        checkpoint(1);
        for (auto& worker : impl->workers)
            if (worker->thread.joinable()) worker->thread.join();
        if (impl->failed || impl->pending_detector_jobs || queue.count || queue.occupied_count ||
            queue.head != queue.tail || std::any_of(queue.occupied.begin(), queue.occupied.end(), [](bool v) { return v; }) ||
            std::any_of(impl->detector_busy.begin(), impl->detector_busy.end(), [](bool v) { return v; }))
            throw std::logic_error("Scheduler bypass changed failure, drain, or reserved-slot accounting");
    }
#endif

    Impl(uint32_t rate, double center, double lower, double upper, const DiscoveryDecoderOptions& options)
        : bank(rate, center, lower, upper), requested_lower(lower), requested_upper(upper) {
        for (const auto& channel : bank.subbands()) {
            detectors.push_back(std::make_unique<LoRaPreambleDiscovery>(channel.center_hz));
            SubbandProgress progress;
            progress.band = channel;
            view.subbands.push_back(progress);
        }
        if (options.enabled) {
            const auto capacity = std::min(DiscoveryDecoder::maximum_history_samples,
                DiscoveryDecoder::maximum_total_history_samples / bank.subbands().size());
            for (size_t i = 0; i < bank.subbands().size(); ++i)
                decoders.push_back(std::make_unique<DiscoveryDecoder>(i, bank.subbands(), lower, upper, options, capacity));
            decoder_stats.resize(decoders.size());
        }
        staging.resize(detectors.size());
        detector_busy.resize(detectors.size());
        view.detector_queue_high_water.resize(detector_workers);
        for (auto& worker : workers) worker = std::make_unique<Worker>();
        view.fault.reserve(160);
    }
    ~Impl() {
        finish();
        for (auto& block : sources) std::fill(block.values.begin(), block.values.end(), Complex{});
        for (auto& worker : workers)
            for (auto& job : worker->jobs) std::fill(job.values.begin(), job.values.end(), Complex{});
        for (auto& block : staging) std::fill(block.values.begin(), block.values.end(), Complex{});
    }

    void start() {
        try {
            for (size_t i = 0; i < workers.size(); ++i)
                workers[i]->thread = std::thread([this] { consume(); });
            producer = std::thread([this] { produce(); });
        } catch (...) {
            fail("Could not start experimental discovery workers.");
            closed = true;
            producer_done = true;
            notify_all();
            for (auto& worker : workers) if (worker->thread.joinable()) worker->thread.join();
            throw;
        }
    }
    void notify_all() {
        // closed/failed/producer_done also participate in wait predicates.
        // Synchronize notifications with each wait mutex so an atomic state
        // transition cannot be lost between predicate inspection and sleeping.
        {
            std::lock_guard lock(source_mutex);
            source_available.notify_all();
        }
        {
            std::lock_guard lock(detector_mutex);
            detector_available.notify_all();
            detector_space.notify_all();
        }
    }
    void fail(const char* text) {
        failed = true;
        {
            std::lock_guard lock(state_mutex);
            view.failed = true;
            if (view.fault.empty()) view.fault.assign(text, std::min<size_t>(160, std::char_traits<char>::length(text)));
        }
        notify_all();
    }
    // Caller holds state_mutex. A bounded queue may lose individual interval
    // details after overflow; its counter makes that incompleteness explicit.
    void gap_locked(uint64_t first, uint64_t end, GapReason reason,
                    size_t subband = all_subbands, bool source_loss = true) {
        if (end <= first) return;
        if (source_loss && subband == all_subbands) {
            add(view.source_gap_input_samples, end - first);
            for (auto& band : view.subbands) add(band.source_gap_input_samples, end - first);
        }
        if (gap_count) {
            auto& previous = gaps[(gap_head + gap_count - 1) % gap_capacity];
            if (previous.reason == reason && previous.subband_index == subband && previous.end_input_sample == first) {
                previous.end_input_sample = end;
                return;
            }
        }
        if (gap_count == gap_capacity) { add(view.gap_overflows, 1); return; }
        gaps[(gap_head + gap_count) % gap_capacity] = {first, end, subband, reason};
        ++gap_count;
    }
    bool reject(uint64_t first, uint64_t end, GapReason reason) {
        std::lock_guard lock(state_mutex);
        add(view.rejected_input_samples, end - first);
        if (reason == GapReason::source_queue_full) add(view.source_queue_drops, 1);
        else if (reason != GapReason::processing_failure) add(view.invalid_submissions, 1);
        // Repeated/backward coordinates are invalid caller input, not additional
        // elapsed RF time; keep the event but never inflate source-gap duration.
        gap_locked(first, end, reason, all_subbands,
                   reason != GapReason::invalid_input_order && reason != GapReason::invalid_sample_coordinate);
        return false;
    }
    bool submit(std::span<const Complex> input, uint64_t first) {
        if (closed) {
            std::lock_guard lock(state_mutex);
            add(view.rejected_after_close, 1);
            return false;
        }
        if (input.size() > maximum_coordinate || first > maximum_coordinate - input.size())
            return reject(first, first, GapReason::invalid_sample_coordinate);
        const uint64_t end = first + input.size();
        if (input.empty()) return true;
        if (submitted_before && first < expected_submission)
            return reject(first, end, GapReason::invalid_input_order);
        if (submitted_before && first > expected_submission) {
            std::lock_guard lock(state_mutex);
            gap_locked(expected_submission, first, GapReason::input_discontinuity);
        }
        submitted_before = true;
        expected_submission = end;
        if (input.size() > maximum_input_block)
            return reject(first, end, GapReason::input_too_large);
        // Reception may continue after this optional worker fails. Keep its
        // unprocessed source coverage explicit until the caller closes it.
        if (failed) return reject(first, end, GapReason::processing_failure);

        // The consumer never holds this mutex during channelization, detector
        // work or detector-queue waits. Only bounded slot bookkeeping is shared.
        std::unique_lock lock(source_mutex);
        if (closed) {
            std::lock_guard state(state_mutex);
            add(view.rejected_after_close, 1);
            return false;
        }
        if (failed) return reject(first, end, GapReason::processing_failure);
        if (source_count == source_capacity) return reject(first, end, GapReason::source_queue_full);
        auto& block = sources[source_tail];
        block.first = first;
        block.count = input.size();
        std::copy(input.begin(), input.end(), block.values.begin());
        source_tail = (source_tail + 1) % source_capacity;
        ++source_count;
        {
            std::lock_guard state(state_mutex);
            add(view.accepted_input_samples, input.size());
            view.source_queue_high_water = std::max(view.source_queue_high_water, source_count);
        }
        lock.unlock();
        source_available.notify_one();
        return true;
    }

    void enqueue(size_t band, bool reset) {
        const size_t index = band % detector_workers;
        auto& worker = *workers[index];
        auto& pending = staging[band];
        std::unique_lock lock(detector_mutex);
        detector_space.wait(lock, [&] { return worker.occupied_count < detector_queue_capacity || failed.load(); });
        if (failed) throw std::runtime_error("Experimental discovery stopped");
        const auto free = std::find(worker.occupied.begin(), worker.occupied.end(), false);
        if (free == worker.occupied.end()) throw std::logic_error("Discovery job-slot accounting mismatch");
        const auto slot = static_cast<size_t>(free - worker.occupied.begin());
        auto& job = worker.jobs[slot];
        job.reset = reset;
        job.subband = band;
        job.segment = segment;
        job.first_local = pending.next_local;
        job.anchor = pending.anchor;
        job.stride = pending.stride;
        job.count = reset ? 0 : pending.count;
        if (!reset) {
            std::copy_n(pending.values.begin(), pending.count, job.values.begin());
            std::fill_n(pending.values.begin(), pending.count, Complex{});
            pending.next_local += pending.count;
            pending.count = 0;
        }
        worker.occupied[slot] = true;
        ++worker.occupied_count;
        worker.ready[worker.tail] = slot;
        worker.tail = (worker.tail + 1) % detector_queue_capacity;
        ++worker.count;
        ++pending_detector_jobs;
        const auto occupied_count = worker.occupied_count;
        lock.unlock();
        {
            std::lock_guard state(state_mutex);
            view.detector_queue_high_water[index] = std::max(view.detector_queue_high_water[index], occupied_count);
        }
        detector_available.notify_all();
    }
    void flush_staging() {
        for (size_t band = 0; band < staging.size(); ++band)
            if (staging[band].count) enqueue(band, false);
    }
    void new_segment(uint64_t first) {
        flush_staging();
        bank.reset();
        if (source_initialized) {
            std::lock_guard lock(state_mutex);
            add(view.stream_resets, 1);
        }
        source_initialized = true;
        expected_source = first;
        ++segment;
        for (size_t band = 0; band < staging.size(); ++band) {
            staging[band] = {};
            enqueue(band, true);
        }
    }
    void append(const DiscoveryChannelizer::Samples& block) {
        auto& pending = staging[block.subband_index];
        if (!pending.anchored) {
            pending.anchor = block.first_input_sample;
            pending.stride = block.input_sample_stride;
            pending.anchored = true;
        }
        const uint64_t expected = pending.anchor + (pending.next_local + pending.count) * pending.stride;
        if (block.first_input_sample != expected || block.input_sample_stride != pending.stride)
            throw std::runtime_error("Unexpected experimental channelizer discontinuity");
        auto values = block.values;
        while (!values.empty()) {
            const size_t count = std::min(values.size(), detector_batch - pending.count);
            std::copy_n(values.begin(), count, pending.values.begin() + static_cast<std::ptrdiff_t>(pending.count));
            pending.count += count;
            values = values.subspan(count);
            if (pending.count == detector_batch) enqueue(block.subband_index, false);
        }
    }
    void produce() {
        try {
            while (!failed) {
                std::unique_lock lock(source_mutex);
                source_available.wait(lock, [&] { return source_count || closed.load() || failed.load(); });
                if (failed || !source_count) break;
                auto& block = sources[source_head];
                lock.unlock();
                if (!source_initialized || block.first != expected_source) new_segment(block.first);
                bank.feed(std::span(block.values).first(block.count), block.first,
                          [this](const auto& samples) { append(samples); });
                expected_source = block.first + block.count;
                {
                    std::lock_guard state(state_mutex);
                    add(view.channelized_input_samples, block.count);
                }
                std::fill_n(block.values.begin(), block.count, Complex{});
                lock.lock();
                block.count = 0;
                source_head = (source_head + 1) % source_capacity;
                --source_count;
            }
            if (!failed) flush_staging();
        } catch (...) {
            fail("Experimental discovery processing failed; some intervals were not analyzed.");
        }
        if (failed) discard_pending_sources();
        producer_done = true;
        notify_all();
    }
    void discard_pending_sources() {
        std::lock_guard lock(source_mutex);
        std::lock_guard state(state_mutex);
        while (source_count) {
            auto& block = sources[source_head];
            add(view.abandoned_input_samples, block.count);
            gap_locked(block.first, block.first + block.count, GapReason::processing_failure,
                       all_subbands, false);
            std::fill_n(block.values.begin(), block.count, Complex{});
            block.count = 0;
            source_head = (source_head + 1) % source_capacity;
            --source_count;
        }
        for (size_t band = 0; band < staging.size(); ++band) {
            auto& pending = staging[band];
            add(view.subbands[band].abandoned_output_samples, pending.count);
            if (pending.count) gap_locked(pending.anchor + pending.next_local * pending.stride,
                pending.anchor + (pending.next_local + pending.count) * pending.stride,
                GapReason::processing_failure, band, false);
            pending.count = 0;
            std::fill(pending.values.begin(), pending.values.end(), Complex{});
        }
    }
    void record_result(const Job& job, const LoRaDiscovery& found) {
        const double lower = found.center_hz - found.bandwidth_hz / 2.;
        const double upper = found.center_hz + found.bandwidth_hz / 2.;
        // The PFB preserves guard frequencies to avoid clipping unknown chirps.
        // Entirely guard-only candidates are not observations of the requested
        // range. Keep an intersecting edge candidate, even if its center is
        // outside the range, with complete_in_requested_range=false.
        if (upper <= requested_lower || lower >= requested_upper) {
            std::lock_guard lock(state_mutex);
            add(view.subbands[job.subband].outside_range_candidates, 1);
            return;
        }
        Result result;
        result.subband_index = job.subband;
        result.segment_id = job.segment;
        result.first_input_anchor = job.anchor;
        result.input_sample_stride = job.stride;
        result.waveform = found;
        result.first_observed_upchirp_input_sample = static_cast<double>(job.anchor) +
            found.first_observed_upchirp_sample * job.stride;
        result.delimiter_input_sample = static_cast<double>(job.anchor) + found.delimiter_sample * job.stride;
        result.complete_in_requested_range = lower >= requested_lower && upper <= requested_upper;
        std::lock_guard lock(state_mutex);
        auto& band = view.subbands[job.subband];
        add(band.discoveries, 1);
        if (result_count == result_capacity) {
            add(view.result_overflows, 1);
            add(band.result_overflows, 1);
            return;
        }
        results[(result_head + result_count) % result_capacity] = result;
        ++result_count;
    }
    void record_frame(DiscoveryDecodedFrame&& frame) {
        std::lock_guard lock(state_mutex);
        if (frame_count == frame_capacity) {
            add(view.automatic_decoder.frame_overflows, 1);
            // The temporary owning handoff erases bytes when the caller's
            // callback returns. Queue saturation never blocks detector work.
            return;
        }
        frames[(frame_head + frame_count) % frame_capacity] = std::move(frame);
        ++frame_count;
    }
    struct DetectorSelection {
        size_t queue = detector_workers, offset = 0;
    };
    // Caller holds detector_mutex. Search each queue oldest first so another
    // band's pending work is not hidden behind a busy band. A given band lives
    // in one queue: its first pending job is always chosen before later jobs,
    // and none is eligible while its reset or sample block is still active.
    DetectorSelection eligible_detector_job() const noexcept {
        for (size_t offset = 0; offset < workers.size(); ++offset) {
            const auto index = (next_detector_queue + offset) % workers.size();
            const auto& queue = *workers[index];
            for (size_t pending = 0; pending < queue.count; ++pending) {
                const auto slot = queue.ready[(queue.head + pending) % detector_queue_capacity];
                if (!detector_busy[queue.jobs[slot].subband]) return {index, pending};
            }
        }
        return {};
    }
    void process_job(Job& job) {
#ifdef OVMESH_DISCOVERY_SCHEDULER_TEST
        if (before_process_test) before_process_test(job.subband, job.reset, job.first_local);
#endif
        auto& detector = *detectors[job.subband];
        if (job.reset) {
            detector.reset();
            if (!decoders.empty()) {
                decoders[job.subband]->reset();
                std::lock_guard state(state_mutex);
                decoder_stats[job.subband] = decoders[job.subband]->stats();
            }
            if (job.segment > 1) {
                std::lock_guard state(state_mutex);
                add(view.subbands[job.subband].resets_after_gap, 1);
            }
        } else {
            const auto frame_callback = [this](DiscoveryDecodedFrame&& frame) { record_frame(std::move(frame)); };
            if (!decoders.empty()) decoders[job.subband]->feed(std::span(job.values).first(job.count),
                job.first_local, job.anchor, job.stride, job.segment, frame_callback);
            detector.feed(std::span(job.values).first(job.count), job.first_local,
                          [this, &job, &frame_callback](const auto& found) {
                              record_result(job, found);
                              if (!decoders.empty()) decoders[job.subband]->observe(found, frame_callback);
                          });
            const auto diagnostic = detector.stats();
            std::lock_guard state(state_mutex);
            auto& band = view.subbands[job.subband];
            if (!band.processed_output_samples)
                band.first_processed_input_sample = job.anchor + job.first_local * job.stride;
            add(band.processed_output_samples, job.count);
            band.last_processed_input_sample = job.anchor + (job.first_local + job.count) * job.stride;
            band.fft_searches = diagnostic.fft_searches;
            band.windows = diagnostic.windows;
            band.candidate_limit_hits = diagnostic.candidate_limit_hits;
            band.track_limit_hits = diagnostic.track_limit_hits;
            if (!decoders.empty()) decoder_stats[job.subband] = decoders[job.subband]->stats();
        }
    }
    // Caller holds state_mutex. An active owner's exception accounts for its
    // own job; failure cleanup only visits entries still in the pending FIFO.
    void abandon_job_locked(const Job& job) {
        add(view.subbands[job.subband].abandoned_output_samples, job.count);
        if (job.count) gap_locked(job.anchor + job.first_local * job.stride,
            job.anchor + (job.first_local + job.count) * job.stride,
            GapReason::processing_failure, job.subband, false);
    }
    void discard_detector_jobs() {
        std::lock_guard lock(detector_mutex);
        std::lock_guard state(state_mutex);
        for (auto& worker : workers) {
            while (worker->count) {
                const auto slot = worker->ready[worker->head];
                auto& job = worker->jobs[slot];
                abandon_job_locked(job);
                std::fill_n(job.values.begin(), job.count, Complex{});
                job.count = 0;
                worker->occupied[slot] = false;
                --worker->occupied_count;
                worker->head = (worker->head + 1) % detector_queue_capacity;
                --worker->count;
                --pending_detector_jobs;
            }
        }
        detector_space.notify_all();
    }
    void consume() {
        for (;;) {
            size_t index = 0, slot = 0;
            {
                std::unique_lock lock(detector_mutex);
                detector_available.wait(lock, [&] {
                    return failed.load() || eligible_detector_job().queue < workers.size() ||
                           (producer_done.load() && !pending_detector_jobs);
                });
                if (failed) break;
                const auto selected = eligible_detector_job();
                index = selected.queue;
                if (index == workers.size()) break; // producer finished; no queued work
                auto& worker = *workers[index];
                slot = worker.ready[(worker.head + selected.offset) % detector_queue_capacity];
                detector_busy[worker.jobs[slot].subband] = true;
                // Remove only the selected pending index. IQ remains in its
                // reserved slot, and all remaining jobs retain relative order.
                if (!selected.offset) {
                    worker.ready[worker.head] = 0;
                    worker.head = (worker.head + 1) % detector_queue_capacity;
                } else {
                    for (size_t pending = selected.offset; pending + 1 < worker.count; ++pending)
                        worker.ready[(worker.head + pending) % detector_queue_capacity] =
                            worker.ready[(worker.head + pending + 1) % detector_queue_capacity];
                    worker.tail = (worker.tail + detector_queue_capacity - 1) % detector_queue_capacity;
                    worker.ready[worker.tail] = 0;
                }
                --worker.count;
                --pending_detector_jobs;
                next_detector_queue = (index + 1) % workers.size();
            }
            // The fixed slot remains occupied while DSP runs. Other workers
            // may use other bands, but never this detector or replay history.
            auto& worker = *workers[index];
            auto& job = worker.jobs[slot];
            try {
                process_job(job);
            } catch (...) {
                fail("Experimental discovery processing failed; some intervals were not analyzed.");
                std::lock_guard state(state_mutex);
                abandon_job_locked(job);
            }
            std::fill_n(job.values.begin(), job.count, Complex{});
            job.count = 0;
            {
                std::lock_guard lock(detector_mutex);
                detector_busy[job.subband] = false;
                worker.occupied[slot] = false;
                --worker.occupied_count;
            }
            detector_space.notify_all();
            detector_available.notify_all();
        }
        if (failed) discard_detector_jobs();
    }
    void finish() {
        std::lock_guard lifecycle(lifecycle_mutex);
        {
            std::lock_guard lock(source_mutex);
            closed = true;
        }
        source_available.notify_all();
        if (producer.joinable()) producer.join();
        else { producer_done = true; notify_all(); }
        for (auto& worker : workers) if (worker && worker->thread.joinable()) worker->thread.join();
        // Keep only metadata after shutdown. Completed surveys must not leave
        // idle discovery objects holding their last transient IQ windows.
        bank.reset();
        for (auto& detector : detectors) detector->reset();
        for (size_t i = 0; i < decoders.size(); ++i) decoders[i]->reset();
        std::lock_guard state(state_mutex);
        for (size_t i = 0; i < decoders.size(); ++i) decoder_stats[i] = decoders[i]->stats();
        view.input_closed = true;
        view.finished = true;
        view.failed = failed;
    }
    Snapshot snapshot() const {
        std::lock_guard lock(state_mutex);
        Snapshot result = view;
        result.input_closed = closed;
        result.failed = failed;
        result.queued_results = result_count;
        result.queued_gaps = gap_count;
        result.queued_frames = frame_count;
        for (const auto& stats : decoder_stats) {
#define SUM(field) add(result.automatic_decoder.field, stats.field)
            SUM(candidates); SUM(started); SUM(completed); SUM(crc_valid); SUM(history_misses);
            SUM(active_limit_hits); SUM(duplicate_candidates); SUM(excluded_candidates);
            SUM(outside_range_candidates); SUM(unsupported_candidates); SUM(timeouts);
            SUM(resets); SUM(abandoned_decoders);
#undef SUM
            result.automatic_decoder.active_decoders += stats.active_decoders;
            result.automatic_decoder.history_samples += stats.history_samples;
#define SUM(field) add(result.automatic_decoder.phy.field, stats.phy.field)
            SUM(preamble_candidates); SUM(sync_matches); SUM(sync_rejections); SUM(sync_low_ratio);
            SUM(sync_timeout); SUM(sync_first_mismatch); SUM(sync_second_mismatch);
            SUM(headers_valid); SUM(headers_failed); SUM(completed_frames);
#undef SUM
        }
        return result;
    }
    std::vector<Result> take_results() {
        std::lock_guard lock(state_mutex);
        std::vector<Result> output;
        output.reserve(result_count);
        while (result_count) {
            output.push_back(results[result_head]);
            results[result_head] = {};
            result_head = (result_head + 1) % result_capacity;
            --result_count;
        }
        return output;
    }
    std::vector<Gap> take_gaps() {
        std::lock_guard lock(state_mutex);
        std::vector<Gap> output;
        output.reserve(gap_count);
        while (gap_count) {
            output.push_back(gaps[gap_head]);
            gaps[gap_head] = {};
            gap_head = (gap_head + 1) % gap_capacity;
            --gap_count;
        }
        return output;
    }
    std::vector<DiscoveryDecodedFrame> take_frames() {
        std::lock_guard lock(state_mutex);
        std::vector<DiscoveryDecodedFrame> output;
        output.reserve(frame_count);
        while (frame_count) {
            output.push_back(std::move(frames[frame_head]));
            frame_head = (frame_head + 1) % frame_capacity;
            --frame_count;
        }
        return output;
    }
};

#ifdef OVMESH_DISCOVERY_SCHEDULER_TEST
void discovery_scheduler_probe(size_t initial_head,
    const std::function<void(size_t, bool, uint64_t)>& before_process,
    const std::function<void(unsigned)>& checkpoint) {
    discovery_scheduler_probe_impl(initial_head, before_process, checkpoint);
}
#endif

DiscoveryWorker::DiscoveryWorker(uint32_t rate, double center, double lower, double upper,
    const DiscoveryDecoderOptions& options)
    : impl_(std::make_unique<Impl>(rate, center, lower, upper, options)) { impl_->start(); }
DiscoveryWorker::~DiscoveryWorker() = default;
bool DiscoveryWorker::submit(std::span<const Complex> input, uint64_t first) { return impl_->submit(input, first); }
void DiscoveryWorker::finish() { impl_->finish(); }
DiscoveryWorker::Snapshot DiscoveryWorker::snapshot() const { return impl_->snapshot(); }
std::vector<DiscoveryWorker::Result> DiscoveryWorker::take_results() { return impl_->take_results(); }
std::vector<DiscoveryWorker::Gap> DiscoveryWorker::take_gaps() { return impl_->take_gaps(); }
std::vector<DiscoveryDecodedFrame> DiscoveryWorker::take_frames() { return impl_->take_frames(); }

} // namespace ovmesh
