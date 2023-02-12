// SPDX-License-Identifier: GPL-2.0-only
/*
 * Remote processor machine-specific module for bflb
 *
 * Copyright (C) 2023 Justin Hammond
 */

#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/reset.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/remoteproc.h>
#include <linux/mailbox_client.h>
#include <linux/mailbox_controller.h>
#include <linux/delay.h>
#include <linux/bflb-mailbox.h>
#include "remoteproc_internal.h"

/**
 * struct bflb_mbox - bflb mailbox instance state
 * @name: the name of the mailbox
 * @client: the mailbox client
 * @chan: the mailbox channel
 * @vq_work: the workqueue for the virtqueue
 * @vq_id: the virtqueue id
 */

struct bflb_mbox {
	unsigned char name[10];
	struct mbox_client client;
	struct mbox_chan *chan;
	struct work_struct vq_work;
	int vq_id;
};


/**
 * struct bflb_rproc - bflb remote processor instance state
 * @rproc: rproc handle
 * @mbox: the mailbox channel
 * @client: the mailbox client
 */
struct bflb_rproc {
	struct rproc *rproc;
	struct bflb_mbox mbox_rx;
	struct bflb_mbox mbox_tx;
	struct workqueue_struct *workqueue;
};

/* We have to fundge the resource table for the M0
 * as its already running firmware by the time Linux Loads
 * and its not a ELF image, so these setup the Virtqueue,
 * and RPMSG structures to communicate with it
 */

/* The feature bitmap for virtio rpmsg */
#define VIRTIO_RPMSG_F_NS	0 /* RP supports name service notifications */

#define FW_RSC_U32_ADDR_ANY	0xFFFFFFFFUL

#define RPMSG_VDEV_DFEATURES	(1 << VIRTIO_RPMSG_F_NS)

/* VirtIO rpmsg device id */
#define VIRTIO_ID_RPMSG_	7

/* Resource table entries */
#define NUM_VRINGS		0x02
#define VRING_ALIGN		0x1000
#define RING_TX			FW_RSC_U32_ADDR_ANY
#define RING_RX			FW_RSC_U32_ADDR_ANY
/* VRING_SIZE is the number of VRINGS per TX/RX ring, so
 * actual number is double this. Size of the space needed
 * is VRING_SIZE * 2 * MAX_RPMSG_BUF_SIZE
 */
#define VRING_SIZE		8
#define NUM_TABLE_ENTRIES	1
#define NO_RESOURCE_ENTRIES	1

/* this is the structure of the Resource Table normally
 * loaded from a ELF header.
 * its setup for just 1 VDEV entry, which is the RPMSG
 * structure. We manually setup the vring's and vbuffers
 * in our prepre op below.
 */
struct remote_resource_table {
	u32 version;
	u32 num;
	u32 reserved[2];
	u32 offset[NO_RESOURCE_ENTRIES];
	u32 type; /* the vdev type that follows */
	struct fw_rsc_vdev rpmsg_vdev;
	struct fw_rsc_vdev_vring rpmsg_vring0;
	struct fw_rsc_vdev_vring rpmsg_vring1;
} __packed;

/* Setup one RPMSG VDEV and two VRINGs */
struct remote_resource_table resources = {
	/* Version */
	.version = 1,

	/* NUmber of table entries */
	.num = NUM_TABLE_ENTRIES,

	/* reserved fields */
	.reserved = {0, 0,},

	/* Offsets of rsc entries */
	.offset[0] = offsetof(struct remote_resource_table, type),

	.type = RSC_VDEV,
	/* Virtio device entry */
	{
	 VIRTIO_ID_RPMSG_, 0, RPMSG_VDEV_DFEATURES, 0, 0, 0,
	 NUM_VRINGS, {0, 0},
	},

	/* Vring rsc entry - part of vdev rsc entry */
	{FW_RSC_U32_ADDR_ANY, VRING_ALIGN, VRING_SIZE, 0, 0},
	{FW_RSC_U32_ADDR_ANY, VRING_ALIGN, VRING_SIZE, 1, 0},
};

/* return a pointer to our resource table */
struct resource_table *bflb_rproc_get_loaded_rsc_table(struct rproc *rproc, size_t *size)
{
	*size = sizeof(resources);
	return (struct resource_table *)&resources;
}

