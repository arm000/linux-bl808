/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/*
 * Copyright (C) 2023 Allen Martin <armartin@gmail.com>
 */

#ifndef __DT_BINDINGS_MAILBOX_BFLB_IPC_H
#define __DT_BINDINGS_MAILBOX_BFLB_IPC_H

/* Source processor */
#define BFLB_IPC_SOURCE_M0		0
#define BFLB_IPC_SOURCE_LP		1

/* Peripheral device ID */
#define BFLB_IPC_DEVICE_SDHCI		0
#define BFLB_IPC_DEVICE_UART2		1
#define BFLB_IPC_DEVICE_USB		    2
#define BFLB_IPC_DEVICE_EMAC		3
#define BFLB_IPC_DEVICE_GPIO        4

#endif
