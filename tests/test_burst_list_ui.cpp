// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/ui.cpp"
#include <iostream>
using namespace ovmesh;
void require(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
int main(){try{
    BurstListState list;Snapshot s;s.session_id="fixture";s.running=true;
    SpectrumBurst b;b.id=1;s.recent_spectrum_bursts={b};s.spectrum_bursts=1;
    list.update(s,0);require(list.count==1,"Initial list visible");
    s.spectrum_bursts=1000;list.update(s,.999);require(list.count==1,"Rapid arrivals do not reorder the table every frame");
    list.update(s,1);require(list.count==1000,"Refresh at one second");
    list.paused=true;s.spectrum_bursts=2000;list.update(s,2);require(list.count==1000,"Pause preserves visible snapshot");
    list.paused=false;list.update(s,2);require(list.count==2000,"Resume catches up");
    s.running=false;s.spectrum_bursts=2001;list.update(s,2.1);require(list.count==2001,"Stop displays final groups immediately");
    ImGui::CreateContext();auto& io=ImGui::GetIO();io.IniFilename=nullptr;io.LogFilename=nullptr;
    io.DisplaySize={1300,700};io.DeltaTime=1.0f/60;unsigned char* pixels;int w,h;io.Fonts->GetTexDataAsRGBA32(&pixels,&w,&h);
    DesktopState ui;
    for(int pass=0;pass<3;++pass){ImGui::NewFrame();ImGui::SetNextWindowSize(io.DisplaySize);ImGui::Begin("Burst list");
        ui.burst_list.show_fragments=pass==2;live_burst_table(ui,s,250);ImGui::End();ImGui::Render();
        require(ImGui::GetDrawData()->TotalVtxCount>0,"Actual ImGui burst/detail rendering produces geometry");}
    ImGui::DestroyContext();std::cout<<"Paced burst-list UI checks passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