/* allocate vdev0buffer */
static int bflb_rproc_mem_alloc(struct rproc *rproc,
			      struct rproc_mem_entry *mem)
{
	struct device *dev = rproc->dev.parent;
	void *va;

	va = ioremap_wc(mem->dma, mem->len);
	if (!va) {
		dev_err(dev, "Unable to map memory region: %pa+%zx\n",
			&mem->dma, mem->len);
		return -ENOMEM;
	}

	/* Update memory entry va */
	mem->va = va;

	return 0;
}

/* release vdev0buffer */
static int bflb_rproc_mem_release(struct rproc *rproc,
				struct rproc_mem_entry *mem)
{
	iounmap(mem->va);

	return 0;
}

/*
 * Pull the memory ranges for virtio from the device tree and register them.
 * Called as prepare.
 */
static int bflb_rproc_setupmem(struct rproc *rproc)
{
	struct device *dev = rproc->dev.parent;
	struct device_node *np = dev->of_node;
	struct rproc_mem_entry *mem;
	struct reserved_mem *rmem;
	struct of_phandle_iterator it;
	int index = 0;

	dev_dbg(dev, "%s %s", __func__, np->name);

	of_phandle_iterator_init(&it, np, "memory-region", NULL, 0);
	while (of_phandle_iterator_next(&it) == 0) {
		rmem = of_reserved_mem_lookup(it.node);
		if (!rmem) {
			dev_err(dev, "unable to acquire memory-region\n");
			return -EINVAL;
		}

		/*  No need to map vdev buffer */
		if (strcmp(it.node->name, "vdev0buffer")) {
			/* Register memory region */
			mem = rproc_mem_entry_init(dev, NULL,
						   (dma_addr_t)rmem->base,
						   rmem->size, rmem->base,
						   bflb_rproc_mem_alloc,
						   bflb_rproc_mem_release,
						   it.node->name);
		} else {
			/* Register reserved memory for vdev buffer allocation */
			mem = rproc_of_resm_mem_entry_init(dev, index,
							   rmem->size,
							   rmem->base,
							   it.node->name);
		}

		if (!mem) {
			dev_err(dev, "unable to allocate memory entry %s", it.node->name);
			return -ENOMEM;
		}
		rproc_add_carveout(rproc, mem);
		index++;
	}
	return 0;
}


/* M0 is already started. Do Nothing
 */
static int bflb_rproc_start(struct rproc *rproc)
{
	struct device *dev = rproc->dev.parent;

	dev_dbg(dev, "bflb_rproc_start");

	return 0;
}

/* We don't want to stop M0, as it will crash. Do Nothing */
static int bflb_rproc_stop(struct rproc *rproc)
{
	struct device *dev = rproc->dev.parent;

	dev_dbg(dev, "bflb_rproc_stop");

	return 0;
}

/* kick the virtqueue to let M0 know there is a update to the vring */
static void bflb_rproc_send_kick(struct rproc *rproc, int vqid)
{
	struct device *dev = rproc->dev.parent;
	struct bflb_rproc *drproc = (struct bflb_rproc *)rproc->priv;
	struct bflb_mbox *mb = &drproc->mbox_tx;
	struct mbox_chan *chan = mb->chan;
	struct bflb_mbox_msg *msg;
	int ret;
	int i;
	static int count;

	/* just for debugging atm */
	count++;

	msg = kzalloc(sizeof(struct bflb_mbox_msg), GFP_KERNEL);
	if (!msg)
		return;

	/* we need a small delay before kicking the other side 
	 * (I assume to allow the ring to update/flush etc?)
	 * without this, we get lots of "empty ring" messages on the 
	 * other side
	 */
	mdelay(1);

	msg->param = vqid;
	msg->id = count;
	/* Kick the other CPU to let it know the vrings are updated */
	dev_dbg(dev, "%s %d Mailbox: %s %d", __func__, msg->id, mb->name, msg->param);
	/* we occasionally get a EOI timeout, so retry upto 3 times */
	for (i = 0; i < 3; i++) {
		ret = mbox_send_message(chan, msg);
		if (ret >= 0)
			goto done;
		dev_warn(dev, "%s %d Mailbox %s sending %d Failed: %d - Retrying %d", __func__, msg->id, mb->name, msg->param, ret, i);
	}
done:
	dev_dbg(dev, "%s %d Mailbox %s done %d: %d", __func__, msg->id, mb->name, msg->param, ret);
	kfree(msg);
}

