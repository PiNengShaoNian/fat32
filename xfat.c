#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "xfat.h"

#define XFAT_MB(n) ((n) * 1024 * 1024LL)
#define XFAT_GB(n) ((n) * 1024 * 1024 * 1024LL)
#define XFAT_MAX(a, b) ((a) > (b) ? (a) : (b))
#define DOT_FILE ".          "
#define DOT_DOT_FILE "..         "

#define xfat_get_disk(xfat) ((xfat)->disk_part->disk)
#define file_get_disk(file) ((file)->xfat->disk_part->disk)
#define is_path_sep(ch) (((ch) == '/') || ((ch) == '\\'))
#define is_path_end(path) ((path) == 0 || (*path == '\0'))
#define to_sector(disk, offset) ((offset) / (disk)->sector_size)
#define to_sector_offset(disk, offset) ((offset) % (disk)->sector_size)
#define to_cluster_offset(xfat, pos) ((pos) % (xfat)->cluster_byte_size)
#define to_cluster(xfat, pos) ((pos) / (xfat)->cluster_byte_size)
#define to_cluster_count(xfat, size) ((size) ? (to_cluster(xfat, size) + 1) : 0)


u32_t to_fat_sector(xfat_t* xfat, u32_t cluster) {
	u32_t sector_size = xfat_get_disk(xfat)->sector_size;
	return cluster * sizeof(cluster32_t) / sector_size + xfat->fat_start_sector;
}

u32_t to_fat_offset(xfat_t* xfat, u32_t cluster) {
	u32_t sector_size = xfat_get_disk(xfat)->sector_size;
	return cluster * sizeof(cluster32_t) % sector_size;
}

static xfat_t* xfat_list;

void xfat_list_init(void) {
	xfat_list = (xfat_t*)0;
}

void xfat_list_add(xfat_t* xfat) {
	if (xfat_list == (xfat_t*)0) {
		xfat_list = xfat;
		xfat->next = (xfat_t*)0;
	}
	else {
		xfat->next = xfat_list;
		xfat_list = xfat;
	}
}

void xfat_list_remove(xfat_t* xfat) {
	xfat_t* pre = (xfat_t*)0;
	xfat_t* curr = xfat_list;

	while (curr != xfat && curr != (xfat_t*)0) {
		pre = curr;
		curr = curr->next;
	}

	if (curr != (xfat_t*)0 && curr == xfat) {
		if (curr == xfat_list) {
			xfat_list = curr->next;
		}
		else {
			pre->next = curr->next;
		}

		curr->next = (xfat_t*)0;
	}
}

int is_mount_name_match(xfat_t* xfat, const char* name) {
	const char* s = xfat->name;
	const char* d = name;

	while (is_path_sep(*d)) {
		d++;
	}

	while ((*s != '\0') && (*d != '\0')) {
		if (is_path_sep(*d)) {
			return 0;
		}

		if (*s++ != *d++) {
			return 0;
		}
	}

	return (*s == '\0') && (*d == '\0' || is_path_sep(*d));
}

xfat_t* xfat_find_by_name(const char* name) {
	xfat_t* curr = xfat_list;
	while (curr != (xfat_t*)0) {
		if (is_mount_name_match(curr, name)) {
			return curr;
		}
		curr = curr->next;
	}

	return (xfat_t*)0;
}

xfat_err_t xfat_init(void) {
	xfat_list_init();
	return FS_ERR_OK;
}

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
	xfat->fsi_sector = dbr->fat32.BPB_FsInfo;
	xfat->backup_sector = dbr->fat32.BPB_BkBootSec;

	return FS_ERR_OK;
}

xfat_err_t add_to_mount(xfat_t* xfat, const char* mount_name) {
	memset(xfat->name, 0, XFAT_NAME_LEN);
	strncpy(xfat->name, mount_name, XFAT_NAME_LEN);
	xfat->name[XFAT_NAME_LEN - 1] = '\0';

	if (xfat_find_by_name(xfat->name)) {
		return FS_ERR_EXISTED;
	}

	xfat_list_add(xfat);
	return FS_ERR_OK;
}

static xfat_err_t load_cluster_free_info(xfat_t* xfat) {
	xfat_buf_t* buf;
	xfat_err_t err = xfat_bpool_read_sector(to_obj(xfat), &buf, xfat->fsi_sector + xfat->disk_part->start_sector);
	if (err < 0) {
		return err;
	}

	fsinfo_t* fsinfo = (fsinfo_t*)(buf->buf);
	if ((fsinfo->FSI_LoadSig == 0x41615252) &&
		(fsinfo->FSI_StrucSig == 0x61417272) &&
		(fsinfo->FSI_TrailSig == 0xAA550000) &&
		(fsinfo->FSI_Next_Free != 0xFFFFFFFF) &&
		(fsinfo->FSI_Free_Count != 0xFFFFFFFF)) {
		xfat->cluster_next_free = fsinfo->FSI_Next_Free;
		xfat->cluster_total_free = fsinfo->FSI_Free_Count;
	}
	else {
		u32_t sector_size = xfat_get_disk(xfat)->sector_size;
		u32_t start_sector = xfat->fat_start_sector;
		u32_t sector_count = xfat->fat_tbl_sectors;
		u32_t free_count = 0;
		u32_t next_free = 0;

		while (sector_count--) {
			err = xfat_bpool_read_sector(to_obj(xfat), &buf, start_sector++);
			if (err < 0) {
				return err;
			}

			cluster32_t* cluster = (cluster32_t*)(buf->buf);
			for (u32_t i = 0; i < sector_size; i += sizeof(cluster32_t), cluster++) {
				if (cluster->s.next == CLUSTER_FREE) {
					free_count++;
					if (next_free == 0) {
						next_free = i / sizeof(cluster32_t);
					}
				}
			}
		}

		xfat->cluster_next_free = next_free;
		xfat->cluster_total_free = free_count;
	}

	return FS_ERR_OK;
}

static xfat_err_t save_cluster_free_info(xdisk_t* disk, u32_t total_free, u32_t next_free,
	u32_t fsinfo_sector, u32_t backup_sector) {
	xfat_buf_t* buf = (xfat_buf_t*)0;
	xfat_err_t err = xfat_bpool_alloc(to_obj(disk), &buf, fsinfo_sector);
	if (err < 0) {
		return err;
	}

	fsinfo_t* fsinfo = (fsinfo_t*)(buf->buf);
	memset(fsinfo, 0, sizeof(fsinfo_t));
	fsinfo->FSI_LoadSig = 0x41615252;
	fsinfo->FSI_StrucSig = 0x61417272;
	fsinfo->FSI_Free_Count = total_free;
	fsinfo->FSI_Next_Free = next_free;
	fsinfo->FSI_TrailSig = 0xAA550000;

	err = xfat_bpool_write_sector(to_obj(disk), buf, 1);
	if (err < 0) {
		return err;
	}
	buf->sector_no += backup_sector;
	err = xfat_bpool_write_sector(to_obj(disk), buf, 1);
	if (err < 0) {
		return err;
	}

	return FS_ERR_OK;
}

xfat_err_t xfat_mount(xfat_t* xfat, xdisk_part_t* xdisk_part, const char* mount_name) {
	dbr_t* dbr;
	xdisk_t* xdisk = xdisk_part->disk;
	xfat->disk_part = xdisk_part;
	xfat_buf_t* buf = (xfat_buf_t*)0;

	xfat_obj_init(to_obj(xfat), XFAT_OBJ_FAT);

	xfat_err_t err = xfat_bpool_init(to_obj(xfat), 0, 0, 0);
	if (err < 0) {
		return err;
	}

	err = xfat_bpool_read_sector(to_obj(xfat), &buf, xdisk_part->start_sector);
	if (err < 0) {
		return err;
	}
	dbr = (dbr_t*)buf->buf;
	err = parse_fat_header(xfat, dbr);
	if (err < 0) {
		return err;
	}

	err = load_cluster_free_info(xfat);
	if (err < 0) {
		return err;
	}

	return add_to_mount(xfat, mount_name);
}

void xfat_unmount(xfat_t* xfat) {
	save_cluster_free_info(xfat_get_disk(xfat), xfat->cluster_total_free, xfat->cluster_next_free,
		xfat->fsi_sector, xfat->backup_sector);
	xfat_bpool_flush(to_obj(xfat));
	xfat_list_remove(xfat);
}

xfat_err_t xfat_set_buf(xfat_t* xfat, u8_t* buf, u32_t size) {
	xdisk_part_t* part = xfat->disk_part;
	xfat_err_t err = xfat_bpool_flush_sectors(to_obj(xfat), part->start_sector, part->total_sector);
	if (err < 0) {
		return err;
	}

	err = xfat_bpool_invalid_sectors(to_obj(xfat), part->start_sector, part->total_sector);
	if (err < 0) {
		return err;
	}

	return xfat_bpool_init(to_obj(xfat), xfat_get_disk(xfat)->sector_size, buf, size);
}

xfat_err_t xfat_fmt_ctrl_init(xfat_fmt_ctrl_t* ctrl) {
	ctrl->type = FS_WIN95_FAT32_0;
	ctrl->cluster_size = XFAT_CLUSTER_AUTO;
	ctrl->vol_name = (const char*)0;
	return FS_ERR_OK;
}

