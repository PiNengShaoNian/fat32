#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include "xdisk.h"
#include "xfat.h"

static xfat_err_t xdisk_hw_open(struct _xdisk_t* disk, void* init_data) {
	const char* path = (const char*)init_data;
	FILE* file = fopen(path, "rb+");
	if (file == NULL) {
		char buffer[128];
		strerror_s(buffer, 128, errno);
		printf("open disk failed: %s, reason: %s\n", path, buffer);
		return FS_ERR_IO;
	}

	disk->data = file;
	disk->sector_size = 512;

	fseek(file, 0, SEEK_END);
	disk->total_sector = ftell(file) / disk->sector_size;
	return FS_ERR_OK;
}

static xfat_err_t xdisk_hw_close(struct _xdisk_t* disk) {
	FILE* file = (FILE*)disk->data;
	fclose(file);
	return FS_ERR_OK;
}

static xfat_err_t xdisk_hw_read_sector(struct _xdisk_t* disk, u8_t* buffer, u32_t start_sector, u32_t count) {
	u32_t offset = start_sector * disk->sector_size;
	FILE* file = (FILE*)disk->data;

	xfat_err_t err = fseek(file, offset, SEEK_SET);
	if (err == -1) {
		printf("seek disk failed: 0x%x\n", offset);
		return FS_ERR_IO;
	}
	err = (xfat_err_t)fread(buffer, disk->sector_size, count, file);
	if (err == -1) {
		printf("read disk failed: sector: %d, count: %d\n", start_sector, count);
		return FS_ERR_IO;
	}
	return FS_ERR_OK;
}

static xfat_err_t xdisk_hw_write_sector(struct _xdisk_t* disk, u8_t* buffer, u32_t start_sector, u32_t count) {
	u32_t offset = start_sector * disk->sector_size;
	FILE* file = (FILE*)disk->data;

	xfat_err_t err = fseek(file, offset, SEEK_SET);
	if (err == -1) {
		printf("seek disk failed: 0x%x\n", offset);
		return FS_ERR_IO;
	}
	err = (xfat_err_t)fwrite(buffer, disk->sector_size, count, file);
	if (err == -1) {
		printf("write disk failed: sector: %d, count: %d\n", start_sector, count);
		return FS_ERR_IO;
	}
	fflush(file);
	return FS_ERR_OK;
}

xfat_err_t curr_time(struct _xdisk_t* disk, struct _xfile_time_t* timeinfo) {
	time_t raw_time;
	struct tm* local_time;

	time(&raw_time);
	local_time = localtime(&raw_time);

	timeinfo->year = local_time->tm_year + 1900;
	timeinfo->month = local_time->tm_mon + 1;
	timeinfo->day = local_time->tm_mday;
	timeinfo->hour = local_time->tm_hour;
	timeinfo->minute = local_time->tm_min;
	timeinfo->second = local_time->tm_sec;

	return FS_ERR_OK;
}

xdisk_driver_t vdisk_driver = {
	.open = xdisk_hw_open,
	.close = xdisk_hw_close,
	.curr_time = curr_time,
	.read_sector = xdisk_hw_read_sector,
	.write_sector = xdisk_hw_write_sector,
};