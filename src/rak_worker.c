/* SPDX-License-Identifier: GPL-3.0-or-later
 * Isolated POSIX RAK5146 USB receiver. No files, network, or transmitter access.
 * Raw frame bytes travel only over the parent pipe and are wiped after writing.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include "loragw_hal.h"
#include "loragw_reg.h"
#include "loragw_com.h"
#include "loragw_mcu.h"
#include "loragw_sx1261.h"

#define RAK_LINE_MAX 1024
#define RAK_SCAN_BINS 33
#define RAK_MIN_HZ 902000000U
#define RAK_MAX_HZ 928000000U
struct rak_options {
    const char *device;
    uint32_t frequency, bandwidth, sf, sync, lower, upper, step, samples, board;
};
static volatile sig_atomic_t stopping;
static volatile sig_atomic_t operation_phase; /* 0 startup, 1 RX, 2 scan, 3 cleanup/other */
static double monotonic_now(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) return 0;
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}
static void wipe(void *data, size_t n) {
    volatile unsigned char *p = data;
    while (n--) *p++ = 0;
}
static void interrupted(int signo) {(void)signo; stopping = 1;}
static void watchdog(int signo) {
    (void)signo;
    /* Only fixed literals and async-signal-safe writes; no radio bytes or formatting. */
#define RAK_TIMEOUT(text) (void)write(STDOUT_FILENO, text, sizeof(text)-1)
    switch(operation_phase) {
        case 0: RAK_TIMEOUT("ERROR initialization-timeout\nEND\n");break;
        case 1: RAK_TIMEOUT("ERROR receive-timeout\nEND\n");break;
        case 2: RAK_TIMEOUT("ERROR scan-timeout\nEND\n");break;
        default: RAK_TIMEOUT("ERROR operation-timeout\nEND\n");break;
    }
#undef RAK_TIMEOUT
    _exit(70);
}
static bool integer(const char *text, uint32_t *out) {
    if (!text || !*text || *text == '-' || *text == '+' || *text == ' ') return false;
    char *end;
    errno = 0;
    unsigned long value = strtoul(text, &end, 0);
    if (errno || *end || value > UINT32_MAX) return false;
    *out = (uint32_t)value;
    return true;
}
static bool valid_device_path(const char *path) {
    const char *suffix;
    if (!path || strlen(path) >= 50) return false;
#if defined(__APPLE__)
    if (strncmp(path, "/dev/cu.usbmodem", 15)) return false;
    suffix = path + 15;
#else
    if (strncmp(path, "/dev/ttyACM", 11)) return false;
    suffix = path + 11;
#endif
    if (!*suffix) return false;
    for (; *suffix; ++suffix)
        if (!((*suffix >= '0' && *suffix <= '9') || (*suffix >= 'a' && *suffix <= 'z') ||
              (*suffix >= 'A' && *suffix <= 'Z') || *suffix == '-' || *suffix == '_')) return false;
    return true;
}
static bool parse_options(int argc, char **argv, struct rak_options *o) {
    *o = (struct rak_options){.bandwidth=250000, .sf=11, .sync=0x2b, .step=200000, .samples=2000};
    uint32_t seen = 0;
    for (int i = 1; i < argc; i += 2) {
        if (i + 1 >= argc) return false;
        static const char *names[] = {"--device", "--frequency", "--bandwidth", "--sf", "--sync",
            "--scan-lower", "--scan-upper", "--scan-step", "--scan-samples", "--board-index"};
        unsigned k;
        for (k = 0; k < sizeof names/sizeof names[0]; k++) if (!strcmp(argv[i], names[k])) break;
        if (k == sizeof names/sizeof names[0] || (seen & (1U << k))) return false;
        seen |= 1U << k;
        if (k == 0) {o->device = argv[i+1]; continue;}
        uint32_t *values[] = {NULL, &o->frequency, &o->bandwidth, &o->sf, &o->sync,
            &o->lower, &o->upper, &o->step, &o->samples, &o->board};
        if (!integer(argv[i+1], values[k])) return false;
    }
    if (!valid_device_path(o->device) || o->board > 1 ||
        (o->sync != 0x2b && o->sync != 0x12 && o->sync != 0x34) ||
        o->sf < 7 || o->sf > 12 ||
        (o->bandwidth != 125000 && o->bandwidth != 250000 && o->bandwidth != 500000)) return false;
    if (o->frequency && (o->frequency < RAK_MIN_HZ || o->frequency > RAK_MAX_HZ)) return false;
    if ((o->lower == 0) != (o->upper == 0)) return false;
    if (o->lower && (o->lower < RAK_MIN_HZ || o->upper > RAK_MAX_HZ || o->lower > o->upper ||
        o->step < 25000 || o->step > 1000000 || (o->upper-o->lower)/o->step > 1039)) return false;
    if (o->samples != 2000 || (!o->frequency && !o->lower)) return false;
    return true;
}
/* Only STOP or EOF is accepted on stdin. Never interpret a device command. */
static void poll_parent(void) {
    static char input[5];
    static size_t used;
    char buffer[64];
    struct pollfd descriptor = {.fd=STDIN_FILENO, .events=POLLIN};
    int ready = poll(&descriptor, 1, 0);
    if (ready < 0 && errno == EINTR) return;
    if (ready < 0 || (descriptor.revents & (POLLERR | POLLNVAL))) {stopping = 1; return;}
    if (!ready) return;
    ssize_t n = read(STDIN_FILENO, buffer, sizeof buffer);
    if (n == 0) {stopping = 1; return;}
    if (n < 0) {if (errno != EAGAIN && errno != EINTR) stopping = 1; return;}
    for (ssize_t i = 0; i < n; i++) {
        if (buffer[i] == '\n') {
            /* Invalid parent input also closes the worker rather than acting on it. */
            stopping = 1;
            used = 0;
            break;
        }
        if (used >= sizeof input - 1 || buffer[i] != "STOP"[used]) {stopping = 1; break;}
        input[used++] = buffer[i];
    }
    wipe(buffer, sizeof buffer);
}
static bool send_line(const char *line, size_t size) {
    if (!size || size >= RAK_LINE_MAX || line[size-1] != '\n') return false;
    double deadline = monotonic_now() + 1;
    for (size_t done = 0; done < size;) {
        int remaining = (int)((deadline-monotonic_now())*1000);
        if (remaining <= 0) return false;
        struct pollfd p = {.fd=STDOUT_FILENO, .events=POLLOUT};
        int ready = poll(&p, 1, remaining > 25 ? 25 : remaining);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0 || (p.revents & (POLLERR | POLLHUP | POLLNVAL))) return false;
        if (!ready) {poll_parent(); if (stopping) return false; continue;}
        ssize_t n = write(STDOUT_FILENO, line+done, size-done);
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (n <= 0) return false;
        done += (size_t)n;
    }
    return true;
}
static bool literal(const char *text) {return send_line(text, strlen(text));}
static uint8_t bandwidth_enum(uint32_t hz) {
    return hz == 125000 ? BW_125KHZ : hz == 250000 ? BW_250KHZ : BW_500KHZ;
}
static unsigned bandwidth_hz(uint8_t value) {
    return value == BW_125KHZ ? 125000U : value == BW_250KHZ ? 250000U : value == BW_500KHZ ? 500000U : 0;
}
static bool set_sync(uint8_t sync) {
    const uint16_t registers[] = {
        SX1302_REG_RX_TOP_FRAME_SYNCH0_SF5_PEAK1_POS_SF5,
        SX1302_REG_RX_TOP_FRAME_SYNCH1_SF5_PEAK2_POS_SF5,
        SX1302_REG_RX_TOP_FRAME_SYNCH0_SF6_PEAK1_POS_SF6,
        SX1302_REG_RX_TOP_FRAME_SYNCH1_SF6_PEAK2_POS_SF6,
        SX1302_REG_RX_TOP_FRAME_SYNCH0_SF7TO12_PEAK1_POS_SF7TO12,
        SX1302_REG_RX_TOP_FRAME_SYNCH1_SF7TO12_PEAK2_POS_SF7TO12,
        SX1302_REG_RX_TOP_LORA_SERVICE_FSK_FRAME_SYNCH0_PEAK1_POS,
        SX1302_REG_RX_TOP_LORA_SERVICE_FSK_FRAME_SYNCH1_PEAK2_POS};
    for (unsigned i=0; i<8; i++) {
        int32_t expected=(i&1 ? sync&15 : sync>>4)*2, observed=0;
        if (lgw_reg_w(registers[i], expected) || lgw_reg_r(registers[i], &observed) ||
            ((uint32_t)observed&31U) != (uint32_t)expected) return false;
    }
    return true;
}
static bool configure_packets(const struct rak_options *o) {
    struct lgw_conf_board_s board = {.com_type=LGW_COM_USB, .clksrc=0, .lorawan_public=false};
    snprintf(board.com_path, sizeof board.com_path, "%s", o->device);
    if (lgw_board_setconf(&board)) return false;
    int32_t intermediate = o->frequency >= RAK_MIN_HZ+200000U ? 200000 : -200000;
    for (unsigned i=0; i<2; i++) {
        struct lgw_conf_rxrf_s rf = {.enable=true, .freq_hz=(uint32_t)((int64_t)o->frequency-intermediate), .type=LGW_RADIO_TYPE_SX1250,
            .tx_enable=false, .single_input_mode=false, .rssi_offset=-215.4f};
        if (lgw_rxrf_setconf(i, &rf)) return false;
    }
    for (unsigned i=0; i<10; i++) {struct lgw_conf_rxif_s off={0}; if (lgw_rxif_setconf(i, &off)) return false;}
    struct lgw_conf_rxif_s channel = {.enable=true, .rf_chain=0, .freq_hz=intermediate,
        .bandwidth=bandwidth_enum(o->bandwidth), .datarate=o->sf, .implicit_hdr=false};
    return !lgw_rxif_setconf(8, &channel);
}
static size_t packet_line(const struct lgw_pkt_rx_s *p, char line[RAK_LINE_MAX]) {
    unsigned bw=bandwidth_hz(p->bandwidth);
    if (p->modulation != MOD_LORA || !bw || p->datarate < 7 || p->datarate > 12 ||
        p->coderate < CR_LORA_4_5 || p->coderate > CR_LORA_4_8 || p->size > 255 ||
        !isfinite(p->rssis) || !isfinite(p->snr)) return 0;
    bool valid=p->status == STAT_CRC_OK;
    int n=snprintf(line, RAK_LINE_MAX, "PACKET %u %u %u %u %.3f %.3f %u %u ",
        p->freq_hz, bw, p->datarate, (unsigned)p->coderate+4U,
        (double)p->rssis, (double)p->snr, valid?1U:0U, p->count_us);
    if (n < 0 || (size_t)n+2U*p->size+3 >= RAK_LINE_MAX) return 0;
    size_t used=(size_t)n;
    static const char hex[]="0123456789abcdef";
    if (!valid || !p->size) line[used++]='-';
    else for (unsigned i=0; i<p->size; i++) {line[used++]=hex[p->payload[i]>>4]; line[used++]=hex[p->payload[i]&15];}
    line[used++]='\n';line[used]=0;
    return used;
}
#ifndef OVMESH_RAK_WORKER_NO_MAIN
int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);signal(SIGINT, interrupted);signal(SIGTERM, interrupted);signal(SIGALRM, watchdog);
    int flags=fcntl(STDOUT_FILENO,F_GETFL,0);
    if (flags < 0 || fcntl(STDOUT_FILENO,F_SETFL,flags|O_NONBLOCK)) return 2;
    struct rak_options options;
    if (!parse_options(argc,argv,&options)) {literal("ERROR invalid-arguments\n");literal("END\n");return 2;}
    poll_parent();
    if (stopping) {literal("END\n");return 0;}
    struct stat status;
    if (lstat(options.device,&status) || !S_ISCHR(status.st_mode)) {literal("ERROR device-unavailable\n");literal("END\n");return 2;}
    bool packet_started=false, scan_connected=false, scan_active=false;
    int result=1;
    const char *error="ERROR initialization-failed\n";
    alarm(30);
    if (options.frequency) {
        if (!configure_packets(&options) || lgw_start()) goto finish;
        packet_started=true;
        if (!set_sync((uint8_t)options.sync)) goto finish;
    } else if (lgw_com_open(LGW_COM_USB,options.device)) goto finish;
    if (options.lower) {
        if (sx1261_connect(LGW_COM_USB,NULL)) goto finish;
        scan_connected=true;
        struct timespec settle={.tv_nsec=10000000};nanosleep(&settle,NULL);
        if (sx1261_load_pram() || sx1261_calibrate(915000000U) || sx1261_setup()) goto finish;
    }
    alarm(0);
    if (!literal("READY\n")) {error=NULL;goto finish;}
    uint32_t scan_frequency=options.lower;
    double scan_start=0, heartbeat=monotonic_now();
    while (!stopping) {
        poll_parent();if (stopping) break;
        /* HAL/bridge calls stay serialized within this process. */
        alarm(5);
        if (packet_started) {
            operation_phase=1;
            struct lgw_pkt_rx_s packets[16];memset(packets,0,sizeof packets);
            int count=lgw_receive(16,packets);
            if (count < 0 || count > 16) {wipe(packets,sizeof packets);error="ERROR receive-failed\n";goto finish;}
            for (int i=0; i<count; i++) {
                char line[RAK_LINE_MAX];size_t n=packet_line(&packets[i],line);
                bool sent=!n || send_line(line,n);wipe(line,sizeof line);
                if (!sent) {wipe(packets,sizeof packets);error=NULL;goto finish;}
            }
            wipe(packets,sizeof packets);
        }
        if (scan_connected) {
            operation_phase=2;
            if (!scan_active) {
                scan_start=monotonic_now();
                if (sx1261_set_rx_params(scan_frequency,BW_125KHZ)) {
                    error="ERROR scan-tune-failed\n";goto finish;
                }
                if (sx1261_spectral_scan_start((uint16_t)options.samples)) {
                    error="ERROR scan-command-failed\n";goto finish;
                }
                scan_active=true;
            }
            lgw_spectral_scan_status_t scan_status;
            if (sx1261_spectral_scan_status(&scan_status)) {error="ERROR scan-status-failed\n";goto finish;}
            if (scan_status == LGW_SPECTRAL_SCAN_STATUS_COMPLETED) {
                int16_t levels[RAK_SCAN_BINS];uint16_t counts[RAK_SCAN_BINS];
                if (sx1261_spectral_scan_get_results(-11,levels,counts)) {error="ERROR scan-read-failed\n";goto finish;}
                unsigned total=0;for (unsigned i=0;i<RAK_SCAN_BINS;i++) total+=counts[i];
                if (total != options.samples) {error="ERROR scan-count-mismatch\n";goto finish;}
                char line[RAK_LINE_MAX];
                int n=snprintf(line,sizeof line,"SCAN %u %.9f %.9f",scan_frequency,scan_start,monotonic_now());
                for (unsigned i=0;i<RAK_SCAN_BINS && n>0 && n<(int)sizeof line;i++)
                    n+=snprintf(line+n,sizeof line-(size_t)n," %u",counts[i]);
                if (n<0 || n+2>=(int)sizeof line) {error="ERROR record-overflow\n";goto finish;}
                line[n++]='\n';line[n]=0;
                if (!send_line(line,(size_t)n)) {error=NULL;goto finish;}
                scan_active=false;
                scan_frequency = options.upper-scan_frequency < options.step ? options.lower : scan_frequency+options.step;
            } else if ((scan_status != LGW_SPECTRAL_SCAN_STATUS_ON_GOING &&
                        scan_status != LGW_SPECTRAL_SCAN_STATUS_NONE) || monotonic_now()-scan_start>2) {
                error="ERROR scan-incomplete\n";goto finish;
            }
        }
        alarm(0);
        if (monotonic_now()-heartbeat>=1) {
            if (!literal("HEARTBEAT\n")) {error=NULL;goto finish;}
            heartbeat=monotonic_now();
        }
        struct pollfd parent={.fd=STDIN_FILENO,.events=POLLIN};poll(&parent,1,2);
    }
    result=0;error=NULL;
finish:
    operation_phase=3;
    alarm(5);
    if (scan_connected) {
        if (scan_active && !mcu_transport_failed()) (void)sx1261_spectral_scan_abort();
        if (sx1261_disconnect()) {result=1;if (!error) error="ERROR scanner-close-failed\n";}
    }
    if (packet_started) {
        if (lgw_stop()) {result=1;if (!error) error="ERROR receiver-close-failed\n";}
    } else if (lgw_com_target() && lgw_com_close()) {result=1;if (!error) error="ERROR device-close-failed\n";}
    alarm(0);
    if (error) {
        switch (mcu_transport_timeout_kind()) {
            case 1:error="ERROR usb-ack-header-timeout\n";break;
            case 2:error="ERROR usb-ack-body-timeout\n";break;
            case 3:error="ERROR usb-request-timeout\n";break;
            default:break;
        }
    }
    if (error) literal(error);
    literal("END\n");
    return result;
}
#endif