static u8_t dbr_data[512] = {
	0xEB, 0x58, 0x90, 0x4D, 0x53, 0x44, 0x4F, 0x53, 0x35, 0x2E, 0x30, 0x00, 0x02, 0x01, 0x1E, 0x21,
	0x02, 0x00, 0x00, 0x00, 0x00, 0xF8, 0x00, 0x00, 0x3F, 0x00, 0xFF, 0x00, 0x00, 0x08, 0x00, 0x00,
	0x00, 0xF8, 0x07, 0x00, 0x71, 0x0F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x80, 0x00, 0x29, 0x6F, 0xA4, 0x11, 0xB0, 0x4E, 0x4F, 0x20, 0x4E, 0x41, 0x4D, 0x45, 0x20, 0x20,
	0x20, 0x20, 0x46, 0x41, 0x54, 0x33, 0x32, 0x20, 0x20, 0x20, 0x33, 0xC9, 0x8E, 0xD1, 0xBC, 0xF4,
	0x7B, 0x8E, 0xC1, 0x8E, 0xD9, 0xBD, 0x00, 0x7C, 0x88, 0x4E, 0x02, 0x8A, 0x56, 0x40, 0xB4, 0x41,
	0xBB, 0xAA, 0x55, 0xCD, 0x13, 0x72, 0x10, 0x81, 0xFB, 0x55, 0xAA, 0x75, 0x0A, 0xF6, 0xC1, 0x01,
	0x74, 0x05, 0xFE, 0x46, 0x02, 0xEB, 0x2D, 0x8A, 0x56, 0x40, 0xB4, 0x08, 0xCD, 0x13, 0x73, 0x05,
	0xB9, 0xFF, 0xFF, 0x8A, 0xF1, 0x66, 0x0F, 0xB6, 0xC6, 0x40, 0x66, 0x0F, 0xB6, 0xD1, 0x80, 0xE2,
	0x3F, 0xF7, 0xE2, 0x86, 0xCD, 0xC0, 0xED, 0x06, 0x41, 0x66, 0x0F, 0xB7, 0xC9, 0x66, 0xF7, 0xE1,
	0x66, 0x89, 0x46, 0xF8, 0x83, 0x7E, 0x16, 0x00, 0x75, 0x38, 0x83, 0x7E, 0x2A, 0x00, 0x77, 0x32,
	0x66, 0x8B, 0x46, 0x1C, 0x66, 0x83, 0xC0, 0x0C, 0xBB, 0x00, 0x80, 0xB9, 0x01, 0x00, 0xE8, 0x2B,
	0x00, 0xE9, 0x2C, 0x03, 0xA0, 0xFA, 0x7D, 0xB4, 0x7D, 0x8B, 0xF0, 0xAC, 0x84, 0xC0, 0x74, 0x17,
	0x3C, 0xFF, 0x74, 0x09, 0xB4, 0x0E, 0xBB, 0x07, 0x00, 0xCD, 0x10, 0xEB, 0xEE, 0xA0, 0xFB, 0x7D,
	0xEB, 0xE5, 0xA0, 0xF9, 0x7D, 0xEB, 0xE0, 0x98, 0xCD, 0x16, 0xCD, 0x19, 0x66, 0x60, 0x80, 0x7E,
	0x02, 0x00, 0x0F, 0x84, 0x20, 0x00, 0x66, 0x6A, 0x00, 0x66, 0x50, 0x06, 0x53, 0x66, 0x68, 0x10,
	0x00, 0x01, 0x00, 0xB4, 0x42, 0x8A, 0x56, 0x40, 0x8B, 0xF4, 0xCD, 0x13, 0x66, 0x58, 0x66, 0x58,
	0x66, 0x58, 0x66, 0x58, 0xEB, 0x33, 0x66, 0x3B, 0x46, 0xF8, 0x72, 0x03, 0xF9, 0xEB, 0x2A, 0x66,
	0x33, 0xD2, 0x66, 0x0F, 0xB7, 0x4E, 0x18, 0x66, 0xF7, 0xF1, 0xFE, 0xC2, 0x8A, 0xCA, 0x66, 0x8B,
	0xD0, 0x66, 0xC1, 0xEA, 0x10, 0xF7, 0x76, 0x1A, 0x86, 0xD6, 0x8A, 0x56, 0x40, 0x8A, 0xE8, 0xC0,
	0xE4, 0x06, 0x0A, 0xCC, 0xB8, 0x01, 0x02, 0xCD, 0x13, 0x66, 0x61, 0x0F, 0x82, 0x75, 0xFF, 0x81,
	0xC3, 0x00, 0x02, 0x66, 0x40, 0x49, 0x75, 0x94, 0xC3, 0x42, 0x4F, 0x4F, 0x54, 0x4D, 0x47, 0x52,
	0x20, 0x20, 0x20, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0D, 0x0A, 0x52, 0x65,
	0x6D, 0x6F, 0x76, 0x65, 0x20, 0x64, 0x69, 0x73, 0x6B, 0x73, 0x20, 0x6F, 0x72, 0x20, 0x6F, 0x74,
	0x68, 0x65, 0x72, 0x20, 0x6D, 0x65, 0x64, 0x69, 0x61, 0x2E, 0xFF, 0x0D, 0x0A, 0x44, 0x69, 0x73,
	0x6B, 0x20, 0x65, 0x72, 0x72, 0x6F, 0x72, 0xFF, 0x0D, 0x0A, 0x50, 0x72, 0x65, 0x73, 0x73, 0x20,
	0x61, 0x6E, 0x79, 0x20, 0x6B, 0x65, 0x79, 0x20, 0x74, 0x6F, 0x20, 0x72, 0x65, 0x73, 0x74, 0x61,
	0x72, 0x74, 0x0D, 0x0A, 0x00, 0x00, 0x00, 0x00, 0x00, 0xAC, 0xCB, 0xD8, 0x00, 0x00, 0x55, 0xAA
};

static xfat_err_t create_vol_id_label(xdisk_t* disk, dbr_t* dbr) {
	xfile_time_t time;
	xfat_err_t err = xdisk_curr_time(disk, &time);
	if (err < 0) {
		return err;
	}

	u32_t sec = time.hour * 3600 + time.minute * 60 + time.second;
	dbr->fat32.BS_VolID = sec;
	memcpy(dbr->fat32.BS_VolLab, "NO NAME    ", 11);
	return FS_ERR_OK;
}

/**
 * 计算所需要的fat表扇区数
 * @param dbr dbr分区参数
 * @param xdisk_part 分区信息
 * @param ctrl 格式化控制参数
 * @return 每个fat表项大小
 */
static u32_t cal_fat_tbl_sectors(dbr_t* dbr, xdisk_part_t* xdisk_part, xfat_fmt_ctrl_t* ctrl) {
	// 保留区扇区数 + fat表数量 * fat表扇区数 + 数据区扇区数 = 总扇区数
	// fat表扇区数 * 扇区大小 / 4 = 数据区扇区数 / 每簇扇区数  
	// fat表扇区数=(总扇区数-保留区扇区数)/(FAT表数量+扇区大小*每簇扇区数/4)
	u32_t sector_size = xdisk_part->disk->sector_size;
	u32_t fat_dat_sectors = xdisk_part->total_sector - dbr->bpb.BPB_RsvdSecCnt;
	u32_t fat_sector_count = fat_dat_sectors / (dbr->bpb.BPB_NumFATs + sector_size * dbr->bpb.BPB_SecPerClus / sizeof(cluster32_t));

	return fat_sector_count;
}

static u32_t get_default_cluster_size(xdisk_part_t* disk_part) {
	u32_t sector_size = disk_part->disk->sector_size;
	u64_t part_size = disk_part->total_sector * sector_size;
	u32_t cluster_size;

	if (part_size <= XFAT_MB(64)) {
		cluster_size = XFAT_MAX(XFAT_CLUSTER_512B, sector_size);
	}
	else if (part_size <= XFAT_MB(128)) {
		cluster_size = XFAT_MAX(XFAT_CLUSTER_1K, sector_size);
	}
	else if (sector_size <= XFAT_MB(256)) {
		cluster_size = XFAT_MAX(XFAT_CLUSTER_2K, sector_size);
	}
	else if (sector_size <= XFAT_GB(8)) {
		cluster_size = XFAT_MAX(XFAT_CLUSTER_4K, sector_size);
	}
	else if (sector_size <= XFAT_GB(16)) {
		cluster_size = XFAT_MAX(XFAT_CLUSTER_8K, sector_size);
	}
	else if (sector_size <= XFAT_GB(32)) {
		cluster_size = XFAT_MAX(XFAT_CLUSTER_16K, sector_size);
	}
	else {
		cluster_size = XFAT_MAX(XFAT_CLUSTER_32K, sector_size);
	}

	return cluster_size;
}

/**
 * 根据分区及格式化参数，创建dbr头
 * @param dbr dbr头结构
 * @param xdisk_part 分区结构
 * @param ctrl 格式化参数
 * @return
 */
static xfat_err_t create_dbr(xdisk_part_t* xdisk_part, xfat_fmt_ctrl_t* ctrl, xfat_fmt_info_t* fmt_info) {
	xfat_err_t err;
	xdisk_t* disk = xdisk_part->disk;
	u32_t cluster_size;
	dbr_t* dbr;
	xfat_buf_t* buf = (xfat_buf_t*)0;

	// 计算簇大小，簇大小不能比扇区大小要小
	if ((u32_t)ctrl->cluster_size < disk->sector_size) {
		return FS_ERR_PARAM;
	}
	else if (ctrl->cluster_size == XFAT_CLUSTER_AUTO) {
		cluster_size = get_default_cluster_size(xdisk_part);
	}
	else {
		cluster_size = ctrl->cluster_size;
	}

	err = xfat_bpool_alloc(to_obj(disk), &buf, xdisk_part->start_sector);
	if (err < 0) {
		return err;
	}
	dbr = (dbr_t*)buf->buf;

	memset(dbr, 0, disk->sector_size);
	dbr->bpb.BS_jmpBoot[0] = 0xEB;          // 这几个跳转代码是必须的
	dbr->bpb.BS_jmpBoot[1] = 0x58;          // 不加win会识别为未格式化
	dbr->bpb.BS_jmpBoot[2] = 0x00;
	strncpy((char*)dbr->bpb.BS_OEMName, "XFAT SYS", 8);
	dbr->bpb.BPB_BytsPerSec = disk->sector_size;
	dbr->bpb.BPB_SecPerClus = to_sector(disk, cluster_size);
	dbr->bpb.BPB_RsvdSecCnt = 8478;         // 固定值为32，这里根据实际测试，设置为6182
	dbr->bpb.BPB_NumFATs = 2;               // 固定为2
	dbr->bpb.BPB_RootEntCnt = 0;            // FAT32未用
	dbr->bpb.BPB_TotSec16 = 0;              // FAT32未用
	dbr->bpb.BPB_Media = 0xF8;              // 固定值
	dbr->bpb.BPB_FATSz16 = 0;               // FAT32未用
	dbr->bpb.BPB_SecPerTrk = 0xFFFF;        // 不支持硬盘结构
	dbr->bpb.BPB_NumHeads = 0xFFFF;         // 不支持硬盘结构
	dbr->bpb.BPB_HiddSec = xdisk_part->relative_sector;    // 是否正确?
	dbr->bpb.BPB_TotSec32 = xdisk_part->total_sector;
	dbr->fat32.BPB_FATSz32 = cal_fat_tbl_sectors(dbr, xdisk_part, ctrl);
	dbr->fat32.BPB_ExtFlags = 0;            // 固定值，实时镜像所有FAT表
	dbr->fat32.BPB_FSVer = 0;               // 版本号，0
	dbr->fat32.BPB_RootClus = 2;            // 固定为2，如果为坏簇怎么办？
	dbr->fat32.BPB_FsInfo = 1;              // fsInfo的扇区号

	memset(dbr->fat32.BPB_Reserved, 0, 12);
	dbr->fat32.BS_DrvNum = 0x80;            // 固定为0
	dbr->fat32.BS_Reserved1 = 0;
	dbr->fat32.BS_BootSig = 0x29;           // 固定0x29
	err = create_vol_id_label(disk, dbr);
	if (err < 0) {
		return err;
	}
	memcpy(dbr->fat32.BS_FileSysType, "FAT32   ", 8);

	((u8_t*)dbr)[510] = 0x55;
	((u8_t*)dbr)[511] = 0xAA;

	err = xfat_bpool_write_sector(to_obj(disk), buf, 1);
	if (err < 0) {
		return err;
	}

	// 同时在备份区中写一个备份
	buf->sector_no = xdisk_part->start_sector + dbr->fat32.BPB_BkBootSec;
	err = xfat_bpool_write_sector(to_obj(disk), buf, 0);
	if (err < 0) {
		return err;
	}

	// 提取格式化相关的参数信息，避免占用内部缓冲区
	fmt_info->fat_count = dbr->bpb.BPB_NumFATs;
	fmt_info->media = dbr->bpb.BPB_Media;
	fmt_info->fat_sectors = dbr->fat32.BPB_FATSz32;
	fmt_info->rsvd_sectors = dbr->bpb.BPB_RsvdSecCnt;
	fmt_info->root_cluster = dbr->fat32.BPB_RootClus;
	fmt_info->sec_per_cluster = dbr->bpb.BPB_SecPerClus;
	fmt_info->backup_sector = dbr->fat32.BPB_BkBootSec;
	fmt_info->fsinfo_sector = dbr->fat32.BPB_FsInfo;
	return err;
}

int xfat_is_fs_supported(xfs_type_t type) {
	switch (type)
	{
	case FS_FAT32:
	case FS_WIN95_FAT32_0:
	case FS_WIN95_FAT32_1:
		return 1;
	default:
		return 0;
	}
}