static void bflb_rproc_recv_kick(struct work_struct *work)
{
	struct bflb_mbox *mb = container_of(work, struct bflb_mbox, vq_work);
	struct rproc *rproc = dev_get_drvdata(mb->client.dev);

	dev_dbg(rproc->dev.parent, "%s mailbox: %s: %d", __func__, mb->name, mb->vq_id);

	/* not a bad thing if there is no messages, probably
	 * means that the previous ring kick processed the message
	 */
	if (rproc_vq_interrupt(rproc, mb->vq_id) == IRQ_NONE)
		dev_dbg(&rproc->dev, "no message found in vq%d\n", mb->vq_id);
}

/* M0 signaled us there is a update on the vring, check it
 */
static void bflb_rproc_rx_mbox_callback(struct mbox_client *client, void *data)
{
	struct device *dev = client->dev;
	struct rproc *rproc = dev_get_drvdata(dev);
	struct bflb_rproc *drproc = (struct bflb_rproc *)rproc->priv;
	struct bflb_mbox *mb = &drproc->mbox_rx;
	struct bflb_mbox_msg *msg = data;

	mb->vq_id = msg->param;

	dev_dbg(dev, "%s mailbox %s: %d", __func__, mb->name, mb->vq_id);

	queue_work(drproc->workqueue, &mb->vq_work);
	mbox_chan_txdone(mb->chan, 0);
}

/* M0 is already running when we boot
 * so just attach to it.
 * we also register a mailbox to get kicks from M0 when vrings are updated
 */
static int bflb_rproc_attach(struct rproc *rproc)
{
	struct device *dev = &rproc->dev;
//	struct bflb_rproc *drproc = (struct bflb_rproc *)rproc->priv;

	dev_info(dev, "Attaching to %s", rproc->name);

	return 0;
}

/* Detach. Do Nothing? */
static int bflb_rproc_detach(struct rproc *rproc)
{
	struct device *dev = rproc->dev.parent;

	dev_info(dev, "Detaching from %s", rproc->name);
	return 0;
}

static const struct rproc_ops bflb_rproc_ops = {
	.start = bflb_rproc_start,
	.stop = bflb_rproc_stop,
	.attach = bflb_rproc_attach,
	.detach = bflb_rproc_detach,
	.kick = bflb_rproc_send_kick,
	.prepare = bflb_rproc_setupmem,
	.get_loaded_rsc_table = bflb_rproc_get_loaded_rsc_table,
};

static int bflb_rproc_setup_mbox(struct rproc *rproc)
{
	struct bflb_rproc *drproc = (struct bflb_rproc *)rproc->priv;

	struct bflb_mbox *tx_bflb_mbox = &drproc->mbox_tx;
	struct mbox_client *tx_mbox_cl = &tx_bflb_mbox->client;

	struct bflb_mbox *rx_bflb_mbox = &drproc->mbox_rx;
	struct mbox_client *rx_mbox_cl = &rx_bflb_mbox->client;

	struct device *dev = &rproc->dev;
	int ret = 0;

	dev_dbg(dev, "bflb_rpoc_setup_mbox");

	/* request the TX mailboxs */
	tx_mbox_cl->dev = dev->parent;
	tx_mbox_cl->tx_block = true;
	tx_mbox_cl->tx_tout = 200;
	strncpy(tx_bflb_mbox->name, "virtio-tx", sizeof(tx_bflb_mbox->name));
	tx_bflb_mbox->chan = mbox_request_channel_byname(tx_mbox_cl, "virtio-tx");
	if (IS_ERR(tx_bflb_mbox->chan)) {
		ret = -EBUSY;
		dev_err(dev, "mbox_request_channel tx failed: %ld\n",
			PTR_ERR(tx_bflb_mbox->chan));
		goto del_rx_mbox;
	}

	/* request the RX mailboxs */
	rx_mbox_cl->dev = dev->parent;
	rx_mbox_cl->rx_callback = bflb_rproc_rx_mbox_callback;
	rx_mbox_cl->tx_block = true;
	strncpy(rx_bflb_mbox->name, "virtio-rx", sizeof(rx_bflb_mbox->name));

	rx_bflb_mbox->chan = mbox_request_channel_byname(rx_mbox_cl, "virtio-rx");
	if (IS_ERR(rx_bflb_mbox->chan)) {
		ret = -EBUSY;
		dev_err(dev, "mbox_request_channel rx failed: %ld\n",
			PTR_ERR(rx_bflb_mbox->chan));
		goto del_tx_mbox;
	}
	INIT_WORK(&rx_bflb_mbox->vq_work, bflb_rproc_recv_kick);

	dev_dbg(dev, "bflb_rpoc_setup_mbox done");

	return ret;

del_tx_mbox:
	mbox_free_channel(tx_bflb_mbox->chan);
del_rx_mbox:
	mbox_free_channel(rx_bflb_mbox->chan);
	return ret;
}

