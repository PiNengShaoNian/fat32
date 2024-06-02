#include "xdisk.h"

u8_t temp_buffer[512];

xfat_err_t xdisk_open(xdisk_t* disk, const char* name, xdisk_driver_t* driver, void* init_data) {
	xfat_err_t err;
	disk->driver = driver;
	err = disk->driver->open(disk, init_data);
	if (err < 0) {
		return err;
	}
	disk->name = name;
	return FS_ERR_OK;
}
xfat_err_t xdisk_close(xdisk_t* disk) {
	return disk->driver->close(disk);
}

xfat_err_t xdisk_read_sector(xdisk_t* disk, u8_t* buffer, u32_t start_sector, u32_t count) {
	if (start_sector + count >= disk->total_sector) {
		return FS_ERR_PARAM;
	}
	return disk->driver->read_sector(disk, buffer, start_sector, count);
}

xfat_err_t xdisk_write_sector(xdisk_t* disk, u8_t* buffer, u32_t start_sector, u32_t count) {
	if (start_sector + count >= disk->total_sector) {
		return FS_ERR_PARAM;
	}
	return disk->driver->write_sector(disk, buffer, start_sector, count);
}

xfat_err_t xdisk_get_part_count(xdisk_t* disk, u32_t* count) {
	mbr_part_t* part;
	u8_t* disk_buffer = temp_buffer;
	int err = xdisk_read_sector(disk, disk_buffer, 0, 1);
	if (err < 0) {
		return err;
	}

	int r_count = 0;
	part = ((mbr_t*)disk_buffer)->part_info;
	for (int i = 0; i < MBR_PRIMARY_PART_NR; i++, part++) {
		if (part->system_id == FS_NOT_VALID) {
			continue;
		}
		else {
			r_count++;
		}
	}
	*count = r_count;
	return FS_ERR_OK;
}