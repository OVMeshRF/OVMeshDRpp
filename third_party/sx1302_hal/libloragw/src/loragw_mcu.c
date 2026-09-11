/*
 / _____)             _              | |
( (____  _____ ____ _| |_ _____  ____| |__
 \____ \| ___ |    (_   _) ___ |/ ___)  _ \
 _____) ) ____| | | || |_| ____( (___| | | |
(______/|_____)_|_|_| \__)_____)\____)_| |_|
  (C)2020 Semtech

Description:
    Host specific functions to address the LoRa concentrator registers through
    a USB interface.
    Single-byte read/write and burst read/write.

License: Revised BSD License, see LICENSE.TXT file include in the project
*/


/* -------------------------------------------------------------------------- */
/* --- DEPENDANCIES --------------------------------------------------------- */

#include <stdint.h>     /* C99 types */
#include <stdbool.h>    /* bool type */
#include <stdio.h>      /* printf fprintf */
#include <stdlib.h>     /* rand */
#include <unistd.h>     /* lseek, close */
#include <string.h>     /* memset */
#include <errno.h>      /* Error number definitions */
#include <poll.h>
#include <time.h>
#include <termios.h>    /* POSIX terminal control definitions */

#include "loragw_mcu.h"
#include "loragw_aux.h"

/* -------------------------------------------------------------------------- */
/* --- PRIVATE MACROS ------------------------------------------------------- */

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#if DEBUG_MCU == 1
    #define DEBUG_MSG(str)                fprintf(stdout, str)
    #define DEBUG_PRINTF(fmt, args...)    fprintf(stdout, fmt, args)
    #define CHECK_NULL(a)                if(a==NULL){fprintf(stderr,"%s:%d: ERROR: NULL POINTER AS ARGUMENT\n", __FUNCTION__, __LINE__);return -1;}
#else
    #define DEBUG_MSG(str)
    #define DEBUG_PRINTF(fmt, args...)
    #define CHECK_NULL(a)                if(a==NULL){return -1;}
#endif

/* -------------------------------------------------------------------------- */
/* --- PRIVATE CONSTANTS ---------------------------------------------------- */

#if DEBUG_MCU == 1
#define DEBUG_VERBOSE 0
#endif

#define HEADER_CMD_SIZE 4

/* -------------------------------------------------------------------------- */
/* --- PRIVATE TYPES -------------------------------------------------------- */

typedef struct spi_req_bulk_s {
    uint16_t size;
    uint8_t nb_req;
    uint8_t buffer[LGW_USB_BURST_CHUNK];
} spi_req_bulk_t;

/* -------------------------------------------------------------------------- */
/* --- PRIVATE VARIABLES  --------------------------------------------------- */

static uint8_t buf_hdr[HEADER_CMD_SIZE];

static spi_req_bulk_t spi_bulk_buffer = {
    .size = 0,
    .nb_req = 0,
    .buffer = { 0 }
};

/* -------------------------------------------------------------------------- */
/* --- PRIVATE FUNCTIONS DEFINITION ----------------------------------------- */

int spi_req_bulk_insert(spi_req_bulk_t * bulk_buffer, uint8_t * req, uint16_t req_size) {
    /* Check input parameters */
    CHECK_NULL(bulk_buffer);
    CHECK_NULL(req);

    if (bulk_buffer->nb_req == 255) {
        printf("ERROR: cannot insert a new SPI request in bulk buffer - too many requests\n");
        return -1;
    }

    if ((bulk_buffer->size + req_size) > LGW_USB_BURST_CHUNK) {
        printf("ERROR: cannot insert a new SPI request in bulk buffer - buffer full\n");
        return -1;
    }

    /* Add a new request entry in storage buffer */
    memcpy(bulk_buffer->buffer + bulk_buffer->size, req, req_size);

    bulk_buffer->nb_req += 1;
    bulk_buffer->size += req_size;

    return 0;
}

/* ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */

uint32_t bytes_be_to_uint32_le(const uint8_t * bytes) {
    uint32_t val = 0;

    if (bytes != NULL) {
        /* Big endian to Little Endian */
        val  = ((uint32_t)bytes[0] << 24);
        val |= (uint32_t)(bytes[1] << 16);
        val |= (uint32_t)(bytes[2] << 8);
        val |= (uint32_t)(bytes[3] << 0);
    }

    return val;
}

/* ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */

