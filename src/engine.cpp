// SPDX-License-Identifier: GPL-3.0-or-later
#include "ovmesh/engine.hpp"
#include "ovmesh/phy.hpp"
#include "ovmesh/report.hpp"
#include "gps.hpp"
#include "channelizer.hpp"
#include "storage.hpp"
#include "spectrum.hpp"
#include "bursts.hpp"
#include "discovery_worker.hpp"
#include "discovery_observations.hpp"
#include "rtl_input.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <limits>
#include <mutex>
#include <numbers>
#include <stdexcept>
#include <thread>
#ifdef OVMESH_HAVE_HACKRF
#if __has_include(<libhackrf/hackrf.h>)
#include <libhackrf/hackrf.h>
#else
#include <hackrf.h>
#endif
#endif
#ifdef OVMESH_HAVE_RTLSDR
#include <rtl-sdr.h>
#endif

namespace ovmesh {
const char* receiver_source_name(const ReceiverConfig& config) noexcept {
    return config.synthetic ? "Synthetic RF" :
        config.hardware_receiver == HardwareReceiver::RtlSdr ? "RTL-SDR" : "HackRF";
}
uint64_t tuned_center_hz(const ReceiverConfig& config) {
    if(config.center_hz<1000000 || config.center_hz>6000000000ULL)
        throw std::runtime_error("Center frequency must be between 1 MHz and 6 GHz");
    if(config.tuning_offset_hz< -100000 || config.tuning_offset_hz>100000)
        throw std::runtime_error("Tuning offset must be between -100000 and 100000 Hz");
    const auto command=static_cast<int64_t>(config.center_hz)+config.tuning_offset_hz;
    if(command<1000000 || command>6000000000LL)
        throw std::runtime_error("Corrected tuner frequency must be between 1 MHz and 6 GHz");
    if(!config.synthetic&&config.hardware_receiver==HardwareReceiver::RtlSdr&&
       (config.center_hz<24000000||config.center_hz>1766000000ULL||command<24000000||command>1766000000LL))
        throw std::runtime_error("RTL-SDR center and corrected tuner frequency must be between 24 and 1766 MHz; tuning also depends on the attached tuner");
    return static_cast<uint64_t>(command);
}
namespace {
constexpr size_t block_bytes=262144, pool_blocks=32;
constexpr double pi=std::numbers::pi;
using Complex=std::complex<float>;
struct RawBlock {std::array<int8_t,block_bytes> data{};size_t size=0;uint64_t first=0;double arrival=0;};


void validate(const ReceiverConfig& c) {
    (void)tuned_center_hz(c);
    const bool rtl=!c.synthetic&&c.hardware_receiver==HardwareReceiver::RtlSdr;
    if(!c.synthetic&&c.hardware_receiver!=HardwareReceiver::HackRf&&c.hardware_receiver!=HardwareReceiver::RtlSdr)
        throw std::runtime_error("Unknown hardware receiver");
    const std::array<uint32_t,5> rates{8000000,10000000,12000000,16000000,20000000};
    if(rtl) {
        if(c.sample_rate!=1000000&&c.sample_rate!=2000000)
            throw std::runtime_error("RTL-SDR supports a 1 or 2 MS/s sample rate in this build");
        if(c.rtl_gain_tenths_db< -100||c.rtl_gain_tenths_db>600)
            throw std::runtime_error("RTL-SDR manual gain must be between -10 and 60 dB; the nearest supported tuner gain is applied");
        if(c.amplifier)throw std::runtime_error("The HackRF RF amplifier control is unavailable for RTL-SDR; disable it before starting");
        if(c.discover_lora&&(c.sample_rate!=2000000||c.survey_span_hz>1500000))
            throw std::runtime_error("RTL-SDR LoRa discovery requires 2 MS/s and a survey span of at most 1.5 MHz; other supported settings can measure spectrum with discovery disabled");
    } else if(std::find(rates.begin(),rates.end(),c.sample_rate)==rates.end())
        throw std::runtime_error("Choose an 8, 10, 12, 16 or 20 MS/s sample rate");
    if(c.survey_span_hz<500000 || c.survey_span_hz>c.sample_rate*4/5)throw std::runtime_error("Survey span must be 0.5 MHz through 80% of sample rate; passband remains uncalibrated");
    if(c.center_hz<c.survey_span_hz/2 || c.center_hz+c.survey_span_hz/2>6000000000ULL)throw std::runtime_error("Survey edges exceed receiver range");
    if(rtl&&(c.center_hz-c.survey_span_hz/2<24000000||c.center_hz+c.survey_span_hz/2>1766000000ULL))
        throw std::runtime_error("Survey edges exceed the supported RTL-SDR 24–1766 MHz range");
    if(!rtl&&(c.lna_gain>40||c.lna_gain%8||c.vga_gain>62||c.vga_gain%2))throw std::runtime_error("LNA must be 0–40 dB in 8 dB steps; VGA 0–62 dB in 2 dB steps");
    if(c.device_serial.size()>256||c.device_serial.find('\0')!=std::string::npos)
        throw std::runtime_error("Receiver serial is invalid or too long");
    if(!std::isfinite(c.activity_threshold_dbfs)||c.activity_threshold_dbfs>0||c.activity_threshold_dbfs< -140)throw std::runtime_error("Activity threshold must be -140 through 0 dBFS/bin");
    if(c.antenna_description.size()>240||c.receiver_description.size()>240||c.survey_notes.size()>2000)
        throw std::runtime_error("Survey provenance text is too long");
    if(c.lanes.size()>4)throw std::runtime_error("This build permits at most four explicitly configured decoder lanes");
    for(const auto& l:c.lanes){if(l.label.size()>80||l.channel_name.size()>80)throw std::runtime_error("Profile labels are too long");if(!l.enabled)continue;
        if(l.bandwidth_hz!=125000&&l.bandwidth_hz!=250000&&l.bandwidth_hz!=500000)throw std::runtime_error("Supported LoRa bandwidths are 125, 250 and 500 kHz");
        if(l.spreading_factor<7||l.spreading_factor>12||l.coding_rate<5||l.coding_rate>8)throw std::runtime_error("Supported LoRa profiles use SF7–12 and CR4/5–4/8");
        if(c.sample_rate%(l.bandwidth_hz*4)!=0)throw std::runtime_error("Sample rate must support integer decoder decimation; 500 kHz legacy profiles need RTL-SDR at 2 MS/s");
        if(l.frequency_hz>6000000000ULL)throw std::runtime_error("Decoder frequency exceeds receiver range");
        auto offset=std::abs(static_cast<int64_t>(l.frequency_hz)-static_cast<int64_t>(c.center_hz));
        if(offset+l.bandwidth_hz/2>c.survey_span_hz/2)throw std::runtime_error("Each decoder's entire bandwidth must be inside the survey span");
    }
}
}

struct Engine::Impl {
    mutable std::mutex mutex;
    std::mutex lifecycle;
    Snapshot view;
    std::array<protocol::Profile,protocol::max_keyring_profiles> profiles;
    std::vector<RawBlock> pool;
    std::atomic<uint64_t> head{0},tail{0},next_sample{0},dropped{0};
    std::atomic<double> last_input_arrival{0};
    std::atomic<bool> run{false};
    // False until the input producer/callback has stopped. The worker then drains
    // every accepted block before finalizing the session and freeing the pool.
    std::atomic<bool> input_finished{true};
    std::thread worker,producer;
    std::condition_variable wake;
    std::mutex wake_mutex;
    // Requests and confirmation are protected by mutex. Only process() writes
    // the recording connection; lifecycle serializes callers waiting for it.
    std::condition_variable save_finished;
    uint64_t save_requested = 0, save_completed = 0;
    bool final_save_confirmed = false;
    std::unique_ptr<SessionStore> store;
    std::unique_ptr<DiscoveryWorker> prepared_discovery;
    std::string saved_path;
    SerialGps gps;
    std::optional<PositionFix> current_fix;
    std::vector<PositionFix> fix_history;
    double started=0,started_utc=0;
#ifdef OVMESH_HAVE_HACKRF
    hackrf_device* device=nullptr;
    bool hackrf_initialized=false;
#endif
#ifdef OVMESH_HAVE_RTLSDR
    rtlsdr_dev_t* rtl_device=nullptr;
    RtlAsyncPump rtl_pump;
#endif
    Impl() {
        for(size_t i=0;i<profiles.size();++i) {
            profiles[i].id="key-"+std::to_string(i);
            profiles[i].restrict_channel_name=false;
        }
#ifdef OVMESH_HAVE_HACKRF
        view.hardware_available=true;
#endif
#ifdef OVMESH_HAVE_RTLSDR
        view.rtl_sdr_available=true;
#endif
    }
    void fail(const std::string& message) {std::lock_guard lock(mutex);view.error=message;view.state="Failed";view.incomplete=true;view.running=false;run=false;wake.notify_all();save_finished.notify_all();}
    void deliver(const int8_t* bytes,size_t size,double arrival) noexcept {
        if(!run)return;last_input_arrival.store(arrival,std::memory_order_relaxed);const auto first=next_sample.fetch_add(size/2,std::memory_order_relaxed);
        const auto h=head.load(std::memory_order_relaxed);
        if(size>block_bytes||size%2||h-tail.load(std::memory_order_acquire)>=pool_blocks){dropped.fetch_add(size/2,std::memory_order_relaxed);return;}
        auto& block=pool[h%pool_blocks];std::memcpy(block.data.data(),bytes,size);block.size=size;block.first=first;block.arrival=arrival;
        head.store(h+1,std::memory_order_release);wake.notify_one();
    }
#ifdef OVMESH_HAVE_HACKRF
    static int receive(hackrf_transfer* transfer) noexcept {
        auto* self=static_cast<Impl*>(transfer->rx_ctx);if(!self->run)return -1;
        if(transfer->valid_length<=0||transfer->buffer==nullptr)return 0;
        self->deliver(reinterpret_cast<int8_t*>(transfer->buffer),static_cast<size_t>(transfer->valid_length),monotonic_now());return 0;
    }
#endif
#ifdef OVMESH_HAVE_RTLSDR
    static void receive_rtl(unsigned char* bytes,uint32_t length,void* context) noexcept {
        auto* self=static_cast<Impl*>(context);
        if(!self->run) { rtlsdr_cancel_async(self->rtl_device);return; }
        if(!bytes||!length)return;
        // Same bounded SPSC queue as HackRF. Preserve the original unsigned
        // bytes until the consumer performs the correct offset-binary conversion.
        self->deliver(reinterpret_cast<const int8_t*>(bytes),length,monotonic_now());
    }
    void configure_rtl(ReceiverConfig& config) {
        const auto require=[](int result,const char* operation) {
            if(result<0)throw std::runtime_error(std::string("RTL-SDR ")+operation+" failed (driver code "+std::to_string(result)+")");
        };
        uint32_t index=0;
        if(config.device_serial.empty()) {
            const auto count=rtlsdr_get_device_count();
            if(!count)throw std::runtime_error("No RTL-SDR receiver found. Check the USB connection and operating-system driver");
            if(count!=1)throw std::runtime_error("More than one RTL-SDR receiver is connected; select its exact device serial before starting");
        } else {
            const auto selected=rtlsdr_get_index_by_serial(config.device_serial.c_str());
            if(selected<0)throw std::runtime_error("The configured RTL-SDR serial was not found");
            index=static_cast<uint32_t>(selected);
        }
        require(rtlsdr_open(&rtl_device,index),"open");
        if(rtlsdr_get_tuner_type(rtl_device)==RTLSDR_TUNER_UNKNOWN)
            throw std::runtime_error("RTL-SDR tuner was not identified; direct-sampling operation is not supported");
        require(rtlsdr_set_bias_tee(rtl_device,0),"bias tee disable");
        // A newly opened supported tuner is already in ordinary IQ mode.
        // Redundantly disabling direct sampling reinitializes its tuner and
        // retunes the initial cached zero frequency. Refuse an unexpected mode.
        if(rtlsdr_get_direct_sampling(rtl_device)!=0)
            throw std::runtime_error("RTL-SDR opened in an unexpected direct-sampling mode; ordinary tuner IQ is required");
        require(rtlsdr_set_testmode(rtl_device,0),"test mode disable");
        require(rtlsdr_set_agc_mode(rtl_device,0),"digital AGC disable");
        // Rate/bandwidth setters may retune the cached center. Establish a real
        // frequency first; the reviewed R82xx wrapper fails if its PLL is unlocked.
        require(rtlsdr_set_center_freq(rtl_device,static_cast<uint32_t>(tuned_center_hz(config))),"initial frequency setup / tuner lock");
        if(rtlsdr_get_freq_correction(rtl_device)!=0)
            require(rtlsdr_set_freq_correction(rtl_device,0),"PPM correction reset");
        require(rtlsdr_set_sample_rate(rtl_device,config.sample_rate),"sample rate setup");
        if(rtlsdr_get_sample_rate(rtl_device)!=config.sample_rate)
            throw std::runtime_error("RTL-SDR driver did not apply the requested sample rate; measurement timing is not accepted");
        require(rtlsdr_set_tuner_bandwidth(rtl_device,0),"automatic tuner filter setup");
        require(rtlsdr_set_center_freq(rtl_device,static_cast<uint32_t>(tuned_center_hz(config))),"final frequency setup / tuner lock");
        if(rtlsdr_get_center_freq(rtl_device)!=tuned_center_hz(config))
            throw std::runtime_error("RTL-SDR driver did not apply the requested tuner frequency");
        require(rtlsdr_set_tuner_gain_mode(rtl_device,config.rtl_auto_gain?0:1),"tuner gain mode setup");
        if(!config.rtl_auto_gain) {
            const auto count=rtlsdr_get_tuner_gains(rtl_device,nullptr);
            if(count<=0||count>256)throw std::runtime_error("RTL-SDR tuner returned an invalid manual gain table");
            std::vector<int> gains(static_cast<size_t>(count));
            if(rtlsdr_get_tuner_gains(rtl_device,gains.data())!=count)
                throw std::runtime_error("RTL-SDR tuner gain table changed during setup");
            const auto selected=nearest_rtl_gain(gains,config.rtl_gain_tenths_db);
            require(rtlsdr_set_tuner_gain(rtl_device,selected),"manual tuner gain setup");
            if(rtlsdr_get_tuner_gain(rtl_device)!=selected)
                throw std::runtime_error("RTL-SDR driver did not apply the selected manual tuner gain");
            config.rtl_gain_tenths_db=selected;
        }
        // RTL has no separately controlled HackRF LNA/VGA or RF amplifier.
        // Do not retain dormant HackRF gains as this session's acquisition setup.
        config.lna_gain=0;config.vga_gain=0;config.amplifier=false;
        require(rtlsdr_reset_buffer(rtl_device),"input buffer reset");
    }
    void start_rtl() {
        rtl_pump.start([this] {
            try {
                const auto result=rtlsdr_read_async(rtl_device,&Impl::receive_rtl,this,15,static_cast<uint32_t>(block_bytes));
                if(run)fail("RTL-SDR receive stream stopped unexpectedly (driver code "+std::to_string(result)+")");
            } catch(const std::exception& e) { fail(e.what()); }
              catch(...) { fail("RTL-SDR receive thread failed"); }
            input_finished=true;wake.notify_all();
        },[this] { if(rtl_device)rtlsdr_cancel_async(rtl_device); });
    }
#endif
    void close_hardware() {
        // Input has already stopped and the worker has joined before this method.
#ifdef OVMESH_HAVE_HACKRF
        if(device){hackrf_close(device);device=nullptr;}
        if(hackrf_initialized){hackrf_exit();hackrf_initialized=false;}
#endif
#ifdef OVMESH_HAVE_RTLSDR
        if(rtl_device){rtlsdr_close(rtl_device);rtl_device=nullptr;}
#endif
    }
    void position(PositionFix f) {
        std::lock_guard lock(mutex);current_fix=f;
        fix_history.push_back(f);if(fix_history.size()>24000)fix_history.erase(fix_history.begin(),fix_history.begin()+2000);
        if(view.historical)return; // A live receiver fix must not alter a reopened survey's track.
        view.gps_status=live_position_status_locked();
        if(f.valid){view.track.push_back(f);if(view.track.size()>12000)view.track.erase(view.track.begin(),view.track.begin()+1000);}
    }
    std::string live_position_status_locked() const {
        const auto connection=gps.status();
        if(connection.state==GpsConnectionState::Disconnected&&current_fix&&current_fix->valid&&current_fix->manual)
            return "Manual fixed receiver position";
        return connection.detail;
    }
    std::optional<PositionFix> associated_position(double observed_monotonic=0) {
        std::lock_guard lock(mutex);
        const double target=observed_monotonic>0?observed_monotonic:monotonic_now();
        // Observation association is historical; missing coverage must not
        // overwrite the separate, current serial connection status.
        return position_at(fix_history,target);
    }
    void clear_live_position_locked(const char* reason) {
        current_fix.reset();
        PositionFix marker;marker.monotonic_seconds=monotonic_now();marker.utc_seconds=utc_now();marker.source=reason;
        fix_history.push_back(std::move(marker));
        if(!view.historical)view.gps_status=gps.status().detail;
    }
    void simulate(ReceiverConfig c) {
        try {
            auto first_enabled=std::find_if(c.lanes.begin(),c.lanes.end(),[](const auto& l){return l.enabled&&l.protocol=="Meshtastic";});
            LaneConfig lane=first_enabled==c.lanes.end()?LaneConfig{}:*first_enabled;
            PhyConfig phy;phy.bandwidth_hz=lane.bandwidth_hz;phy.spreading_factor=lane.spreading_factor;phy.coding_rate=lane.coding_rate;
            auto profile=protocol::synthetic_profile();auto frame=protocol::synthetic_text_frame(profile,1,"Synthetic RF survey — native LoRa decoding");auto wave=modulate_lora(frame,phy,c.sample_rate);
            std::fill(frame.begin(),frame.end(),uint8_t{});
            const double period=std::max(3.0,double(wave.size())/c.sample_rate+1.0);
            const uint64_t period_samples=static_cast<uint64_t>(std::llround(period*c.sample_rate));
            const uint64_t wave_start=static_cast<uint64_t>(c.sample_rate)*3/10;
            const uint64_t wave_end=wave_start+wave.size();
            const uint64_t other_period=static_cast<uint64_t>(c.sample_rate)*13/10;
            const uint64_t other_on=static_cast<uint64_t>(c.sample_rate)*24/100;
            // The synthetic transmitter keeps its nominal RF frequency. Moving
            // the modeled tuner shifts both mesh and other RF within the samples.
            const auto step=std::polar(1.0,2*pi*(static_cast<double>(lane.frequency_hz)-static_cast<double>(tuned_center_hz(c)))/c.sample_rate);
            const auto other_step=std::polar(1.0,2*pi*(static_cast<double>(c.survey_span_hz)*.32-static_cast<double>(c.tuning_offset_hz))/c.sample_rate);
            std::complex<double> osc{1,0},other{1,0};uint32_t random=0x18237491;uint64_t sample=0,period_position=0,other_position=0;std::array<int8_t,block_bytes> block{};
            auto noise=[&](){random^=random<<13;random^=random>>17;random^=random<<5;return (double(random&65535)/32768.0-1.0)*.009;};
            // Preparation is not RF time. Pace the completed synthetic source
            // from here rather than burst old samples to catch up with setup.
            // Synthetic acquisition timestamps still use their sample origin.
            const double simulation_started=monotonic_now();
            while(run){for(size_t i=0;i<block.size()/2;++i,++sample){Complex x{};
                    if(period_position>=wave_start&&period_position<wave_end)x=wave[period_position-wave_start]*Complex(osc)*.22f;
                    if(other_position<other_on)x+=Complex(other)*.10f;
                    x+=Complex(static_cast<float>(noise()),static_cast<float>(noise()));
                    block[i*2]=static_cast<int8_t>(std::clamp(x.real(),-.99f,.99f)*127);block[i*2+1]=static_cast<int8_t>(std::clamp(x.imag(),-.99f,.99f)*127);
                    osc*=step;other*=other_step;if((sample&4095)==0){osc/=std::abs(osc);other/=std::abs(other);}
                    if(++period_position==period_samples)period_position=0;if(++other_position==other_period)other_position=0;
                }
                deliver(block.data(),block.size(),monotonic_now());
                double target=simulation_started+static_cast<double>(sample)/c.sample_rate;
                std::unique_lock lock(wake_mutex);wake.wait_for(lock,std::chrono::duration<double>(std::max(0.0,target-monotonic_now())),[&]{return !run.load();});
            }
            std::fill(wave.begin(),wave.end(),Complex{});std::fill(block.begin(),block.end(),int8_t{});
        }catch(const std::exception& e){fail(e.what());}
        input_finished=true;wake.notify_all();
    }
    void process(ReceiverConfig c) {
        try {
            struct Lane {size_t index;std::unique_ptr<Downconverter> convert;std::unique_ptr<LoRaReceiver> receive;std::vector<Complex> samples;uint64_t delivered=0;double segment_start=0;};
            std::vector<Lane> lanes;
            // Demo material belongs only to this synthetic worker invocation.
            // Configured user profiles are never replaced by, or mixed with, it.
            std::array<protocol::Profile,1> synthetic_profiles;
            if(c.synthetic)synthetic_profiles[0]=protocol::synthetic_profile();
            // Lifecycle locking prevents edits until this worker has joined.
            // Synthetic keys never enter or combine with the user's keyring.
            const std::span<const protocol::Profile> keyring=c.synthetic
                ? std::span<const protocol::Profile>(synthetic_profiles)
                : std::span<const protocol::Profile>(profiles);
            for(size_t i=0;i<c.lanes.size();++i){const auto& l=c.lanes[i];if(!l.enabled||l.protocol!="Meshtastic")continue;PhyConfig pc;pc.bandwidth_hz=l.bandwidth_hz;pc.spreading_factor=l.spreading_factor;pc.coding_rate=l.coding_rate;
                lanes.push_back({i,std::make_unique<Downconverter>(c.sample_rate,l.bandwidth_hz,static_cast<int64_t>(l.frequency_hz)-static_cast<int64_t>(c.center_hz)),std::make_unique<LoRaReceiver>(pc),{},0,0});}
            std::vector<Complex> samples(block_bytes/2);
            SpectrumProcessor spectrum(c.center_hz,c.sample_rate,c.survey_span_hz,c.activity_threshold_dbfs);
            std::vector<FrequencySummary> bins(spectrum.bin_count()),window_bins;
            std::vector<double> power_sum(bins.size()),window_power(bins.size());
            for(size_t i=0;i<bins.size();++i) {
                bins[i].center_hz=static_cast<uint64_t>(std::llround(spectrum.first_center_hz()+i*spectrum.bin_width_hz()));
                bins[i].width_hz=static_cast<uint32_t>(std::llround(spectrum.bin_width_hz()));
            }
            window_bins=bins;
            uint64_t window_id=0,gap_id=0,measured_samples=0;
            double window_start=0,last_measurement_end=0;
            bool window_started=false;
            uint64_t expected=0,event_id=0;double last_publish=0,last_save=0,last_fix_saved=-1;
            std::vector<float> display(1024,-180);
            bool time_anchored=c.synthetic;double input_epoch=0;
            auto discovery=std::move(prepared_discovery);
            std::unique_ptr<DiscoveryObservations> discovered;
            uint64_t discovery_gap_id=0, discovery_observation_count=0;
            bool discovery_metadata_failed=false;
            double last_discovery_poll=0;
            if(discovery) {
                try {
                    discovered=std::make_unique<DiscoveryObservations>(c.sample_rate,discovery->snapshot().subbands.size());
                } catch(const std::exception&) {
                    // Optional classification failure must not prevent RF measurements.
                    discovery.reset();
                    std::lock_guard lock(mutex);view.discovery.enabled=true;view.discovery.failed=true;
                    view.discovery.fault="LoRa discovery could not initialize; spectrum recording continues";
                }
            }
            auto poll_discovery=[&] {
                if(!discovery)return;
                for(const auto& result:discovery->take_results()) {
                    DiscoveryObservations::Update update{};
                    try {
                        update=discovered->observe(result.waveform,result.subband_index,
                            result.first_input_anchor,result.input_sample_stride);
                    } catch(const std::invalid_argument&) {
                        discovery_metadata_failed=true;continue;
                    } catch(const std::overflow_error&) {
                        discovery_metadata_failed=true;continue;
                    }
                    const auto& recent=discovered->observations();
                    const auto found=std::find_if(recent.begin(),recent.end(),[&](const auto& v){return v.id==update.id;});
                    if(found==recent.end())continue;
                    WaveformObservation observation;observation.id=found->id;
                    observation.center_hz=found->received_center_hz;observation.bandwidth_hz=found->bandwidth_hz;
                    observation.spreading_factor=found->spreading_factor;
                    observation.first_observed_elapsed=input_epoch+found->first_observed_upchirp_input_sample/c.sample_rate;
                    observation.delimiter_elapsed=input_epoch+found->delimiter_input_sample/c.sample_rate;
                    observation.delimiter_utc=started_utc+observation.delimiter_elapsed;
                    observation.up_match=found->up_match;observation.down_match=found->down_match;
                    observation.contributing_subbands=static_cast<unsigned>(found->contributing_subbands.size());
                    observation.complete_in_requested_range=observation.center_hz-observation.bandwidth_hz/2.>=double(c.center_hz)-c.survey_span_hz/2. &&
                        observation.center_hz+observation.bandwidth_hz/2.<=double(c.center_hz)+c.survey_span_hz/2.;
                    observation.association_ambiguous=found->association_ambiguous;
                    observation.receiver_position=associated_position(started+observation.delimiter_elapsed);
                    if(store)store->append(observation);
                    if(!update.merged)++discovery_observation_count;
                    std::lock_guard lock(mutex);
                    const auto previous=std::find_if(view.waveforms.begin(),view.waveforms.end(),[&](const auto& v){return v.id==observation.id;});
                    if(previous!=view.waveforms.end())*previous=observation;
                    else {view.waveforms.insert(view.waveforms.begin(),observation);if(view.waveforms.size()>256)view.waveforms.pop_back();}
                }
                for(const auto& gap:discovery->take_gaps()) {
                    DiscoveryGap record;record.id=++discovery_gap_id;record.first_input_sample=gap.first_input_sample;
                    record.end_input_sample=gap.end_input_sample;
                    record.subband_index=gap.subband_index==DiscoveryWorker::all_subbands?-1:static_cast<int>(gap.subband_index);
                    switch(gap.reason) {
                        case DiscoveryWorker::GapReason::source_queue_full:record.reason="source_queue_full";break;
                        case DiscoveryWorker::GapReason::input_discontinuity:record.reason="input_discontinuity";break;
                        case DiscoveryWorker::GapReason::input_too_large:record.reason="input_too_large";break;
                        case DiscoveryWorker::GapReason::invalid_input_order:record.reason="invalid_input_order";break;
                        case DiscoveryWorker::GapReason::invalid_sample_coordinate:record.reason="invalid_sample_coordinate";break;
                        case DiscoveryWorker::GapReason::processing_failure:record.reason="processing_failure";break;
                    }
                    if(store)store->append(record);
                }
                const auto progress=discovery->snapshot();DiscoveryStatus status;
                status.enabled=true;status.finished=progress.finished;status.failed=progress.failed;status.fault=progress.fault;
                if(discovery_metadata_failed){status.failed=true;status.fault="Invalid waveform metadata rejected; discovery results are incomplete";}
                status.accepted_input_samples=progress.accepted_input_samples;status.rejected_input_samples=progress.rejected_input_samples;
                status.channelized_input_samples=progress.channelized_input_samples;status.abandoned_input_samples=progress.abandoned_input_samples;
                status.source_queue_drops=progress.source_queue_drops;status.stream_resets=progress.stream_resets;
                status.result_overflows=progress.result_overflows;status.gap_overflows=progress.gap_overflows;
                status.observations=discovery_observation_count;
                for(size_t i=0;i<progress.subbands.size();++i) {
                    const auto& band=progress.subbands[i];DiscoveryBandCoverage record;
                    record.subband_index=static_cast<unsigned>(i);record.center_hz=band.band.center_hz;
                    record.processed_samples=band.processed_output_samples;record.abandoned_samples=band.abandoned_output_samples;
                    record.source_gap_input_samples=band.source_gap_input_samples;
                    record.candidate_limit_hits=band.candidate_limit_hits;record.track_limit_hits=band.track_limit_hits;
                    status.bands.push_back(record);
                }
                std::lock_guard lock(mutex);view.discovery=std::move(status);
            };
            {std::lock_guard lock(mutex);view.spectrum_bin_width_hz=spectrum.bin_width_hz();
                view.spectrum_enbw_hz=spectrum.enbw_hz();view.spectrum_fft_size=4096;}
            auto publish_bins=[&]{view.frequencies=bins;};
            auto checkpoint=[&] {
                if(!store)return;
                auto fix=associated_position();
                if(fix&&fix->monotonic_seconds!=last_fix_saved){store->append(*fix);last_fix_saved=fix->monotonic_seconds;}
                Snapshot saved;uint64_t request;
                {std::lock_guard lock(mutex);publish_bins();saved=view;request=save_requested;}
                // FULL-synchronous COMMIT is the durability boundary. A WAL
                // merge is not required for recovery or SQLite-aware copying.
                store->update(saved);
                {std::lock_guard lock(mutex);save_completed=request;}
                save_finished.notify_all();last_save=monotonic_now();
            };
            auto save_window=[&](double end) {
                if(!store||!window_started||end<=window_start||window_bins.empty()||window_bins[0].observed_seconds==0)return;
                SurveyWindow record;record.id=++window_id;
                record.elapsed_start_seconds=window_start;record.elapsed_end_seconds=end;
                record.utc_start_seconds=started_utc+window_start;record.utc_end_seconds=started_utc+end;
                record.receiver_position=associated_position(started+end);
                record.frequencies=window_bins;store->append(record);
                std::fill(window_power.begin(),window_power.end(),0.0);
                for(auto& bin:window_bins){bin.mean_dbfs=-180;bin.peak_dbfs=-180;bin.observed_seconds=0;bin.active_seconds=0;}
                window_start=end;
            };
            auto stamp=[&](auto& record) {
                record.elapsed_start_seconds=input_epoch+double(record.first_sample)/c.sample_rate;
                record.elapsed_end_seconds=input_epoch+double(record.end_sample)/c.sample_rate;
                record.utc_start_seconds=started_utc+record.elapsed_start_seconds;
                record.utc_end_seconds=started_utc+record.elapsed_end_seconds;
                record.receiver_start=associated_position(started+record.elapsed_start_seconds);
                record.receiver_end=associated_position(started+record.elapsed_end_seconds);
                record.quality|=SurveyUncalibrated;
                if(!c.synthetic)record.quality|=SurveyUpstreamLossUnknown;
                if(!record.receiver_start||!record.receiver_end)record.quality|=SurveyPositionMissing;
            };
            BurstGrouper bursts(double(c.center_hz));
            auto on_burst=[&](SpectrumBurst burst) {
                std::lock_guard lock(mutex); ++view.spectrum_bursts;
                view.recent_spectrum_bursts.insert(view.recent_spectrum_bursts.begin(), std::move(burst));
                if(view.recent_spectrum_bursts.size()>200)view.recent_spectrum_bursts.pop_back();
            };
            auto on_tile=[&](SpectrumTile tile) {
                stamp(tile);
                bursts.consume(tile,on_burst);
                const double dt=double(tile.end_sample-tile.first_sample)/c.sample_rate;
                const double frame_dt=4096.0/c.sample_rate;
                const size_t stride=(bins.size()+7)/8;
                for(size_t i=0;i<bins.size();++i) {
                    double active=0;
                    for(size_t f=0;f<tile.frame_count;++f)
                        if(tile.activity[f*stride+i/8]&(1u<<(i%8)))active+=frame_dt;
                    const double energy=std::pow(10.0,tile.mean_dbfs[i]/10.0)*dt;
                    auto& bin=bins[i];auto& wb=window_bins[i];
                    bin.observed_seconds+=dt;wb.observed_seconds+=dt;
                    bin.active_seconds+=active;wb.active_seconds+=active;
                    power_sum[i]+=energy;window_power[i]+=energy;
                    bin.mean_dbfs=10*std::log10(std::max(power_sum[i]/bin.observed_seconds,1e-18));
                    wb.mean_dbfs=10*std::log10(std::max(window_power[i]/wb.observed_seconds,1e-18));
                    bin.peak_dbfs=bin.observed_seconds<=dt?tile.peak_dbfs[i]:std::max(bin.peak_dbfs,double(tile.peak_dbfs[i]));
                    wb.peak_dbfs=wb.observed_seconds<=dt?tile.peak_dbfs[i]:std::max(wb.peak_dbfs,double(tile.peak_dbfs[i]));
                }
                // Max pooling keeps a narrow peak visible when the canvas has
                // fewer columns than measurement bins. Logs retain every bin.
                for(size_t i=0;i<display.size();++i) {
                    const size_t lo=i*bins.size()/display.size();
                    const size_t hi=std::max(lo+1,(i+1)*bins.size()/display.size());
                    float peak=-180;
                    for(size_t j=lo;j<std::min(hi,bins.size());++j)peak=std::max(peak,tile.peak_dbfs[j]);
                    display[i]=.4f*peak+.6f*display[i];
                }
                if(store)store->append(tile);
                last_measurement_end=tile.elapsed_end_seconds;
                if(last_measurement_end-window_start>=5.0)save_window(last_measurement_end);
                std::lock_guard lock(mutex);++view.spectrum_tiles;view.clipped_samples+=tile.clipped_samples;
                measured_samples+=tile.end_sample-tile.first_sample;
                view.background_dbfs=tile.background_dbfs;view.measurement_seconds=double(measured_samples)/c.sample_rate;
            };
            auto on_event=[&](SpectrumEvent event) {
                stamp(event);
                if(event.lower_hz<=double(c.center_hz)+2*spectrum.bin_width_hz()&&
                   event.upper_hz>=double(c.center_hz)-2*spectrum.bin_width_hz())event.quality|=SurveyDcSuspect;
                if(store)store->append(event);
                std::lock_guard lock(mutex);++view.spectrum_events;
                view.recent_spectrum_events.insert(view.recent_spectrum_events.begin(),std::move(event));
                if(view.recent_spectrum_events.size()>200)view.recent_spectrum_events.pop_back();
            };
            auto record_gap=[&](uint64_t first,uint64_t end,const std::string& reason) {
                if(end<=first)return;
                CoverageGap gap;gap.id=++gap_id;gap.missing_samples=end-first;gap.reason=reason;
                gap.elapsed_start_seconds=input_epoch+double(first)/c.sample_rate;
                gap.elapsed_end_seconds=input_epoch+double(end)/c.sample_rate;
                gap.utc_start_seconds=started_utc+gap.elapsed_start_seconds;
                gap.utc_end_seconds=started_utc+gap.elapsed_end_seconds;
                if(store)store->append(gap);
            };
            while(run||!input_finished||tail.load(std::memory_order_relaxed)!=head.load(std::memory_order_acquire)){
                bool requested=false;{std::lock_guard lock(mutex);requested=save_requested>save_completed;}
                if(requested)checkpoint();
                auto t=tail.load(std::memory_order_relaxed);if(t==head.load(std::memory_order_acquire)){std::unique_lock lock(wake_mutex);wake.wait_for(lock,std::chrono::milliseconds(30));
                    // No worker thread reads or calls through the device handle.
                    // This watchdog observes callback progress without racing close/stop.
                    if(run&&!c.synthetic&&monotonic_now()-last_input_arrival.load(std::memory_order_relaxed)>2){
                        if(store&&time_anchored){CoverageGap g;g.id=++gap_id;g.reason="source_stall";
                            g.elapsed_start_seconds=input_epoch+double(expected)/c.sample_rate;
                            g.elapsed_end_seconds=std::max(g.elapsed_start_seconds,monotonic_now()-started);
                            g.utc_start_seconds=started_utc+g.elapsed_start_seconds;g.utc_end_seconds=started_utc+g.elapsed_end_seconds;
                            if(g.elapsed_end_seconds>g.elapsed_start_seconds)store->append(g);}
                        fail(std::string(receiver_source_name(c))+" delivered no sample blocks for two seconds; stream may have stopped or disconnected");break;}
                    continue;}
                const auto begin=monotonic_now();auto& block=pool[t%pool_blocks];size_t count=block.size/2;bool gap=block.first!=expected;
                if(!time_anchored){input_epoch=std::max(0.0,block.arrival-started-static_cast<double>(block.first+count)/c.sample_rate);time_anchored=true;}
                if(!window_started){window_start=input_epoch+static_cast<double>(block.first)/c.sample_rate;window_started=true;}
                if(gap){const auto partial=spectrum.pending_samples();spectrum.gap(on_tile,on_event);
                    if(partial)record_gap(expected-partial,expected,"partial_fft");
                    record_gap(expected,block.first,"application_drop");
                    for(auto& lane:lanes){lane.convert->reset();lane.receive->reset();lane.delivered=0;lane.segment_start=static_cast<double>(block.first)/c.sample_rate;}std::lock_guard lock(mutex);for(auto& h:view.lane_health)++h.resets;}
                expected=block.first+count;
                if(!c.synthetic&&c.hardware_receiver==HardwareReceiver::RtlSdr) {
                    for(size_t i=0;i<count;++i)samples[i]=rtl_iq_sample(static_cast<uint8_t>(block.data[2*i]),static_cast<uint8_t>(block.data[2*i+1]));
                } else for(size_t i=0;i<count;++i)samples[i]={block.data[2*i]/128.0f,block.data[2*i+1]/128.0f};
                const uint64_t first=block.first;std::fill_n(block.data.begin(),block.size,int8_t{});tail.store(t+1,std::memory_order_release);
                // Publish accepted input before a tile callback publishes its
                // measured subset, so a live snapshot cannot exceed 100% duty.
                {std::lock_guard lock(mutex);view.delivered_samples+=count;view.input_seconds=double(view.delivered_samples)/c.sample_rate;}
                spectrum.feed(std::span<const Complex>(samples.data(),count),first,on_tile,on_event);
                if(discovery)discovery->submit(std::span<const Complex>(samples.data(),count),first);
                for(auto& lane:lanes){const auto& config=c.lanes[lane.index];lane.convert->feed(std::span<const Complex>(samples.data(),count),lane.samples);lane.delivered+=lane.samples.size();
                    lane.receive->feed(lane.samples,[&](PhyFrame&& f){Reception r;r.id=++event_id;r.elapsed_seconds=input_epoch+lane.segment_start+lane.convert->first_output_seconds()+double(f.first_sample)/config.bandwidth_hz;r.utc_seconds=started_utc+r.elapsed_seconds;r.frequency_hz=config.frequency_hz;r.bandwidth_hz=config.bandwidth_hz;r.spreading_factor=config.spreading_factor;r.coding_rate=f.coding_rate;r.duration_seconds=double(f.last_sample-f.first_sample)/config.bandwidth_hz;r.snr_db=std::isfinite(f.snr_db)?f.snr_db:0;r.frequency_error_hz=std::isfinite(f.frequency_error_hz)?f.frequency_error_hz:0;r.header_valid=f.header_valid;r.crc_valid=f.payload_crc_valid;r.lane_label=config.label;
                        r.decoded=protocol::decode_meshtastic(f.bytes,f.header_valid&&f.payload_crc_valid,keyring);std::fill(f.bytes.begin(),f.bytes.end(),uint8_t{});r.receiver_position=associated_position(started+r.elapsed_seconds);
                        if(store)store->append(r);
                        std::lock_guard lock(mutex);auto& h=view.lane_health[lane.index];++h.frames;if(!r.crc_valid)++h.crc_failures;if(r.decoded.authorized){++h.decoded;++view.authorized_messages;}++view.total_receptions;view.receptions.insert(view.receptions.begin(),std::move(r));if(view.receptions.size()>512)view.receptions.pop_back();
                    });
                    std::lock_guard lock(mutex);view.lane_health[lane.index].processed_seconds+=double(lane.samples.size())/config.bandwidth_hz;
                    view.lane_health[lane.index].phy=lane.receive->diagnostics();
                }
                std::fill_n(samples.begin(),count,Complex{});
                double now=monotonic_now();
                if(now-last_discovery_poll>=.1){poll_discovery();last_discovery_poll=now;}
                {std::lock_guard lock(mutex);if(view.error.empty()){view.running=run.load();view.state=run?("Receiving "+std::string(receiver_source_name(c))):"Stopping";}view.dropped_samples=dropped.load();view.elapsed_seconds=now-started;view.processing_load=.1*((now-begin)/(double(count)/c.sample_rate))+.9*view.processing_load;
                    if(now-last_publish>=.05){view.spectrum_dbfs=display;++view.spectrum_sequence;publish_bins();last_publish=now;}}
                if(store&&now-last_save>=1)checkpoint();

                (void)first;
            }
            const auto partial=spectrum.pending_samples();spectrum.finish(on_tile,on_event);
            bursts.finish(on_burst);
            if(partial)record_gap(expected-partial,expected,"partial_fft");
            const auto produced=next_sample.load();
            if(produced>expected)record_gap(expected,produced,"application_drop");
            std::fill(samples.begin(),samples.end(),Complex{});
            if(discovery){discovery->finish();poll_discovery();}
            save_window(last_measurement_end);
            Snapshot final;{std::lock_guard lock(mutex);publish_bins();view.spectrum_dbfs=display;view.elapsed_seconds=monotonic_now()-started;view.dropped_samples=dropped.load();final=view;}
            if(store){store->update(final,true);std::lock_guard lock(mutex);final_save_confirmed=true;save_completed=save_requested;}
        }catch(const std::exception& e){fail(e.what());}
        std::lock_guard lock(mutex);view.running=false;view.recording=false;if(view.state!="Failed")view.state="Stopped";save_finished.notify_all();
    }
    // Caller owns lifecycle. Keeping stop/start in the same critical section
    // prevents a concurrent start from clearing the previous worker's buffers.
    void stop_locked() {
        run=false;wake.notify_all();
#ifdef OVMESH_HAVE_HACKRF
        if(device)hackrf_stop_rx(device);
#endif
#ifdef OVMESH_HAVE_RTLSDR
        rtl_pump.stop();
#endif
        if(producer.joinable())producer.join();input_finished=true;wake.notify_all();if(worker.joinable())worker.join();
        close_hardware();
        for(auto& b:pool)std::fill(b.data.begin(),b.data.end(),int8_t{});pool.clear();store.reset();
        prepared_discovery.reset();
        std::lock_guard lock(mutex);view.running=false;view.recording=false;if(view.state!="Failed"&&view.state!="Idle"&&!view.historical)view.state="Stopped";
    }
    // Caller owns lifecycle. Never reads/writes the worker's SQLite connection.
    bool save_locked(std::string& error) {
        {
            std::unique_lock lock(mutex);
            if(view.historical){error="This session is historical and read-only; use Save copy to keep another copy";return false;}
            if(saved_path.empty()){error="This session was not recorded. Enable recording before starting a new session; earlier memory-only history cannot be recovered";return false;}
            if(view.running&&run) {
                const auto request=++save_requested;wake.notify_all();
                if(!save_finished.wait_for(lock,std::chrono::seconds(5),[&]{return save_completed>=request||!view.running;})) {
                    error="Save was not confirmed within five seconds. Recording may still be saving; keep this session open and check its status";return false;
                }
                if(save_completed>=request){error.clear();return true;}
                error="Save could not be confirmed: "+(view.error.empty()?std::string("recording stopped before the checkpoint"):view.error);return false;
            }
        }
        stop_locked();
        {
            std::lock_guard lock(mutex);
            if(!final_save_confirmed){error="The final recording save was not confirmed. Keep this session open; Save copy can preserve its last readable checkpoint";return false;}
        }
        try {SessionStore verified;verified.open_readonly(saved_path);const auto saved=verified.read();
            std::lock_guard lock(mutex);
            if(saved.session_id!=view.session_id||saved.delivered_samples!=view.delivered_samples||saved.total_receptions!=view.total_receptions)
                throw std::runtime_error("Saved session does not match the current finalized measurements");
            error.clear();return true;
        }catch(const std::exception& e){error=std::string("Cannot verify the saved session: ")+e.what();return false;}
    }
};

Engine::Engine():impl_(std::make_unique<Impl>()){}
Engine::~Engine(){stop();disconnect_gps();}
std::string Engine::version(){return OVMESH_VERSION;}
bool Engine::start(const ReceiverConfig& requested,bool hardware_permission,std::string& error) {
    std::lock_guard life(impl_->lifecycle);auto& p=*impl_;p.stop_locked();
    auto config=requested;
    try {
        validate(config);
        if(!config.synthetic&&!hardware_permission)throw std::runtime_error("Explicit permission is required before opening "+std::string(receiver_source_name(config)));
#ifndef OVMESH_HAVE_HACKRF
        if(!config.synthetic&&config.hardware_receiver==HardwareReceiver::HackRf)throw std::runtime_error("This build does not include libhackrf");
#endif
#ifndef OVMESH_HAVE_RTLSDR
        if(!config.synthetic&&config.hardware_receiver==HardwareReceiver::RtlSdr)throw std::runtime_error("This build does not include librtlsdr");
#endif
        const bool capability=p.view.hardware_available,rtl_capability=p.view.rtl_sdr_available;{
            std::lock_guard lock(p.mutex);p.view=Snapshot{};p.view.hardware_available=capability;p.view.rtl_sdr_available=rtl_capability;p.view.config=config;p.view.state="Starting";p.view.session_id=std::to_string(static_cast<uint64_t>(utc_now()*1000000));p.view.upstream_loss_unknown=!config.synthetic;p.save_requested=0;p.save_completed=0;p.final_save_confirmed=false;p.saved_path.clear();
            for(const auto& l:config.lanes){LaneHealth h;h.label=l.label;h.frequency_hz=l.frequency_hz;h.state=!l.enabled?"disabled":l.protocol!="Meshtastic"?"unsupported protocol":"searching; one frame at a time";p.view.lane_health.push_back(h);}
            p.fix_history.clear();if(p.current_fix)p.fix_history.push_back(*p.current_fix);
            if(p.current_fix&&p.current_fix->valid)p.view.track.push_back(*p.current_fix);
            p.view.gps_status=p.live_position_status_locked();}
        if(config.discover_lora) {
            {std::lock_guard lock(p.mutex);p.view.discovery.enabled=true;}
            try {
                // Allocate queues/filter histories before opening the receiver;
                // discovery startup must not consume the acquisition queue.
                p.prepared_discovery=std::make_unique<DiscoveryWorker>(config.sample_rate,double(config.center_hz),
                    double(config.center_hz)-config.survey_span_hz/2.,double(config.center_hz)+config.survey_span_hz/2.);
            } catch(const std::exception&) {
                std::lock_guard lock(p.mutex);p.view.discovery.failed=true;
                p.view.discovery.fault="LoRa discovery could not initialize; spectrum recording continues";
            }
        }
        p.pool.clear();p.pool.resize(pool_blocks);p.head=0;p.tail=0;p.next_sample=0;p.dropped=0;p.input_finished=false;p.started=monotonic_now();p.last_input_arrival=p.started;p.started_utc=utc_now();
#ifdef OVMESH_HAVE_HACKRF
        if(!config.synthetic&&config.hardware_receiver==HardwareReceiver::HackRf){
            auto require=[](int result){if(result!=HACKRF_SUCCESS)throw std::runtime_error(std::string("HackRF operation failed: ")+hackrf_error_name(static_cast<hackrf_error>(result)));};
            require(hackrf_init());p.hackrf_initialized=true;
            if(config.device_serial.empty())require(hackrf_open(&p.device));else require(hackrf_open_by_serial(config.device_serial.c_str(),&p.device));
            require(hackrf_set_sample_rate(p.device,config.sample_rate));require(hackrf_set_baseband_filter_bandwidth(p.device,hackrf_compute_baseband_filter_bw_round_down_lt(config.sample_rate+1)));
            require(hackrf_set_freq(p.device,tuned_center_hz(config)));require(hackrf_set_lna_gain(p.device,config.lna_gain));require(hackrf_set_vga_gain(p.device,config.vga_gain));require(hackrf_set_amp_enable(p.device,config.amplifier?1:0));require(hackrf_set_antenna_enable(p.device,0));
        }
#endif
#ifdef OVMESH_HAVE_RTLSDR
        if(!config.synthetic&&config.hardware_receiver==HardwareReceiver::RtlSdr) {
            p.configure_rtl(config);
            std::lock_guard lock(p.mutex);p.view.config=config;
        }
#endif
        if(!config.session_path.empty()){p.store=std::make_unique<SessionStore>();p.store->create(config.session_path,config,p.view.session_id);p.saved_path=config.session_path;std::lock_guard lock(p.mutex);p.view.recording=true;}
        // Hardware setup is not RF exposure. Anchor elapsed/UTC immediately
        // before input startup, including the no-callback watchdog baseline.
        p.started=monotonic_now();p.started_utc=utc_now();p.last_input_arrival=p.started;
        p.run=true;{std::lock_guard lock(p.mutex);p.view.running=true;}
#ifdef OVMESH_HAVE_HACKRF
        if(p.device) {
            const auto result=hackrf_start_rx(p.device,&Impl::receive,&p);
            if(result!=HACKRF_SUCCESS)throw std::runtime_error(std::string("HackRF operation failed: ")+hackrf_error_name(static_cast<hackrf_error>(result)));
        }
#endif
        p.worker=std::thread([&p,config]{p.process(config);});if(config.synthetic)p.producer=std::thread([&p,config]{p.simulate(config);});
#ifdef OVMESH_HAVE_RTLSDR
        if(p.rtl_device)p.start_rtl();
#endif
        error.clear();return true;
    }catch(const std::exception& e){error=e.what();p.run=false;
#ifdef OVMESH_HAVE_HACKRF
        if(p.device)hackrf_stop_rx(p.device);
#endif
#ifdef OVMESH_HAVE_RTLSDR
        p.rtl_pump.stop();
#endif
        if(p.producer.joinable())p.producer.join();p.input_finished=true;p.wake.notify_all();if(p.worker.joinable())p.worker.join();
        p.close_hardware();
        p.prepared_discovery.reset();p.store.reset();p.fail(error);return false;}
}
void Engine::stop(){auto& p=*impl_;std::lock_guard life(p.lifecycle);p.stop_locked();}
Snapshot Engine::snapshot() const {
    std::lock_guard lock(impl_->mutex);auto result=impl_->view;
    if(!result.historical)result.gps_status=impl_->live_position_status_locked();
    return result;
}
bool Engine::set_channel_key(size_t slot,const std::string& name,const std::string& input,std::string& error) {
    auto& p=*impl_;std::lock_guard life(p.lifecycle);
    if(p.run||p.worker.joinable()){error="Stop reception before changing keys";return false;}
    if(slot>=p.profiles.size()||name.empty()||name.size()>32){error="Choose a key record from 1 through 16 and a channel name of 1 through 32 bytes";return false;}
    auto key=protocol::ChannelKey::from_user_input(input);
    if(!key){error="Enter a 16- or 32-byte key as hex or padded Base64, or the explicitly authorized AQ== public shorthand";return false;}
    protocol::Profile replacement;replacement.id="key-"+std::to_string(slot);replacement.label=name;
    replacement.channel_name=name;replacement.restrict_channel_name=true;replacement.keys.push_back(std::move(*key));
    p.profiles[slot]=std::move(replacement);error.clear();return true;
}
bool Engine::set_survey_key(size_t slot,const std::string& label,const std::string& input,std::string& error) {
    auto& p=*impl_;std::lock_guard life(p.lifecycle);
    if(p.run||p.worker.joinable()){error="Stop reception before changing keys";return false;}
    if(slot>=p.profiles.size()||label.empty()||label.size()>80){error="Choose a key record from 1 through 16 and a label of 1 through 80 bytes";return false;}
    auto key=protocol::ChannelKey::from_user_input(input);
    if(!key){error="Enter a 16- or 32-byte key as hex or padded Base64, or the explicitly authorized AQ== public shorthand";return false;}
    protocol::Profile replacement;replacement.id="key-"+std::to_string(slot);replacement.label=label;
    replacement.restrict_channel_name=false;replacement.keys.push_back(std::move(*key));
    p.profiles[slot]=std::move(replacement);error.clear();return true;
}
void Engine::clear_keys(){std::lock_guard life(impl_->lifecycle);impl_->stop_locked();for(auto& p:impl_->profiles)p.keys.clear();}
bool Engine::has_channel_key(size_t slot,const std::string& name)const{auto& p=*impl_;std::lock_guard life(p.lifecycle);return slot<p.profiles.size()&&!p.profiles[slot].keys.empty()&&p.profiles[slot].restrict_channel_name&&p.profiles[slot].channel_name==name;}
bool Engine::has_survey_key(size_t slot)const{auto& p=*impl_;std::lock_guard life(p.lifecycle);return slot<p.profiles.size()&&!p.profiles[slot].keys.empty()&&!p.profiles[slot].restrict_channel_name;}
size_t Engine::configured_key_count()const{auto& p=*impl_;std::lock_guard life(p.lifecycle);return static_cast<size_t>(std::count_if(p.profiles.begin(),p.profiles.end(),[](const auto& profile){return !profile.keys.empty();}));}
std::vector<KeyRecordInfo> Engine::key_records()const {
    auto& p=*impl_;std::lock_guard life(p.lifecycle);std::vector<KeyRecordInfo> result;result.reserve(p.profiles.size());
    for(size_t i=0;i<p.profiles.size();++i){const auto& profile=p.profiles[i];result.push_back({i,profile.label,profile.channel_name,profile.restrict_channel_name,!profile.keys.empty()});}
    return result;
}
void Engine::set_fixed_position(double lat,double lon,std::optional<double> alt){if(!std::isfinite(lat)||!std::isfinite(lon)||std::abs(lat)>90||std::abs(lon)>180||(alt&&!std::isfinite(*alt)))return;std::lock_guard life(impl_->lifecycle);impl_->gps.stop();PositionFix f;f.latitude=lat;f.longitude=lon;f.altitude_m=alt;f.valid=true;f.manual=true;f.source="manual fixed";f.monotonic_seconds=monotonic_now();f.utc_seconds=utc_now();impl_->position(f);}
void Engine::clear_position(){std::lock_guard lock(impl_->mutex);impl_->clear_live_position_locked("Position cleared");}
bool Engine::connect_gps(const std::string& path,unsigned baud,std::string& error){
    std::lock_guard life(impl_->lifecycle);
    // Keep callbacks out until the new source's invalidation marker is added.
    // SerialGps never invokes callbacks with its status mutex held.
    std::lock_guard lock(impl_->mutex);
    const bool connected=impl_->gps.start(path,baud,[p=impl_.get()](PositionFix f){p->position(std::move(f));},error);
    if(connected)impl_->clear_live_position_locked("Serial GPS source changed; awaiting fix");
    else if(!impl_->view.historical)impl_->view.gps_status=impl_->gps.status().detail;
    return connected;
}
void Engine::disconnect_gps(){
    std::lock_guard life(impl_->lifecycle);impl_->gps.stop();
    // Joining the callback thread precedes acquiring the engine data mutex.
    std::lock_guard lock(impl_->mutex);
    if(!impl_->current_fix||!impl_->current_fix->manual)impl_->clear_live_position_locked("Serial GPS disconnected");
}
GpsConnectionStatus Engine::gps_connection_status() const {return impl_->gps.status();}
bool Engine::open_session(const std::string& path,std::string& error){std::lock_guard life(impl_->lifecycle);impl_->stop_locked();try{SessionStore store;store.open_readonly(path);auto snapshot=store.read();snapshot.config.session_path=path;snapshot.hardware_available=impl_->view.hardware_available;snapshot.rtl_sdr_available=impl_->view.rtl_sdr_available;std::lock_guard lock(impl_->mutex);impl_->view=std::move(snapshot);impl_->saved_path=path;error.clear();return true;}catch(const std::exception& e){error=e.what();return false;}}
bool Engine::save_session(std::string& error) {
    auto& p=*impl_;std::lock_guard life(p.lifecycle);return p.save_locked(error);
}
bool Engine::save_session_copy(const std::string& path,std::string& error) {
    auto& p=*impl_;std::unique_lock life(p.lifecycle);
    if(p.saved_path.empty()){error="This session has no saved history to copy. Enable recording before starting a new session";return false;}
    bool active;{std::lock_guard lock(p.mutex);active=p.view.running&&p.run;}
    if(active) {if(!p.save_locked(error))return false;}
    else p.stop_locked();
    try {SessionStore source;source.open_readonly(p.saved_path);
        const auto saved=source.read();
        {std::lock_guard lock(p.mutex);if(saved.session_id!=p.view.session_id)
            throw std::runtime_error("The saved file no longer belongs to the current session");}
        // This reader owns its connection and source identity. A large copy
        // must not hold up key/status calls or the receiver's Stop action.
        life.unlock();source.save_copy(path);error.clear();return true;}
    catch(const std::exception& e){error=std::string("Save copy failed; the original recording is preserved. ")+e.what();return false;}
}
bool Engine::new_session(std::string& error,bool discard_unrecorded) {
    auto& p=*impl_;std::lock_guard life(p.lifecycle);
    bool historical,has_data;
    {std::lock_guard lock(p.mutex);historical=p.view.historical;
        has_data=p.view.delivered_samples||p.view.total_receptions||p.view.spectrum_tiles;
        if(!historical&&p.saved_path.empty()&&(has_data||p.view.running)&&!discard_unrecorded){
            error="This session contains unrecorded measurements. Creating a new session will discard them; explicit discard confirmation is required";return false;
        }
    }
    p.stop_locked();
    {std::lock_guard lock(p.mutex);has_data=p.view.delivered_samples||p.view.total_receptions||p.view.spectrum_tiles;}
    if(!historical&&!p.saved_path.empty()&&has_data&&!p.save_locked(error))return false;
    std::lock_guard lock(p.mutex);
    const bool capability=p.view.hardware_available,rtl_capability=p.view.rtl_sdr_available;auto config=p.view.config;config.session_path.clear();
    p.view=Snapshot{};p.view.hardware_available=capability;p.view.rtl_sdr_available=rtl_capability;p.view.config=std::move(config);
    p.view.gps_status=p.live_position_status_locked();
    p.saved_path.clear();p.save_requested=0;p.save_completed=0;p.final_save_confirmed=false;
    p.fix_history.clear();if(p.current_fix)p.fix_history.push_back(*p.current_fix);
    error.clear();return true;
}
bool Engine::export_session(const std::string& path,const ExportOptions& opt,std::string& error)const {
    std::lock_guard life(impl_->lifecycle);
    if(impl_->run||impl_->worker.joinable()){error="Stop recording before exporting the complete saved survey";return false;}
    if(impl_->saved_path.empty()){error="This session was not recorded; choose a session path before starting a survey";return false;}
    try {
        SessionStore store;store.open_readonly(impl_->saved_path);
        auto ext=std::filesystem::path(std::u8string(path.begin(),path.end())).extension().u8string();
        for(auto& c:ext)if(c>=u8'A'&&c<=u8'Z')c=static_cast<char8_t>(c+(u8'a'-u8'A'));
        if(ext==u8".geojson"||ext==u8".json")store.export_geojson(path,opt);
        else store.export_csv(path,opt);
        error.clear();return true;
    }catch(const std::exception& e){error=e.what();return false;}
}
bool Engine::analyze_survey(const SurveyQuery& query,SurveyAnalysis& result,std::string& error) const {
    std::unique_lock life(impl_->lifecycle);
    if(impl_->saved_path.empty()){error="Save a survey before querying its recorded measurements";return false;}
    try {SessionStore store;store.open_readonly(impl_->saved_path);
        store.with_read_snapshot([&] {
            const auto source=store.read();
            {std::lock_guard lock(impl_->mutex);if(source.session_id!=impl_->view.session_id)
                throw std::runtime_error("The saved file no longer belongs to the selected session");}
            // Source identity and a committed revision are pinned before a
            // concurrent New/Open can replace the desktop selection. Long
            // analysis must not hold up Stop, key metadata or other UI status.
            life.unlock();result=store.analyze(query);
        });error.clear();return true;}
    catch(const std::exception& e){error=e.what();return false;}
}

bool Engine::export_report(const std::string& path,const ReportOptions& options,std::string& error) const {
    std::lock_guard life(impl_->lifecycle);
    if(impl_->run||impl_->worker.joinable()){error="Stop recording before exporting a saved survey report";return false;}
    if(impl_->saved_path.empty()){error="This session has no saved measurements to report";return false;}
    try {
        auto ext=std::filesystem::path(std::u8string(path.begin(),path.end())).extension().u8string();
        for(auto& c:ext)if(c>=u8'A'&&c<=u8'Z')c=static_cast<char8_t>(c+(u8'a'-u8'A'));
        const auto expected = options.kind == ReportKind::Analysis ? u8".html" : u8".csv";
        if(ext!=expected)throw std::runtime_error("Choose .html for an analysis report or .csv for a data report");
        SessionStore store;store.open_readonly(impl_->saved_path);
        export_survey_report(store,path,options);error.clear();return true;
    }catch(const std::exception& e){error=e.what();return false;}
}

}
