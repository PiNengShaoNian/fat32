#include "xdisk.h"
#include "xfat.h"

static xfat_err_t xdisk_hw_open(struct _xdisk_t* disk, void* init_data) {

}
static xfat_err_t xdisk_hw_close(struct _xdisk_t* disk) {
}

static xfat_err_t xdisk_hw_read_sector(struct _xdisk_t* disk, u8_t* buffer, u32_t start_sector, u32_t count) {
}
static xfat_err_t xdisk_hw_write_sector(struct _xdisk_t* disk, u8_t* buffer, u32_t start_sector, u32_t count) {
}

xdisk_driver_t vdisk_driver = {
	.open = xdisk_hw_open,
	.close = xdisk_hw_close,
	.read_sector = xdisk_hw_read_sector,
	.write_sector = xdisk_hw_write_sector,
};