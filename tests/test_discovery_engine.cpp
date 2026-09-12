// SPDX-License-Identifier: GPL-3.0-or-later
#include "ovmesh/engine.hpp"
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
void require(bool condition,const char* message) {if(!condition)throw std::runtime_error(message);}
}
int main() {
    try {
        ovmesh::Engine engine;ovmesh::ReceiverConfig config;std::string error;
        config.synthetic=true;config.sample_rate=8000000;config.survey_span_hz=1000000;
        config.center_hz=906800000;config.tuning_offset_hz=900;
        config.lanes.clear();config.discover_lora=true;
        const auto unique=std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        const auto directory=std::filesystem::current_path()/"discovery-engine-test-output"/unique;
        std::filesystem::create_directories(directory);
        config.session_path=(directory/"waveform.sqlite").string();
        engine.set_fixed_position(0,0);
        require(engine.start(config,false,error),error.c_str());
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(12);
        while(std::chrono::steady_clock::now()<deadline) {
            const auto s=engine.snapshot();
            require(s.error.empty(),s.error.c_str());
            require(!s.discovery.failed,s.discovery.fault.c_str());
            if(s.discovery.observations && s.measurement_seconds>=.8)break;
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        engine.stop();const auto final=engine.snapshot();
        require(final.error.empty(),final.error.c_str());
        require(final.discovery.enabled&&final.discovery.finished&&!final.discovery.failed,"Discovery must drain cleanly");
        bool instrumented_loss=false;
#ifdef OVMESH_TEST_SANITIZED
        // Sanitizers do not provide the live DSP budget. When they force loss,
        // verify explicit accounting/readback; Release still requires the first
        // actual transmission, and independent offline fixtures test the DSP.
        instrumented_loss=final.dropped_samples||final.discovery.rejected_input_samples;
#endif
        require(instrumented_loss||(final.discovery.observations>0&&!final.waveforms.empty()),"Synthetic LoRa was not discovered without RF lanes");
        require(final.discovery.accepted_input_samples+final.discovery.rejected_input_samples==final.delivered_samples,
                "Every measured source sample must be accepted or explicitly rejected by discovery");
        require(final.discovery.channelized_input_samples==final.discovery.accepted_input_samples,
                "Clean drain processes all accepted discovery input");
        require(final.total_receptions==0&&final.classified_receptions==0&&engine.configured_key_count()==0,
                "Waveform discovery must not enable payload decoding or introduce keys");
        require(final.measurement_seconds>0&&final.measurement_seconds<=final.input_seconds,"RF measurements remain independent");
        bool found=false;
        for(const auto& wave:final.waveforms) {
            if(wave.bandwidth_hz==250000&&wave.spreading_factor==11&&std::abs(wave.center_hz-906874100.)<500.) {
                if(std::abs(wave.delimiter_elapsed-.381920)<.003)found=true;
                require(wave.delimiter_elapsed>wave.first_observed_elapsed,"Preamble evidence order");
                require(wave.receiver_position&&wave.receiver_position->manual,"Associate receiver position at acquisition time");
            }
        }
        require(instrumented_loss||found,"Discover the first actual waveform and its BW/SF/center; a later packet cannot mask an initial miss");
        ovmesh::Engine historical;
        require(historical.open_session(config.session_path,error),error.c_str());
        const auto saved=historical.snapshot();
        require(saved.historical&&saved.discovery.observations==final.discovery.observations,"Retain discovery count after reopening");
        require(saved.waveforms.size()==final.waveforms.size(),"Retain waveform evidence after reopening");
        require(saved.discovery.rejected_input_samples==final.discovery.rejected_input_samples,"Retain separate discovery loss accounting");
        std::cout<<"Engine waveform discovery and saved readback passed; observations="<<final.discovery.observations
                 <<" RF_drops="<<final.dropped_samples<<" discovery_rejected="<<final.discovery.rejected_input_samples
                 <<" instrumented_loss_accounting="<<instrumented_loss<<'\n';
        std::filesystem::remove_all(directory);
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
