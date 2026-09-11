/* SPDX-License-Identifier: GPL-3.0-or-later
 * Actual auxiliary scan configuration, with an offline MCU transaction spy.
 * No USB descriptor is opened and no RF command is sent to hardware.
 */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "loragw_hal.h"
#include "loragw_reg.h"
#include "loragw_mcu.h"
#include "loragw_sx1261.h"
#include "sx1261_com.h"

static int dummy_fd=123;
void *lgw_com_target(void) {return &dummy_fd;}
static unsigned calls,operations,maximum,fail_at,stored;
static bool histogram;
static uint8_t histogram_bytes[66];
static uint8_t queue[4096];static size_t queued;
static const uint8_t commands[][10]={
    {0x0d,0x08,0x9b,0x00}, {0xc1}, {0x86,0x39,0x30,0x00,0x00},
    {0x0d,0x08,0x9b,0x14}, {0x8a,0x00},
    {0x8b,0x00,0x14,0x00,0x00,0x0a,0x02,0xe9,0x0f},
    {0x8c,0x00,0x20,0x05,0x20,0x00,0x01,0xff,0x00,0x00},
    {0x82,0xff,0xff,0xff}, {0x9b,0x07,0xd0,0x0b,0,0,0,0,0,0}
};
static const unsigned lengths[]={4,1,5,4,2,9,10,4,10};
int mcu_spi_write(int fd,uint8_t *data,size_t size) {
    assert(fd==dummy_fd && size<=sizeof queue);++calls;
    if(size>maximum)maximum=(unsigned)size;
    if(histogram) {
        const unsigned offset=(calls-1)*24;
        const unsigned count=calls<3?24:18;
        assert(calls<=3 && size==count+9 && size<=33);
        assert(data[1]==MCU_SPI_REQ_TYPE_READ_WRITE && data[2]==MCU_SPI_TARGET_SX1261);
        assert(data[3]==0 && data[4]==count+4 && data[5]==0x1d);
        assert((((unsigned)data[6]<<8)|data[7])==0x0401+offset && data[8]==0);
        for(unsigned i=0;i<count;++i)assert(data[9+i]==0);
        memcpy(data+9,histogram_bytes+offset,count);
        return fail_at && calls==fail_at ? -1 : 0;
    }
    for(size_t offset=0;offset<size;) {
        assert(size-offset>=6 && operations<9);
        assert(data[offset+1]==MCU_SPI_REQ_TYPE_READ_WRITE && data[offset+2]==MCU_SPI_TARGET_SX1261);
        size_t raw=((size_t)data[offset+3]<<8)|data[offset+4];
        assert(raw==lengths[operations] && 5+raw<=size-offset);
        assert(memcmp(data+offset+5,commands[operations],raw)==0);
        ++operations;offset+=5+raw;
    }
    return fail_at && calls==fail_at ? -1 : 0;
}
int mcu_spi_store(uint8_t *data,size_t size) {
    assert(queued+size<=sizeof queue);memcpy(queue+queued,data,size);queued+=size;++stored;return 0;
}
int mcu_spi_flush(int fd) {
    int result=mcu_spi_write(fd,queue,queued);queued=0;return result;
}
static void reset(unsigned failure) {
    calls=operations=maximum=stored=0;queued=0;fail_at=failure;histogram=false;
    assert(sx1261_com_set_write_mode(LGW_COM_WRITE_MODE_SINGLE)==0);
}
int main(void) {
    assert(sx1261_connect(LGW_COM_USB,NULL)==0);
    reset(0);
    assert(sx1261_set_rx_params(915000000,BW_125KHZ)==0 && operations==8);
#ifdef OVMESH_TEST_EXPECT_BATCH
    assert(calls==1 && maximum==79 && stored==8);
#else
    assert(calls==8 && maximum==15 && stored==0);
#endif
    assert(sx1261_spectral_scan_start(2000)==0 && operations==9);
#ifndef OVMESH_TEST_EXPECT_BATCH
    assert(calls==9 && maximum==15 && stored==0);
    for(unsigned failure=1;failure<=8;++failure) {
        reset(failure);
        assert(sx1261_set_rx_params(915000000,BW_125KHZ)==LGW_REG_ERROR);
        assert(calls==failure && operations==failure && stored==0 && queued==0);
    }
#endif
    uint16_t results[33];int16_t levels[33];
    for(unsigned i=0;i<33;++i) {
        uint16_t value=(uint16_t)((i*2053)^0xa55a);
        histogram_bytes[2*i]=(uint8_t)(value>>8);histogram_bytes[2*i+1]=(uint8_t)value;
    }
    reset(0);histogram=true;
    assert(sx1261_spectral_scan_get_results(-11,levels,results)==0 && calls==3 && maximum==33);
    for(unsigned i=0;i<33;++i) {
        assert(results[i]==(uint16_t)((i*2053)^0xa55a));
        assert(levels[i]==-11-4*(int)(i<32?i:31));
    }
    for(unsigned failure=1;failure<=3;++failure) {
        reset(failure);histogram=true;
        for(unsigned i=0;i<33;++i){results[i]=0x55aa;levels[i]=1234;}
        assert(sx1261_spectral_scan_get_results(-11,levels,results)==LGW_REG_ERROR && calls==failure);
        for(unsigned i=0;i<33;++i)assert(results[i]==0x55aa && levels[i]==1234);
    }
    assert(sx1261_disconnect()==0);
    return 0;
}