int32_t bytes_be_to_int32_le(const uint8_t * bytes) {
    int32_t val = 0;

    if (bytes != NULL) {
        /* Big endian to Little Endian */
        val  = (int32_t)(bytes[0] << 24);
        val |= (int32_t)(bytes[1] << 16);
        val |= (int32_t)(bytes[2] << 8);
        val |= (int32_t)(bytes[3] << 0);
    }

    return val;
}

/* ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */

const char * cmd_get_str(const uint8_t cmd) {
    switch (cmd) {
        case ORDER_ID__REQ_PING:
            return "REQ_PING";
        case ORDER_ID__REQ_GET_STATUS:
            return "REQ_GET_STATUS";
        case ORDER_ID__REQ_BOOTLOADER_MODE:
            return "REQ_BOOTLOADER_MODE";
        case ORDER_ID__REQ_RESET:
            return "REQ_RESET";
        case ORDER_ID__REQ_WRITE_GPIO:
            return "REQ_WRITE_GPIO";
        case ORDER_ID__REQ_MULTIPLE_SPI:
            return "REQ_MULTIPLE_SPI";
        default:
            return "UNKNOWN";
    }
}

/* ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */

uint8_t cmd_get_id(const uint8_t * bytes) {
    return bytes[0];
}

/* ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */

uint16_t cmd_get_size(const uint8_t * bytes) {
    return (uint16_t)(bytes[1] << 8) | bytes[2];
}

/* ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */

uint8_t cmd_get_type(const uint8_t * bytes) {
    return bytes[3];
}

/* ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */

const char * spi_status_get_str(const uint8_t status) {
    switch (status) {
        case SPI_STATUS_OK:
            return "SPI_STATUS_OK";
        case SPI_STATUS_FAIL:
            return "SPI_STATUS_FAIL";
        case SPI_STATUS_WRONG_PARAM:
            return "SPI_STATUS_WRONG_PARAM";
        case SPI_STATUS_TIMEOUT:
            return "SPI_STATUS_TIMEOUT";
        default:
            return "SPI_STATUS_UNKNOWN";
    }
}

/* ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */

/* Local receive-only experiment: metadata diagnostics, no radio payload logs. */
#ifndef BENCH_IO_TIMEOUT_MS
#define BENCH_IO_TIMEOUT_MS 2000
#endif
static bool failed_link = true, request_pending = false;
static int timeout_kind;
static int connection_fd = -1;
static uint8_t request_id, request_type;
static unsigned long request_sequence;
static uint16_t request_length;
static int request_target = -1, request_address = -1, request_write = -1;
struct expected_spi_transaction {
    uint8_t id, type;
    uint16_t frame_length;
};
/* Every legal request occupies at least six bytes; requests are capped at 600. */
static struct expected_spi_transaction expected_spi[100];
static size_t expected_spi_count, expected_spi_reply_bytes;

