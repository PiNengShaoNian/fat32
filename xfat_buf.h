#ifndef XFAT_BUF_H
#define XFAT_BUF_H

#include "xtypes.h"
#include "xfat_obj.h"

#define XFAT_BUF_STATE_FREE        (0 << 0)			// 扇区空闲，未被写入任何数据
#define XFAT_BUF_STATE_CLEAN       (1 << 0)			// 扇区干净，未被写数据
#define XFAT_BUF_STATE_DIRTY       (2 << 0)			// 扇区脏，已经被写入数据，未回写到磁盘
#define XFAT_BUF_STATE_MSK         (3 << 0)         // 写状态掩码


typedef struct _xfat_buf_t {
	u8_t* buf;
	u32_t sector_no;
	u32_t flags;

	struct _xfat_buf_t* next;
	struct _xfat_buf_t* pre;
} xfat_buf_t;

#define xfat_buf_state(buf) ((buf)->flags & XFAT_BUF_STATE_MASK)
void xfat_buf_set_state(xfat_buf_t* buf, u32_t state);

typedef struct _xfat_bpool_t {
	xfat_buf_t* first;
	xfat_buf_t* last;
	u32_t size;
} xfat_bpool_t;

#define XFAT_BUF_SIZE(sector_size, sector_nr) ((sizeof(xfat_buf_t) + (sector_size)) * (sector_nr))

xfat_err_t xfat_bpool_init(xfat_obj_t* obj, u32_t sector_size, u8_t* buffer, u32_t buf_size);

#endif // !XFAT_BUF_H
