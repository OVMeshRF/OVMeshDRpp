/* SPDX-License-Identifier: GPL-3.0-or-later
 * Fault injection for the narrowly patched RTL async allocator. This translation
 * unit includes the reviewed driver with test-only allocator/USB-call redirects.
 * No production test switches, USB context, hardware handle or raw samples.
 */
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libusb.h>
#include "tuner_r82xx.h"

static unsigned allocation_calls, fail_at, live_allocations, submit_calls;
static int unexpected_usb, allow_mock_transfers;
static void (*cancel_current)(void);
static int allow_mock_tuning, tuner_result, tuner_lock;
static uint32_t tuner_requested_frequency;

static void* checked_alloc(size_t count,size_t size) {
    void* result;
    ++allocation_calls;
    if(fail_at&&allocation_calls==fail_at)return NULL;
    result=calloc(count,size);
    if(result)++live_allocations;
    return result;
}
static void* test_malloc(size_t size) {return checked_alloc(1,size);}
static void* test_calloc(size_t count,size_t size) {return checked_alloc(count,size);}
static void test_free(void* value) {
    if(value) {
        if(!live_allocations) {fputs("Untracked or duplicate allocation cleanup\n",stderr);abort();}
        --live_allocations;
    }
    free(value);
}
static struct libusb_transfer* LIBUSB_CALL test_alloc_transfer(int packets) {
    if(packets!=0)abort();
    return checked_alloc(1,sizeof(struct libusb_transfer));
}
static void LIBUSB_CALL test_free_transfer(struct libusb_transfer* transfer) {test_free(transfer);}
static int LIBUSB_CALL test_submit_transfer(struct libusb_transfer* transfer) {
    (void)transfer;++submit_calls;
    if(!allow_mock_transfers) {unexpected_usb=1;return LIBUSB_ERROR_IO;}
    return 0;
}
static int LIBUSB_CALL test_cancel_transfer(struct libusb_transfer* transfer) {
    if(!allow_mock_transfers) {unexpected_usb=1;return LIBUSB_ERROR_IO;}
    transfer->status=LIBUSB_TRANSFER_CANCELLED;return 0;
}
static int LIBUSB_CALL test_handle_events(libusb_context* context,struct timeval* timeout,int* completed) {
    (void)context;(void)timeout;(void)completed;
    if(!allow_mock_transfers) {unexpected_usb=1;return LIBUSB_ERROR_IO;}
    if(cancel_current)cancel_current();
    return 0;
}
static int LIBUSB_CALL test_control_transfer(libusb_device_handle* handle,uint8_t request_type,
    uint8_t request,uint16_t value,uint16_t index,unsigned char* data,uint16_t length,unsigned timeout) {
    (void)handle;(void)request;(void)value;(void)index;(void)timeout;
    if(!allow_mock_tuning) {unexpected_usb=1;return LIBUSB_ERROR_IO;}
    if(request_type&LIBUSB_ENDPOINT_IN)memset(data,0,length);
    return length;
}
static int test_r82xx_set_freq(struct r82xx_priv* tuner,uint32_t frequency) {
    if(!allow_mock_tuning) {unexpected_usb=1;return -EIO;}
    tuner_requested_frequency=frequency;tuner->has_lock=tuner_lock;
    return tuner_result;
}

#define malloc test_malloc
#define calloc test_calloc
#define free test_free
#define libusb_alloc_transfer test_alloc_transfer
#define libusb_free_transfer test_free_transfer
#define libusb_submit_transfer test_submit_transfer
#define libusb_cancel_transfer test_cancel_transfer
#define libusb_handle_events_timeout_completed test_handle_events
#define libusb_control_transfer test_control_transfer
#define r82xx_set_freq test_r82xx_set_freq
#include "../third_party/rtlsdr/src/librtlsdr.c"
#undef malloc
#undef calloc
#undef free
#undef libusb_alloc_transfer
#undef libusb_free_transfer
#undef libusb_submit_transfer
#undef libusb_cancel_transfer
#undef libusb_handle_events_timeout_completed
#undef libusb_control_transfer
#undef r82xx_set_freq

