// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2023, Allen Martin <armartin@gmail.com>
 */

#include <linux/bitfield.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/irqdomain.h>
#include <linux/mailbox_controller.h>
#include <linux/module.h>
#include <linux/platform_device.h>

#include <linux/bflb-mailbox.h>
#include <dt-bindings/mailbox/bflb-ipc.h>

/* IPC Register offsets */
#define IPC_REG_ISWR		0x00	/* Interrupt Set Write Register     */
#define IPC_REG_IRSRR		0x04	/* Interrupt raw status Register    */
#define IPC_REG_ICR		0x08	/* Interrupt Clear Register         */
#define IPC_REG_IUSR		0x0c	/* Interrupt Unmask Set Register    */
#define IPC_REG_IUCR		0x10	/* Interrupt Unmask Clear Register  */
#define IPC_REG_ILSLR		0x14	/* Interrupt Line Sel Low Register  */
#define IPC_REG_ILSHR		0x18	/* Interrupt Line Sel High Register */
#define IPC_REG_ISR		0x1c	/* Interrupt status Register        */

/**
 * struct bflb_ipc_chan_info - Per-mailbox-channel info
 * @cpu_id:	The cpu_id is a identifier of what CPU is being called
 * @service_id:	The service_id is a identifier of what service is being called on the remote CPU
 * @op_id:	The operation we want to execute on the remote CPU
 */
struct bflb_ipc_chan_info {
	u8  cpu_id;
	u16 service_id;
	u16 op_id;
};

/**
 * struct bflb_ipc - Holder for the mailbox driver
 * @dev:		Device associated with this instance
 * @base:		Base address of each IPC frame (LP, M0)
 * @irq_domain:		The irq_domain associated with this instance
 * @chans:		The mailbox channels array
 * @outchat:		The outbound (singal the other CPU) mailbox channel info
 * @inchan:		The inbound (receive the signal from the other CPU) mailbox channel info
 * @mbox:		The mailbox controller
 * @num_chans:		Number of @chans elements
 * @irq:		Summary irq
 */
struct bflb_ipc {
	struct device *dev;
	void __iomem *base[4];
	struct irq_domain *irq_domain;
	struct mbox_chan *chans;
	struct bflb_ipc_chan_info *bflbchan;
	struct mbox_controller mboxctlr;
	int num_chans;
	int irq;
};

static inline struct bflb_ipc *to_bflb_ipc(struct mbox_controller *mboxctlr)
{
	return container_of(mboxctlr, struct bflb_ipc, mboxctlr);
}

static inline u32 bflb_ipc_get_hwirq(u16 source, u16 device)
{
//	dev_dbg("%s: source: %u, device: %u\n", __func__, source, device);

	return device;
}


#if 0
static void bflb_ipc_dump_regs(struct bflb_ipc *ipc)
{
	int i;

	for (i = 0; i < 4; i++) {
		dev_dbg(ipc->dev, "base %px %d\n", ipc->base[i], i);
		dev_dbg(ipc->dev, "ISWR:  0x%08x\n", readl(ipc->base[i] + IPC_REG_ISWR));
		dev_dbg(ipc->dev, "IRSRR: 0x%08x\n", readl(ipc->base[i] + IPC_REG_IRSRR));
		dev_dbg(ipc->dev, "ICR:   0x%08x\n", readl(ipc->base[i] + IPC_REG_ICR));
		dev_dbg(ipc->dev, "IUSR:  0x%08x\n", readl(ipc->base[i] + IPC_REG_IUSR));
		dev_dbg(ipc->dev, "IUCR:  0x%08x\n", readl(ipc->base[i] + IPC_REG_IUCR));
		dev_dbg(ipc->dev, "ILSLR: 0x%08x\n", readl(ipc->base[i] + IPC_REG_ILSLR));
		dev_dbg(ipc->dev, "ILSHR: 0x%08x\n", readl(ipc->base[i] + IPC_REG_ILSHR));
		dev_dbg(ipc->dev, "ISR:   0x%08x\n", readl(ipc->base[i] + IPC_REG_ISR));
	}
}
#endif

struct mbox_chan *bflb_mbox_find_chan(struct bflb_ipc *ipc, struct bflb_ipc_chan_info *chaninfo)
{
	struct bflb_ipc_chan_info *mchan;
	struct mbox_controller *mboxctlr = &ipc->mboxctlr;
	struct mbox_chan *chan;
	struct device *dev;
	int chan_id;

