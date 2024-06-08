#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "xdisk.h"
#include "xfat.h"

extern xdisk_driver_t vdisk_driver;

const char* disk_path_test = "disk_test.img";
const char* disk_path = "disk.img";
static u32_t write_buffer[160 * 1024];
static u32_t read_buffer[160 * 1024];

int disk_io_test(void) {
	int err;

	xdisk_t disk_test;

	disk_test.driver = &vdisk_driver;
	memset(read_buffer, 0, sizeof(read_buffer));


	err = xdisk_open(&disk_test, "vdisk_test", &vdisk_driver, (void*)disk_path_test);
	if (err) {
		printf("open disk failed!\n");
		return -1;
	}

	err = xdisk_write_sector(&disk_test, (u8_t*)write_buffer, 0, 2);
	if (err) {
		printf("write disk failed!\n");
		return -1;
	}

	err = xdisk_read_sector(&disk_test, (u8_t*)read_buffer, 0, 2);
	if (err) {
		printf("write disk failed!\n");
		return -1;
	}
	err = memcmp((u8_t*)read_buffer, (u8_t*)write_buffer, disk_test.sector_size * 2);
	if (err != 0) {
		printf("data not equal!\n");
		return -1;
	}

	err = xdisk_close(&disk_test);
	if (err) {
		printf("disk close failed!\n");
		return -1;
	}

	printf("disk io test ok!\n");
	return 0;
}

xdisk_t disk;
xdisk_part_t disk_part;
xfat_t xfat;
int disk_part_test(void) {
	u32_t count;
	int err;
	printf("partition read test...\n");
	err = xdisk_get_part_count(&disk, &count);
	if (err < 0) {
		printf("partition count detect failed\n");
		return err;
	}

	for (u32_t i = 0; i < count; i++) {
		xdisk_part_t part;
		int err = xdisk_get_part(&disk, &part, i);
		if (err < 0) {
			printf("read partiton failed!\n");
			return -1;
		}

		printf("no: %d, start: %d, count: %d, capacity: %.0f M\n", i, part.start_sector, part.total_sector,
			part.total_sector * disk.sector_size / 1024 / 1024.0);
	}

	printf("partition count: %d\n", count);
	return 0;
}

void show_dir_info(diritem_t* diritem) {
	char filename[12];

	memset(filename, 0, sizeof(filename));
	memcpy(filename, diritem->DIR_Name, sizeof(diritem->DIR_Name));
	if (filename[0] == 0x05) {
		filename[0] = 0xE5;
	}
	printf("\n name: %s, ", filename);

	printf("\n\t");
	u8_t attr = diritem->DIR_Attr;
	if (attr & DIRITEM_ATTR_READ_ONLY) {
		printf("readonly, ");
	}
	if (attr & DIRITEM_ATTR_HIDDEN) {
		printf("hidden, ");
	}
	if (attr & DIRITEM_ATTR_SYSTEM) {
		printf("system, ");
	}
	if (attr & DIRITEM_ATTR_DIRECTORY) {
		printf("directory, ");
	}
	if (attr & DIRITEM_ATTR_ARCHIVE) {
		printf("achive, ");
	}

	printf("\n\tcreate: %d-%d-%d %d:%d:%d, ", diritem->DIR_CrtDate.year_from_1980 + 1980,
		diritem->DIR_CrtDate.month, diritem->DIR_CrtDate.day,
		diritem->DIR_CrtTime.hour, diritem->DIR_CrtTime.minute,
		diritem->DIR_CrtTime.second_2 * 2);

	printf("\n\tlast write: %d-%d-%d %d:%d:%d, ", diritem->DIR_WrtDate.year_from_1980 + 1980,
		diritem->DIR_WrtDate.month, diritem->DIR_WrtDate.day,
		diritem->DIR_WrtTime.hour, diritem->DIR_WrtTime.minute,
		diritem->DIR_WrtTime.second_2 * 2);

	printf("\n\tlast access: %d-%d-%d, ", diritem->DIR_LastAccDate.year_from_1980 + 1980,
		diritem->DIR_LastAccDate.month, diritem->DIR_LastAccDate.day);

	printf("\n\t size %f KB, ", diritem->DIR_FileSize / 1024.0);
	printf("\n\t cluster %d\n", (diritem->DIR_FstClusHI << 16) | diritem->DIR_FstClusL0);
}

