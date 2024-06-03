#ifndef XFAT_H
#define XFAT_H

#include "xtypes.h"
#include "xdisk.h"

#pragma pack(1)

/**
 * FAT�ļ�ϵͳ��BPB�ṹ
 */
typedef struct _bpb_t {
	u8_t BS_jmpBoot[3];                 // ��ת����
	u8_t BS_OEMName[8];                 // OEM����
	u16_t BPB_BytsPerSec;               // ÿ�����ֽ���
	u8_t BPB_SecPerClus;                // ÿ��������
	u16_t BPB_RsvdSecCnt;               // ������������
	u8_t BPB_NumFATs;                   // FAT������
	u16_t BPB_RootEntCnt;               // ��Ŀ¼��Ŀ��
	u16_t BPB_TotSec16;                 // �ܵ�������
	u8_t BPB_Media;                     // ý������
	u16_t BPB_FATSz16;                  // FAT�����С
	u16_t BPB_SecPerTrk;                // ÿ�ŵ�������
	u16_t BPB_NumHeads;                 // ��ͷ��
	u32_t BPB_HiddSec;                  // ����������
	u32_t BPB_TotSec32;                 // �ܵ�������
} bpb_t;

/**
 * BPB�е�FAT32�ṹ
 */
typedef struct _fat32_hdr_t {
	u32_t BPB_FATSz32;                  // FAT����ֽڴ�С
	u16_t BPB_ExtFlags;                 // ��չ���
	u16_t BPB_FSVer;                    // �汾��
	u32_t BPB_RootClus;                 // ��Ŀ¼�Ĵغ�
	u16_t BPB_FsInfo;                   // fsInfo��������
	u16_t BPB_BkBootSec;                // ��������
	u8_t BPB_Reserved[12];
	u8_t BS_DrvNum;                     // �豸��
	u8_t BS_Reserved1;
	u8_t BS_BootSig;                    // ��չ���
	u32_t BS_VolID;                     // �����к�
	u8_t BS_VolLab[11];                 // �������
	u8_t BS_FileSysType[8];             // �ļ���������
} fat32_hdr_t;

typedef struct _dbr_t {
	bpb_t bpb;
	fat32_hdr_t fat32;
} dbr_t;

#pragma pack()

typedef struct _xfat_t {
	u32_t fat_start_sector; // Fat��������ʼ����
	u32_t fat_tbl_nr; // Fat����������
	u32_t fat_tbl_sectors; // ÿ���ļ������FAT��ռ�õ���������
	u32_t total_sectors; // ��������

	xdisk_part_t* disk_part;
} xfat_t;

xfat_err_t xfat_open(xfat_t* xfat, xdisk_part_t* part);

#endif