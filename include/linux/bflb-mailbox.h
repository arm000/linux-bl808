/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * BL808 mailbox message format
 *
 * Copyright (C) 2021 The Asahi Linux Contributors
 */

#ifndef _LINUX_BFLB_MAILBOX_H_
#define _LINUX_BFLB_MAILBOX_H_

#include <linux/types.h>

/* encodes a single 32bit message sent over the single channel */
struct bflb_mbox_msg {
	u32 param;
	u32 id;
};

#endif
