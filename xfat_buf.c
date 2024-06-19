#include "xfat_buf.h"

void xfat_buf_set_state(xfat_buf_t* buf, u32_t state) {
	buf->flags &= ~XFAT_BUF_STATE_MSK;
	buf->flags |= state;
}