	dev = ipc->dev;

	for (chan_id = 0; chan_id < mboxctlr->num_chans; chan_id++) {
		chan = &ipc->chans[chan_id];
		mchan = chan->con_priv;

		if (!mchan)
			break;
		else if (mchan->cpu_id == chaninfo->cpu_id &&
				mchan->service_id == chaninfo->service_id &&
					mchan->op_id == chaninfo->op_id)
			return chan;
	}
	dev_err(dev, "%s: No channel found for cpu_id %d service_id %d op_id %d",
		__func__, chaninfo->cpu_id, chaninfo->service_id, chaninfo->op_id);

	return ERR_PTR(-EINVAL);

}

/* called when we get a RX interrupt from another processor
 * this indicates there is a message waitinf for us in
 * IPC_REG_ILSHR and IPC_REG_ILSLR registers to process
 */
static void bflb_mbox_rx_irq_fn(int from_cpu, struct bflb_ipc *ipc)
{
	struct mbox_chan *chan;
	struct bflb_ipc_chan_info bflbchan;
	struct bflb_mbox_msg msg;
	u32 sig_op;

	/* update this when we support LP */
	sig_op = readl(ipc->base[1] + IPC_REG_ILSHR);
	msg.param = readl(ipc->base[1] + IPC_REG_ILSLR);

	WARN_ON(sig_op == 0);

	bflbchan.cpu_id = from_cpu;
	bflbchan.service_id = (sig_op >> 16) & 0xFFFF;
	bflbchan.op_id = sig_op & 0xFFFF;

	chan = bflb_mbox_find_chan(ipc, &bflbchan);
	if (IS_ERR(chan)) {
		dev_err(ipc->dev, "no channel for signal cpu_id: %d service: %d op: %d\r\n", bflbchan.cpu_id, bflbchan.service_id, bflbchan.op_id);
		return;
	}

	dev_dbg(ipc->dev, "Got MBOX Signal cpu: %d service %d op %d param %x\r\n", bflbchan.cpu_id, bflbchan.service_id, bflbchan.op_id, msg.param);

	mbox_chan_received_data(chan, &msg);
}

/* called when we get a interupt back on our TX IRQ.
 * This is a EOI interupt
 */
static void bflb_mbox_tx_irq_fn(u8 from_cpu, struct bflb_ipc *ipc)
{
	struct mbox_chan *chan;
	struct bflb_ipc_chan_info bflbchan;

	u32 sig_op = readl(ipc->base[2] + IPC_REG_ILSHR);
	u32 param = readl(ipc->base[2] + IPC_REG_ILSLR);

	bflbchan.cpu_id = from_cpu;
	bflbchan.service_id = (sig_op >> 16) & 0xFFFF;
	bflbchan.op_id = sig_op & 0xFFFF;

	chan = bflb_mbox_find_chan(ipc, &bflbchan);
	if (IS_ERR(chan)) {
		dev_err(ipc->dev, "no channel for EOI signal cpu_id: %d service: %d op: %d Param: %d $$$$$$$$$", bflbchan.cpu_id, bflbchan.service_id, bflbchan.op_id, param);
		return;
	}

	dev_dbg(ipc->dev, "Got MBOX EOI Signal cpu: %d service %d op %d Param: %d $$$$$$$$$$$$$$$$$$$$", bflbchan.cpu_id, bflbchan.service_id, bflbchan.op_id, param);

	/* clear the IPC_REG_ILSLR and IPC_REG_ILSHR */
	writel(0, ipc->base[2] + IPC_REG_ILSLR);
	writel(0, ipc->base[2] + IPC_REG_ILSHR);

	mbox_chan_txdone(chan, 0);
}

static irqreturn_t bflb_ipc_irq_fn(int irq, void *data)
{
	struct bflb_ipc *ipc = data;
	unsigned long stat;
	int pos;

	stat = readl(ipc->base[1] + IPC_REG_ISR);

	for_each_set_bit(pos, &stat, 32) {
		if (pos == BFLB_IPC_DEVICE_MBOX_RX)
			bflb_mbox_rx_irq_fn(BFLB_IPC_SOURCE_M0, ipc);
		else if (pos == BFLB_IPC_DEVICE_MBOX_TX)
			/* we use target here as its a EOI from a send */
			bflb_mbox_tx_irq_fn(BFLB_IPC_TARGET_M0, ipc);
		else
			generic_handle_domain_irq(ipc->irq_domain, pos);
	}
	writel(stat, ipc->base[1] + IPC_REG_ICR);

	/* Signal EOI to the other processes except when we recieve a EOI ourselves */
	if (stat != (1 << BFLB_IPC_DEVICE_MBOX_TX))
		writel(stat, ipc->base[2] + IPC_REG_ISWR);

	return IRQ_HANDLED;
}