static int bflb_rproc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct bflb_rproc *drproc;
	struct rproc *rproc;
	int ret;

	dev_dbg(dev, "bflb_rproc_probe");

	rproc = rproc_alloc(dev, "M0", &bflb_rproc_ops, NULL,
		sizeof(*drproc));
	if (!rproc) {
		ret = -ENOMEM;
		goto free_mem;
	}

	/* error recovery is not supported at present */
	rproc->recovery_disabled = true;

	/* M0 is running when linux boots */
	//atomic_inc(&rproc->power);
	rproc->state = RPROC_DETACHED;

	drproc = rproc->priv;
	drproc->rproc = rproc;
	rproc->has_iommu = false;
	rproc->sysfs_read_only = true;

	platform_set_drvdata(pdev, rproc);

	drproc->workqueue = create_workqueue(dev_name(dev));
	if (!drproc->workqueue) {
		dev_err(dev, "cannot create workqueue\n");
		ret = -ENOMEM;
		goto free_wkq;
	}

	ret = bflb_rproc_setup_mbox(rproc);
	if (ret) {
		dev_err(dev, "bflb_rpoc_setup_mbox failed: %d\n", ret);
		goto free_rproc;
	}

	ret = rproc_add(rproc);
	if (ret) {
		dev_err(dev, "rproc_add failed: %d\n", ret);
		goto free_rproc;
	}

	dev_info(dev, "Bouffalo Labs Remote Processor Control Driver Started");

	return 0;

free_rproc:
	rproc_free(rproc);
free_wkq:
	destroy_workqueue(drproc->workqueue);
free_mem:
	if (dev->of_node)
		of_reserved_mem_device_release(dev);
	return ret;
}

static int bflb_rproc_remove(struct platform_device *pdev)
{
	struct rproc *rproc = platform_get_drvdata(pdev);
	struct bflb_rproc *drproc = (struct bflb_rproc *)rproc->priv;
	struct bflb_mbox *tx_bflb_mbox = &drproc->mbox_tx;
	struct bflb_mbox *rx_bflb_mbox = &drproc->mbox_rx;
	struct device *dev = &pdev->dev;

	dev_info(dev, "Bouffalo Labs Remote Processor Control Driver Removed");


	mbox_free_channel(tx_bflb_mbox->chan);
	mbox_free_channel(rx_bflb_mbox->chan);
	destroy_workqueue(drproc->workqueue);

	rproc_del(rproc);
	rproc_free(rproc);
	if (dev->of_node)
		of_reserved_mem_device_release(dev);

	return 0;
}

static const struct of_device_id davinci_rproc_of_match[] __maybe_unused = {
	{ .compatible = "bflb,bflb-rproc", },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, davinci_rproc_of_match);

static struct platform_driver bflb_rproc_driver = {
	.probe = bflb_rproc_probe,
	.remove = bflb_rproc_remove,
	.driver = {
		.name = "bflb-rproc",
		.of_match_table = of_match_ptr(davinci_rproc_of_match),
	},
};

module_platform_driver(bflb_rproc_driver);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("justin@dynam.ac");
MODULE_DESCRIPTION("bflb Remote Processor control driver");