void mcu_transport_opened(int fd) {
    connection_fd = fd;
    failed_link = false;
    request_pending = false;
    request_sequence = 0;
    expected_spi_count = expected_spi_reply_bytes = 0;
    timeout_kind = 0;
}
int mcu_transport_failed(void) { return failed_link; }
int mcu_transport_timeout_kind(void) { return timeout_kind; }
static double monotonic_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}
static int transaction_failure(const char *reason, const char *phase,
                               size_t done, size_t expected, int error_number) {
    if (!failed_link) {
        if (!strcmp(reason,"deadline") || !strcmp(reason,"poll-timeout")) {
            timeout_kind = !strcmp(phase,"ack-header") ? 1 :
                           !strcmp(phase,"ack-body") ? 2 : 3;
        }
        fprintf(stderr, "USB LINK FAILED seq=%lu cmd=0x%02x id=%u request-bytes=%u target=%d address=%d write=%d phase=%s transferred=%zu/%zu reason=%s error=%d; later requests blocked until a new connection\n",
            request_sequence, request_type, request_id, request_length,
            request_target, request_address, request_write, phase, done, expected,
            reason, error_number);
    }
    failed_link = true;
    request_pending = false;
    return -1;
}
static int transfer_exact(int fd, uint8_t *buf, size_t size, bool writing,
                          const char *phase) {
    size_t done = 0;
    double deadline = monotonic_seconds() + BENCH_IO_TIMEOUT_MS / 1000.0;
    if (failed_link || fd != connection_fd) return -1;
    while (done < size) {
        int remaining = (int)((deadline - monotonic_seconds()) * 1000);
        if (remaining <= 0)
            return transaction_failure("deadline", phase, done, size, 0);
        struct pollfd p = {.fd=fd, .events=writing ? POLLOUT : POLLIN};
        int ready = poll(&p, 1, remaining);
        if (ready < 0 && errno == EINTR) continue;
        if (ready == 0)
            return transaction_failure("poll-timeout", phase, done, size, 0);
        if (ready < 0)
            return transaction_failure("poll-error", phase, done, size, errno);
        if (p.revents & (POLLERR | POLLHUP | POLLNVAL))
            return transaction_failure("descriptor-event", phase, done, size, p.revents);
        ssize_t n = writing ? write(fd, buf+done, size-done > 64 ? 64 : size-done)
                            : read(fd, buf+done, size-done);
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (n < 0)
            return transaction_failure("io-error", phase, done, size, errno);
        if (n == 0) continue;
        done += (size_t)n;
        if (writing) {
            struct timespec pause = {.tv_sec=0, .tv_nsec=1000000}, rem;
            while (nanosleep(&pause, &rem) != 0 && errno == EINTR) pause = rem;
        }
    }
    return 0;
}
static int record_expected_spi(const uint8_t *payload, size_t size) {
    size_t offset = 0;
    expected_spi_count = expected_spi_reply_bytes = 0;
    while (offset < size) {
        if (size-offset < 6 || expected_spi_count >= sizeof expected_spi / sizeof expected_spi[0])
            return transaction_failure("invalid-spi-request-size", "request", offset, size, 0);
        struct expected_spi_transaction *expected = &expected_spi[expected_spi_count];
        expected->id = payload[offset];
        expected->type = payload[offset+1];
        if (expected->type == MCU_SPI_REQ_TYPE_READ_WRITE) {
            size_t frame = ((size_t)payload[offset+3] << 8) | payload[offset+4];
            if (!frame || frame > size-offset-5 || payload[offset+2] > MCU_SPI_TARGET_SX1261)
                return transaction_failure("invalid-spi-request-frame", "request", offset, size, 0);
            expected->frame_length = (uint16_t)frame;
            offset += 5 + frame;
            expected_spi_reply_bytes += 5 + frame;
        } else if (expected->type == MCU_SPI_REQ_TYPE_READ_MODIFY_WRITE) {
            expected->frame_length = 0;
            offset += 6;
            expected_spi_reply_bytes += 5;
        } else {
            return transaction_failure("invalid-spi-request-type", "request", offset, size, 0);
        }
        expected_spi_count++;
    }
    if (!expected_spi_count)
        return transaction_failure("empty-spi-request", "request", 0, 6, 0);
    return 0;
}
int write_req(int fd, order_id_t cmd, const uint8_t *payload, uint16_t payload_size) {
    if (failed_link || fd != connection_fd) return -1;
    if (request_pending)
        return transaction_failure("unconsumed-ack", "request", 0, 0, 0);
    /* No reboot, reset-to-bootloader or persistent-flash command is available. */
    if (cmd != ORDER_ID__REQ_PING && cmd != ORDER_ID__REQ_GET_STATUS &&
        cmd != ORDER_ID__REQ_WRITE_GPIO && cmd != ORDER_ID__REQ_MULTIPLE_SPI) {
        fprintf(stderr, "Receive-only bench refused MCU command 0x%02x\n", cmd);
        return -1;
    }
    if (payload_size > 600 || (payload_size && !payload)) {
        fprintf(stderr, "Bench refused invalid MCU request length=%u\n", payload_size);
        return -1;
    }
    request_id = rand() % 255;
    request_type = cmd;
    request_length = payload_size;
    request_sequence++;
    request_target = request_address = request_write = -1;
    expected_spi_count = expected_spi_reply_bytes = 0;
    if (cmd == ORDER_ID__REQ_MULTIPLE_SPI && record_expected_spi(payload, payload_size)) return -1;
    if (cmd == ORDER_ID__REQ_MULTIPLE_SPI && payload_size >= 3) {
        request_target = payload[2];
        if (payload[1] == MCU_SPI_REQ_TYPE_READ_MODIFY_WRITE && payload_size >= 6) {
            request_target = MCU_SPI_TARGET_SX1302;
            request_address = ((unsigned)payload[2] << 8) | payload[3];
            request_write = 1;
        }
        if (payload[1] == MCU_SPI_REQ_TYPE_READ_WRITE && payload[2] == MCU_SPI_TARGET_SX1302 && payload_size >= 8) {
            request_address = ((payload[6] & 0x7f) << 8) | payload[7];
            request_write = !!(payload[6] & 0x80);
        }
    }
    uint8_t header[4] = {request_id, payload_size >> 8, payload_size & 255, cmd};
    request_pending = true;
    if (transfer_exact(fd, header, 4, true, "request-header")) return -1;
    return transfer_exact(fd, (uint8_t *)payload, payload_size, true, "request-body");
}
int read_ack(int fd, uint8_t *hdr, uint8_t *buf, size_t capacity) {
    if (failed_link || fd != connection_fd) return -1;
    if (!request_pending || !hdr || (!buf && capacity))
        return transaction_failure("invalid-ack-state", "ack-header", 0, 4, 0);
    if (transfer_exact(fd, hdr, 4, false, "ack-header")) return -1;
    size_t n = cmd_get_size(hdr);
    if (hdr[0] != request_id || hdr[3] != (request_type | 0x40) || n > capacity || (n && !buf))
        return transaction_failure("ack-id-type-or-length", "ack-header", 4, 4, 0);
    if (request_type == ORDER_ID__REQ_MULTIPLE_SPI && n != expected_spi_reply_bytes)
        return transaction_failure("spi-ack-total-length-mismatch", "ack-header", n, expected_spi_reply_bytes, 0);
    if (capacity) memset(buf, 0, capacity);
    if (transfer_exact(fd, buf, n, false, "ack-body")) return -1;
    size_t minimum = request_type == ORDER_ID__REQ_PING ? ACK_PING_SIZE :
                     request_type == ORDER_ID__REQ_GET_STATUS ? ACK_GET_STATUS_SIZE :
                     request_type == ORDER_ID__REQ_WRITE_GPIO ? ACK_GPIO_WRITE_SIZE : 5;
    if (n < minimum)
        return transaction_failure("short-ack", "ack-body", n, minimum, 0);
    if (request_type == ORDER_ID__REQ_MULTIPLE_SPI) {
        size_t offset = 0;
        for (size_t index = 0; index < expected_spi_count; index++) {
            const struct expected_spi_transaction *expected = &expected_spi[index];
            if (n-offset < 5)
                return transaction_failure("short-spi-ack", "ack-body", n, 5, 0);
            uint8_t kind = buf[offset+1];
            if (buf[offset] != expected->id || kind != expected->type)
                return transaction_failure("spi-ack-id-or-type-mismatch", "ack-body", index, expected_spi_count, 0);
            size_t raw_frame = kind == MCU_SPI_REQ_TYPE_READ_WRITE ?
                ((size_t)buf[offset+3] << 8) + buf[offset+4] : 0;
            if (raw_frame != expected->frame_length)
                return transaction_failure("spi-ack-frame-length-mismatch", "ack-body", raw_frame, expected->frame_length, 0);
            size_t frame = 5 + raw_frame;
            if (frame > n-offset)
                return transaction_failure("truncated-spi-ack", "ack-body", n, frame, 0);
            if (buf[offset+2] != 0)
                return transaction_failure("spi-status-error", "ack-body", n, n, buf[offset+2]);
            offset += frame;
        }
        if (offset != n)
            return transaction_failure("unexpected-spi-ack-data", "ack-body", offset, n, 0);
    }
    request_pending = false;
    if (request_length >= 40)
        fprintf(stderr, "USB ACK seq=%lu cmd=0x%02x request=%u response=%zu target=%d address=%d write=%d\n",
            request_sequence, request_type, request_length, n, request_target, request_address, request_write);
    return (int)n;
}