static rtlsdr_dev_t* current_device;
static void cancel_mock_read(void) {(void)rtlsdr_cancel_async(current_device);}
static int check(int condition,const char* message) {
    if(!condition)fprintf(stderr,"%s\n",message);
    return condition;
}
static int tuner_lock_checks(void) {
    rtlsdr_dev_t device;
    unsigned model;
    allow_mock_tuning=1;
    for(model=RTLSDR_TUNER_R820T;model<=RTLSDR_TUNER_R828D;++model) {
        memset(&device,0,sizeof(device));device.tuner=&tuners[model];
        device.tuner_type=(enum rtlsdr_tuner)model;
        device.freq=900000000;device.r82xx_p.has_lock=1;
        tuner_result=0;tuner_lock=0;
        if(!check(rtlsdr_set_center_freq(&device,907500000)==-EIO&&
            rtlsdr_get_center_freq(&device)==0&&tuner_requested_frequency==907500000,
            "Zero return without PLL lock must fail the RF tune and clear cached frequency"))return 0;
        tuner_result=0;tuner_lock=1;
        if(!check(rtlsdr_set_center_freq(&device,907500000)==0&&
            rtlsdr_get_center_freq(&device)==907500000,
            "Locked R82xx RF tune should succeed after an unlocked attempt"))return 0;
        tuner_result=-7;tuner_lock=1;
        if(!check(rtlsdr_set_center_freq(&device,908750000)==-7&&
            rtlsdr_get_center_freq(&device)==0,
            "Tuner communication failure must propagate even when a prior lock flag remains set"))return 0;
    }
    allow_mock_tuning=0;
    return check(!unexpected_usb,"RF tune validation attempted actual USB access");
}
int main(void) {
    rtlsdr_dev_t device;
    unsigned points,n;
    memset(&device,0,sizeof(device));
    device.xfer_buf_num=3;device.xfer_buf_len=16384;
    if(!check(_rtlsdr_alloc_async_buffers(&device)==0,"Fake allocation baseline failed"))return 1;
    points=allocation_calls;
    if(!check(_rtlsdr_free_async_buffers(&device)==0&&live_allocations==0,
        "Fake allocation baseline leaked resources"))return 1;
    for(n=1;n<=points;++n) {
        memset(&device,0,sizeof(device));
        allocation_calls=0;fail_at=n;submit_calls=0;unexpected_usb=0;allow_mock_transfers=0;
        if(!check(rtlsdr_read_async(&device,NULL,NULL,3,16384)==-ENOMEM,
            "Every allocation failure must propagate ENOMEM"))return 1;
        if(!check(device.xfer==NULL&&device.xfer_buf==NULL&&live_allocations==0&&
            device.async_status==RTLSDR_INACTIVE&&device.async_cancel==0&&device.cb==NULL&&device.cb_ctx==NULL,
            "An allocation failure retained resources or stale running state"))return 1;
        if(!check(submit_calls==0&&!unexpected_usb,"Allocation failure attempted USB transfer submission"))return 1;
        /* Reuse the same failed device state with deterministic in-memory USB
         * stubs. The mock cancels normally; no actual libusb I/O is called. */
        fail_at=0;allow_mock_transfers=1;current_device=&device;cancel_current=cancel_mock_read;
        if(!check(rtlsdr_read_async(&device,NULL,NULL,3,16384)==0&&submit_calls==3,
            "Receiver could not restart after an allocation failure"))return 1;
        if(!check(device.xfer==NULL&&device.xfer_buf==NULL&&live_allocations==0&&
            device.async_status==RTLSDR_INACTIVE,"Mock receive/cancel failed to clean up"))return 1;
    }
    memset(&device,0,sizeof(device));allocation_calls=0;allow_mock_transfers=0;submit_calls=0;
    if(!check(rtlsdr_read_async(&device,NULL,NULL,3,(uint32_t)INT_MAX+1U)==-EINVAL&&
        allocation_calls==0&&submit_calls==0,"Oversized unsigned transfer length reached signed libusb length"))return 1;
    if(!tuner_lock_checks())return 1;
    printf("RTL async allocation: %u failure points rejected and restarted; both R82xx tuner lock paths fail closed; no hardware I/O\n",points);
    return 0;
}