static void bflb_ipc_mask_irq(struct irq_data *irqd)
{
	struct bflb_ipc *ipc = irq_data_get_irq_chip_data(irqd);
	irq_hw_number_t hwirq = irqd_to_hwirq(irqd);

	writel(BIT(hwirq), ipc->base[1] + IPC_REG_IUCR);
}

static void bflb_ipc_unmask_irq(struct irq_data *irqd)
{
	struct bflb_ipc *ipc = irq_data_get_irq_chip_data(irqd);
	irq_hw_number_t hwirq = irqd_to_hwirq(irqd);

	writel(BIT(hwirq), ipc->base[1] + IPC_REG_IUSR);
}

static struct irq_chip bflb_ipc_irq_chip = {
	.name = "BFLB MBOXIC",
	.irq_mask = bflb_ipc_mask_irq,
	.irq_unmask = bflb_ipc_unmask_irq,
	.flags = IRQCHIP_SKIP_SET_WAKE,
};

static int bflb_ipc_domain_map(struct irq_domain *d, unsigned int irq,
			       irq_hw_number_t hw)
{
	struct bflb_ipc *ipc = d->host_data;

	irq_set_chip_and_handler(irq, &bflb_ipc_irq_chip, handle_level_irq);
	irq_set_chip_data(irq, ipc);
	irq_set_noprobe(irq);

	return 0;
}

static int bflb_ipc_domain_xlate(struct irq_domain *d,
				  struct device_node *node, const u32 *intspec,
				  unsigned int intsize,
				  unsigned long *out_hwirq,
				  unsigned int *out_type)
{
	if (intsize != 3)
		return -EINVAL;

	*out_hwirq = bflb_ipc_get_hwirq(intspec[0], intspec[1]);
	*out_type = intspec[2] & IRQ_TYPE_SENSE_MASK;

	return 0;
}

static const struct irq_domain_ops bflb_ipc_irq_ops = {
	.map = bflb_ipc_domain_map,
	.xlate = bflb_ipc_domain_xlate,
};

#if 0
/* Shouldn't actually be fail as we clear the High/Low registers in a EOI
 * but this protects if we screw up our mailbox handling
 */
static bool bflb_ipc_mbox_can_send(struct mbox_chan *chan)
{
	struct bflb_ipc *ipc = to_bflb_ipc(chan->mbox);

	/* check the low register first as we clear that last in our EOI, so this
	 * should protected to a limited extent
	 */
	u32 mbox_low = readl(ipc->base[2] + IPC_REG_ILSLR);
	u32 mbox_high = readl(ipc->base[2] + IPC_REG_ILSHR);


	if (mbox_low | mbox_high)
		dev_warn_ratelimited(ipc->dev, "%s: low: 0x%08x high: 0x%08x\r\n", __func__, mbox_low, mbox_high);

	writel(0, ipc->base[2] + IPC_REG_ILSLR);
	writel(0, ipc->base[2] + IPC_REG_ILSHR);

	return !(mbox_low | mbox_high);
}
#endif

static int bflb_ipc_mbox_send_data(struct mbox_chan *chan, void *data)
{
	struct bflb_ipc *ipc = to_bflb_ipc(chan->mbox);
	struct bflb_ipc_chan_info *mchan = chan->con_priv;
	struct bflb_mbox_msg *msg = data;
	u32 tmpVal = (mchan->service_id << 16) | (mchan->op_id & 0xFFFF);

#if 0
	if (!bflb_ipc_mbox_can_send(chan))
		return -EBUSY;
#endif

	dev_dbg(ipc->dev, "%s %d: cpu: %d singal: %d op: %d (0x%x) param: %d", __func__, msg->id, mchan->cpu_id, mchan->service_id, mchan->op_id, tmpVal, msg->param);

	// /* write our signal number to high register */
	writel(tmpVal, ipc->base[2] + IPC_REG_ILSHR);
	// /* write our data to low register */
	writel(msg->param, ipc->base[2] + IPC_REG_ILSLR);

	/* and now kick the remote processor */
	writel((1 << BFLB_IPC_DEVICE_MBOX_TX), ipc->base[2] + IPC_REG_ISWR);
	dev_dbg(ipc->dev, "%s %d: done param: %d", __func__, msg->id, msg->param);
	return 0;
}