int decode_ack_ping(const uint8_t * hdr, const uint8_t * payload, s_ping_info * info) {
    /* sanity checks */
    if ((hdr == NULL) || (payload == NULL) || (info == NULL)) {
        printf("ERROR: invalid parameter\n");
        return -1;
    }

    if (cmd_get_type(hdr) != ORDER_ID__ACK_PING) {
        printf("ERROR: wrong ACK type for PING (expected:0x%02X, got 0x%02X)\n", ORDER_ID__ACK_PING, cmd_get_type(hdr));
        return -1;
    }

    if (cmd_get_size(hdr) < ACK_PING_SIZE) return -1;

    /* payload info */
    info->unique_id_high = bytes_be_to_uint32_le(&payload[ACK_PING__UNIQUE_ID_0]);
    info->unique_id_mid  = bytes_be_to_uint32_le(&payload[ACK_PING__UNIQUE_ID_4]);
    info->unique_id_low  = bytes_be_to_uint32_le(&payload[ACK_PING__UNIQUE_ID_8]);

    memcpy(info->version, &payload[ACK_PING__VERSION_0], (sizeof info->version) - 1);
    info->version[(sizeof info->version) - 1] = '\0'; /* terminate string */

#if DEBUG_VERBOSE
    DEBUG_MSG   ("## ACK_PING\n");
    DEBUG_PRINTF("   id:           0x%02X\n", cmd_get_id(hdr));
    DEBUG_PRINTF("   size:         %u\n", cmd_get_size(hdr));
    DEBUG_PRINTF("   unique_id:    0x%08X%08X%08X\n", info->unique_id_high, info->unique_id_mid, info->unique_id_low);
    DEBUG_PRINTF("   FW version:   %s\n", info->version);
#endif

    return 0;
}

/* ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */

int decode_ack_bootloader_mode(const uint8_t * hdr) {
     /* sanity checks */
    if (hdr == NULL) {
        printf("ERROR: invalid parameter\n");
        return -1;
    }

    if (cmd_get_type(hdr) != ORDER_ID__ACK_BOOTLOADER_MODE) {
        printf("ERROR: wrong ACK type for ACK_BOOTLOADER_MODE (expected:0x%02X, got 0x%02X)\n", ORDER_ID__ACK_BOOTLOADER_MODE, cmd_get_type(hdr));
        return -1;
    }

#if DEBUG_VERBOSE
    DEBUG_MSG   ("## ACK_BOOTLOADER_MODE\n");
    DEBUG_PRINTF("   id:           0x%02X\n", cmd_get_id(hdr));
    DEBUG_PRINTF("   size:         %u\n", cmd_get_size(hdr));
#endif

    return 0;
}

/* ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */

int decode_ack_get_status(const uint8_t * hdr, const uint8_t * payload, s_status * status) {
    int16_t temperature_sensor;

    /* sanity checks */
    if ((payload == NULL) || (status == NULL)) {
        printf("ERROR: invalid parameter\n");
        return -1;
    }

    if (cmd_get_type(hdr) != ORDER_ID__ACK_GET_STATUS) {
        printf("ERROR: wrong ACK type for GET_STATUS (expected:0x%02X, got 0x%02X)\n", ORDER_ID__ACK_GET_STATUS, cmd_get_type(hdr));
        return -1;
    }

    /* payload info */
    status->system_time_ms = bytes_be_to_uint32_le(&payload[ACK_GET_STATUS__SYSTEM_TIME_31_24]);

    temperature_sensor = (int16_t)(payload[ACK_GET_STATUS__TEMPERATURE_15_8] << 8) |
                         (int16_t)(payload[ACK_GET_STATUS__TEMPERATURE_7_0]  << 0);
    status->temperature = (float)temperature_sensor / 100.0;


#if DEBUG_VERBOSE
    DEBUG_MSG   ("## ACK_GET_STATUS\n");
    DEBUG_PRINTF("   id:            0x%02X\n", cmd_get_id(hdr));
    DEBUG_PRINTF("   size:          %u\n", cmd_get_size(hdr));
    DEBUG_PRINTF("   sys_time:      %u\n", status->system_time_ms);
    DEBUG_PRINTF("   temperature:   %.1f\n", status->temperature);
#endif

    return 0;
}

/* ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */

int decode_ack_gpio_access(const uint8_t * hdr, const uint8_t * payload, uint8_t * write_status) {
    if ((hdr == NULL) || (payload == NULL) || (write_status == NULL)) {
        printf("ERROR: invalid parameter\n");
        return -1;
    }

    if (cmd_get_type(hdr) != ORDER_ID__ACK_WRITE_GPIO) {
        printf("ERROR: wrong ACK type for WRITE_GPIO (expected:0x%02X, got 0x%02X)\n", ORDER_ID__ACK_WRITE_GPIO, cmd_get_type(hdr));
        return -1;
    }

    /* payload info */
    *write_status = payload[ACK_GPIO_WRITE__STATUS];

#if DEBUG_VERBOSE
    DEBUG_MSG   ("## ACK_WRITE_GPIO\n");
    DEBUG_PRINTF("   id:           0x%02X\n", cmd_get_id(hdr));
    DEBUG_PRINTF("   size:         %u\n", cmd_get_size(hdr));
    DEBUG_PRINTF("   status:       %u\n", *write_status);
#endif

    return 0;
}

/* ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */

int decode_ack_spi_bulk(const uint8_t * hdr, const uint8_t * payload) {
    uint8_t req_id, req_type, req_status;
    uint16_t frame_size;
    int i;

    /* sanity checks */
    if ((hdr == NULL) || (payload == NULL)) {
        printf("ERROR: invalid parameter\n");
        return -1;
    }

    if (cmd_get_type(hdr) != ORDER_ID__ACK_MULTIPLE_SPI) {
        printf("ERROR: wrong ACK type for ACK_MULTIPLE_SPI (expected:0x%02X, got 0x%02X)\n", ORDER_ID__ACK_MULTIPLE_SPI, cmd_get_type(hdr));
        return -1;
    }

#if DEBUG_VERBOSE
    DEBUG_MSG   ("## ACK_SPI_BULK\n");
    DEBUG_PRINTF("   id:           0x%02X\n", cmd_get_id(hdr));
    DEBUG_PRINTF("   size:         %u\n", cmd_get_size(hdr));
#endif

    i = 0;
    while (i < cmd_get_size(hdr)) {
        /* parse the request */
        req_id      = payload[i + 0];
        req_type    = payload[i + 1];
        if (req_type != MCU_SPI_REQ_TYPE_READ_WRITE && req_type != MCU_SPI_REQ_TYPE_READ_MODIFY_WRITE) {
            printf("ERROR: %s: wrong type for SPI request %u (0x%02X)\n", __FUNCTION__, req_id, req_type);
            return -1;
        }
        req_status  = payload[i + 2];
        if (req_status != 0) {
            /* Exit if any of the requests failed */
            printf("ERROR: %s: SPI request %u failed with %u - %s\n", __FUNCTION__, req_id, req_status, spi_status_get_str(req_status));
            return -1;
        }
#if DEBUG_VERBOSE
        DEBUG_PRINTF("   ----- REQ_SPI %u -----\n", req_id);
        DEBUG_PRINTF("   type %s\n", (req_type == MCU_SPI_REQ_TYPE_READ_WRITE) ? "read/write" : "read-modify-write");
        DEBUG_PRINTF("   status %u\n", req_status);
#endif
        /* Move to the next REQ */
        if (req_type == MCU_SPI_REQ_TYPE_READ_WRITE) {
            frame_size = (uint16_t)(payload[i + 3] << 8) | (uint16_t)(payload[i + 4]);
#if DEBUG_VERBOSE
            int j;
            DEBUG_PRINTF("   RAW SPI frame (sz:%u): ", frame_size);
            for (j = 0; j < frame_size; j++) {
                DEBUG_PRINTF(" %02X", payload[i + 5 + j]);
            }
            DEBUG_MSG("\n");
#endif
            i += (5 + frame_size); /* REQ ACK metadata + SPI raw frame */
        } else {
#if DEBUG_VERBOSE
            DEBUG_PRINTF("   read value     0x%02X\n", payload[i + 3]);
            DEBUG_PRINTF("   modified value 0x%02X\n", payload[i + 4]);
#endif
            i += 5;
        }
    }

    return 0;
}

/* -------------------------------------------------------------------------- */
/* --- PUBLIC FUNCTIONS DEFINITION ------------------------------------------ */

int mcu_ping(int fd, s_ping_info * info) {
    uint8_t buf_ack[ACK_PING_SIZE];

    CHECK_NULL(info);

    if (write_req(fd, ORDER_ID__REQ_PING, NULL, 0) != 0) {
        printf("ERROR: failed to write PING request\n");
        return -1;
    }

    if (read_ack(fd, buf_hdr, buf_ack, sizeof buf_ack) < 0) {
        printf("ERROR: failed to read PING ack\n");
        return -1;
    }

    if (decode_ack_ping(buf_hdr, buf_ack, info) != 0) {
        printf("ERROR: invalid PING ack\n");
        return -1;
    }

    return 0;
}

/* ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */

int mcu_boot(int fd) {
    if (write_req(fd, ORDER_ID__REQ_BOOTLOADER_MODE, NULL, 0) != 0) {
        printf("ERROR: failed to write BOOTLOADER_MODE request\n");
        return -1;
    }

    if (read_ack(fd, buf_hdr, NULL, 0) < 0) {
        printf("ERROR: failed to read BOOTLOADER_MODE ack\n");
        return -1;
    }

    if (decode_ack_bootloader_mode(buf_hdr) != 0) {
        printf("ERROR: invalid BOOTLOADER_MODE ack\n");
        return -1;
    }

    return 0;
}

/* ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */

int mcu_get_status(int fd, s_status * status) {
    uint8_t buf_ack[ACK_GET_STATUS_SIZE];

    CHECK_NULL(status);

    if (write_req(fd, ORDER_ID__REQ_GET_STATUS, NULL, 0) != 0) {
        printf("ERROR: failed to write GET_STATUS request\n");
        return -1;
    }

    if (read_ack(fd, buf_hdr, buf_ack, sizeof buf_ack) < 0) {
        printf("ERROR: failed to read GET_STATUS ack\n");
        return -1;
    }

    if (decode_ack_get_status(buf_hdr, buf_ack, status) != 0) {
        printf("ERROR: invalid GET_STATUS ack\n");
        return -1;
    }

    return 0;
}

/* ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */

int mcu_gpio_write(int fd, uint8_t gpio_port, uint8_t gpio_id, uint8_t gpio_value) {
    uint8_t status;
    uint8_t buf_req[REQ_WRITE_GPIO_SIZE];
    uint8_t buf_ack[ACK_GPIO_WRITE_SIZE];

    buf_req[REQ_WRITE_GPIO__PORT]   = gpio_port;
    buf_req[REQ_WRITE_GPIO__PIN]    = gpio_id;
    buf_req[REQ_WRITE_GPIO__STATE]  = gpio_value;
    if (write_req(fd, ORDER_ID__REQ_WRITE_GPIO, buf_req, REQ_WRITE_GPIO_SIZE) != 0) {
        printf("ERROR: failed to write REQ_WRITE_GPIO request\n");
        return -1;
    }

    if (read_ack(fd, buf_hdr, buf_ack, sizeof buf_ack) < 0) {
        printf("ERROR: failed to read PING ack\n");
        return -1;
    }

    if (decode_ack_gpio_access(buf_hdr, buf_ack, &status) != 0) {
        printf("ERROR: invalid REQ_WRITE_GPIO ack\n");
        return -1;
    }

    if (status != 0) {
        printf("ERROR: Failed to write GPIO (port:%u id:%u value:%u)\n", gpio_port, gpio_id, gpio_value);
        return -1;
    }

    return 0;
}

/* ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */

int mcu_spi_write(int fd, uint8_t * in_out_buf, size_t buf_size) {
    /* Check input parameters */
    CHECK_NULL(in_out_buf);

    if (write_req(fd, ORDER_ID__REQ_MULTIPLE_SPI, in_out_buf, buf_size) != 0) {
        printf("ERROR: failed to write REQ_MULTIPLE_SPI request\n");
        return -1;
    }

    if (read_ack(fd, buf_hdr, in_out_buf, buf_size) < 0) {
        printf("ERROR: failed to read REQ_MULTIPLE_SPI ack\n");
        return -1;
    }

    if (decode_ack_spi_bulk(buf_hdr, in_out_buf) != 0) {
        printf("ERROR: invalid REQ_MULTIPLE_SPI ack\n");
        return -1;
    }

    return 0;
}

/* ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */

int mcu_spi_store(uint8_t * in_out_buf, size_t buf_size) {
    CHECK_NULL(in_out_buf);

    return spi_req_bulk_insert(&spi_bulk_buffer, in_out_buf, buf_size);
}

/* ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */

int mcu_spi_flush(int fd) {
    /* Write pending SPI requests to MCU */
    if (mcu_spi_write(fd, spi_bulk_buffer.buffer, spi_bulk_buffer.size) != 0) {
        printf("ERROR: %s: failed to write SPI requests to MCU\n", __FUNCTION__);
        return -1;
    }

    /* Reset bulk storage buffer */
    spi_bulk_buffer.nb_req = 0;
    spi_bulk_buffer.size = 0;

    return 0;
}

/* --- EOF ------------------------------------------------------------------ */
