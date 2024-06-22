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

#define CLUSTER_INVALID                 0x0FFFFFFF          // 无效的簇号
#define CLUSTER_FREE 0
#define FILE_DEFAULT_CLUSTER 0
#define DIRITEM_NAME_FREE               0xE5                // 目录项空闲名标记
#define DIRITEM_NAME_END                0x00                // 目录项结束名标记

#define DIRITEM_NTRES_BODY_LOWER 0x08 // (1 << 3)
#define DIRITEM_NTRES_EXT_LOWER 0x10 // (1 << 4)
#define DIRITEM_NTRES_CASE_MASK 0x18

#define DIRITEM_ATTR_READ_ONLY          0x01                // 目录项属性：只读
#define DIRITEM_ATTR_HIDDEN             0x02                // 目录项属性：隐藏
#define DIRITEM_ATTR_SYSTEM             0x04                // 目录项属性：系统类型
#define DIRITEM_ATTR_VOLUME_ID          0x08                // 目录项属性：卷id
#define DIRITEM_ATTR_DIRECTORY          0x10                // 目录项属性：目录
#define DIRITEM_ATTR_ARCHIVE            0x20                // 目录项属性：归档
#define DIRITEM_ATTR_LONG_NAME          0x0F                // 目录项属性：长文件名

#define DIRITEM_GET_FREE (1 << 0)
#define DIRITEM_GET_USED (1 << 1)
#define DIRITEM_GET_END (1 << 2)
#define DIRITEM_GET_ALL (0XFF)

/**
 * FAT目录项的日期类型
 */
typedef struct _diritem_date_t {
	u16_t day : 5;                  // 日
	u16_t month : 4;                // 月
	u16_t year_from_1980 : 7;       // 年
} diritem_date_t;

/**
 * FAT目录项的时间类型
 */
typedef struct _diritem_time_t {
	u16_t second_2 : 5;             // 最多精确到2秒，取值为0-29
	u16_t minute : 6;               // 分
	u16_t hour : 5;                 // 时
} diritem_time_t;

typedef struct _diritem_t {
	u8_t DIR_Name[8];                   // 文件名
	u8_t DIR_ExtName[3];                // 扩展名
	u8_t DIR_Attr;                      // 属性
	u8_t DIR_NTRes;
	u8_t DIR_CrtTimeTeenth;             // 创建时间的毫秒
	diritem_time_t DIR_CrtTime;         // 创建时间
	diritem_date_t DIR_CrtDate;         // 创建日期
	diritem_date_t DIR_LastAccDate;     // 最后访问日期
	u16_t DIR_FstClusHI;                // 簇号高16位
	diritem_time_t DIR_WrtTime;         // 修改时间
	diritem_date_t DIR_WrtDate;         // 修改时期
	u16_t DIR_FstClusL0;                // 簇号低16位
	u32_t DIR_FileSize;                 // 文件字节大小
} diritem_t;

typedef union _cluster32_t {
	struct {
		u32_t next : 28;
		u32_t reserved : 4;
	} s;
	u32_t v;
} cluster32_t;

/**
 * FSInfo结构
 */
typedef struct _fsinfo_t {
	u32_t FSI_LoadSig;                  // 固定标记：0x41615252
	u8_t FSI_Reserved1[480];
	u32_t FSI_StrucSig;                 // 固定标记： 0x61417272
	u32_t FSI_Free_Count;               // 最新剩余簇数
	u32_t FSI_Next_Free;                // 从何处开始找剩余簇
	u8_t FSI_Reserved2[12];
	u32_t FSI_TrailSig;                 // 固定标记： 0xAA550000
} fsinfo_t;

#pragma pack()

#define XFAT_NAME_LEN 16

typedef struct _xfat_t {
	xfat_obj_t obj;
	char name[XFAT_NAME_LEN];
	u32_t fat_start_sector; // Fat分配表的起始扇区
	u32_t fat_tbl_nr; // Fat分配表的数量
	u32_t fat_tbl_sectors; // 每个文件分配表（FAT）占用的扇区数。
	u32_t sec_per_cluster; // 每个簇有几个扇区
	u32_t root_cluster; // 根目录簇
	u32_t cluster_byte_size; // 一个簇占几个字节
	u32_t total_sectors; // 总扇区数

	u8_t* fat_buffer;
	xdisk_part_t* disk_part;

	xfat_bpool_t bpool;

	struct _xfat_t* next;
} xfat_t;

typedef enum _xfile_type_t {
	FAT_DIR,
	FAT_FILE,
	FAT_VOL,
} xfile_type_t;

#define XFILE_ATTR_READONLY (1 << 0)
#define SFN_LEN 11

#define XFILE_LOCATE_NORMAL (1 << 0)
#define XFILE_LOCATE_DOT (1 << 1) // ., ..
#define XFILE_LOCATE_VOL (1 << 2)
#define XFILE_LOCATE_SYSTEM (1 << 3)
#define XFILE_LOCATE_HIDDEN (1 << 4)
#define XFILE_LOCATE_ALL 0xFF

