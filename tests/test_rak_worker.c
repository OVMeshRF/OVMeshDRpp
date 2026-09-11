/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifdef NDEBUG
#undef NDEBUG /* Keep these safety checks active in Release/RelWithDebInfo builds. */
#endif
#define OVMESH_RAK_WORKER_NO_MAIN 1
#include "../src/rak_worker.c"
#include <assert.h>
#include "loragw_sx1302_rx.h"
#include "loragw_sx1302.h"

int main(void) {
    /* These public entry points must fail before dereferencing inputs or touching USB. */
    assert(lgw_send(NULL)==LGW_HAL_ERROR);
    assert(sx1302_send(LGW_RADIO_TYPE_SX1250,NULL,false,NULL,NULL)==LGW_REG_ERROR);
    assert(mcu_boot(-1)!=0);
#if defined(__APPLE__)
    char path[]="/dev/cu.usbmodemTEST";
#else
    char path[]="/dev/ttyACM0";
#endif
    struct rak_options o;
    char *good[]={"worker","--device",path,"--frequency","906875000",
        "--bandwidth","250000","--sf","11","--sync","0x2b",
        "--scan-lower","902100000","--scan-upper","927900000","--scan-step","200000",
        "--scan-samples","2000","--board-index","1"};
    int argc=(int)(sizeof good/sizeof good[0]);
    assert(parse_options(argc,good,&o));assert(o.sync==43 && o.board==1);
    good[16]="25000";assert(parse_options(argc,good,&o));
    good[16]="24999";assert(!parse_options(argc,good,&o));
    good[16]="200000";good[10]="0x12";assert(parse_options(argc,good,&o));
    good[10]="0x34";assert(parse_options(argc,good,&o));
    good[10]="0x00";assert(!parse_options(argc,good,&o));good[10]="0x2b";
    good[4]="0";assert(parse_options(argc,good,&o));
    good[4]="4294967296";assert(!parse_options(argc,good,&o));
    good[4]="928000001";assert(!parse_options(argc,good,&o));
    good[4]="906875000";good[6]="200000";assert(!parse_options(argc,good,&o));
    good[6]="500000";good[8]="13";assert(!parse_options(argc,good,&o));
    good[8]="11";good[12]="927900001";assert(!parse_options(argc,good,&o));
    good[12]="902100000";good[18]="65536";assert(!parse_options(argc,good,&o));
    good[18]="1999";assert(!parse_options(argc,good,&o));
    good[18]="2000";good[20]="2";assert(!parse_options(argc,good,&o));
    assert(!valid_device_path("/dev/ttyUSB0"));assert(!valid_device_path("/tmp/radio"));
    assert(!valid_device_path("/dev/cu.usbmodem../../tmp/file"));
    struct lgw_pkt_rx_s packet={.modulation=MOD_LORA,.bandwidth=BW_500KHZ,.datarate=11,
        .coderate=CR_LORA_4_5,.freq_hz=908750000,.rssis=-80.5f,.snr=7.25f,
        .status=STAT_CRC_OK,.count_us=0x80000000U,.size=255};
    memset(packet.payload,0xa5,255);
    char line[RAK_LINE_MAX];size_t n=packet_line(&packet,line);
    assert(n>510 && n<RAK_LINE_MAX && line[n-1]=='\n');
    assert(strstr(line,"PACKET 908750000 500000 11 5 -80.500 7.250 1 2147483648 ")==line);
    assert(strstr(line,"a5a5a5a5"));
    packet.status=STAT_CRC_BAD;n=packet_line(&packet,line);assert(n && line[n-2]=='-');
    assert(!strstr(line,"a5a5"));
    packet.status=STAT_CRC_OK;packet.size=0;n=packet_line(&packet,line);assert(n && line[n-2]=='-');
    packet.size=256;assert(!packet_line(&packet,line));
    packet.size=1;packet.snr=NAN;assert(!packet_line(&packet,line));
    packet.snr=0;packet.coderate=9;assert(!packet_line(&packet,line));
    wipe(line,sizeof line);for(size_t i=0;i<sizeof line;i++)assert(line[i]==0);
    rx_buffer_t input;rx_packet_t output;
    for(unsigned available=0;available<23;available++) {
        memset(&input,0,sizeof input);input.buffer_size=(uint16_t)available;
        input.buffer[0]=0xa5;input.buffer[1]=0xc0;
        assert(rx_buffer_pop(&input,&output)!=LGW_REG_SUCCESS);
    }
    memset(&input,0,sizeof input);input.buffer_size=4096;input.buffer_index=4095;
    input.buffer[4095]=0xa5;assert(rx_buffer_pop(&input,&output)!=LGW_REG_SUCCESS);
    input.buffer_index=-1;assert(rx_buffer_pop(&input,&output)!=LGW_REG_SUCCESS);
    input.buffer_index=0;input.buffer[0]=0xa5;input.buffer[1]=0xc0;input.buffer[21]=128;
    assert(rx_buffer_pop(&input,&output)!=LGW_REG_SUCCESS);
    puts("PASS: RAK worker arguments, bounded record encoding, badCRC suppression and wipe");
    return 0;
}
