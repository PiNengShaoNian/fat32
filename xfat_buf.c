#include "xfat_buf.h"
#include "xdisk.h"
#include "xfat.h"

static xfat_bpool_t* get_obj_bpool(xfat_obj_t* obj) {
	if (obj->type == XFAT_OBJ_FILE) {
		xfile_t* file = to_type(obj, xfile_t);
		obj = to_obj(file->xfat->disk_part->disk);
	}

	if (obj->type == XFAT_OBJ_FAT) {
		xfat_t* xfat = to_type(obj, xfat_t);
		obj = to_obj(xfat->disk_part->disk);
	}

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

static xdisk_t* get_obj_disk(xfat_obj_t* obj) {
	switch (obj->type) {
	case XFAT_OBJ_FILE:
		return to_type(obj, xfile_t)->xfat->disk_part->disk;
	case XFAT_OBJ_FAT:
		return to_type(obj, xfat_t)->disk_part->disk;
	case XFAT_OBJ_DISK:
		return (xdisk_t*)obj;
	}
	return (xdisk_t*)0;
}

static xfat_err_t bpool_moveto_first(xfat_bpool_t* pool, xfat_buf_t* buf) {
	if (pool->first == buf) {
		return FS_ERR_OK;
	}

	buf->pre->next = buf->next;
	buf->next->pre = buf->pre;
	if (pool->last == buf) {
		pool->last = buf->pre;
	}

	pool->last->next = buf;
	pool->first->pre = buf;

	buf->next = pool->first;
	buf->pre = pool->first->pre;
	pool->first = buf;
	return FS_ERR_OK;
}

static xfat_err_t bpool_find_buf(xfat_bpool_t* pool, u32_t sector_no, xfat_buf_t** buf) {
	if (pool->first == (xfat_buf_t*)0) {
		return FS_ERR_NO_BUFFER;
	}

	xfat_buf_t* r_buf = pool->first;
	u32_t size = pool->size;
	xfat_buf_t* free_buf = (xfat_buf_t*)0;
	while (size--) {
		switch (xfat_buf_state(r_buf)) {
			free_buf = r_buf;
			break;
		case XFAT_BUF_STATE_CLEAN:
		case XFAT_BUF_STATE_DIRTY:
			if (r_buf->sector_no == sector_no) {
				*buf = r_buf;
				return bpool_moveto_first(pool, r_buf);
			}
			break;
		}

		r_buf = r_buf->next;
	}

	if (free_buf != (xfat_buf_t*)0) {
		*buf = free_buf;
	}
	else {
		*buf = pool->last;
	}

	return bpool_moveto_first(pool, *buf);
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

xfat_err_t xfat_bpool_read_sector(xfat_obj_t* obj, xfat_buf_t** buf, u32_t sector_no) {
	xfat_bpool_t* pool = get_obj_bpool(obj);
	if (pool == (xfat_bpool_t*)0) {
		return FS_ERR_OK;
	}

	xfat_buf_t* r_buf = (xfat_buf_t*)0;
	u32_t err = bpool_find_buf(pool, sector_no, &r_buf);
	if (err < 0) {
		return err;
	}

	if (r_buf == (xfat_buf_t*)0) {
		return FS_ERR_NONE;
	}

	if ((sector_no == r_buf->sector_no) && (xfat_buf_state(r_buf) != XFAT_BUF_STATE_FREE)) {
		*buf = r_buf;
		return FS_ERR_OK;
	}

	switch (xfat_buf_state(r_buf)) {
	case XFAT_BUF_STATE_FREE:
	case XFAT_BUF_STATE_CLEAN:
		break;
	case XFAT_BUF_STATE_DIRTY:
		err = xdisk_write_sector(get_obj_disk(obj), r_buf->buf, r_buf->sector_no, 1);
		if (err < 0) {
			return err;
		}
		break;
	}

	err = xdisk_read_sector(get_obj_disk(obj), r_buf->buf, sector_no, 1);
	if (err < 0) {
		return err;
	}

	xfat_buf_set_state(r_buf, XFAT_BUF_STATE_CLEAN);
	r_buf->sector_no = sector_no;
	*buf = r_buf;
	return FS_ERR_OK;
}

xfat_err_t xfat_bpool_write_sector(xfat_obj_t* obj, xfat_buf_t* buf, u8_t is_through) {
	if (is_through) {
		xfat_err_t	err = xdisk_write_sector(get_obj_disk(obj), buf->buf, buf->sector_no, 1);
		if (err < 0) {
			return err;
		}

		xfat_buf_set_state(buf, XFAT_BUF_STATE_CLEAN);
	}
	else {
		xfat_buf_set_state(buf, XFAT_BUF_STATE_DIRTY);
	}

	return FS_ERR_OK;
}

xfat_err_t xfat_bpool_alloc(xfat_obj_t* obj, xfat_buf_t** buf, u32_t sector_no) {
	xfat_bpool_t* pool = get_obj_bpool(obj);
	if (pool == (xfat_bpool_t*)0) {
		return FS_ERR_OK;
	}

	xfat_buf_t* r_buf = (xfat_buf_t*)0;
	u32_t err = bpool_find_buf(pool, sector_no, &r_buf);
	if (err < 0) {
		return err;
	}

	if (r_buf == (xfat_buf_t*)0) {
		return FS_ERR_NONE;
	}

	if ((sector_no == r_buf->sector_no) && (xfat_buf_state(r_buf) != XFAT_BUF_STATE_FREE)) {
		*buf = r_buf;
		return FS_ERR_OK;
	}

	switch (xfat_buf_state(r_buf)) {
	case XFAT_BUF_STATE_FREE:
	case XFAT_BUF_STATE_CLEAN:
		break;
	case XFAT_BUF_STATE_DIRTY:
		err = xdisk_write_sector(get_obj_disk(obj), r_buf->buf, r_buf->sector_no, 1);
		if (err < 0) {
			return err;
		}
		break;
	}

	xfat_buf_set_state(r_buf, XFAT_BUF_STATE_FREE);
	r_buf->sector_no = sector_no;
	*buf = r_buf;
	return FS_ERR_OK;
}

xfat_err_t xfat_bpool_flush(xfat_obj_t* obj) {
	xfat_bpool_t* pool = get_obj_bpool(obj);
	u32_t size = pool->size;

	if (pool == (xfat_bpool_t*)0) {
		return FS_ERR_PARAM;
	}

	xfat_buf_t* cur_buf = pool->first;
	while (size--) {
		switch (xfat_buf_state(cur_buf)) {
		case XFAT_BUF_STATE_FREE:
		case XFAT_BUF_STATE_CLEAN:
		{
			break;
		}
		case XFAT_BUF_STATE_DIRTY:
		{
			xfat_err_t err = xdisk_write_sector(get_obj_disk(obj), cur_buf->buf, cur_buf->sector_no, 1);
			if (err < 0) {
				return err;
			}
			xfat_buf_set_state(cur_buf, XFAT_BUF_STATE_CLEAN);
			break;
		}
		}
		cur_buf = cur_buf->next;
	}

	return FS_ERR_OK;
}