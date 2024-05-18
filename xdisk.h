#ifndef XDISK_H
#define XDISK_H

#include "xtypes.h"

struct _xdisk_t;

typedef struct _xdisk_driver_t {
	xfat_err_t(*open)(struct _xdisk_t* disk, void* init_data);
	xfat_err_t(*close)(struct _xdisk_t* disk);
	xfat_err_t(*read_sector)(struct _xdisk_t* disk, u8_t* buffer, u32_t start_sector, u32_t count);
	xfat_err_t(*write_sector)(struct _xdisk_t* disk, u8_t* buffer, u32_t start_sector, u32_t count);
} xdisk_driver_t;

typedef struct _xdisk_t {
	u32_t sector_size;
	u32_t total_sector;
	xdisk_driver_t* driver;
} xdisk_t;

#endif