typedef struct _xfile_t {
	xfat_obj_t obj;
	xfat_t* xfat;
	u32_t size;
	u16_t attr;
	xfile_type_t type;
	u32_t pos;
	xfat_err_t err;
	u32_t start_cluster;
	u32_t curr_cluster;

	u32_t dir_cluster;
	u32_t dir_cluster_offset;

	xfat_bpool_t bpool;
} xfile_t;

typedef enum _xfile_origin_t {
	XFAT_SEEK_SET,
	XFAT_SEEK_CUR,
	XFAT_SEEK_END,
} xfile_origin_t;

typedef enum _stime_type_t {
	XFAT_TIME_ATIME,
	XFAT_TIME_CTIME,
	XFAT_TIME_MTIME,
} stime_type_t;

typedef enum _xcluster_size_t {
	XFAT_CLUSTER_512B = 512,
	XFAT_CLUSTER_1K = 1024,
	XFAT_CLUSTER_2K = (2 * 1024),
	XFAT_CLUSTER_4K = (4 * 1024),
	XFAT_CLUSTER_8K = (8 * 1024),
	XFAT_CLUSTER_16K = (16 * 1024),
	XFAT_CLUSTER_32K = (32 * 1024),
	XFAT_CLUSTER_AUTO,
} xcluster_size_t;

typedef struct _xfat_fmt_ctrl_t {
	xfs_type_t type;
	xcluster_size_t cluster_size;
	const char* vol_name;
} xfat_fmt_ctrl_t;

typedef struct _xfile_time_t {
	u16_t year;
	u8_t month;
	u8_t day;
	u8_t hour;
	u8_t minute;
	u8_t second;
} xfile_time_t;

typedef struct _xfileinfo_t {
#define X_FILEINFO_NAME_SIZE 32
	char file_name[X_FILEINFO_NAME_SIZE];
	u32_t size;
	u16_t attr;
	xfile_type_t type;
	xfile_time_t create_time;
	xfile_time_t last_acctime;
	xfile_time_t modify_time;
} xfileinfo_t;

typedef struct _xfat_fmt_info_t {
	u8_t fat_count;
	u8_t media;
	u32_t fat_sectors;
	u32_t rsvd_sectors;
	u32_t root_cluster;
	u32_t sec_per_cluster;
	u32_t backup_sector;
	u32_t fsinfo_sector;
} xfat_fmt_info_t;

int is_cluster_valid(u32_t cluster);
xfat_err_t get_next_cluster(xfat_t* xfat, u32_t curr_cluster, u32_t* next_cluster);
xfat_err_t xfat_init(void);
xfat_err_t xfat_mount(xfat_t* xfat, xdisk_part_t* part, const char* mount_name);
void xfat_unmount(xfat_t* xfat);
xfat_err_t xfat_set_buf(xfat_t* xfat, u8_t* buf, u32_t size);

xfat_err_t xfat_fmt_ctrl_init(xfat_fmt_ctrl_t* ctrl);
xfat_err_t xfat_format(xdisk_part_t* disk_part, xfat_fmt_ctrl_t* ctrl);

xfat_err_t read_cluster(xfat_t* xfat, u8_t* buffer, u32_t cluster, u32_t count);

xfat_err_t xfile_open(xfile_t* file, const char* path);
xfat_err_t xfile_open_sub(xfile_t* dir, const char* sub_path, xfile_t* sub_file);
xfat_err_t xfile_close(xfile_t* file);
xfat_err_t xfile_set_buf(xfile_t* file, u8_t* buf, u32_t size);

xfat_err_t xdir_first_file(xfile_t* file, xfileinfo_t* info);
xfat_err_t xdir_next_file(xfile_t* file, xfileinfo_t* info);

xfat_err_t xfile_error(xfile_t* file);
void xfile_clear_err(xfile_t* file);

xfat_err_t xfile_mkdir(const char* path);
xfat_err_t xfile_mkfile(const char* path);
xfat_err_t xfile_rmfile(const char* path);
xfat_err_t xfile_rmdir(const char* path);
xfat_err_t xfile_rmdir_tree(const char* path);
xfile_size_t xfile_read(void* buffer, xfile_size_t elem_size, xfile_size_t count, xfile_t* file);
xfile_size_t xfile_write(void* buffer, xfile_size_t elem_size, xfile_size_t count, xfile_t* file);

xfat_err_t xfile_eof(xfile_t* file);
xfile_size_t xfile_tell(xfile_t* file);
xfat_err_t xfile_seek(xfile_t* file, xfile_ssize_t ssize, xfile_origin_t origin);

xfat_err_t xfile_size(xfile_t* file, xfile_size_t* size);
xfat_err_t xfile_resize(xfile_t* file, xfile_size_t size);
xfat_err_t xfile_rename(const char* path, const char* new_name);

xfat_err_t xfile_set_atime(const char* path, xfile_time_t* time);
xfat_err_t xfile_set_mtime(const char* path, xfile_time_t* time);
xfat_err_t xfile_set_ctime(const char* path, xfile_time_t* time);

#endif