/**
 * 格式化FAT表
 * @param dbr db结构
 * @param xdisk_part 分区信息
 * @param ctrl 格式化参数
 * @return
 */
static xfat_err_t create_fat_table(xfat_fmt_info_t* fmt_info, xdisk_part_t* xdisk_part, xfat_fmt_ctrl_t* ctrl) {
	u32_t i, j;
	xdisk_t* disk = xdisk_part->disk;
	xfat_err_t err = FS_ERR_OK;
	u32_t fat_start_sector = fmt_info->rsvd_sectors + xdisk_part->start_sector;
	xfat_buf_t* buf = (xfat_buf_t*)0;

	err = xfat_bpool_alloc(to_obj(disk), &buf, fat_start_sector);
	if (err < 0) {
		return err;
	}
	cluster32_t* fat_buffer = (cluster32_t*)buf->buf;
	// 逐个写多个FAT表，最好是挂载磁盘后查看下FAT表二进制内容
	memset(fat_buffer, 0, disk->sector_size);
	for (i = 0; i < fmt_info->fat_count; i++) {
		buf->sector_no = fat_start_sector + fmt_info->fat_sectors * i;

		// 每个FAT表的前1、2簇已经被占用, 簇2分配给根目录，保留
		fat_buffer[0].v = (u32_t)(0x0FFFFF00 | fmt_info->media);
		fat_buffer[1].v = 0x0FFFFFFF;
		fat_buffer[2].v = 0x0FFFFFFF;
		err = xfat_bpool_write_sector(to_obj(disk), buf, 1);
		if (err < 0) {
			return err;
		}

		// 再写其余扇区的簇
		fat_buffer[0].v = fat_buffer[1].v = fat_buffer[2].v = 0;
		for (j = 1; j < fmt_info->fat_sectors; j++) {
			++buf->sector_no;
			err = xfat_bpool_write_sector(to_obj(disk), buf, 1);
			if (err < 0) {
				return err;
			}
		}
	}
	return err;
}

static xfat_err_t diritem_init_default(diritem_t* dir_item, xdisk_t* disk, u8_t is_dir, const char* name, u32_t cluster);
static xfat_err_t create_root_dir(xfat_fmt_info_t* fmt_info, xdisk_part_t* disk_part, xfat_fmt_ctrl_t* ctrl) {
	xfat_err_t err;
	xdisk_t* disk = disk_part->disk;

	xfat_buf_t* buf = (xfat_buf_t*)0;
	diritem_t* diritem;
	u32_t data_sector = fmt_info->rsvd_sectors +
		(fmt_info->fat_count * fmt_info->fat_sectors) +
		(fmt_info->root_cluster - 2) * fmt_info->sec_per_cluster;

	err = xfat_bpool_alloc(to_obj(disk), &buf, disk_part->start_sector + data_sector);
	diritem = (diritem_t*)buf->buf;

	memset(buf->buf, 0, disk->sector_size);
	for (u32_t i = 0; i < fmt_info->sec_per_cluster; i++) {
		err = xfat_bpool_write_sector(to_obj(disk), buf, 1);
		buf->sector_no++;
		if (err < 0) {
			return err;
		}
	}

	if (ctrl->vol_name) {
		diritem_init_default(diritem, disk, 0, ctrl->vol_name ? ctrl->vol_name : "DISK", 0);
		diritem->DIR_Attr |= DIRITEM_ATTR_VOLUME_ID;

		buf->sector_no = disk_part->start_sector + data_sector;
		err = xfat_bpool_write_sector(to_obj(disk), buf, 0);
		if (err < 0) {
			return err;
		}
	}

	return FS_ERR_OK;
}

static xfat_err_t create_fsinfo(xfat_fmt_info_t* fmt_info, xdisk_part_t* xdisk_part, xfat_fmt_ctrl_t* ctrl) {
	u32_t total_free = fmt_info->fat_sectors * xdisk_part->disk->sector_size / sizeof(cluster32_t) - (2 + 1);
	return save_cluster_free_info(xdisk_part->disk, total_free, 3,
		fmt_info->fsinfo_sector, fmt_info->fsinfo_sector);
}

static xfat_err_t rewrite_partition_table(xdisk_part_t* disk_part, xfat_fmt_ctrl_t* ctrl) {
	return xdisk_set_part_type(disk_part, ctrl->type);
}

xfat_err_t xfat_format(xdisk_part_t* disk_part, xfat_fmt_ctrl_t* ctrl) {
	if (!xfat_is_fs_supported(ctrl->type)) {
		return FS_ERR_INVALID_FS;
	}

	xfat_fmt_info_t fmt_info;
	u32_t err = create_dbr(disk_part, ctrl, &fmt_info);
	if (err < 0) {
		return err;
	}

	err = create_fat_table(&fmt_info, disk_part, ctrl);
	if (err < 0) {
		return err;
	}

	err = create_root_dir(&fmt_info, disk_part, ctrl);
	if (err < 0) {
		return err;
	}

	err = create_fsinfo(&fmt_info, disk_part, ctrl);
	if (err < 0) {
		return err;
	}

	err = rewrite_partition_table(disk_part, ctrl);

	return err;
}

// 获取fat文件系统中，第cluster_no个簇的第一个扇区编号
u32_t cluster_first_sector(xfat_t* xfat, u32_t cluster_no) {
	u32_t data_start_sector = xfat->fat_start_sector + xfat->fat_tbl_sectors * xfat->fat_tbl_nr;
	// 需要减去起始的两个保留簇，才是真实的簇号
	return data_start_sector + (cluster_no - 2) * xfat->sec_per_cluster;
}

