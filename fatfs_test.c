#include <stdio.h>
#include <string.h>
#include "xdisk.h"

extern xdisk_driver_t vdisk_driver;

const char* disk_path_test = "disk_test.img";
static u32_t write_buffer[160 * 1024];
static u32_t read_buffer[160 * 1024];

int disk_io_test(void) {
	int err;

	xdisk_t disk_test;

	disk_test.driver = &vdisk_driver;
	memset(read_buffer, 0, sizeof(read_buffer));


	err = xdisk_open(&disk_test, "vdisk_test", &vdisk_driver, (void *)disk_path_test);
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

#pragma pack(1)
typedef struct _mbr_t {
	u32_t a;
	u16_t b;
	u32_t c;
} mbr_t;
#pragma pack()

int main(void) {
	for (int i = 0; i < sizeof(write_buffer) / sizeof(u32_t); i++) {
		write_buffer[i] = i;
	}

	/*int err = disk_io_test();
	if (err) {
		return err;
	}*/

	mbr_t* mbr = (mbr_t*)0x100;
	printf("%p\n", &(mbr->c));

	printf("test end!\n");
	return 0;
}