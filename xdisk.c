#include "xdisk.h"

xfat_err_t xdisk_open(xdisk_t* disk, const char* name, xdisk_driver_t* driver,
	void* init_data, u8_t* disk_buf, u32_t buf_size) {
	xfat_err_t err;
	xfat_obj_init(&disk->obj, XFAT_OBJ_DISK);
	disk->driver = driver;
	err = disk->driver->open(disk, init_data);
	if (err < 0) {
		return err;
	}
	err = xfat_bpool_init(to_obj(disk), disk->sector_size, disk_buf, buf_size);
	if (err < 0) {
		return err;
	}
	disk->name = name;
	return FS_ERR_OK;
}
xfat_err_t xdisk_close(xdisk_t* disk) {
	xfat_err_t err = xfat_bpool_flush(to_obj(disk));
	if (err < 0) {
		return err;
	}
	return disk->driver->close(disk);
}

xfat_err_t xdisk_read_sector(xdisk_t* disk, u8_t* buffer, u32_t start_sector, u32_t count) {
	if (start_sector + count >= disk->total_sector) {
		return FS_ERR_PARAM;
	}
	return disk->driver->read_sector(disk, buffer, start_sector, count);
}

xfat_err_t xdisk_write_sector(xdisk_t* disk, u8_t* buffer, u32_t start_sector, u32_t count) {
	if (start_sector + count >= disk->total_sector) {
		return FS_ERR_PARAM;
	}
	return disk->driver->write_sector(disk, buffer, start_sector, count);
}

xfat_err_t xdisk_curr_time(struct _xdisk_t* disk, struct _xfile_time_t* timeinfo) {
	return disk->driver->curr_time(disk, timeinfo);
}

static xfat_err_t disk_get_extend_part_count(xdisk_t* disk, u32_t start_sector, u32_t* count) {
	int r_count = 0;
	u32_t ext_start_sector = start_sector;
	mbr_part_t* part;
	do {
		xfat_buf_t* disk_buf;
		int err = xfat_bpool_read_sector(to_obj(disk), &disk_buf, start_sector);
		if (err < 0) {
			return err;
		}
		part = ((mbr_t*)disk_buf->buf)->part_info;
		if (part->system_id == FS_NOT_VALID) {
			break;
		}
		r_count++;
		if ((++part)->system_id != FS_EXTEND) {
			break;
		}
		start_sector = ext_start_sector + part->relative_sectors;
	} while (1);

	*count = r_count;
	return FS_ERR_OK;
}

xfat_err_t xdisk_get_part_count(xdisk_t* disk, u32_t* count) {
	mbr_part_t* part;
	xfat_buf_t* disk_buf;
	int err = xfat_bpool_read_sector(to_obj(disk), &disk_buf, 0);
	if (err < 0) {
		return err;
	}

	int r_count = 0;
	u8_t extend_part_flag = 0;
	u32_t start_sector[4];
	part = ((mbr_t*)disk_buf->buf)->part_info;
	for (int i = 0; i < MBR_PRIMARY_PART_NR; i++, part++) {
		if (part->system_id == FS_NOT_VALID) {
			continue;
		}
		else if (part->system_id == FS_EXTEND) {
			start_sector[i] = part->relative_sectors;
			extend_part_flag |= 1 << i;
		}
		else {
			r_count++;
		}
	}
	if (extend_part_flag) {
		for (int i = 0; i < MBR_PRIMARY_PART_NR; i++) {
			if (extend_part_flag & (1 << i)) {
				int ext_count = 0;
				err = disk_get_extend_part_count(disk, start_sector[i], &ext_count);
				if (err < 0) {
					return err;
				}

				r_count += ext_count;
			}
		}
	}
	*count = r_count;
	return FS_ERR_OK;
}

static xfat_err_t disk_get_extend_part(xdisk_t* disk, xdisk_part_t* xdisk_part, u32_t start_sector,
	int part_no, u32_t* count) {
	xfat_err_t err = FS_ERR_OK;
	int curr_no = -1;
	u32_t ext_start_sector = start_sector;
	do {
		mbr_part_t* part;
		xfat_buf_t* disk_buf;
		err = xfat_bpool_read_sector(to_obj(disk), &disk_buf, start_sector);
		if (err < 0) {
			return err;
		}
		part = ((mbr_t*)disk_buf->buf)->part_info;
		if (part->system_id == FS_NOT_VALID) {
			err = FS_ERR_EOF;
			break;
		}
		if (++curr_no == part_no) {
			xdisk_part->type = part->system_id;
			xdisk_part->start_sector = part->relative_sectors + start_sector;
			xdisk_part->total_sector = part->total_sectors;
			xdisk_part->relative_sector = part->relative_sectors;
			xdisk_part->disk = disk;
			break;
		}
		if ((++part)->system_id != FS_EXTEND) {
			err = FS_ERR_EOF;
			break;
		}

		start_sector = ext_start_sector + part->relative_sectors;
	} while (1);

	*count = curr_no + 1;
	return err;
}

