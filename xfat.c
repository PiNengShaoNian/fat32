#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "xfat.h"

extern u8_t temp_buffer[512];

#define xfat_get_disk(xfat) ((xfat)->disk_part->disk)
#define is_path_sep(ch) (((ch) == '/') || ((ch) == '\\'))
#define to_sector(disk, offset) ((offset) / (disk)->sector_size)
#define to_sector_offset(disk, offset) ((offset) % (disk)->sector_size)
#define to_cluster_offset(xfat, pos) ((pos) % (xfat)->cluster_byte_size)

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

// 获取fat文件系统中，第cluster_no个簇的第一个扇区编号
u32_t cluster_first_sector(xfat_t* xfat, u32_t cluster_no) {
	u32_t data_start_sector = xfat->fat_start_sector + xfat->fat_tbl_sectors * xfat->fat_tbl_nr;
	// 需要减去起始的两个保留簇，才是真实的簇号
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

static int is_filename_match(const char* name_in_dir, const char* to_find_name) {
	char temp_name[SFN_LEN];
	to_sfn(temp_name, to_find_name);
	return memcmp(temp_name, name_in_dir, SFN_LEN) == 0;
}

static const char* skip_first_path_sep(const char* path) {
	const char* c = path;
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
		dest->year = date->year_from_1980 + 1980;
		dest->month = date->month;
		dest->day = date->day;
	}
	else {
		dest->year = 0;
		dest->month = 0;
		dest->day = 0;
	}

	if (time) {
		dest->hour = time->hour;
		dest->minute = time->minute;
		dest->second = time->second_2 * 2 + mil_sec / 100;
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

static xfat_err_t locate_file_dir_item(xfat_t* xfat, u32_t* dir_cluster, u32_t* cluster_offset, const char* path, u32_t* r_moved_bytes, diritem_t** r_diritem) {
	u32_t curr_cluster = *dir_cluster;
	xdisk_t* xdisk = xfat_get_disk(xfat);
	u32_t initial_sector = to_sector(xdisk, *cluster_offset);
	u32_t initial_offset = to_sector_offset(xdisk, *cluster_offset);
	u32_t move_bytes = 0;

	do {
		u32_t start_sector = cluster_first_sector(xfat, curr_cluster);
		for (u32_t i = initial_sector; i < xfat->sec_per_cluster; i++) {
			xfat_err_t err = xdisk_read_sector(xdisk, temp_buffer, start_sector + i, 1);
			if (err < 0) {
				return err;
			}

			for (int j = initial_offset / sizeof(diritem_t); j < xdisk->sector_size / sizeof(diritem_t); j++) {
				diritem_t* dir_item = ((diritem_t*)temp_buffer) + j;
				if (dir_item->DIR_Name[0] == DIRITEM_NAME_END) {
					return FS_ERR_EOF;
				}
				else if (dir_item->DIR_Name[0] == DIRITEM_NAME_FREE) {
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
	if ((path != '\0') && (*path != '\0')) {
		// a/b/c/d.txt
		const char* curr_path = path;
		diritem_t* diritem = (diritem_t*)0;
		while (curr_path != (const char*)0) {
			u32_t moved_bytes = 0;
			diritem = (diritem_t*)0;
			xfat_err_t err = locate_file_dir_item(xfat, &parent_cluster, &parent_cluster_offset,
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
			}
		}

		file->size = diritem->DIR_FileSize;
		file->type = get_file_type(diritem);
		file->start_cluster = file_start_cluster;
		file->curr_cluster = file_start_cluster;
	}
	else {
		file->size = 0;
		file->type = FAT_DIR;
		file->start_cluster = dir_cluster;
		file->curr_cluster = dir_cluster;
	}

	file->xfat = xfat;
	file->pos = 0;
	file->err = FS_ERR_OK;
	file->attr = 0;

	return FS_ERR_OK;
}

xfat_err_t xfile_open(xfat_t* xfat, xfile_t* file, const char* path) {
	return open_sub_file(xfat, xfat->root_cluster, file, path);
}

xfat_err_t xfile_close(xfile_t* file) {
	return FS_ERR_OK;
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
	xfat_err_t err = locate_file_dir_item(file->xfat, &file->curr_cluster, &cluster_offset, "", &moved_bytes, &diritem);
	if (err < 0) {
		return err;
	}
	file->pos += moved_bytes;
	if (diritem == (diritem_t*)0) {
		return FS_ERR_OK;
	}
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
	xfat_err_t err = locate_file_dir_item(file->xfat, &file->curr_cluster, &cluster_offset, "", &moved_bytes, &diritem);
	if (err < 0) {
		return err;
	}
	file->pos += moved_bytes;
	if (cluster_offset + sizeof(diritem_t) >= file->xfat->cluster_byte_size) {
		err = get_next_cluster(file->xfat, file->curr_cluster, &file->curr_cluster);
		if (err < 0) {
			return err;
		}
	}
	if (diritem == (diritem_t*)0) {
		return FS_ERR_EOF;
	}
	copy_file_info(info, diritem);
	return err;
}