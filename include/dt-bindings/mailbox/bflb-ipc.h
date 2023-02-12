/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/*
 * Copyright (C) 2023 Allen Martin <armartin@gmail.com>
 */

#ifndef __DT_BINDINGS_MAILBOX_BFLB_IPC_H
#define __DT_BINDINGS_MAILBOX_BFLB_IPC_H

/* Source processor */
#define BFLB_IPC_SOURCE_M0		0
#define BFLB_IPC_SOURCE_LP		1
#define BFLB_IPC_SOURCE_D0              2 

/* Peripheral device ID */
#define BFLB_IPC_DEVICE_SDHCI		0
#define BFLB_IPC_DEVICE_UART2		1
#define BFLB_IPC_DEVICE_USB             2
#define BFLB_IPC_DEVICE_EMAC		3
#define BFLB_IPC_DEVICE_MBOX_RX         5
#define BFLB_IPC_DEVICE_MBOX_TX         6

/* TARGET for Sending to other processors, continues from the BFLB_IPC_SOURCE_* range */
#define BFLB_IPC_TARGET_M0		3
#define BFLB_IPC_TARGET_LP		4
#define BFLB_IPC_TARGET_D0              5

/* MailBox Service */
#define BFLB_IPC_MBOX_VIRTIO            1


/* Operations for MBOX_VIRTIO */
#define BFLB_IPC_MBOX_VIRTIO_OP_KICK    1


#endif