u32_t to_phy_sector(xfat_t* xfat, u32_t cluster, u32_t cluster_offset) {
	xdisk_t* disk = xfat_get_disk(xfat);
	return cluster_first_sector(xfat, cluster) + to_sector(disk, cluster_offset);
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

static xfat_err_t destory_cluster_chain(xfat_t* xfat, u32_t cluster) {
	u32_t curr_cluster = cluster;

	while (is_cluster_valid(curr_cluster)) {
		xfat_buf_t* buf = (xfat_buf_t*)0;
		xfat_err_t err = xfat_bpool_read_sector(to_obj(xfat), &buf, to_fat_sector(xfat, curr_cluster));
		if (err < 0) {
			return err;
		}
		cluster32_t* cluster32_buf = (cluster32_t*)(buf->buf + to_fat_offset(xfat, curr_cluster));
		u32_t next_cluster = cluster32_buf->s.next;
		cluster32_buf->s.next = CLUSTER_FREE;
		err = xfat_bpool_write_sector(to_obj(xfat), buf, 1);
		if (err < 0) {
			return err;
		}

		for (u32_t i = 0; i < xfat->fat_tbl_nr; i++) {
			buf->sector_no += xfat->fat_tbl_sectors;
			err = xfat_bpool_write_sector(to_obj(xfat), buf, 1);
			if (err < 0) {
				return err;
			}
		}

		curr_cluster = next_cluster;
		xfat->cluster_total_free++;
	}

	if (!is_cluster_valid(xfat->cluster_next_free)) {
		xfat->cluster_next_free = cluster;
	}

	return FS_ERR_OK;
}

xfat_err_t move_cluster_pos(xfat_t* xfat, u32_t curr_cluster, u32_t curr_offset, u32_t move_bytes, u32_t* next_cluster, u32_t* next_offset) {
	if (curr_offset + move_bytes >= xfat->cluster_byte_size) {
		xfat_err_t err = get_next_cluster(xfat, curr_cluster, next_cluster);
		if (err < 0) {
			return err;
		}

		*next_offset = 0;
	}
	else {
		*next_cluster = curr_cluster;
		*next_offset = curr_offset + move_bytes;
	}
	return FS_ERR_OK;
}

xfat_err_t get_next_diritem(xfat_t* xfat, u8_t type, u32_t start_cluster,
	u32_t start_offset, u32_t* found_cluster, u32_t* found_offset,
	u32_t* next_cluster, u32_t* next_offset, xfat_buf_t** buf, diritem_t** diritem) {
	diritem_t* r_diritem;
	while (is_cluster_valid(start_cluster)) {
		xfat_err_t err = move_cluster_pos(xfat, start_cluster, start_offset, sizeof(diritem_t), next_cluster, next_offset);
		if (err < 0) {
			return err;
		}

		u32_t sector_offset = to_sector_offset(xfat_get_disk(xfat), start_offset);
		u32_t curr_sector = to_phy_sector(xfat, start_cluster, start_offset);
		err = xfat_bpool_read_sector(to_obj(xfat), buf, curr_sector);
		if (err < 0) {
			return err;
		}

		r_diritem = (diritem_t*)((*buf)->buf + sector_offset);
		switch (r_diritem->DIR_Name[0]) {
		case DIRITEM_NAME_END:
			if (type & DIRITEM_GET_END) {
				*diritem = r_diritem;
				*found_cluster = start_cluster;
				*found_offset = start_offset;
				return FS_ERR_OK;
			}
			break;
		case DIRITEM_NAME_FREE:
			if (type & DIRITEM_GET_FREE) {
				*diritem = r_diritem;
				*found_cluster = start_cluster;
				*found_offset = start_offset;
				return FS_ERR_OK;
			}
			break;
		default:
			if (type & DIRITEM_GET_USED) {
				*diritem = r_diritem;
				*found_cluster = start_cluster;
				*found_offset = start_offset;
				return FS_ERR_OK;
			}
			break;
		}

		start_cluster = *next_cluster;
		start_offset = *next_offset;
	}

	*diritem = (diritem_t*)0;
	return FS_ERR_EOF;
}

int is_cluster_valid(u32_t cluster) {
	cluster &= 0x0FFFFFFF;
	return (cluster < 0x0FFFFFF0) && (cluster >= 2);
}

xfat_err_t get_next_cluster(xfat_t* xfat, u32_t curr_cluster, u32_t* next_cluster) {
	if (is_cluster_valid(curr_cluster)) {
		xfat_buf_t* buf;
		xfat_err_t err = xfat_bpool_read_sector(to_obj(xfat), &buf, to_fat_sector(xfat, curr_cluster));
		if (err < 0) {
			return err;
		}

		cluster32_t* cluster32_buf = (cluster32_t*)(buf->buf + to_fat_offset(xfat, curr_cluster));
		*next_cluster = cluster32_buf->s.next;
	}
	else {
		*next_cluster = CLUSTER_INVALID;
	}

	return FS_ERR_OK;
}

static xfat_err_t to_sfn(char* dest_name, const char* my_name) {
	int i, name_len;
	char* dest = dest_name;
	const char* ext_dot;
	const char* p;
	int ext_existed;

	memset(dest, ' ', SFN_LEN);

	// 跳过开头的分隔符
	while (is_path_sep(*my_name)) {
		my_name++;
	}

	// 找到第一个斜杠之前的字符串，将ext_dot定位到那里，且记录有效长度
	ext_dot = my_name;
	p = my_name;
	name_len = 0;
	while ((*p != '\0') && !is_path_sep(*p)) {
		if (*p == '.') {
			ext_dot = p;
		}
		p++;
		name_len++;
	}

	// 如果文件名以.结尾，意思就是没有扩展名？
	// todo: 长文件名处理?
	ext_existed = (ext_dot > my_name) && (ext_dot < (my_name + name_len - 1));

	// 遍历名称，逐个复制字符, 算上.分隔符，最长12字节，如果分离符，则只应有
	p = my_name;
	for (i = 0; (i < SFN_LEN) && (*p != '\0') && !is_path_sep(*p); i++) {
		if (ext_existed) {
			if (p == ext_dot) {
				dest = dest_name + 8;
				p++;
				i--;
				continue;
			}
			else if (p < ext_dot) {
				*dest++ = toupper(*p++);
			}
			else {
				*dest++ = toupper(*p++);
			}
		}
		else {
			*dest++ = toupper(*p++);
		}
	}
	return FS_ERR_OK;
}

/**
 * 检查sfn字符串中是否是大写。如果中间有任意小写，都认为是小写
 * @param name
 * @return
 */
static u8_t get_sfn_case_cfg(const char* sfn_name) {
	u8_t case_cfg = 0;

	int name_len;
	const char* src_name = sfn_name;
	const char* ext_dot;
	const char* p;
	int ext_existed;

	// 跳过开头的分隔符
	while (is_path_sep(*src_name)) {
		src_name++;
	}

	// 找到第一个斜杠之前的字符串，将ext_dot定位到那里，且记录有效长度
	ext_dot = src_name;
	p = src_name;
	name_len = 0;
	while ((*p != '\0') && !is_path_sep(*p)) {
		if (*p == '.') {
			ext_dot = p;
		}
		p++;
		name_len++;
	}

	// 如果文件名以.结尾，意思就是没有扩展名？
	// todo: 长文件名处理?
	ext_existed = (ext_dot > src_name) && (ext_dot < (src_name + name_len - 1));
	for (p = src_name; p < src_name + name_len; p++) {
		if (ext_existed) {
			if (p < ext_dot) { // 文件名主体部分大小写判断
				case_cfg |= islower(*p) ? DIRITEM_NTRES_BODY_LOWER : 0;
			}
			else if (p > ext_dot) {
				case_cfg |= islower(*p) ? DIRITEM_NTRES_EXT_LOWER : 0;
			}
		}
		else {
			case_cfg |= islower(*p) ? DIRITEM_NTRES_BODY_LOWER : 0;
		}
	}

	return case_cfg;
}

static int is_filename_match(const char* name_in_dir, const char* to_find_name) {
	char temp_name[SFN_LEN];
	to_sfn(temp_name, to_find_name);
	return memcmp(temp_name, name_in_dir, SFN_LEN) == 0;
}

static const char* skip_first_path_sep(const char* path) {
	const char* c = path;

	if (c == (const char*)0) {
		return (const char*)0;
	}

	while (is_path_sep(*c)) {
		c++;
	}
	return c;
}

static const char* get_child_path(const char* dir_path) {
	const char* c = skip_first_path_sep(dir_path);

	// /a/b/c/d.txt -> b/c/d.txt
	while ((*c != '\0') && !is_path_sep(*c)) {
		c++;
	}

	return (*c == '\0') ? (const char*)0 : c + 1;
}

static xfile_type_t get_file_type(const diritem_t* diritem) {
	if (diritem->DIR_Attr & DIRITEM_ATTR_VOLUME_ID) {
		return FAT_VOL;
	}
	else if (diritem->DIR_Attr & DIRITEM_ATTR_DIRECTORY) {
		return FAT_DIR;
	}
	else {
		return FAT_FILE;
	}
}

static void set_diritem_cluster(diritem_t* item, u32_t cluster) {
	item->DIR_FstClusHI = (u16_t)(cluster >> 16);
	item->DIR_FstClusL0 = (u16_t)(cluster & 0xFFFF);
}

static xfat_err_t erase_cluster(xfat_t* xfat, u32_t cluster, u32_t erase_state) {
	u32_t sector = cluster_first_sector(xfat, cluster);
	xdisk_t* disk = xfat_get_disk(xfat);
	xfat_buf_t* buf = (xfat_buf_t*)0;
	xfat_err_t err = xfat_bpool_alloc(to_obj(xfat), &buf, sector);
	if (err < 0) {
		return err;
	}

	memset(buf->buf, erase_state, disk->sector_size);
	for (u32_t i = 0; i < xfat->sec_per_cluster; i++) {
		xfat_err_t err = xfat_bpool_write_sector(to_obj(xfat), buf, 1);
		buf->sector_no++;
		if (err < 0) {
			return err;
		}
	}
	return FS_ERR_OK;
}

static xfat_err_t put_next_cluster(xfat_t* xfat, u32_t curr_cluster, u32_t next_cluster) {
	if (is_cluster_valid(curr_cluster)) {
		xfat_buf_t* buf;
		xfat_err_t err = xfat_bpool_read_sector(to_obj(xfat), &buf, to_fat_sector(xfat, curr_cluster));
		if (err < 0) return err;

		cluster32_t* cluster32_buf = (cluster32_t*)(buf->buf + to_fat_offset(xfat, curr_cluster));
		cluster32_buf->s.next = next_cluster;

		err = xfat_bpool_write_sector(to_obj(xfat), buf, 1);
		if (err < 0) return err;

		for (u32_t i = 1; i < xfat->fat_tbl_nr; i++) {
			buf->sector_no += xfat->fat_tbl_sectors;
			err = xfat_bpool_write_sector(to_obj(xfat), buf, 1);
			if (err < 0) return err;
		}
	}

	return FS_ERR_OK;
}

static xfat_err_t allocate_free_cluster(xfat_t* xfat, u32_t curr_cluster, u32_t count,
	u32_t* r_start_cluster, u32_t* r_allocated_count, u8_t en_erase, u8_t erase_data) {
	u32_t allocated_count = 0;
	u32_t searched_count = 0;
	u32_t pre_cluster = curr_cluster;
	u32_t first_free_cluster = CLUSTER_INVALID;

	u32_t total_clusters = xfat->fat_tbl_sectors * xfat_get_disk(xfat)->sector_size / sizeof(cluster32_t);
	while (xfat->cluster_total_free &&
		(allocated_count < count) &&
		(searched_count < total_clusters)) {
		u32_t next_cluster;
		xfat_err_t err = get_next_cluster(xfat, xfat->cluster_next_free, &next_cluster);
		if (err < 0) {
			destory_cluster_chain(xfat, curr_cluster);
			return err;
		}
		if (next_cluster == CLUSTER_FREE) {
			u32_t free_cluster = xfat->cluster_next_free;
			err = put_next_cluster(xfat, pre_cluster, free_cluster);
			if (err < 0) {
				destory_cluster_chain(xfat, curr_cluster);
				return err;
			}

			if (en_erase) {
				err = erase_cluster(xfat, free_cluster, 0);
				if (err < 0) {
					destory_cluster_chain(xfat, curr_cluster);
					return err;
				}
			}

			pre_cluster = free_cluster;
			xfat->cluster_total_free--;
			allocated_count++;

			if (allocated_count == 1) {
				first_free_cluster = xfat->cluster_next_free;
			}
		}

		xfat->cluster_next_free++;
		if (xfat->cluster_next_free >= total_clusters) {
			xfat->cluster_next_free = 0;
		}

		searched_count++;
	}

	if (allocated_count) {
		xfat_err_t err = put_next_cluster(xfat, pre_cluster, CLUSTER_INVALID);
		if (err < 0) {
			destory_cluster_chain(xfat, curr_cluster);
			return err;
		}
	}

	if (r_allocated_count) {
		*r_allocated_count = allocated_count;
	}

	if (r_start_cluster) {
		*r_start_cluster = first_free_cluster;
	}

	return FS_ERR_OK;
}

static u32_t get_diritem_cluster(diritem_t* item) {
	return (item->DIR_FstClusHI << 16) | item->DIR_FstClusL0;
}

static void sfn_to_myname(char* dest_name, const diritem_t* diritem) {
	char* dest = dest_name;
	char* raw_name = (char*)diritem->DIR_Name;
	u8_t ext_exist = raw_name[8] != 0x20;
	u8_t scan_len = ext_exist ? SFN_LEN + 1 : SFN_LEN;

	memset(dest_name, 0, X_FILEINFO_NAME_SIZE);
	for (int i = 0; i < scan_len; i++) {
		if (*raw_name == ' ') {
			raw_name++;
		}
		else if (i == 8 && ext_exist) {
			*dest++ = '.';
		}
		else {
			u8_t lower = 0;
			if (((i < 8) && (diritem->DIR_NTRes & DIRITEM_NTRES_BODY_LOWER)) ||
				((i > 8) && (diritem->DIR_NTRes & DIRITEM_NTRES_EXT_LOWER))) {
				lower = 1;
			}
			*dest++ = lower ? tolower(*raw_name++) : toupper(*raw_name++);
		}
	}

	*dest = '\0';
}

static void copy_date_time(xfile_time_t* dest, const diritem_date_t* date, const diritem_time_t* time, const u8_t mil_sec) {
	if (date) {
		dest->year = date->year_from_1980 + (u16_t)1980;
		dest->month = (u8_t)date->month;
		dest->day = (u8_t)date->day;
	}
	else {
		dest->year = 0;
		dest->month = 0;
		dest->day = 0;
	}

	if (time) {
		dest->hour = (u8_t)time->hour;
		dest->minute = (u8_t)time->minute;
		dest->second = (u8_t)time->second_2 * 2 + mil_sec / 100;
	}
	else {
		dest->hour = 0;
		dest->minute = 0;
		dest->second = 0;
	}
}

static void copy_file_info(xfileinfo_t* info, const diritem_t* dir_item) {
	sfn_to_myname(info->file_name, dir_item);
	info->size = dir_item->DIR_FileSize;
	info->attr = dir_item->DIR_Attr;
	info->type = get_file_type(dir_item);

	copy_date_time(&info->create_time, &dir_item->DIR_CrtDate, &dir_item->DIR_CrtTime, dir_item->DIR_CrtTimeTeenth);
	copy_date_time(&info->last_acctime, &dir_item->DIR_LastAccDate, 0, 0);
	copy_date_time(&info->modify_time, &dir_item->DIR_WrtDate, &dir_item->DIR_WrtTime, 0);
}

/**
 * 检查文件名和类型是否匹配
 * @param dir_item
 * @param locate_type
 * @return
 */
static u8_t is_locate_type_match(diritem_t* dir_item, u8_t locate_type) {
	u8_t match = 1;

	if ((dir_item->DIR_Attr & DIRITEM_ATTR_SYSTEM) && !(locate_type & XFILE_LOCATE_SYSTEM)) {
		match = 0;  // 不显示系统文件
	}
	else if ((dir_item->DIR_Attr & DIRITEM_ATTR_HIDDEN) && !(locate_type & XFILE_LOCATE_HIDDEN)) {
		match = 0;  // 不显示隐藏文件
	}
	else if ((dir_item->DIR_Attr & DIRITEM_ATTR_VOLUME_ID) && !(locate_type & XFILE_LOCATE_VOL)) {
		match = 0;  // 不显示卷标
	}
	else if ((memcmp(DOT_FILE, dir_item->DIR_Name, SFN_LEN) == 0)
		|| (memcmp(DOT_DOT_FILE, dir_item->DIR_Name, SFN_LEN) == 0)) {
		if (!(locate_type & XFILE_LOCATE_DOT)) {
			match = 0;// 不显示dot文件
		}
	}
	else if (!(locate_type & XFILE_LOCATE_NORMAL)) {
		match = 0;
	}
	return match;
}

static xfat_err_t locate_file_dir_item(xfat_t* xfat, u8_t locate_type, u32_t* dir_cluster, u32_t* cluster_offset, const char* path, u32_t* r_moved_bytes, diritem_t** r_diritem) {
	u32_t curr_cluster = *dir_cluster;
	xdisk_t* xdisk = xfat_get_disk(xfat);
	u32_t initial_sector = to_sector(xdisk, *cluster_offset);
	u32_t initial_offset = to_sector_offset(xdisk, *cluster_offset);
	u32_t move_bytes = 0;
	xfat_buf_t* buf = (xfat_buf_t*)0;
	do {
		u32_t start_sector = cluster_first_sector(xfat, curr_cluster);
		for (u32_t i = initial_sector; i < xfat->sec_per_cluster; i++) {
			xfat_err_t err = xfat_bpool_read_sector(to_obj(xfat), &buf, start_sector + i);
			if (err < 0) {
				return err;
			}

			for (int j = initial_offset / sizeof(diritem_t); j < xdisk->sector_size / sizeof(diritem_t); j++) {
				diritem_t* dir_item = ((diritem_t*)buf->buf) + j;
				if (dir_item->DIR_Name[0] == DIRITEM_NAME_END) {
					return FS_ERR_EOF;
				}
				else if (dir_item->DIR_Name[0] == DIRITEM_NAME_FREE) {
					move_bytes += sizeof(diritem_t);
					continue;
				}
				else if (!is_locate_type_match(dir_item, locate_type)) {
					move_bytes += sizeof(diritem_t);
					continue;
				}

				if (path == (const char*)0 ||
					(*path == 0) ||
					is_filename_match((const char*)dir_item->DIR_Name, path)) {
					u32_t total_offset = i * xdisk->sector_size + j * sizeof(diritem_t);
					*dir_cluster = curr_cluster;
					*cluster_offset = total_offset;
					*r_moved_bytes = move_bytes + sizeof(diritem_t);
					if (r_diritem) {
						*r_diritem = dir_item;
					}
					return FS_ERR_OK;
				}

				move_bytes += sizeof(diritem_t);
			}
		}
		xfat_err_t err = get_next_cluster(xfat, curr_cluster, &curr_cluster);
		if (err < 0) {
			return 0;
		}
		initial_sector = 0;
		initial_offset = 0;
	} while (is_cluster_valid(curr_cluster));

	return FS_ERR_EOF;
}

static xfat_err_t open_sub_file(xfat_t* xfat, u32_t dir_cluster, xfile_t* file, const char* path) {
	path = skip_first_path_sep(path);
	u32_t parent_cluster = dir_cluster;
	u32_t parent_cluster_offset = 0;
	u32_t file_start_cluster = 0;

	xfat_obj_init(to_obj(file), XFAT_OBJ_FILE);

	xfat_err_t err = xfat_bpool_init(&file->obj, 0, 0, 0);
	if (err < 0) {
		return err;
	}

	if ((path != 0) && (*path != '\0')) {
		// a/b/c/d.txt
		const char* curr_path = path;
		diritem_t* diritem = (diritem_t*)0;
		while (curr_path != (const char*)0) {
			u32_t moved_bytes = 0;
			diritem = (diritem_t*)0;
			xfat_err_t err = locate_file_dir_item(xfat, XFILE_LOCATE_DOT | XFILE_LOCATE_NORMAL, &parent_cluster, &parent_cluster_offset,
				curr_path, &moved_bytes, &diritem);
			if (err < 0) {
				return err;
			}
			if (diritem == (diritem_t*)0) {
				return FS_ERR_NONE;
			}

			curr_path = get_child_path(curr_path);
			if (curr_path != (const char*)0) {
				parent_cluster = get_diritem_cluster(diritem);
				parent_cluster_offset = 0;
			}
			else {
				file_start_cluster = get_diritem_cluster(diritem);
				if (memcmp((void*)(diritem->DIR_Name), DOT_DOT_FILE, SFN_LEN) == 0 && (file_start_cluster == 0)) {
					file_start_cluster = xfat->root_cluster;
				}
			}
		}

		file->size = diritem->DIR_FileSize;
		file->type = get_file_type(diritem);
		file->attr = (diritem->DIR_Attr & DIRITEM_ATTR_READ_ONLY) ? XFILE_ATTR_READONLY : 0;
		file->start_cluster = file_start_cluster;
		file->curr_cluster = file_start_cluster;
		file->dir_cluster = parent_cluster;
		file->dir_cluster_offset = parent_cluster_offset;
	}
	else {
		file->size = 0;
		file->type = FAT_DIR;
		file->attr = 0;
		file->start_cluster = parent_cluster;
		file->curr_cluster = parent_cluster;
		file->dir_cluster = CLUSTER_INVALID;
		file->dir_cluster_offset = 0;
	}

	file->xfat = xfat;
	file->pos = 0;
	file->err = FS_ERR_OK;
	file->attr = 0;

	return FS_ERR_OK;
}

xfat_err_t xfile_open(xfile_t* file, const char* path) {
	xfat_t* xfat = xfat_find_by_name(path);
	if (xfat == (xfat_t*)0) {
		return FS_ERR_NOT_MOUNT;
	}

	path = get_child_path(path);
	if (!is_path_end(path)) {
		path = skip_first_path_sep(path);
		if (memcmp(path, "..", 2) == 0) {
			// /..
			return FS_ERR_PARAM;
		}
		else if (memcmp(path, ".", 1) == 0) {
			path++;
		}
	}

	return open_sub_file(xfat, xfat->root_cluster, file, path);
}

xfat_err_t xfile_open_sub(xfile_t* dir, const char* sub_path, xfile_t* sub_file) {
	if (dir->type != FAT_DIR) {
		return FS_ERR_PARAM;
	}

	sub_path = skip_first_path_sep(sub_path);
	if (memcmp(sub_path, ".", 1) == 0) {
		return FS_ERR_PARAM;
	}

	return open_sub_file(dir->xfat, dir->start_cluster, sub_file, sub_path);
}

xfat_err_t xfile_close(xfile_t* file) {
	return FS_ERR_OK;
}

xfat_err_t xfile_set_buf(xfile_t* file, u8_t* buf, u32_t size) {
	xfat_t* xfat = file->xfat;
	xdisk_part_t* part = xfat->disk_part;

	xfat_err_t err = xfat_bpool_flush_sectors(to_obj(xfat), part->start_sector, part->total_sector);
	if (err < 0) {
		return err;
	}

	err = xfat_bpool_invalid_sectors(to_obj(xfat), part->start_sector, part->total_sector);
	if (err < 0) {
		return err;
	}

	return xfat_bpool_init(to_obj(file), xfat_get_disk(xfat)->sector_size, buf, size);
}

xfat_err_t xdir_first_file(xfile_t* file, xfileinfo_t* info) {
	if (file->type != FAT_DIR) {
		return FS_ERR_PARAM;
	}

	file->curr_cluster = file->start_cluster;
	file->pos = 0;

	u32_t cluster_offset = 0;
	u32_t moved_bytes = 0;
	diritem_t* diritem = (diritem_t*)0;
	xfat_err_t err = locate_file_dir_item(file->xfat, XFILE_LOCATE_NORMAL, &file->curr_cluster, &cluster_offset, "", &moved_bytes, &diritem);
	if (err < 0) {
		return err;
	}
	if (diritem == (diritem_t*)0) {
		return FS_ERR_EOF;
	}
	file->pos += moved_bytes;
	copy_file_info(info, diritem);
	return err;
}

xfat_err_t xdir_next_file(xfile_t* file, xfileinfo_t* info) {
	if (file->type != FAT_DIR) {
		return FS_ERR_PARAM;
	}

	u32_t cluster_offset = to_cluster_offset(file->xfat, file->pos);
	u32_t moved_bytes = 0;
	diritem_t* diritem = (diritem_t*)0;
	xfat_err_t err = locate_file_dir_item(file->xfat, XFILE_LOCATE_NORMAL, &file->curr_cluster, &cluster_offset, "", &moved_bytes, &diritem);
	if (err != FS_ERR_OK) {
		return err;
	}
	file->pos += moved_bytes;
	if (cluster_offset + sizeof(diritem_t) >= file->xfat->cluster_byte_size) {
		err = get_next_cluster(file->xfat, file->curr_cluster, &file->curr_cluster);
		if (err != FS_ERR_OK) {
			return err;
		}
	}

	copy_file_info(info, diritem);
	return err;
}

xfat_err_t xfile_error(xfile_t* file) {
	return file->err;
}

void xfile_clear_err(xfile_t* file) {
	file->err = FS_ERR_OK;
}

static xfat_err_t move_file_pos(xfile_t* file, u32_t move_bytes) {
	u32_t to_move = move_bytes;
	u32_t cluster_offset;

	// 不要超过文件的大小
	if (file->pos + move_bytes >= file->size) {
		to_move = file->size - file->pos;
	}

	// 簇间移动调整，需要调整簇
	cluster_offset = to_cluster_offset(file->xfat, file->pos);
	if (cluster_offset + to_move >= file->xfat->cluster_byte_size) {
		u32_t curr_cluster = file->curr_cluster;
		xfat_err_t err = get_next_cluster(file->xfat, curr_cluster, &curr_cluster);
		if (err != FS_ERR_OK) {
			file->err = err;
			return err;
		}

		if (is_cluster_valid(curr_cluster)) {
			file->curr_cluster = curr_cluster;
		}
	}

	file->pos += to_move;
	return FS_ERR_OK;
}

static xfat_err_t diritem_init_default(diritem_t* dir_item, xdisk_t* disk, u8_t is_dir, const char* name, u32_t cluster) {
	xfile_time_t timeinfo;
	xfat_err_t err = xdisk_curr_time(disk, &timeinfo);
	if (err < 0) {
		return err;
	}
	to_sfn((char*)dir_item->DIR_Name, name);
	set_diritem_cluster(dir_item, cluster);
	dir_item->DIR_FileSize = 0;
	dir_item->DIR_Attr = (u8_t)(is_dir ? DIRITEM_ATTR_DIRECTORY : 0);
	dir_item->DIR_NTRes = get_sfn_case_cfg(name);

	dir_item->DIR_CrtTime.hour = timeinfo.hour;
	dir_item->DIR_CrtTime.minute = timeinfo.minute;
	dir_item->DIR_CrtTime.second_2 = (u16_t)timeinfo.second / 2;
	dir_item->DIR_CrtTimeTeenth = (u8_t)((timeinfo.second & 1) * 1000);

	dir_item->DIR_CrtDate.year_from_1980 = (u16_t)(timeinfo.year - 1980);
	dir_item->DIR_CrtDate.month = timeinfo.month;
	dir_item->DIR_CrtDate.day = timeinfo.day;

	dir_item->DIR_WrtTime = dir_item->DIR_CrtTime;
	dir_item->DIR_WrtDate = dir_item->DIR_CrtDate;
	dir_item->DIR_LastAccDate = dir_item->DIR_CrtDate;

	return FS_ERR_OK;
}

static xfat_err_t create_sub_file(xfat_t* xfat, u8_t is_dir, u32_t parent_cluster,
	const char* child_name, u32_t* file_cluster) {
	xdisk_t* disk = xfat_get_disk(xfat);
	u32_t found_cluster;
	u32_t found_offset;
	u32_t next_cluster;
	u32_t next_offset;
	u32_t curr_cluster = parent_cluster;
	u32_t curr_offset = 0;

	diritem_t* target_item = (diritem_t*)0;
	u32_t free_item_cluster = CLUSTER_INVALID;
	u32_t free_item_offset = 0;
	u32_t file_diritem_sector;
	xfat_buf_t* buf = (xfat_buf_t*)0;

	do {
		diritem_t* diritem = (diritem_t*)0;

		xfat_err_t err = get_next_diritem(xfat, DIRITEM_GET_ALL, curr_cluster, curr_offset, &found_cluster,
			&found_offset, &next_cluster, &next_offset, &buf, &diritem);
		if (err < 0) {
			return err;
		}

		if (diritem == (diritem_t*)0) {
			break;
		}

		if (diritem->DIR_Name[0] == DIRITEM_NAME_END) {
			target_item = diritem;
			break;
		}
		else if (diritem->DIR_Name[0] == DIRITEM_NAME_FREE) {
			// 空闲项, 还要继续检查，看是否有同名项
			// 记录空闲项的位置
			if (!is_cluster_valid(free_item_cluster)) {
				free_item_cluster = curr_cluster;
				free_item_offset = curr_offset;
			}
		}
		else if (is_filename_match((const char*)diritem->DIR_Name, child_name)) {
			int item_is_dir = diritem->DIR_Attr & DIRITEM_ATTR_DIRECTORY;
			if ((is_dir && item_is_dir) || (!is_dir && !item_is_dir)) {
				*file_cluster = get_diritem_cluster(diritem);
				return FS_ERR_EXISTED;
			}
			else {
				return FS_ERR_NAME_USED;
			}
		}

		curr_cluster = next_cluster;
		curr_offset = next_offset;
	} while (1);

	u32_t file_first_cluster = 0;
	if (is_dir && strncmp(".", child_name, 1) && strncmp("..", child_name, 2)) {
		u32_t cluster_count;
		xfat_err_t err = allocate_free_cluster(xfat, CLUSTER_INVALID, 1, &file_first_cluster, &cluster_count, 1, 0);
		if (err < 0) {
			return err;
		}
		if (cluster_count < 1) {
			return FS_ERR_DISK_FULL;
		}

	}
	else {
		file_first_cluster = *file_cluster;
	}

	if ((target_item == (diritem_t*)0) && !is_cluster_valid(free_item_cluster)) {
		u32_t parent_diritem_cluster;
		u32_t cluster_count;
		xfat_err_t err = allocate_free_cluster(xfat, found_cluster, 1, &parent_diritem_cluster, &cluster_count, 1, 0);
		if (err < 0) {
			return err;
		}

		if (cluster_count < 1) {
			return FS_ERR_DISK_FULL;
		}

		// 读取新建簇中的第一个扇区，获取target_item
		file_diritem_sector = cluster_first_sector(xfat, parent_diritem_cluster);
		err = xfat_bpool_read_sector(to_obj(xfat), &buf, file_diritem_sector);
		if (err < 0) {
			return err;
		}

		target_item = (diritem_t*)buf->buf;
	}
	else {
		u32_t diritem_offset;
		if (is_cluster_valid(free_item_cluster)) {
			file_diritem_sector = cluster_first_sector(xfat, free_item_cluster) + to_sector(disk, free_item_offset);
			diritem_offset = free_item_offset;
		}
		else {
			file_diritem_sector = cluster_first_sector(xfat, found_cluster) + to_sector(disk, found_offset);
			diritem_offset = found_offset;
		}

		xfat_err_t err = xfat_bpool_read_sector(to_obj(xfat), &buf, file_diritem_sector);
		if (err < 0) {
			return err;
		}

		target_item = (diritem_t*)(buf->buf + to_sector_offset(disk, diritem_offset));
	}

	xfat_err_t err = diritem_init_default(target_item, disk, is_dir, child_name, file_first_cluster);
	if (err < 0) {
		return err;
	}

	err = xfat_bpool_write_sector(to_obj(xfat), buf, 0);
	if (err < 0) {
		return err;
	}

	*file_cluster = file_first_cluster;
	return FS_ERR_OK;
}

static xfat_err_t create_empty_dir(xfat_t* xfat, u8_t failed_on_exist, u32_t parent_cluster,
	const char* name, u32_t* new_cluster) {
	xfat_err_t err = create_sub_file(xfat, 1, parent_cluster, name, new_cluster);
	if (err == FS_ERR_EXISTED && !failed_on_exist) {
		return FS_ERR_OK;
	}
	else if (err < 0) {
		return err;
	}

	u32_t dot_cluster;
	u32_t dot_dot_cluster;
	err = create_sub_file(xfat, 1, *new_cluster, ".", &dot_cluster);
	if (err < 0) {
		return err;
	}

	dot_dot_cluster = parent_cluster;
	err = create_sub_file(xfat, 1, *new_cluster, "..", &dot_dot_cluster);
	if (err < 0) {
		return err;
	}

	return FS_ERR_OK;
}

xfat_err_t xfile_mkdir(const char* path) {
	xfat_t* xfat = xfat_find_by_name(path);
	if (xfat == (xfat_t*)0) {
		return FS_ERR_NOT_MOUNT;
	}

	path = get_child_path(path);

	u32_t parent_cluster = xfat->root_cluster;
	while (!is_path_end(path)) {
		u32_t new_dir_cluster = FILE_DEFAULT_CLUSTER;
		const char* next_path = get_child_path(path);
		u8_t failed_on_exist = is_path_end(next_path);
		xfat_err_t err = create_empty_dir(xfat, failed_on_exist, parent_cluster, path, &new_dir_cluster);
		if (err < 0) {
			return err;
		}
		path = get_child_path(path);
		parent_cluster = new_dir_cluster;
	}

	return FS_ERR_OK;
}

xfat_err_t xfile_mkfile(const char* path) {
	xfat_t* xfat = xfat_find_by_name(path);
	if (xfat == (xfat_t*)0) {
		return FS_ERR_NOT_MOUNT;
	}

	path = get_child_path(path);

	u32_t parent_cluster = xfat->root_cluster;

	while (!is_path_end(path)) {
		xfat_err_t err;
		u32_t file_cluster = FILE_DEFAULT_CLUSTER;
		const char* next_path = get_child_path(path);
		if (is_path_end(next_path)) {
			err = create_sub_file(xfat, 0, parent_cluster, path, &file_cluster);
			return err;
		}
		else {
			err = create_empty_dir(xfat, 0, parent_cluster, path, &file_cluster);
			if (err < 0) {
				return err;
			}

			parent_cluster = file_cluster;
		}

		path = next_path;
	}

	return FS_ERR_OK;
}

xfat_err_t xfile_rmfile(const char* path) {
	diritem_t* diritem = (diritem_t*)0;
	u32_t curr_cluster, curr_offset;
	u32_t next_cluster, next_offset;
	u32_t found_cluster, found_offset;
	const char* curr_path;
	xfat_buf_t* buf = (xfat_buf_t*)0;

	xfat_t* xfat = xfat_find_by_name(path);
	if (xfat == (xfat_t*)0) {
		return FS_ERR_NOT_MOUNT;
	}

	path = get_child_path(path);

	curr_cluster = xfat->root_cluster;
	curr_offset = 0;
	for (curr_path = path; curr_path != '\0'; curr_path = get_child_path(curr_path)) {
		do {
			xfat_err_t err = get_next_diritem(xfat, DIRITEM_GET_USED, curr_cluster, curr_offset,
				&found_cluster, &found_offset, &next_cluster, &next_offset, &buf, &diritem);
			if (err < 0) {
				return err;
			}

			if (diritem == (diritem_t*)0) {    // 已经搜索到目录结束
				return FS_ERR_NONE;
			}

			if (is_filename_match((const char*)diritem->DIR_Name, curr_path)) {
				// 找到，比较下一级子目录
				if (get_child_path(curr_path)) {
					curr_cluster = get_diritem_cluster(diritem);
					curr_offset = 0;
				}
				break;
			}

			curr_cluster = next_cluster;
			curr_offset = next_offset;
		} while (1);
	}

	if (diritem && !curr_path) {
		if (diritem->DIR_Attr & DIRITEM_ATTR_DIRECTORY) {
			return FS_ERR_PARAM;
		}

		u32_t dir_sector = to_phy_sector(xfat, found_cluster, found_offset);
		diritem->DIR_Name[0] = DIRITEM_NAME_FREE;

		xfat_err_t err = xfat_bpool_write_sector(to_obj(xfat), buf, 0);
		if (err < 0) {
			return err;
		}

		err = destory_cluster_chain(xfat, get_diritem_cluster(diritem));
		if (err < 0) {
			return err;
		}
	}

	return FS_ERR_OK;
}

static xfat_err_t dir_has_child(xfat_t* xfat, u32_t dir_cluster, int* has_child) {
	diritem_t* diritem = (diritem_t*)0;
	u32_t curr_cluster, curr_offset;
	u32_t next_cluster, next_offset;
	u32_t found_cluster, found_offset;
	xfat_buf_t* buf = (xfat_buf_t*)0;
	curr_cluster = dir_cluster;
	curr_offset = 0;
	*has_child = 0;
	do {
		xfat_err_t err = get_next_diritem(xfat, DIRITEM_GET_USED, curr_cluster, curr_offset,
			&found_cluster, &found_offset, &next_cluster, &next_offset, &buf, &diritem);
		if (err < 0) {
			return err;
		}

		if (diritem == (diritem_t*)0) {    // 已经搜索到目录结束
			return FS_ERR_OK;
		}

		if (is_locate_type_match(diritem, XFILE_LOCATE_NORMAL)) {
			*has_child = 1;
			return FS_ERR_OK;
		}

		curr_cluster = next_cluster;
		curr_offset = next_offset;
	} while (1);

	return FS_ERR_OK;
}

xfat_err_t xfile_rmdir(const char* path) {
	diritem_t* diritem = (diritem_t*)0;
	u32_t curr_cluster, curr_offset;
	u32_t next_cluster, next_offset;
	u32_t found_cluster, found_offset;
	const char* curr_path;
	xfat_buf_t* buf = (xfat_buf_t*)0;

	xfat_t* xfat = xfat_find_by_name(path);
	if (xfat == (xfat_t*)0) {
		return FS_ERR_NOT_MOUNT;
	}

	path = get_child_path(path);

	curr_cluster = xfat->root_cluster;
	curr_offset = 0;
	for (curr_path = path; curr_path != '\0'; curr_path = get_child_path(curr_path)) {
		do {
			xfat_err_t err = get_next_diritem(xfat, DIRITEM_GET_USED, curr_cluster, curr_offset,
				&found_cluster, &found_offset, &next_cluster, &next_offset, &buf, &diritem);
			if (err < 0) {
				return err;
			}

			if (diritem == (diritem_t*)0) {    // 已经搜索到目录结束
				return FS_ERR_NONE;
			}

			if (is_filename_match((const char*)diritem->DIR_Name, curr_path)) {
				// 找到，比较下一级子目录
				if (get_child_path(curr_path)) {
					curr_cluster = get_diritem_cluster(diritem);
					curr_offset = 0;
				}
				break;
			}

			curr_cluster = next_cluster;
			curr_offset = next_offset;
		} while (1);
	}

	if (diritem && !curr_path) {
		if (get_file_type(diritem) != FAT_DIR) {
			return FS_ERR_PARAM;
		}

		int has_child;
		xfat_err_t err = dir_has_child(xfat, get_diritem_cluster(diritem), &has_child);
		if (err < 0) {
			return err;
		}

		if (has_child) {
			return FS_ERR_NOT_EMPTY;
		}

		u32_t dir_sector = to_phy_sector(xfat, found_cluster, found_offset);
		err = xfat_bpool_read_sector(to_obj(xfat), &buf, dir_sector);
		if (err < 0) {
			return err;
		}
		diritem = (diritem_t*)(buf->buf + to_sector_offset(xfat_get_disk(xfat), found_offset));
		diritem->DIR_Name[0] = DIRITEM_NAME_FREE;

		err = xfat_bpool_write_sector(to_obj(xfat), buf, 0);
		if (err < 0) {
			return err;
		}

		err = destory_cluster_chain(xfat, get_diritem_cluster(diritem));
		if (err < 0) {
			return err;
		}
	}

	return FS_ERR_OK;
}

static xfat_err_t rmdir_all_children(xfat_t* xfat, u32_t parent_cluster) {
	diritem_t* diritem = (diritem_t*)0;
	u32_t curr_cluster = parent_cluster;
	uint32_t curr_offset = 0;
	u32_t next_cluster;
	u32_t next_offset;
	u32_t found_cluster;
	u32_t found_offset;
	xfat_buf_t* buf = (xfat_buf_t*)0;

	do {
		xfat_err_t err = get_next_diritem(xfat, DIRITEM_GET_USED, curr_cluster, curr_offset,
			&found_cluster, &found_offset, &next_cluster, &next_offset, &buf, &diritem);
		if (err < 0) {
			return err;
		}

		if (diritem == (diritem_t*)0) {    // 已经搜索到目录结束
			return FS_ERR_OK;
		}

		if (diritem->DIR_Name[0] == DIRITEM_NAME_END) {
			return FS_ERR_OK;
		}

		if (is_locate_type_match(diritem, XFILE_LOCATE_NORMAL)) {
			u32_t diritem_cluster = get_diritem_cluster(diritem);
			u32_t dir_sector = to_phy_sector(xfat, found_cluster, found_offset);

			diritem->DIR_Name[0] = DIRITEM_NAME_FREE;
			err = xfat_bpool_write_sector(to_obj(xfat), buf, 0);
			if (err < 0) {
				return err;
			}

			if (get_file_type(diritem) == FAT_DIR) {
				err = rmdir_all_children(xfat, diritem_cluster);
				if (err < 0) {
					return err;
				}
			}

			err = destory_cluster_chain(xfat, diritem_cluster);
			if (err < 0) {
				return err;
			}
		}

		curr_cluster = next_cluster;
		curr_offset = next_offset;
	} while (1);

	return FS_ERR_OK;
}

xfat_err_t xfile_rmdir_tree(const char* path) {
	diritem_t* diritem = (diritem_t*)0;
	u32_t curr_cluster, curr_offset;
	u32_t next_cluster, next_offset;
	u32_t found_cluster, found_offset;
	const char* curr_path;
	xfat_buf_t* buf = (xfat_buf_t*)0;

	xfat_t* xfat = xfat_find_by_name(path);
	if (xfat == (xfat_t*)0) {
		return FS_ERR_NOT_MOUNT;
	}

	path = get_child_path(path);

	curr_cluster = xfat->root_cluster;
	curr_offset = 0;
	for (curr_path = path; curr_path != '\0'; curr_path = get_child_path(curr_path)) {
		do {
			xfat_err_t err = get_next_diritem(xfat, DIRITEM_GET_USED, curr_cluster, curr_offset,
				&found_cluster, &found_offset, &next_cluster, &next_offset, &buf, &diritem);
			if (err < 0) {
				return err;
			}

			if (diritem == (diritem_t*)0) {    // 已经搜索到目录结束
				return FS_ERR_NONE;
			}

			if (is_filename_match((const char*)diritem->DIR_Name, curr_path)) {
				// 找到，比较下一级子目录
				if (get_child_path(curr_path)) {
					curr_cluster = get_diritem_cluster(diritem);
					curr_offset = 0;
				}
				break;
			}

			curr_cluster = next_cluster;
			curr_offset = next_offset;
		} while (1);
	}

	if (diritem && !curr_path) {
		if (get_file_type(diritem) != FAT_DIR) {
			return FS_ERR_PARAM;
		}

		u32_t diritem_cluster = get_diritem_cluster(diritem);

		u32_t dir_sector = to_phy_sector(xfat, found_cluster, found_offset);
		diritem->DIR_Name[0] = DIRITEM_NAME_FREE;
		xfat_err_t err = xfat_bpool_write_sector(to_obj(xfat), buf, 0);
		if (err < 0) {
			return err;
		}

		err = rmdir_all_children(xfat, diritem_cluster);
		if (err < 0) {
			return err;
		}

		err = destory_cluster_chain(xfat, diritem_cluster);
		if (err < 0) {
			return err;
		}
		return FS_ERR_OK;
	}

	return FS_ERR_NONE;
}

xfile_size_t xfile_read(void* buffer, xfile_size_t elem_size, xfile_size_t count, xfile_t* file) {
	xfile_size_t bytes_to_read = count * elem_size;
	u8_t* read_buffer = (u8_t*)buffer;

	if (file->type != FAT_FILE) {
		file->err = FS_ERR_FSTYPE;
		return 0;
	}

	if (file->pos >= file->size) {
		file->err = FS_ERR_EOF;
		return 0;
	}

	if (file->pos + bytes_to_read > file->size) {
		bytes_to_read = file->size - file->pos;
	}

	xdisk_t* disk = file_get_disk(file);
	xfile_size_t r_count_readed = 0;

	while ((bytes_to_read > 0) && is_cluster_valid(file->curr_cluster)) {
		xfat_err_t err;
		xfile_size_t curr_read_bytes = 0;
		u32_t sector_count = 0;
		// 当前处于curr_cluster中的扇区编号
		u32_t cluster_sector = to_sector(disk, to_cluster_offset(file->xfat, file->pos));
		// 点前处于一个扇区中的哪一个byte
		u32_t sector_offset = to_sector_offset(disk, file->pos);

		// 当前处于fat文件系统中的哪一个扇区
		u32_t start_sector = cluster_first_sector(file->xfat, file->curr_cluster) + cluster_sector;
		// 1) 如果读取的左边界不是扇区对齐的，先读取玩第一部分保证其左边界扇区对齐
		// 2) 如果左边界是扇区对齐的，但是读取的内容大小小于一个扇区则也可以走这个逻辑
		if ((sector_offset != 0) || (!sector_offset && (bytes_to_read < disk->sector_size))) {

			sector_count = 1;
			curr_read_bytes = bytes_to_read;

			if (sector_offset != 0) {
				// 先读取内容的一部分保证左边界能够扇区对齐
				if (sector_offset + bytes_to_read > disk->sector_size) {
					curr_read_bytes = disk->sector_size - sector_offset;
				}
			}

			xfat_buf_t* buf = (xfat_buf_t*)0;
			err = xfat_bpool_read_sector(to_obj(file), &buf, start_sector);
			if (err < 0) {
				file->err = err;
				return 0;
			}

			memcpy(read_buffer, buf->buf + sector_offset, curr_read_bytes);
			read_buffer += curr_read_bytes;
			bytes_to_read -= curr_read_bytes;
		}
		else {
			sector_count = to_sector(disk, bytes_to_read);
			if ((cluster_sector + sector_count) > file->xfat->sec_per_cluster) {
				sector_count = file->xfat->sec_per_cluster - cluster_sector;
			}

			err = xfat_bpool_flush_sectors(to_obj(file), start_sector, count);
			if (err < 0) {
				return err;
			}
			err = xdisk_read_sector(disk, read_buffer, start_sector, sector_count);
			if (err != FS_ERR_OK) {
				file->err = err;
				return r_count_readed / elem_size;
			}

			curr_read_bytes = sector_count * disk->sector_size;
			read_buffer += curr_read_bytes;
			bytes_to_read -= curr_read_bytes;
		}

		r_count_readed += curr_read_bytes;

		err = move_file_pos(file, curr_read_bytes);
		if (err) return 0;
	}

	file->err = file->pos == file->size;
	return r_count_readed / elem_size;
}

static xfat_err_t update_file_size(xfile_t* file, xfile_size_t size) {
	xdisk_t* disk = file_get_disk(file);
	u32_t sector = to_phy_sector(file->xfat, file->dir_cluster, file->dir_cluster_offset);
	u32_t offset = to_sector_offset(disk, file->dir_cluster_offset);
	xfat_buf_t* buf = (xfat_buf_t*)0;
	xfat_err_t err = xfat_bpool_read_sector(to_obj(file), &buf, sector);
	if (err < 0) {
		file->err = err;
		return err;
	}

	diritem_t* diritem = (diritem_t*)(buf->buf + offset);
	diritem->DIR_FileSize = size;
	set_diritem_cluster(diritem, file->start_cluster);
	err = xfat_bpool_write_sector(to_obj(file), buf, 0);
	if (err < 0) {
		file->err = err;
		return err;
	}

	file->size = size;
	return FS_ERR_OK;
}

static int is_fpos_cluster_end(xfile_t* file) {
	xfile_size_t cluster_offset = to_cluster_offset(file->xfat, file->pos);
	return (cluster_offset == 0) && (file->pos == file->size);
}

static xfat_err_t expand_file(xfile_t* file, xfile_size_t size) {
	xfat_t* xfat = file->xfat;
	u32_t curr_cluster_cnt = to_cluster_count(xfat, file->size);
	u32_t expect_cluster_cnt = to_cluster_count(xfat, size);
	xfat_err_t err;

	if (curr_cluster_cnt < expect_cluster_cnt) {
		u32_t cluster_cnt = expect_cluster_cnt - curr_cluster_cnt;
		u32_t start_free_cluster = 0;
		u32_t curr_cluster = file->curr_cluster;

		if (file->size > 0) {
			u32_t next_cluster = curr_cluster;
			do {
				curr_cluster = next_cluster;
				err = get_next_cluster(xfat, curr_cluster, &next_cluster);
				if (err) {
					file->err = err;
					return err;
				}
			} while (is_cluster_valid(next_cluster));
		}

		err = allocate_free_cluster(xfat, curr_cluster, cluster_cnt, &start_free_cluster, 0, 0, 0);
		if (err) {
			file->err = err;
			return err;
		}

		// 空文件,之前还没有内容簇
		if (!is_cluster_valid(file->start_cluster)) {
			file->start_cluster = start_free_cluster;
			file->curr_cluster = start_free_cluster;
		}
		else if (!is_cluster_valid(file->curr_cluster) || is_fpos_cluster_end(file)) {
			// 链接到已有的簇上
			file->curr_cluster = start_free_cluster;
		}
	}

	return update_file_size(file, size);
}

xfile_size_t xfile_write(void* buffer, xfile_size_t elem_size, xfile_size_t count, xfile_t* file) {
	xfile_size_t bytes_to_write = count * elem_size;
	u8_t* write_buffer = (u8_t*)buffer;

	if (file->type != FAT_FILE) {
		file->err = FS_ERR_FSTYPE;
		return 0;
	}

	if (file->attr & XFILE_ATTR_READONLY) {
		file->err = FS_ERR_READONLY;
		return 0;
	}

	if (bytes_to_write == 0) {
		file->err = FS_ERR_OK;
		return 0;
	}

	xdisk_t* disk = file_get_disk(file);
	xfile_size_t r_count_writed = 0;

	if (file->size < file->pos + bytes_to_write) {
		xfat_err_t err = expand_file(file, file->pos + bytes_to_write);
		if (err < 0) {
			file->err = err;
			return 0;
		}
	}

	while ((bytes_to_write > 0) && is_cluster_valid(file->curr_cluster)) {
		xfat_err_t err;
		xfile_size_t curr_write_bytes = 0;
		u32_t sector_count = 0;
		// 当前处于curr_cluster中的扇区编号
		u32_t cluster_sector = to_sector(disk, to_cluster_offset(file->xfat, file->pos));
		// 点前处于一个扇区中的哪一个byte
		u32_t sector_offset = to_sector_offset(disk, file->pos);

		// 当前处于fat文件系统中的哪一个扇区
		u32_t start_sector = cluster_first_sector(file->xfat, file->curr_cluster) + cluster_sector;
		// 1) 如果读取的左边界不是扇区对齐的，先读取玩第一部分保证其左边界扇区对齐
		// 2) 如果左边界是扇区对齐的，但是读取的内容大小小于一个扇区则也可以走这个逻辑
		if ((sector_offset != 0) || (!sector_offset && (bytes_to_write < disk->sector_size))) {
			sector_count = 1;
			curr_write_bytes = bytes_to_write;

			if (sector_offset != 0) {
				// 先读取内容的一部分保证左边界能够扇区对齐
				if (sector_offset + bytes_to_write > disk->sector_size) {
					curr_write_bytes = disk->sector_size - sector_offset;
				}
			}

			xfat_buf_t* buf = (xfat_buf_t*)0;
			err = xfat_bpool_read_sector(to_obj(file), &buf, start_sector);
			if (err < 0) {
				file->err = err;
				return 0;
			}

			memcpy(buf->buf + sector_offset, write_buffer, curr_write_bytes);
			err = xfat_bpool_write_sector(to_obj(file), buf, 0);
			if (err < 0) {
				file->err = err;
				return 0;
			}

			write_buffer += curr_write_bytes;
			bytes_to_write -= curr_write_bytes;
		}
		else {
			sector_count = to_sector(disk, bytes_to_write);
			if ((cluster_sector + sector_count) > file->xfat->sec_per_cluster) {
				sector_count = file->xfat->sec_per_cluster - cluster_sector;
			}

			err = xfat_bpool_invalid_sectors(to_obj(file), start_sector, sector_count);
			if (err < 0) {
				return err;
			}
			err = xdisk_write_sector(disk, write_buffer, start_sector, sector_count);
			if (err != FS_ERR_OK) {
				file->err = err;
				return r_count_writed / elem_size;
			}

			curr_write_bytes = sector_count * disk->sector_size;
			write_buffer += curr_write_bytes;
			bytes_to_write -= curr_write_bytes;
		}

		r_count_writed += curr_write_bytes;

		err = move_file_pos(file, curr_write_bytes);
		if (err) return 0;
	}

	file->err = file->pos == file->size;
	return r_count_writed / elem_size;
}

xfat_err_t xfile_eof(xfile_t* file) {
	return file->pos >= file->size ? FS_ERR_EOF : FS_ERR_OK;
}

xfile_size_t xfile_tell(xfile_t* file) {
	return file->pos;
}

xfat_err_t xfile_seek(xfile_t* file, xfile_ssize_t offset, xfile_origin_t origin) {
	xfile_ssize_t final_pos;

	switch (origin) {
	case XFAT_SEEK_SET:
		final_pos = offset;
		break;
	case XFAT_SEEK_CUR:
		final_pos = file->pos + offset;
		break;
	case XFAT_SEEK_END:
		final_pos = file->size + offset;
		break;
	default:
		return FS_ERR_PARAM;
	}

	if (final_pos < 0 || final_pos > file->size) {
		return FS_ERR_PARAM;
	}

	offset = final_pos - file->pos;
	u32_t curr_cluster;
	u32_t curr_pos;
	u32_t offset_to_move;
	if (offset > 0) {
		curr_cluster = file->curr_cluster;
		curr_pos = file->pos;
		offset_to_move = (xfile_size_t)offset;
	}
	else {
		curr_cluster = file->start_cluster;
		curr_pos = 0;
		offset_to_move = (xfile_size_t)final_pos;
	}

	while (offset_to_move > 0) {
		u32_t cluster_offset = to_cluster_offset(file->xfat, curr_pos);
		xfile_size_t curr_move = offset_to_move;
		if (cluster_offset + curr_move < file->xfat->cluster_byte_size) {
			curr_pos += curr_move;
			break;
		}
		else {
			curr_move = file->xfat->cluster_byte_size - cluster_offset;
			curr_pos += curr_move;
			offset_to_move -= curr_move;
			xfat_err_t err = get_next_cluster(file->xfat, curr_cluster, &curr_cluster);
			if (err < 0) {
				file->err = err;
				return err;
			}
		}
	}

	file->pos = curr_pos;
	file->curr_cluster = curr_cluster;
	return FS_ERR_OK;
}

xfat_err_t xfile_size(xfile_t* file, xfile_size_t* size) {
	*size = file->size;

	return FS_ERR_OK;
}

static xfat_err_t truncate_file(xfile_t* file, xfile_size_t size) {
	xfat_err_t err;
	u32_t curr_cluster = file->start_cluster;
	u32_t pos = 0;

	while (pos < size) {
		u32_t next_cluster;
		err = get_next_cluster(file->xfat, curr_cluster, &next_cluster);
		if (err < 0) {
			return err;
		}
		pos += file->xfat->cluster_byte_size;
		curr_cluster = next_cluster;
	}

	err = destory_cluster_chain(file->xfat, curr_cluster);
	if (err < 0) {
		return err;
	}
	if (size == 0) {
		file->start_cluster = 0;
	}

	return update_file_size(file, size);
}

xfat_err_t xfile_resize(xfile_t* file, xfile_size_t size) {
	if (file->type != FAT_FILE) {
		return FS_ERR_PARAM;
	}

	if (size == file->size) {
		return FS_ERR_OK;
	}
	else if (size > file->size) {
		xfat_err_t err = expand_file(file, size);
		if (err < 0) {
			return err;
		}
		return FS_ERR_OK;
	}
	else {
		xfat_err_t err = truncate_file(file, size);
		if (err < 0) {
			return err;
		}

		if (file->pos >= size) {
			file->pos = 0;
			file->curr_cluster = file->start_cluster;
		}
		return FS_ERR_OK;
	}
}

xfat_err_t xfile_rename(const char* path, const char* new_name) {
	diritem_t* diritem = (diritem_t*)0;
	u32_t curr_cluster;
	u32_t curr_offset;
	u32_t next_cluster;
	u32_t next_offset;
	u32_t found_cluster;
	u32_t found_offset;

	xfat_t* xfat = xfat_find_by_name(path);
	if (xfat == (xfat_t*)0) {
		return FS_ERR_NOT_MOUNT;
	}

	path = get_child_path(path);

	curr_cluster = xfat->root_cluster;
	curr_offset = 0;
	const char* curr_path = path;
	xfat_buf_t* buf = (xfat_buf_t*)0;
	for (; curr_path && *curr_path != '\0'; curr_path = get_child_path(curr_path)) {
		do {
			xfat_err_t err = get_next_diritem(xfat, DIRITEM_GET_USED, curr_cluster, curr_offset,
				&found_cluster, &found_offset, &next_cluster, &next_offset, &buf, &diritem);
			if (err < 0) {
				return err;
			}

			if (diritem == (diritem_t*)0) {
				return FS_ERR_NONE;
			}

			if (is_filename_match((char*)diritem->DIR_Name, curr_path)) {
				if (get_file_type(diritem) == FAT_DIR) {
					curr_cluster = get_diritem_cluster(diritem);
					curr_offset = 0;
				}

				break;
			}

			curr_cluster = next_cluster;
			curr_offset = next_offset;
		} while (1);
	}

	if (diritem && !curr_path) {
		u32_t dir_sector = to_phy_sector(xfat, found_cluster, found_offset);
		to_sfn((char*)diritem->DIR_Name, new_name);
		diritem->DIR_NTRes &= ~DIRITEM_NTRES_CASE_MASK;
		diritem->DIR_NTRes |= get_sfn_case_cfg(new_name);
		return xfat_bpool_write_sector(to_obj(xfat), buf, 0);
	}

	return FS_ERR_OK;
}

/**
 * 设置diritem中相应的时间，用作文件时间修改的回调函数
 * @param xfat xfat结构
 * @param dir_item 目录结构项
 * @param arg1 修改的时间类型
 * @param arg2 新的时间
 * @return
 */
static xfat_err_t set_file_time(const char* path, stime_type_t time_type, xfile_time_t* time) {
	diritem_t* diritem = (diritem_t*)0;
	u32_t curr_cluster, curr_offset;
	u32_t next_cluster, next_offset;
	u32_t found_cluster, found_offset;
	const char* curr_path;

	xfat_t* xfat = xfat_find_by_name(path);
	if (xfat == (xfat_t*)0) {
		return FS_ERR_NOT_MOUNT;
	}

	path = get_child_path(path);

	curr_cluster = xfat->root_cluster;
	curr_offset = 0;
	xfat_buf_t* buf = (xfat_buf_t*)0;
	for (curr_path = path; curr_path != '\0'; curr_path = get_child_path(curr_path)) {
		do {
			xfat_err_t err = get_next_diritem(xfat, DIRITEM_GET_USED, curr_cluster, curr_offset,
				&found_cluster, &found_offset, &next_cluster, &next_offset, &buf, &diritem);
			if (err < 0) {
				return err;
			}

			if (diritem == (diritem_t*)0) {    // 已经搜索到目录结束
				return FS_ERR_NONE;
			}

			if (is_filename_match((const char*)diritem->DIR_Name, curr_path)) {
				// 找到，比较下一级子目录
				if (get_child_path(curr_path)) {
					curr_cluster = get_diritem_cluster(diritem);
					curr_offset = 0;
				}
				break;
			}

			curr_cluster = next_cluster;
			curr_offset = next_offset;
		} while (1);
	}

	if (diritem && !curr_path) {
		// 这种方式只能用于SFN文件项重命名
		u32_t dir_sector = to_phy_sector(xfat, curr_cluster, curr_offset);

		// 根据文件名的实际情况，重新配置大小写
		switch (time_type) {
		case XFAT_TIME_CTIME:
			diritem->DIR_CrtDate.year_from_1980 = (u16_t)(time->year - 1980);
			diritem->DIR_CrtDate.month = time->month;
			diritem->DIR_CrtDate.day = time->day;
			diritem->DIR_CrtTime.hour = time->hour;
			diritem->DIR_CrtTime.minute = time->minute;
			diritem->DIR_CrtTime.second_2 = (u16_t)(time->second / 2);
			diritem->DIR_CrtTimeTeenth = (u8_t)(time->second % 2 * 1000 / 100);
			break;
		case XFAT_TIME_ATIME:
			diritem->DIR_LastAccDate.year_from_1980 = (u16_t)(time->year - 1980);
			diritem->DIR_LastAccDate.month = time->month;
			diritem->DIR_LastAccDate.day = time->day;
			break;
		case XFAT_TIME_MTIME:
			diritem->DIR_WrtDate.year_from_1980 = (u16_t)(time->year - 1980);
			diritem->DIR_WrtDate.month = time->month;
			diritem->DIR_WrtDate.day = time->day;
			diritem->DIR_WrtTime.hour = time->hour;
			diritem->DIR_WrtTime.minute = time->minute;
			diritem->DIR_WrtTime.second_2 = (u16_t)(time->second / 2);
			break;
		}

		return xfat_bpool_write_sector(to_obj(xfat), buf, 0);
	}

	return FS_ERR_OK;
}

xfat_err_t xfile_set_atime(const char* path, xfile_time_t* time) {
	return set_file_time(path, XFAT_TIME_ATIME, time);
}

xfat_err_t xfile_set_mtime(const char* path, xfile_time_t* time) {
	return set_file_time(path, XFAT_TIME_MTIME, time);
}

xfat_err_t xfile_set_ctime(const char* path, xfile_time_t* time) {
	return set_file_time(path, XFAT_TIME_CTIME, time);
}