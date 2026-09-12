// SPDX-License-Identifier: GPL-3.0-or-later
// Engine::Impl member implementation. This path never enters the SDR IQ queue.
    void process_concentrators(ReceiverConfig c) {
        std::vector<std::unique_ptr<RakProcess>> receivers;
        std::vector<double> last_record(c.concentrators.boards.size(),monotonic_now());
        std::vector<double> previous_scan_end(c.concentrators.boards.size(),0);
        uint64_t scan_id=view.concentrator_scans,packet_id=view.total_receptions;
        double last_save=monotonic_now(),last_fix_saved=-1;
        const double clock_shift=monotonic_now()-rak_monotonic_now()-started;
        auto checkpoint=[&](bool final=false) {
            Snapshot saved;uint64_t request=0;
            {std::lock_guard lock(mutex);view.elapsed_seconds=monotonic_now()-started;
                if(!view.acquisitions.empty()){view.acquisitions.back().elapsed_end_seconds=view.elapsed_seconds;view.acquisitions.back().finalized=final;}
                saved=view;request=save_requested;}
            if(store) {
                // Fixed/manual and serial GPS share the existing association
                // rules. GPS is receiver position, never a transmitter location.
                const auto fix=associated_position(monotonic_now());
                if(fix && fix->monotonic_seconds!=last_fix_saved){store->append(*fix);last_fix_saved=fix->monotonic_seconds;}
                store->update(saved,final);
                std::lock_guard lock(mutex);save_completed=request;if(final)final_save_confirmed=true;
                save_finished.notify_all();
            }
            last_save=monotonic_now();
        };
        // The auxiliary filter must fit inside the requested range; leaving
        // edge slivers unmeasured is preferable to claiming outside coverage.
        const uint64_t first=c.center_hz-c.survey_span_hz/2+117150;
        const uint64_t last=c.center_hz+c.survey_span_hz/2-117150;
        const uint64_t points=(last-first)/c.concentrators.scan_step_hz+1;
        const size_t boards=c.concentrators.boards.size();
        // Ordinary reception and the final STOP drain use identical validation,
        // decoding, GPS association and persistence for completed records.
        auto handle=[&](RakMessage& m,size_t i,bool stopping) {
            const double now=monotonic_now();last_record[i]=now;
            if(m.kind==RakMessage::Kind::Error)throw std::runtime_error(m.error);
            if(m.kind==RakMessage::Kind::End) {
                if(stopping)return;
                throw std::runtime_error("RAK worker stopped before the survey was stopped");
            }
            if(m.kind==RakMessage::Kind::Ready) {
                std::lock_guard lock(mutex);auto& h=view.concentrator_health[i];
                if(h.ready)throw std::runtime_error("RAK worker unexpectedly restarted during the survey");
                h.ready=true;h.ready_elapsed_seconds=now-started;h.last_update_elapsed_seconds=now-started;
                h.state="Receiving";
                if(view.error.empty() && std::all_of(view.concentrator_health.begin(),view.concentrator_health.end(),[](const auto& health){return health.ready;}))
                    view.state="Receiving RAK5146";
                return;
            }
            bool ready;
            {std::lock_guard lock(mutex);auto& h=view.concentrator_health[i];ready=h.ready;h.last_update_elapsed_seconds=now-started;}
            if(!ready)throw std::runtime_error("RAK worker returned measurements before receiver initialization completed");
            if(m.kind==RakMessage::Kind::Scan) {
                if(!c.concentrators.scan_enabled)throw std::runtime_error("RAK returned an unrequested scan");
                auto& scan=m.scan;scan.id=++scan_id;scan.board_index=static_cast<unsigned>(i);
                const auto physical=static_cast<int64_t>(scan.frequency_hz)-c.tuning_offset_hz;
                const uint64_t begin=points*i/boards,end=points*(i+1)/boards;
                if(physical<static_cast<int64_t>(first+begin*c.concentrators.scan_step_hz) ||
                   physical>static_cast<int64_t>(first+(end-1)*c.concentrators.scan_step_hz) ||
                   (physical-static_cast<int64_t>(first))%c.concentrators.scan_step_hz)
                    throw std::runtime_error("RAK scan frequency does not match its assigned sweep");
                scan.frequency_hz=static_cast<uint64_t>(physical);
                // C HAL and C++ steady_clock can use different clock
                // epochs on macOS. Translate, never equate the epochs.
                scan.elapsed_start_seconds=m.scan_monotonic_start+clock_shift;
                scan.elapsed_end_seconds=m.scan_monotonic_end+clock_shift;
                if(scan.elapsed_start_seconds<0 || scan.elapsed_end_seconds>now-started+.1 ||
                   scan.elapsed_start_seconds+1e-4<previous_scan_end[i])
                    throw std::runtime_error("RAK scan timing is inconsistent; survey stopped to preserve measurement integrity");
                previous_scan_end[i]=scan.elapsed_end_seconds;
                scan.utc_start_seconds=started_utc+scan.elapsed_start_seconds;
                scan.utc_end_seconds=started_utc+scan.elapsed_end_seconds;
                scan.receiver_position=associated_position(started+scan.elapsed_end_seconds);
                if(store)store->append(scan);
                std::lock_guard lock(mutex);auto& h=view.concentrator_health[i];++h.scans;h.rssi_samples+=concentrator_sample_count(scan);
                ++view.concentrator_scans;view.concentrator_rssi_samples+=concentrator_sample_count(scan);
                auto found=std::find_if(view.recent_concentrator_scans.begin(),view.recent_concentrator_scans.end(),[&](const auto& v){return v.board_index==i && v.frequency_hz==scan.frequency_hz;});
                if(found!=view.recent_concentrator_scans.end())*found=scan;
                else if(view.recent_concentrator_scans.size()<2080)view.recent_concentrator_scans.push_back(scan);
                else throw std::runtime_error("RAK live scan inventory exceeded its configured bound");
            } else if(m.kind==RakMessage::Kind::Packet) {
                const auto& b=c.concentrators.boards[i];
                if(!b.packets_enabled || m.frequency_hz!=uint64_t(int64_t(b.frequency_hz)+c.tuning_offset_hz) ||
                   m.bandwidth_hz!=b.bandwidth_hz || m.spreading_factor!=b.spreading_factor)
                    throw std::runtime_error("RAK packet metadata does not match its configured service modem");
                Reception r;r.id=++packet_id;r.elapsed_seconds=now-started;r.utc_seconds=started_utc+r.elapsed_seconds;
                r.frequency_hz=b.frequency_hz;r.bandwidth_hz=b.bandwidth_hz;r.spreading_factor=b.spreading_factor;
                r.coding_rate=m.coding_rate;r.snr_db=m.snr_db;r.crc_valid=m.crc_valid;r.header_valid=m.crc_valid;
                r.lane_label="Concentrator "+std::to_string(i+1);
                r.concentrator=ConcentratorPacketMetadata{static_cast<unsigned>(i),m.rssi_dbm,m.hardware_timestamp_us};
                // Host receipt UTC is not a synchronized RF timestamp.
                // Preamble duration and measured frequency error are
                // unavailable; leave them unset at their existing zero.
                if(c.concentrators.decode_enabled)
                    r.decoded=protocol::classify_meshtastic(m.payload,m.crc_valid,profiles);
                if(!m.payload.empty())OPENSSL_cleanse(m.payload.data(),m.payload.size());m.payload.clear();
                r.receiver_position=associated_position(now);
                if(store)store->append(r);
                std::lock_guard lock(mutex);auto& h=view.concentrator_health[i];++h.receptions;
                if(!r.crc_valid)++h.crc_failures;
                if(r.decoded.status==protocol::Status::classified && r.decoded.evidence)++view.classified_receptions;
                ++view.total_receptions;view.receptions.insert(view.receptions.begin(),std::move(r));if(view.receptions.size()>512)view.receptions.pop_back();
            }
        };
        try {
            for(size_t i=0;i<boards && run;++i) {
                auto receiver=std::make_unique<RakProcess>();
                const uint64_t begin=points*i/boards,end=points*(i+1)/boards;
                if(c.concentrators.scan_enabled && begin==end)
                    throw std::runtime_error("RAK scan step leaves too few scan centers for two boards; reduce the step or use one board");
                receiver->start(c.concentrators.boards[i],c.tuning_offset_hz,
                    c.concentrators.scan_enabled?first+begin*c.concentrators.scan_step_hz:0,
                    c.concentrators.scan_enabled?first+(end-1)*c.concentrators.scan_step_hz:0,
                    c.concentrators.scan_step_hz,c.concentrators.scan_samples,static_cast<unsigned>(i));
                receivers.push_back(std::move(receiver));
            }
            while(run) {
                for(size_t i=0;i<receivers.size() && run;++i) {
                    auto messages=receivers[i]->poll();
                    for(auto& m:messages) {
                        handle(m,i,false);
                    }
                    bool ready;{std::lock_guard lock(mutex);ready=view.concentrator_health[i].ready;}
                    if(monotonic_now()-last_record[i]>(ready?8:40))
                        throw std::runtime_error("RAK receiver timed out during startup or USB reception");
                }
                bool requested;{std::lock_guard lock(mutex);view.elapsed_seconds=monotonic_now()-started;requested=save_requested>save_completed;}
                if(requested || monotonic_now()-last_save>=1)checkpoint();
                std::unique_lock lock(wake_mutex);wake.wait_for(lock,std::chrono::milliseconds(5),[&]{return !run.load();});
            }
        } catch(const std::exception& e) {fail(e.what());}
        for(size_t i=0;i<receivers.size();++i) {
            const bool clean=receivers[i]->stop([&](RakMessage& m) {
                try {handle(m,i,true);}
                catch(const std::exception& e) {
                    bool already_failed;{std::lock_guard lock(mutex);already_failed=!view.error.empty();}
                    if(!already_failed)fail(e.what());
                    throw;
                }
            });
            if(!clean) {
                bool already_failed;{std::lock_guard lock(mutex);already_failed=!view.error.empty();}
                if(!already_failed)fail("RAK worker did not confirm a clean shutdown; saved measurements are preserved but the session is incomplete");
            }
        }
        {std::lock_guard lock(mutex);for(auto& h:view.concentrator_health){h.state=view.error.empty()?"Stopped":"Stopped after receiver failure";h.ready=false;}}
        try {checkpoint(true);}catch(const std::exception& e){fail(e.what());}
        std::lock_guard lock(mutex);view.running=false;view.recording=false;if(view.state!="Failed")view.state="Stopped";save_finished.notify_all();
    }