static void bflb_ipc_mbox_shutdown(struct mbox_chan *chan)
{
	struct bflb_ipc *ipc = to_bflb_ipc(chan->mbox);

	dev_dbg(ipc->dev, "%s\n", __func__);
	chan->con_priv = NULL;
}

static struct mbox_chan *bflb_ipc_mbox_xlate(struct mbox_controller *mboxctlr,
					const struct of_phandle_args *ph)
{
	struct bflb_ipc *ipc = to_bflb_ipc(mboxctlr);
	struct bflb_ipc_chan_info *mchan;
	struct mbox_chan *chan;
	struct device *dev = ipc->dev;
	int chan_id;


	dev_dbg(dev, "%s\n", __func__);

	if (ph->args_count != 3) {
		dev_err(dev, "invalid number of arguments");
		return ERR_PTR(-EINVAL);
	}

	for (chan_id = 0; chan_id < mboxctlr->num_chans; chan_id++) {
		chan = &ipc->chans[chan_id];
		mchan = chan->con_priv;

		if (!mchan)
			break;
		else if (mchan->cpu_id == ph->args[0] &&
				mchan->service_id == ph->args[1] &&
					mchan->op_id == ph->args[2]) {
						dev_err(dev, "channel already in use %d %d %d", ph->args[0], ph->args[1], ph->args[2]);
						return ERR_PTR(-EBUSY);
					}
	}

	if (chan_id >= mboxctlr->num_chans) {
		dev_err(dev, "no free channels");
		return ERR_PTR(-EBUSY);
	}

	mchan = devm_kzalloc(dev, sizeof(*mchan), GFP_KERNEL);
	if (!mchan)
		return ERR_PTR(-ENOMEM);

	mchan->cpu_id = ph->args[0];
	mchan->service_id = ph->args[1];
	mchan->op_id = ph->args[2];
	chan->con_priv = mchan;

	dev_dbg(dev, "%s mbox %s: %d cpu: %d service: %d op: %d", __func__, ph->np->full_name, chan_id, mchan->cpu_id, mchan->service_id, mchan->op_id);

	return chan;
}


static const struct mbox_chan_ops ipc_mbox_chan_ops = {
	.send_data = bflb_ipc_mbox_send_data,
	.shutdown = bflb_ipc_mbox_shutdown,
//	.last_tx_done = bflb_ipc_mbox_can_send,
};

static int bflb_ipc_setup_mbox(struct bflb_ipc *ipc,
				struct device_node *controller_dn)
{
	struct of_phandle_args curr_ph;
	struct device_node *client_dn;
	struct mbox_controller *mboxctlr;
	struct device *dev = ipc->dev;
	int i, j, ret;

	/*
	 * Find out the number of clients interested in this mailbox
	 * and create channels accordingly.
	 */
	ipc->num_chans = 0;
	for_each_node_with_property(client_dn, "mboxes") {
		if (!of_device_is_available(client_dn))
			continue;
		i = of_count_phandle_with_args(client_dn,
						"mboxes", "#mbox-cells");
		for (j = 0; j < i; j++) {
			ret = of_parse_phandle_with_args(client_dn, "mboxes",
						"#mbox-cells", j, &curr_ph);
			of_node_put(curr_ph.np);
			if (!ret && curr_ph.np == controller_dn) {
				ipc->num_chans++;
				//break;
			}
		}
	}
	dev_dbg(dev, "%s: num_chans: %d", __func__, ipc->num_chans);
	/* If no clients are found, skip registering as a mbox controller */
	if (!ipc->num_chans)
		return 0;

	ipc->chans = devm_kcalloc(dev, ipc->num_chans,
					sizeof(struct mbox_chan), GFP_KERNEL);
	if (!ipc->chans)
		return -ENOMEM;

