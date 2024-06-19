#include "xfat_obj.h"

xfat_err_t xfat_obj_init(xfat_obj_t* obj, xfat_obj_type_t type) {
	obj->type = type;
	return FS_ERR_OK;
}