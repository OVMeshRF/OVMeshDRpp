/* SPDX-License-Identifier: GPL-3.0-or-later
 * Actual vendor FIFO/parser code with an offline register/memory source.
 */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "loragw_reg.h"
#include "loragw_sx1302_rx.h"

static uint8_t fifo[4096];
static uint16_t lengths[2];
static unsigned reads, memory_reads, failed_read;
static bool failed_memory;
int lgw_reg_rb(uint16_t id,uint8_t *data,uint16_t size) {
    assert(id==SX1302_REG_RX_TOP_RX_BUFFER_NB_BYTES_MSB_RX_BUFFER_NB_BYTES && size==2 && reads<2);
    ++reads;
    if(reads==failed_read) return LGW_REG_ERROR;
    data[0]=(uint8_t)(lengths[reads-1]>>8);data[1]=(uint8_t)lengths[reads-1];
    return LGW_REG_SUCCESS;
}
int lgw_reg_r(uint16_t id,int32_t *data) {(void)id;(void)data;assert(!"Unexpected register read");return -1;}
int lgw_mem_rb(uint16_t address,uint8_t *data,uint16_t size,bool is_fifo) {
    assert(address==0x4000 && is_fifo && size<=sizeof fifo);
    ++memory_reads;
    if(failed_memory) return LGW_REG_ERROR;
    memcpy(data,fifo,size);return LGW_REG_SUCCESS;
}
static void setup(uint16_t first,uint16_t second,unsigned fail_read) {
    memset(fifo,0,sizeof fifo);lengths[0]=first;lengths[1]=second;
    reads=memory_reads=0;failed_read=fail_read;failed_memory=false;
}
static size_t frame(size_t start,unsigned payload,unsigned metrics,uint32_t timestamp) {
    size_t n=23+payload+2*metrics;assert(start+n<=sizeof fifo);
    uint8_t *p=fifo+start;memset(p,0,n);
    p[0]=0xa5;p[1]=0xc0;p[2]=(uint8_t)payload;p[3]=8;p[4]=(11<<4)|3;p[5]=16;
    for(unsigned i=0;i<4;++i)p[payload+15+i]=(uint8_t)(timestamp>>(8*i));
    p[payload+21]=(uint8_t)metrics;
    for(size_t i=0;i<n-1;++i)p[n-1]=(uint8_t)(p[n-1]+p[i]);
    return n;
}
int main(void) {
    rx_buffer_t input;rx_packet_t packet;
    for(unsigned failure=1;failure<=2;++failure) {
        setup(4096,4096,failure);memset(&input,0xa5,sizeof input);
        assert(rx_buffer_fetch(&input)==LGW_REG_ERROR);
        assert(reads==failure && memory_reads==0 && input.buffer_size==0 && input.buffer_pkt_nb==0);
    }
    const uint16_t excessive[]={4097,65535};
    for(unsigned i=0;i<2;++i) {
        setup(excessive[i],0,0);assert(rx_buffer_fetch(&input)==LGW_REG_ERROR);
        assert(memory_reads==0 && input.buffer_size==0);
        setup(0,excessive[i],0);assert(rx_buffer_fetch(&input)==LGW_REG_ERROR);
        assert(memory_reads==0 && input.buffer_size==0);
    }
    setup(0,0,0);assert(rx_buffer_fetch(&input)==0 && memory_reads==0 && input.buffer_pkt_nb==0);
    setup(23,23,0);failed_memory=true;assert(rx_buffer_fetch(&input)==LGW_REG_ERROR && memory_reads==1);
    /* Every truncation of a maximal payload/metrics record must clear the FIFO. */
    for(uint16_t available=1;available<23+255+254;++available) {
        setup(available,available,0);frame(0,255,127,UINT32_C(0x80000000));
        assert(rx_buffer_fetch(&input)==0 && input.buffer_pkt_nb==0 && input.buffer_size==0);
    }
    setup(23+255+254,23+255+254,0);frame(0,255,127,UINT32_MAX);
    assert(rx_buffer_fetch(&input)==0 && input.buffer_pkt_nb==1);
    assert(rx_buffer_pop(&input,&packet)==0 && packet.timestamp_cnt==UINT32_MAX && packet.num_ts_metrics_stored==127);
    /* A complete record followed by each short metadata tail never reads beyond it. */
    for(uint16_t tail=1;tail<23;++tail) {
        setup((uint16_t)(23+tail),(uint16_t)(23+tail),0);frame(0,0,0,0);fifo[23]=0xa5;
        assert(rx_buffer_fetch(&input)==0 && input.buffer_pkt_nb==0 && input.buffer_size==0);
    }
    setup(279,279,0);frame(0,0,128,0);
    assert(rx_buffer_fetch(&input)==0 && input.buffer_pkt_nb==0);
    /* Valid records cover all high timestamp bytes through actual fetch and pop. */
    for(unsigned high=0;high<256;++high) {
        setup(46,23,0);frame(0,0,0,(uint32_t)high<<24);frame(23,0,0,UINT32_C(0x80000000));
        assert(rx_buffer_fetch(&input)==0 && input.buffer_size==46 && input.buffer_pkt_nb==2);
        assert(rx_buffer_pop(&input,&packet)==0 && packet.timestamp_cnt==((uint32_t)high<<24));
        assert(rx_buffer_pop(&input,&packet)==0 && packet.timestamp_cnt==UINT32_C(0x80000000));
    }
    /* The full buffer remains bounded while resynchronizing to one final frame. */
    setup(4096,4096,0);frame(4096-23,0,0,UINT32_MAX);
    assert(rx_buffer_fetch(&input)==0 && input.buffer_size==23 && input.buffer_pkt_nb==1);
    assert(rx_buffer_pop(&input,&packet)==0 && packet.timestamp_cnt==UINT32_MAX);
    return 0;
}
