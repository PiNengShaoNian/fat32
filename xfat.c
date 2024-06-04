#include <stdlib.h>
#include "xfat.h"

extern u8_t temp_buffer[512];

#define xfat_get_disk(xfat) ((xfat)->disk_part->disk)

static xfat_err_t parse_fat_header(xfat_t* xfat, dbr_t* dbr) {
	xdisk_part_t* xdisk_part = xfat->disk_part;

	xfat->root_cluster = dbr->fat32.BPB_RootClus;
	xfat->fat_tbl_sectors = dbr->fat32.BPB_FATSz32;

	// Bits 0 - 3: 当前活动的FAT表编号（0 - 15）。
	// Bits 4 - 6 : 保留（通常为0）。
	// Bit 7 : FAT表更新策略（0 = 同步更新所有FAT表，1 = 仅更新当前活动FAT表）。
	// Bits 8 - 15 : 保留（通常为0）。
	if (dbr->fat32.BPB_ExtFlags & (1 << 7)) {
		u32_t table = dbr->fat32.BPB_ExtFlags & 0XF;
		xfat->fat_start_sector = dbr->bpb.BPB_RsvdSecCnt + xdisk_part->start_sector + table * xfat->fat_tbl_sectors;
		xfat->fat_tbl_nr = 1;
	}
	else {
		xfat->fat_start_sector = dbr->bpb.BPB_RsvdSecCnt + xdisk_part->start_sector;
		xfat->fat_tbl_nr = dbr->bpb.BPB_NumFATs;
	}

	xfat->total_sectors = dbr->bpb.BPB_TotSec32;
	xfat->sec_per_cluster = dbr->bpb.BPB_SecPerClus;
	xfat->cluster_byte_size = xfat->sec_per_cluster * dbr->bpb.BPB_BytsPerSec;

	return FS_ERR_OK;
}

xfat_err_t xfat_open(xfat_t* xfat, xdisk_part_t* xdisk_part) {
	dbr_t* dbr = (dbr_t*)temp_buffer;
	xdisk_t* xdisk = xdisk_part->disk;
	xfat->disk_part = xdisk_part;

	xfat_err_t err = xdisk_read_sector(xdisk, (u8_t*)dbr, xdisk_part->start_sector, 1);
	if (err < 0) {
		return err;
	}

	err = parse_fat_header(xfat, dbr);
	if (err < 0) {
		return err;
	}

	xfat->fat_buffer = (u8_t*)malloc(xfat->fat_tbl_sectors * xdisk->sector_size);
	if (xfat->fat_buffer == NULL) {
		return FS_ERR_MEM;
	}
	err = xdisk_read_sector(xdisk, (u8_t*)xfat->fat_buffer, xfat->fat_start_sector, xfat->fat_tbl_sectors);
	if (err < 0) {
		return err;
	}
	return FS_ERR_OK;
}

u32_t cluster_first_sector(xfat_t* xfat, u32_t cluster_no) {
	u32_t data_start_sector = xfat->fat_start_sector + xfat->fat_tbl_sectors * xfat->fat_tbl_nr;
	return data_start_sector + (cluster_no - 2) * xfat->sec_per_cluster;
}

xfat_err_t read_cluster(xfat_t* xfat, u8_t* buffer, u32_t cluster, u32_t count) {
	u8_t* curr_buffer = buffer;
	u32_t curr_sector = cluster_first_sector(xfat, cluster);
	for (u32_t i = 0; i < count; i++) {
		xfat_err_t err = xdisk_read_sector(xfat_get_disk(xfat), curr_buffer, curr_sector, xfat->sec_per_cluster);
		if (err < 0) {
			return err;
		}

		curr_buffer += xfat->cluster_byte_size;
		curr_sector += xfat->sec_per_cluster;
	}

	return FS_ERR_OK;
}

int is_cluster_valid(u32_t cluster) {
	cluster &= 0x0FFFFFFF;
	return (cluster < 0x0FFFFFF0) && (cluster >= 2);
}

xfat_err_t get_next_cluster(xfat_t* xfat, u32_t curr_cluster, u32_t* next_cluster) {
	if (is_cluster_valid(curr_cluster)) {
		cluster32_t* cluster32_buf = (cluster32_t*)xfat->fat_buffer;
		*next_cluster = cluster32_buf[curr_cluster].s.next;
	}
	else {
		*next_cluster = CLUSTER_INVALID;
	}

	return FS_ERR_OK;
}

static xfat_err_t open_sub_file(xfat_t* xfat, u32_t dir_cluster, xfile_t* file, const char* path) {
	file->size = 0;
	file->type = FAT_DIR;
	file->start_cluster = dir_cluster;
	file->curr_cluster = dir_cluster;

	file->xfat = xfat;
	file->pos = 0;
	file->err = FS_ERR_OK;
	return FS_ERR_OK;
}

xfat_err_t xfile_open(xfat_t* xfat, xfile_t* file, const char* path) {
	return open_sub_file(xfat, xfat->root_cluster, file, path);
}

xfat_err_t xfile_close(xfile_t* file) {
	return FS_ERR_OK;
}