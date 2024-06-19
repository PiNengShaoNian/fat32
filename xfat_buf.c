#include "xfat_buf.h"
#include "xdisk.h"

static xfat_bpool_t* get_obj_bpool(xfat_obj_t* obj) {
	if (obj->type == XFAT_OBJ_DISK) {
		xdisk_t* disk = to_type(obj, xdisk_t);
		return &disk->bpool;
	}

	return (xfat_bpool_t*)0;
}

void xfat_buf_set_state(xfat_buf_t* buf, u32_t state) {
	buf->flags &= ~XFAT_BUF_STATE_MSK;
	buf->flags |= state;
}

xfat_err_t xfat_bpool_init(xfat_obj_t* obj, u32_t sector_size, u8_t* buffer, u32_t buf_size) {
	u32_t buf_count = buf_size / ((sizeof(xfat_buf_t)) + sector_size);
	xfat_buf_t* buf_start = (xfat_buf_t*)buffer;
	u8_t* sector_buf_start = buffer + buf_count * sizeof(xfat_buf_t);

	xfat_bpool_t* pool = get_obj_bpool(obj);
	if (pool == (xfat_bpool_t*)0) {
		return FS_ERR_PARAM;
	}

	if (buf_count == 0) {
		pool->first = pool->last = (xfat_buf_t*)0;
		pool->size = 0;
		return FS_ERR_OK;
	}

	xfat_buf_t* buf = buf_start++;
	buf->pre = buf->next = buf;
	buf->buf = sector_buf_start;
	pool->first = pool->last = buf;
	sector_buf_start += sector_size;

	for (u32_t i = 1; i < buf_count; i++) {
		xfat_buf_t* buf = buf_start++;
		buf->next = pool->first;
		buf->pre = pool->first->pre;

		buf->next->pre = buf;
		buf->pre->next = buf;
		pool->first = buf;

		buf->sector_no = 0;
		buf->buf = sector_buf_start;
		buf->flags = XFAT_BUF_STATE_FREE;
		sector_buf_start += sector_size;
	}

	pool->size = buf_count;
	return FS_ERR_OK;
}