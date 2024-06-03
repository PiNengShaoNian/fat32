#ifndef XFAT_H
#define XFAT_H

#include "xtypes.h"
#include "xdisk.h"

#pragma pack(1)

/**
 * FAT文件系统的BPB结构
 */
typedef struct _bpb_t {
	u8_t BS_jmpBoot[3];                 // 跳转代码
	u8_t BS_OEMName[8];                 // OEM名称
	u16_t BPB_BytsPerSec;               // 每扇区字节数
	u8_t BPB_SecPerClus;                // 每簇扇区数
	u16_t BPB_RsvdSecCnt;               // 保留区扇区数
	u8_t BPB_NumFATs;                   // FAT表项数
	u16_t BPB_RootEntCnt;               // 根目录项目数
	u16_t BPB_TotSec16;                 // 总的扇区数
	u8_t BPB_Media;                     // 媒体类型
	u16_t BPB_FATSz16;                  // FAT表项大小
	u16_t BPB_SecPerTrk;                // 每磁道扇区数
	u16_t BPB_NumHeads;                 // 磁头数
	u32_t BPB_HiddSec;                  // 隐藏扇区数
	u32_t BPB_TotSec32;                 // 总的扇区数
} bpb_t;

/**
 * BPB中的FAT32结构
 */
typedef struct _fat32_hdr_t {
	u32_t BPB_FATSz32;                  // FAT表的字节大小
	u16_t BPB_ExtFlags;                 // 扩展标记
	u16_t BPB_FSVer;                    // 版本号
	u32_t BPB_RootClus;                 // 根目录的簇号
	u16_t BPB_FsInfo;                   // fsInfo的扇区号
	u16_t BPB_BkBootSec;                // 备份扇区
	u8_t BPB_Reserved[12];
	u8_t BS_DrvNum;                     // 设备号
	u8_t BS_Reserved1;
	u8_t BS_BootSig;                    // 扩展标记
	u32_t BS_VolID;                     // 卷序列号
	u8_t BS_VolLab[11];                 // 卷标名称
	u8_t BS_FileSysType[8];             // 文件类型名称
} fat32_hdr_t;

typedef struct _dbr_t {
	bpb_t bpb;
	fat32_hdr_t fat32;
} dbr_t;

#pragma pack()

typedef struct _xfat_t {
	u32_t fat_start_sector; // Fat分配表的起始扇区
	u32_t fat_tbl_nr; // Fat分配表的数量
	u32_t fat_tbl_sectors; // 每个文件分配表（FAT）占用的扇区数。
	u32_t total_sectors; // 总扇区数

	xdisk_part_t* disk_part;
} xfat_t;

xfat_err_t xfat_open(xfat_t* xfat, xdisk_part_t* part);

#endif