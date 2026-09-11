#include "loragw_spi.h"
#include "sx1261_spi.h"
int lgw_spi_open(const char * com_path, void **com_target_ptr) { return -1; }
int lgw_spi_close(void *com_target) { return -1; }
int lgw_spi_w(void *com_target, uint8_t spi_mux_target, uint16_t address, uint8_t data) { return -1; }
int lgw_spi_r(void *com_target, uint8_t spi_mux_target, uint16_t address, uint8_t *data) { return -1; }
int lgw_spi_rmw(void *com_target, uint8_t spi_mux_target, uint16_t address, uint8_t offs, uint8_t leng, uint8_t data) { return -1; }
int lgw_spi_wb(void *com_target, uint8_t spi_mux_target, uint16_t address, const uint8_t *data, uint16_t size) { return -1; }
int lgw_spi_rb(void *com_target, uint8_t spi_mux_target, uint16_t address, uint8_t *data, uint16_t size) { return -1; }
uint16_t lgw_spi_chunk_size(void) { return 0; }
int sx1261_spi_w(void *com_target, sx1261_op_code_t op_code, uint8_t *data, uint16_t size) { return -1; }
int sx1261_spi_r(void *com_target, sx1261_op_code_t op_code, uint8_t *data, uint16_t size) { return -1; }
#include "sx1250_spi.h"
int sx1250_spi_w(void *com_target, uint8_t spi_mux_target, sx1250_op_code_t op_code, uint8_t *data, uint16_t size) { return -1; }
int sx1250_spi_r(void *com_target, uint8_t spi_mux_target, sx1250_op_code_t op_code, uint8_t *data, uint16_t size) { return -1; }
#include "sx125x_spi.h"
int sx125x_spi_r(void *com_target, uint8_t spi_mux_target, uint8_t address, uint8_t *data) { return -1; }
int sx125x_spi_w(void *com_target, uint8_t spi_mux_target, uint8_t address, uint8_t data) { return -1; }
#include "loragw_i2c.h"
int i2c_linuxdev_open(const char *path, uint8_t device_addr, int *i2c_fd) { return -1; }
int i2c_linuxdev_close(int i2c_fd) { return -1; }
int i2c_linuxdev_read(int i2c_fd, uint8_t device_addr, uint8_t reg_addr, uint8_t *data) { return -1; }
int i2c_linuxdev_write(int i2c_fd, uint8_t device_addr, uint8_t reg_addr, uint8_t data) { return -1; }
int i2c_linuxdev_write_buffer(int i2c_fd, uint8_t device_addr, uint8_t *buffer, uint8_t size) { return -1; }