xfat_err_t xdisk_get_part(xdisk_t* disk, xdisk_part_t* xdisk_part, int part_no) {
	xfat_buf_t* disk_buf;
	int err = xfat_bpool_read_sector(to_obj(disk), &disk_buf, 0);
	if (err < 0) {
		return err;
	}

	mbr_part_t* mbr_part = ((mbr_t*)disk_buf->buf)->part_info;
	int curr_no = -1;
	for (int i = 0; i < MBR_PRIMARY_PART_NR; i++, mbr_part++) {
		if (mbr_part->system_id == FS_NOT_VALID) {
			continue;
		}
		if (mbr_part->system_id == FS_EXTEND) {
			u32_t count = 0;
			err = disk_get_extend_part(disk, xdisk_part, mbr_part->relative_sectors, part_no - i, &count);
			if (err < 0) {
				return err;
			}
			if (err == FS_ERR_OK) {
				return FS_ERR_OK;
			}
			else {
				curr_no += count;
				err = xfat_bpool_read_sector(to_obj(disk), &disk_buf, 0);
				if (err < 0) {
					return err;
				}
			}
		}
		else {
			if (++curr_no == part_no) {
				xdisk_part->type = mbr_part->system_id;
				xdisk_part->start_sector = mbr_part->relative_sectors;
				xdisk_part->total_sector = mbr_part->total_sectors;
				xdisk_part->relative_sector = mbr_part->relative_sectors;
				xdisk_part->disk = disk;
				return FS_ERR_OK;
			}
		}
	}

	return FS_ERR_NONE;
}

static xfat_err_t set_ext_part_type(xdisk_part_t* part, u32_t ext_start_sector, xfs_type_t type) {
	xfat_err_t err = FS_ERR_OK;
	u32_t start_sector = ext_start_sector;
	xdisk_t* disk = part->disk;

	do {
		mbr_part_t* ext_part;
		xfat_buf_t* disk_buf;
		err = xfat_bpool_read_sector(to_obj(disk), &disk_buf, start_sector);
		if (err < 0) {
			return err;
		}

		ext_part = ((mbr_t*)disk_buf->buf)->part_info;
		if (ext_part->system_id == FS_NOT_VALID) {
			err = FS_ERR_EOF;
			break;
		}

		if (start_sector + ext_part->relative_sectors == part->start_sector) {
			ext_part->system_id = type;

			err = xfat_bpool_write_sector(to_obj(disk), disk_buf, 0);
			return err;
		}

		if ((++ext_part)->system_id != FS_EXTEND) { // 无后续分区，设置未找到, 返回
			err = FS_ERR_EOF;
			break;
		}

		start_sector = ext_start_sector + ext_part->relative_sectors;
	} while (1);

	return err;
}

xfat_err_t xdisk_set_part_type(xdisk_part_t* xdisk_part, xfs_type_t type) {
	xdisk_t* disk = xdisk_part->disk;
	xfat_buf_t* disk_buf;
	int err = xfat_bpool_read_sector(to_obj(disk), &disk_buf, 0);
	if (err < 0) {
		return err;
	}

	mbr_part_t* mbr_part = ((mbr_t*)disk_buf->buf)->part_info;
	for (int i = 0; i < MBR_PRIMARY_PART_NR; i++, mbr_part++) {
		if (mbr_part->system_id == FS_NOT_VALID) {
			continue;
		}
		if (mbr_part->system_id == FS_EXTEND) {
			u32_t count = 0;
			err = set_ext_part_type(xdisk_part, mbr_part->relative_sectors, type);
			if (err < 0) {
				return err;
			}
			if (err == FS_ERR_OK) {
				return FS_ERR_OK;
			}
			else {
				err = xfat_bpool_read_sector(to_obj(disk), &disk_buf, 0);
				if (err < 0) {
					return err;
				}
			}
		}
		else {
			if (mbr_part->relative_sectors == xdisk_part->start_sector) {
				mbr_part->system_id = type;
				return xfat_bpool_write_sector(to_obj(disk), disk_buf, 0);
			}
		}
	}

	return FS_ERR_NONE;
}