	mboxctlr = &ipc->mboxctlr;
	mboxctlr->dev = dev;
	mboxctlr->num_chans = ipc->num_chans;
	mboxctlr->chans = ipc->chans;
	mboxctlr->ops = &ipc_mbox_chan_ops;
	mboxctlr->of_xlate = bflb_ipc_mbox_xlate;
	mboxctlr->txdone_irq = true;
//	mboxctlr->txdone_poll = false;



	/* clear the IPC_REG_ILSLR and IPC_REG_ILSHR */
	writel(0, ipc->base[2] + IPC_REG_ILSLR);
	writel(0, ipc->base[2] + IPC_REG_ILSHR);

	/* unmask our interupt */
	writel(BIT(BFLB_IPC_DEVICE_MBOX_TX), ipc->base[1] + IPC_REG_IUSR);
	writel(BIT(BFLB_IPC_DEVICE_MBOX_RX), ipc->base[1] + IPC_REG_IUSR);

	return devm_mbox_controller_register(dev, mboxctlr);
}

static int bflb_ipc_pm_resume(struct device *dev)
{
	return 0;
}

static int bflb_ipc_probe(struct platform_device *pdev)
{
	struct bflb_ipc *ipc;
	static int id;
	int i;
	char *name;
	int ret;

	ipc = devm_kzalloc(&pdev->dev, sizeof(*ipc), GFP_KERNEL);
	if (!ipc)
		return -ENOMEM;

	ipc->dev = &pdev->dev;

	for (i = 0; i < 4; i++) {
		ipc->base[i] = devm_platform_ioremap_resource(pdev, i);
		if (IS_ERR(ipc->base[i]))
			return PTR_ERR(ipc->base[i]);
	}

	ipc->irq = platform_get_irq(pdev, 0);
	if (ipc->irq < 0)
		return ipc->irq;

	name = devm_kasprintf(&pdev->dev, GFP_KERNEL, "mboxic%d", id++);
	if (!name)
		return -ENOMEM;

	ipc->irq_domain = irq_domain_add_tree(pdev->dev.of_node,
					       &bflb_ipc_irq_ops, ipc);
	if (!ipc->irq_domain)
		return -ENOMEM;

	ret = bflb_ipc_setup_mbox(ipc, pdev->dev.of_node);
	if (ret)
		goto err_mbox;

	ret = devm_request_irq(&pdev->dev, ipc->irq, bflb_ipc_irq_fn,
			       IRQF_TRIGGER_HIGH | IRQF_NO_SUSPEND |
			       IRQF_NO_THREAD, name, ipc);
	if (ret < 0) {
		dev_err(&pdev->dev, "Failed to register the irq: %d\n", ret);
		goto err_req_irq;
	}

	platform_set_drvdata(pdev, ipc);

	dev_info(&pdev->dev, "Bouffalo Lab IPC mailbox interrupt controller");
	return 0;

err_req_irq:
	if (ipc->num_chans)
		mbox_controller_unregister(&ipc->mboxctlr);
err_mbox:
	irq_domain_remove(ipc->irq_domain);

	return ret;
}

static int bflb_ipc_remove(struct platform_device *pdev)
{
	struct bflb_ipc *ipc = platform_get_drvdata(pdev);

	disable_irq_wake(ipc->irq);
	irq_domain_remove(ipc->irq_domain);

	return 0;
}

static const struct of_device_id bflb_ipc_of_match[] = {
	{ .compatible = "bflb,bl808-ipc"},
	{}
};
MODULE_DEVICE_TABLE(of, bflb_ipc_of_match);

static const struct dev_pm_ops bflb_ipc_dev_pm_ops = {
	NOIRQ_SYSTEM_SLEEP_PM_OPS(NULL, bflb_ipc_pm_resume)
};

static struct platform_driver bflb_ipc_driver = {
	.probe = bflb_ipc_probe,
	.remove = bflb_ipc_remove,
	.driver = {
		.name = "bflb-ipc",
		.of_match_table = bflb_ipc_of_match,
		.suppress_bind_attrs = true,
		.pm = pm_sleep_ptr(&bflb_ipc_dev_pm_ops),
	},
};

static int __init bflb_ipc_init(void)
{
	return platform_driver_register(&bflb_ipc_driver);
}
arch_initcall(bflb_ipc_init);

MODULE_AUTHOR("Allen Martin <armartin@gmail.com>");
MODULE_DESCRIPTION("Bouffalo Lab IPC driver");
MODULE_LICENSE("GPL v2");