int fat_dir_test(void) {
	u8_t* cluster_buffer = (u8_t*)malloc(xfat.cluster_byte_size);
	if (cluster_buffer == NULL) {
		printf("alloc cluster buffer failed!\n");
		return -1;
	}
	u32_t curr_cluster = xfat.root_cluster;
	int index = 0;
	while (is_cluster_valid(curr_cluster)) {
		xfat_err_t err = read_cluster(&xfat, cluster_buffer, curr_cluster, 1);

		if (err < 0) {
			printf("read cluster %d, failed!\n", curr_cluster);
			free(cluster_buffer);
			return -1;
		}

		diritem_t* diritem = (diritem_t*)cluster_buffer;
		for (int i = 0; i < xfat.cluster_byte_size / sizeof(diritem_t); i++) {
			u8_t* name = (u8_t*)(diritem[i].DIR_Name);
			if (name[0] == DIRITEM_NAME_FREE) {
				continue;
			}
			else if (name[0] == DIRITEM_NAME_END) {
				break;
			}

			++index;
			printf("no: %d ", index);
			show_dir_info(&diritem[i]);
		}

		err = get_next_cluster(&xfat, curr_cluster, &curr_cluster);
		if (err) {
			printf("get next cluster failed! current cluster: %d\n", curr_cluster);
			free(cluster_buffer);
			return -1;
		}
	}

	free(cluster_buffer);
	return FS_ERR_OK;
}

int fat_file_test(void) {
	u8_t* cluster_buffer = (u8_t*)malloc(xfat.cluster_byte_size);
	if (cluster_buffer == NULL) {
		printf("alloc cluster buffer failed!\n");
		return -1;
	}
	u32_t curr_cluster = 4565;
	int index = 0;
	int size = 0;
	while (is_cluster_valid(curr_cluster)) {
		xfat_err_t err = read_cluster(&xfat, cluster_buffer, curr_cluster, 1);

		if (err < 0) {
			printf("read cluster %d, failed!\n", curr_cluster);
			free(cluster_buffer);
			return -1;
		}

		cluster_buffer[xfat.cluster_byte_size - 1] = '\0';
		printf("%s", (char*)cluster_buffer);
		size += xfat.cluster_byte_size;
		err = get_next_cluster(&xfat, curr_cluster, &curr_cluster);
		if (err) {
			printf("get next cluster failed! current cluster: %d\n", curr_cluster);
			free(cluster_buffer);
			return -1;
		}
	}

	printf("\n file size: %d\n", size);
	free(cluster_buffer);
	return FS_ERR_OK;
}

int fs_open_test(void) {
	printf("fs open test...\n");

	xfile_t file;
	xfat_err_t err = xfile_open(&xfat, &file, "/");
	if (err) {
		printf("open file failed %s!\n", "/");
		return -1;
	}
	xfile_close(&file);

	const char* exist_path = "/12345678ABC";
	err = xfile_open(&xfat, &file, exist_path);
	if (err) {
		printf("open file failed %s!\n", exist_path);
		return -1;
	}
	xfile_close(&file);

	const char* file1 = "/open/file.txt";
	const char* file2 = "/open/a0/a1/a2/a3/a4/a5/a6/a7/a8/a9/a10/a11/a12/a13/a14/a15/a16/a17/a18/a19/file.txt";

	err = xfile_open(&xfat, &file, file1);
	if (err) {
		printf("open file failed %s!\n", file1);
		return -1;
	}
	xfile_close(&file);

	err = xfile_open(&xfat, &file, file2);
	if (err) {
		printf("open file failed %s!\n", file2);
		return -1;
	}
	xfile_close(&file);

	const char* not_exist_file = "/file_not_exist.txt";
	err = xfile_open(&xfat, &file, not_exist_file);
	if (err) {
		printf("open file failed %s!\n", not_exist_file);
		return -1;
	}
	xfile_close(&file);


	printf("file open test ok!\n");
	return 0;
}

void show_file_info(xfileinfo_t* fileinfo) {
	printf("\n\n name: %s, ", fileinfo->file_name);
	switch (fileinfo->type) {
	case FAT_FILE:
		printf("file, ");
		break;
	case FAT_DIR:
		printf("dir, ");
		break;
	case FAT_VOL:
		printf("vol, ");
		break;
	default:
		printf("unknown, ");
	}

	printf("\n\tcreate: %d-%d-%d %d:%d:%d",
		fileinfo->create_time.year,
		fileinfo->create_time.month,
		fileinfo->create_time.day,
		fileinfo->create_time.hour,
		fileinfo->create_time.minute,
		fileinfo->create_time.second);
	printf("\n\tlast write: %d-%d-%d %d:%d:%d",
		fileinfo->modify_time.year,
		fileinfo->modify_time.month,
		fileinfo->modify_time.day,
		fileinfo->modify_time.hour,
		fileinfo->modify_time.minute,
		fileinfo->modify_time.second);
	printf("\n\tlast access: %d-%d-%d",
		fileinfo->last_acctime.year,
		fileinfo->last_acctime.month,
		fileinfo->last_acctime.day);
	printf("\n\tsize %d kB, ", fileinfo->size / 1024);
	printf("\n");
}

int list_sub_file(xfile_t* file, int curr_depth) {
	xfileinfo_t fileinfo;
	xfat_err_t err = xdir_first_file(file, &fileinfo);
	if (err) {
		return err;
	}

	do {
		if (fileinfo.type == FAT_DIR) {
			for (int i = 0; i < curr_depth; i++) {
				printf("-");
			}
			printf("%s\n", fileinfo.file_name);
			xfile_t sub_file;
			err = xfile_open_sub(file, fileinfo.file_name, &sub_file);
			if (err < 0) {
				return err;
			}

			xfile_close(file);

			err = list_sub_file(&sub_file, curr_depth + 1);
			if (err < 0) {
				return err;
			}

			xfile_close(&sub_file);
		}
		else {
			printf("%s\n", fileinfo.file_name);
		}
	} while ((err = xdir_next_file(file, &fileinfo)) == 0);
	return 0;
}

