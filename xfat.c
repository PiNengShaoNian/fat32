#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "xfat.h"

extern u8_t temp_buffer[512];

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
	xdisk_t* disk = xfat_get_disk(xfat);
	u32_t curr_cluster = cluster;
	cluster32_t* cluster32_buf;
	u32_t next_cluster;
	u32_t write_back = 0;

	while (is_cluster_valid(curr_cluster)) {
		xfat_err_t err = get_next_cluster(xfat, curr_cluster, &next_cluster);
		if (err < 0) {
			return err;
		}

		cluster32_buf = (cluster32_t*)xfat->fat_buffer;
		cluster32_buf[curr_cluster].s.next = CLUSTER_FREE;
		curr_cluster = next_cluster;
		write_back = 1;
	}

	if (write_back) {
		for (int i = 0; i < xfat->fat_tbl_nr; i++) {
			u32_t start_sector = xfat->fat_start_sector + xfat->fat_tbl_sectors * i;
			xfat_err_t err = xdisk_write_sector(disk, (u8_t*)xfat->fat_buffer, start_sector, xfat->fat_tbl_sectors);
			if (err < 0) {
				return err;
			}
		}
	}
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
	u32_t* next_cluster, u32_t* next_offset, u8_t* temp_buffer, diritem_t** diritem) {
	diritem_t* r_diritem;
	while (is_cluster_valid(start_cluster)) {
		xfat_err_t err = move_cluster_pos(xfat, start_cluster, start_offset, sizeof(diritem_t), next_cluster, next_offset);
		if (err < 0) {
			return err;
		}

		u32_t sector_offset = to_sector_offset(xfat_get_disk(xfat), start_offset);
		u32_t curr_sector = to_phy_sector(xfat, start_cluster, start_offset);
		err = xdisk_read_sector(xfat_get_disk(xfat), temp_buffer, curr_sector, 1);
		if (err < 0) {
			return err;
		}

		r_diritem = (diritem_t*)(temp_buffer + sector_offset);
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
	memset(temp_buffer, erase_state, disk->sector_size);
	for (u32_t i = 0; i < xfat->sec_per_cluster; i++) {
		xfat_err_t err = xdisk_write_sector(disk, temp_buffer, sector + i, 1);
		if (err < 0) {
			return err;
		}
	}
	return FS_ERR_OK;
}

static xfat_err_t allocate_free_cluster(xfat_t* xfat, u32_t curr_cluster, u32_t count,
	u32_t* r_start_cluster, u32_t* r_allocated_count, u8_t en_erase, u8_t erase_data) {
	xdisk_t* disk = xfat_get_disk(xfat);
	u32_t cluster_count = xfat->fat_tbl_sectors * disk->sector_size / sizeof(cluster32_t);
	u32_t allocated_count = 0;
	u32_t pre_cluster = curr_cluster;
	cluster32_t* cluster32_buf = (cluster32_t*)xfat->fat_buffer;
	u32_t start_cluster = 0;
	for (u32_t i = 2; (i < cluster_count) && (allocated_count < count); i++) {
		if (cluster32_buf[i].s.next == 0) {
			if (is_cluster_valid(pre_cluster)) {
				cluster32_buf[pre_cluster].s.next = i;
			}

			pre_cluster = i;

			if (++allocated_count == 1) {
				start_cluster = i;
			}

			if (allocated_count >= count) {
				break;
			}
		}
	}

	if (allocated_count) {
		cluster32_buf[pre_cluster].s.next = CLUSTER_INVALID;

		if (en_erase) {
			u32_t cluster = start_cluster;
			for (u32_t i = 0; i < allocated_count; i++) {
				xfat_err_t err = erase_cluster(xfat, cluster, erase_data);
				if (err < 0) {
					return err;
				}
				cluster = cluster32_buf[cluster].s.next;
			}
		}

		for (u32_t i = 0; i < xfat->fat_tbl_nr; i++) {
			u32_t start_sector = xfat->fat_start_sector + xfat->fat_tbl_sectors * i;
			xfat_err_t err = xdisk_write_sector(disk, (u8_t*)xfat->fat_buffer, start_sector, xfat->fat_tbl_sectors);
			if (err < 0) {
				return err;
			}
		}
	}

	if (r_allocated_count) {
		*r_allocated_count = allocated_count;
	}
	if (r_start_cluster) {
		*r_start_cluster = start_cluster;
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

xfat_err_t xfile_open(xfat_t* xfat, xfile_t* file, const char* path) {
	path = skip_first_path_sep(path);
	if (memcmp(path, "..", 2) == 0) {
		// /..
		return FS_ERR_PARAM;
	}
	else if (memcmp(path, ".", 1) == 0) {
		path++;
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

	do {
		diritem_t* diritem = (diritem_t*)0;

		xfat_err_t err = get_next_diritem(xfat, DIRITEM_GET_ALL, curr_cluster, curr_offset, &found_cluster,
			&found_offset, &next_cluster, &next_offset, temp_buffer, &diritem);
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
		err = xdisk_read_sector(disk, temp_buffer, file_diritem_sector, 1);
		if (err < 0) {
			return err;
		}

		target_item = (diritem_t*)temp_buffer;
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

		xfat_err_t err = xdisk_read_sector(disk, temp_buffer, file_diritem_sector, 1);
		if (err < 0) {
			return err;
		}

		target_item = (diritem_t*)(temp_buffer + to_sector_offset(disk, diritem_offset));
	}

	xfat_err_t err = diritem_init_default(target_item, disk, is_dir, child_name, file_first_cluster);
	if (err < 0) {
		return err;
	}

	err = xdisk_write_sector(disk, temp_buffer, file_diritem_sector, 1);
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

xfat_err_t xfile_mkdir(xfat_t* xfat, const char* path) {
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

xfat_err_t xfile_mkfile(xfat_t* xfat, const char* path) {
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

xfat_err_t xfile_rmfile(xfat_t* xfat, const char* path) {
	diritem_t* diritem = (diritem_t*)0;
	u32_t curr_cluster, curr_offset;
	u32_t next_cluster, next_offset;
	u32_t found_cluster, found_offset;
	const char* curr_path;

	curr_cluster = xfat->root_cluster;
	curr_offset = 0;
	for (curr_path = path; curr_path != '\0'; curr_path = get_child_path(curr_path)) {
		do {
			xfat_err_t err = get_next_diritem(xfat, DIRITEM_GET_USED, curr_cluster, curr_offset,
				&found_cluster, &found_offset, &next_cluster, &next_offset, temp_buffer, &diritem);
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

		xfat_err_t err = xdisk_write_sector(xfat_get_disk(xfat), temp_buffer, dir_sector, 1);
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
	const char* curr_path;

	curr_cluster = dir_cluster;
	curr_offset = 0;
	*has_child = 0;
	do {
		xfat_err_t err = get_next_diritem(xfat, DIRITEM_GET_USED, curr_cluster, curr_offset,
			&found_cluster, &found_offset, &next_cluster, &next_offset, temp_buffer, &diritem);
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

xfat_err_t xfile_rmdir(xfat_t* xfat, const char* path) {
	diritem_t* diritem = (diritem_t*)0;
	u32_t curr_cluster, curr_offset;
	u32_t next_cluster, next_offset;
	u32_t found_cluster, found_offset;
	const char* curr_path;

	curr_cluster = xfat->root_cluster;
	curr_offset = 0;
	for (curr_path = path; curr_path != '\0'; curr_path = get_child_path(curr_path)) {
		do {
			xfat_err_t err = get_next_diritem(xfat, DIRITEM_GET_USED, curr_cluster, curr_offset,
				&found_cluster, &found_offset, &next_cluster, &next_offset, temp_buffer, &diritem);
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
		err = xdisk_read_sector(xfat_get_disk(xfat), temp_buffer, dir_sector, 1);
		if (err < 0) {
			return err;
		}
		diritem = (diritem_t*)(temp_buffer + to_sector_offset(xfat_get_disk(xfat), found_offset));
		diritem->DIR_Name[0] = DIRITEM_NAME_FREE;

		err = xdisk_write_sector(xfat_get_disk(xfat), temp_buffer, dir_sector, 1);
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
	const char* curr_path;

	do {
		xfat_err_t err = get_next_diritem(xfat, DIRITEM_GET_USED, curr_cluster, curr_offset,
			&found_cluster, &found_offset, &next_cluster, &next_offset, temp_buffer, &diritem);
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
			err = xdisk_write_sector(xfat_get_disk(xfat), temp_buffer, dir_sector, 1);
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

xfat_err_t xfile_rmdir_tree(xfat_t* xfat, const char* path) {
	diritem_t* diritem = (diritem_t*)0;
	u32_t curr_cluster, curr_offset;
	u32_t next_cluster, next_offset;
	u32_t found_cluster, found_offset;
	const char* curr_path;

	curr_cluster = xfat->root_cluster;
	curr_offset = 0;
	for (curr_path = path; curr_path != '\0'; curr_path = get_child_path(curr_path)) {
		do {
			xfat_err_t err = get_next_diritem(xfat, DIRITEM_GET_USED, curr_cluster, curr_offset,
				&found_cluster, &found_offset, &next_cluster, &next_offset, temp_buffer, &diritem);
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
		xfat_err_t err = xdisk_write_sector(xfat_get_disk(xfat), temp_buffer, dir_sector, 1);
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

			err = xdisk_read_sector(disk, temp_buffer, start_sector, 1);
			if (err < 0) {
				file->err = err;
				return 0;
			}

			memcpy(read_buffer, temp_buffer + sector_offset, curr_read_bytes);
			read_buffer += curr_read_bytes;
			bytes_to_read -= curr_read_bytes;
		}
		else {
			sector_count = to_sector(disk, bytes_to_read);
			if ((cluster_sector + sector_count) > file->xfat->sec_per_cluster) {
				sector_count = file->xfat->sec_per_cluster - cluster_sector;
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

	xfat_err_t err = xdisk_read_sector(disk, temp_buffer, sector, 1);
	if (err < 0) {
		file->err = err;
		return err;
	}

	diritem_t* diritem = (diritem_t*)(temp_buffer + offset);
	diritem->DIR_FileSize = size;
	set_diritem_cluster(diritem, file->start_cluster);
	err = xdisk_write_sector(disk, temp_buffer, sector, 1);
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

			err = xdisk_read_sector(disk, temp_buffer, start_sector, 1);
			if (err < 0) {
				file->err = err;
				return 0;
			}

			memcpy(temp_buffer + sector_offset, write_buffer, curr_write_bytes);
			err = xdisk_write_sector(disk, temp_buffer, start_sector, 1);
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

	if (final_pos < 0 || final_pos >= file->size) {
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

xfat_err_t xfile_rename(xfat_t* xfat, const char* path, const char* new_name) {
	diritem_t* diritem = (diritem_t*)0;
	u32_t curr_cluster;
	u32_t curr_offset;
	u32_t next_cluster;
	u32_t next_offset;
	u32_t found_cluster;
	u32_t found_offset;

	curr_cluster = xfat->root_cluster;
	curr_offset = 0;
	const char* curr_path = path;
	for (; curr_path && *curr_path != '\0'; curr_path = get_child_path(curr_path)) {
		do {
			xfat_err_t err = get_next_diritem(xfat, DIRITEM_GET_USED, curr_cluster, curr_offset,
				&found_cluster, &found_offset, &next_cluster, &next_offset, temp_buffer, &diritem);
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
		return xdisk_write_sector(xfat_get_disk(xfat), temp_buffer, dir_sector, 1);
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
static xfat_err_t set_file_time(xfat_t* xfat, const char* path, stime_type_t time_type, xfile_time_t* time) {
	diritem_t* diritem = (diritem_t*)0;
	u32_t curr_cluster, curr_offset;
	u32_t next_cluster, next_offset;
	u32_t found_cluster, found_offset;
	const char* curr_path;

	curr_cluster = xfat->root_cluster;
	curr_offset = 0;
	for (curr_path = path; curr_path != '\0'; curr_path = get_child_path(curr_path)) {
		do {
			xfat_err_t err = get_next_diritem(xfat, DIRITEM_GET_USED, curr_cluster, curr_offset,
				&found_cluster, &found_offset, &next_cluster, &next_offset, temp_buffer, &diritem);
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

		return xdisk_write_sector(xfat_get_disk(xfat), temp_buffer, dir_sector, 1);
	}

	return FS_ERR_OK;
}

xfat_err_t xfile_set_atime(xfat_t* xfat, const char* path, xfile_time_t* time) {
	return set_file_time(xfat, path, XFAT_TIME_ATIME, time);
}

xfat_err_t xfile_set_mtime(xfat_t* xfat, const char* path, xfile_time_t* time) {
	return set_file_time(xfat, path, XFAT_TIME_MTIME, time);
}

xfat_err_t xfile_set_ctime(xfat_t* xfat, const char* path, xfile_time_t* time) {
	return set_file_time(xfat, path, XFAT_TIME_CTIME, time);
}