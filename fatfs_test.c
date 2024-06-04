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
	printf("file open test ok!\n");
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

	err = fs_open_test();
	if (err) {
		return err;
	}

	err = xdisk_close(&disk);
	if (err) {
		printf("disk close failed\n");
		return err;
	}

	printf("test end!\n");
	return 0;
}