int dir_traverse_test(void) {
	printf("\ntrans dir test!\n");

	xfile_t top_dir;
	xfileinfo_t fileinfo;
	xfat_err_t err = xfile_open(&xfat, &top_dir, "/");
	if (err < 0) {
		printf("Open directory failed!\n");
		return -1;
	}

	err = xdir_first_file(&top_dir, &fileinfo);
	if (err < 0) {
		printf("get file info failed!\n");
		return -1;
	}

	show_file_info(&fileinfo);

	while ((err = xdir_next_file(&top_dir, &fileinfo) == 0)) {
		show_file_info(&fileinfo);
	}

	if (err < 0) {
		printf("get file info failed!\n");
	}

	printf("\n try to list all sub files\n");
	err = list_sub_file(&top_dir, 0);
	if (err < 0) {
		printf("list file failed\n");
		return -1;
	}

	err = xfile_close(&top_dir);
	if (err < 0) {
		printf("close file failed!\n");
		return -1;
	}

	printf("file trans test ok!\n");
	return 0;
}

int file_read_and_check(const char* path, xfile_size_t elem_size, xfile_size_t e_count) {
	xfile_t file;
	xfat_err_t err = xfile_open(&xfat, &file, path);
	if (err != FS_ERR_OK) {
		printf("open file failed! %s\n", path);
		return -1;
	}

	xfile_size_t readed_count = 0;
	if ((readed_count = xfile_read(read_buffer, elem_size, e_count, &file) > 0)) {
		xfile_size_t bytes_count = readed_count * elem_size;
		u32_t num_start = 0;
		for (u32_t i = 0; i < bytes_count; i += 4) {
			if (read_buffer[i / 4] != num_start++) {
				printf("read file failed!\n");
				return -1;
			}
		}
	}

	if (xfile_error(&file) < 0) {
		printf("read failed!\n");
		return -1;
	}

	xfile_close(&file);
	return 0;
}

int fs_read_test(void) {
	printf("\n file read test\n");

	const char* file_0b_path = "/read/0b.bin";
	const char* file_1MB_path = "/read/1MB.bin";
	xfat_err_t err;
	memset(read_buffer, 0, sizeof(read_buffer));

	err = file_read_and_check(file_0b_path, 32, 1);
	if (err < 0) {
		printf("read failed!");
		return -1;
	}

	err = file_read_and_check(file_1MB_path, disk.sector_size - 32, 2);
	if (err < 0) {
		printf("read failed!\n");
		return -1;
	}

	err = file_read_and_check(file_1MB_path, disk.sector_size, 2);
	if (err < 0) {
		printf("read failed!\n");
		return -1;
	}

	err = file_read_and_check(file_1MB_path, disk.sector_size + 14, 2);
	if (err < 0) {
		printf("read failed!\n");
		return -1;
	}

	err = file_read_and_check(file_1MB_path, xfat.cluster_byte_size + 32, 2);
	if (err < 0) {
		printf("read failed!\n");
		return -1;
	}

	err = file_read_and_check(file_1MB_path, 2 * xfat.cluster_byte_size + 32, 2);
	if (err < 0) {
		printf("read failed!\n");
		return -1;
	}

	printf("\n file read test ok!\n");
	return 0;
}

int main(void) {
	for (int i = 0; i < sizeof(write_buffer) / sizeof(u32_t); i++) {
		write_buffer[i] = i;
	}

	int err = disk_io_test();
	if (err) {
		return err;
	}

	err = xdisk_open(&disk, "vdisk", &vdisk_driver, (void*)disk_path);
	if (err) {
		printf("open disk failed!\n");
		return -1;
	}

	err = disk_part_test();
	if (err) {
		return err;
	}

	err = xdisk_get_part(&disk, &disk_part, 1);
	if (err < 0) {
		printf("read partion failed!\n");
		return -1;
	}

	err = xfat_open(&xfat, &disk_part);
	if (err < 0) {
		printf("open fat failed!\n");
		return -1;
	}

	//err = fat_dir_test();
	//if (err) {
	//	return err;
	//}

	//err = fat_file_test();
	//if (err) {
	//	return err;
	//}

	//err = fs_open_test();
	//if (err) {
	//	return err;
	//}

	//err = dir_traverse_test();
	//if (err) {
	//	return err;
	//}

	err = fs_read_test();
	if (err < 0) {
		printf("read test failed\n");
		return -1;
	}

	err = xdisk_close(&disk);
	if (err) {
		printf("disk close failed\n");
		return err;
	}

	printf("test end!\n");
	return 0;
}