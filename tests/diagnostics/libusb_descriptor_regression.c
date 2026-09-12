/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Hardware-free regression against the actual configured libusb source.
 * Inputs/invariants: https://github.com/libusb/libusb/issues/1813
 * Fix: https://github.com/libusb/libusb/pull/1814
 * Compile with the configured source root and libusb directory on the include
 * path. This unity build includes upstream descriptor.c with its own LGPL
 * notices intact. No backend, context initialization or device access exists.
 */
#include "descriptor.c"
#include <stdio.h>

const struct usbi_os_backend usbi_backend = {0};
void usbi_log(struct libusb_context *ctx, enum libusb_log_level level,
              const char *function, const char *format, ...)
{
    (void)ctx; (void)level; (void)function; (void)format;
}
int LIBUSB_CALL libusb_control_transfer(libusb_device_handle *device,
    uint8_t request_type, uint8_t request, uint16_t value, uint16_t index,
    unsigned char *data, uint16_t length, unsigned int timeout)
{
    (void)device; (void)request_type; (void)request; (void)value; (void)index;
    (void)data; (void)length; (void)timeout;
    abort(); /* Any unexpected attempt at device I/O fails this test. */
}

int main(void)
{
    const uint8_t malformed_interface[] = {
        9,2,25,0,1,1,0,0xa0,0, 9,4,0,0,1,0xff,0,0,0, 7,5
    };
    const uint8_t malformed_iad[] = {8,0,0,0,0,0,0,0,0xff};
    const uint8_t valid_interface[] = {
        9,2,25,0,1,1,0,0xa0,0, 9,4,0,0,1,0xff,0,0,0,
        7,5,0x81,2,64,0,1
    };
    struct libusb_config_descriptor config = {0};
    struct libusb_interface_association_descriptor_array iad = {0};
    int r = parse_configuration(NULL, &config, malformed_interface,
                                sizeof(malformed_interface));
    if (r < 0 || config.bNumInterfaces != 1 || !config.interface ||
        config.interface[0].num_altsetting != 1 || !config.interface[0].altsetting)
        return 1;
    const struct libusb_interface_descriptor *interface = config.interface[0].altsetting;
    if (interface->bNumEndpoints != 0 || interface->endpoint != NULL) {
        clear_configuration(&config);
        fputs("Malformed interface exposed a nonexistent endpoint array\n", stderr);
        return 2;
    }
    clear_configuration(&config);
    memset(&config, 0, sizeof(config));
    r = parse_configuration(NULL, &config, valid_interface, sizeof(valid_interface));
    if (r < 0 || config.bNumInterfaces != 1 || !config.interface ||
        config.interface[0].num_altsetting != 1 || !config.interface[0].altsetting)
        return 3;
    interface = config.interface[0].altsetting;
    if (interface->bNumEndpoints != 1 || !interface->endpoint ||
        interface->endpoint[0].bEndpointAddress != 0x81) {
        clear_configuration(&config);
        return 4;
    }
    clear_configuration(&config);
    /* Exact allocation makes the one-byte read observable to ASan. */
    uint8_t *bytes = malloc(sizeof(malformed_iad));
    if (!bytes) return 5;
    memcpy(bytes, malformed_iad, sizeof(malformed_iad));
    r = parse_iad_array(NULL, &iad, bytes, sizeof(malformed_iad));
    free((void *)iad.iad);
    free(bytes);
    if (r != LIBUSB_SUCCESS || iad.length != 0) return 6;
    puts("libusb descriptor regressions passed; no USB backend or device access");
    return 0;
}
