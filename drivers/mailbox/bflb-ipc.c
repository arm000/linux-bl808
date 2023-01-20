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
 * @client_id:	The client-id to which the interrupt has to be triggered
 * @signal_id:	The signal-id to which the interrupt has to be triggered
 */
struct bflb_ipc_chan_info {
	u16 client_id;
	u16 signal_id;
};

/**
 * struct bflb_ipc - Holder for the mailbox driver
 * @dev:		Device associated with this instance
 * @base:		Base address of each IPC frame (LP, M0)
 * @irq_domain:		The irq_domain associated with this instance
 * @chans:		The mailbox channels array
 * @mchan:		The per-mailbox channel info array
 * @mbox:		The mailbox controller
 * @num_chans:		Number of @chans elements
 * @irq:		Summary irq
 */
struct bflb_ipc {
	struct device *dev;
	void __iomem *base[4];
	struct irq_domain *irq_domain;
	struct mbox_chan *chans;
	struct bflb_ipc_chan_info *mchan;
	struct mbox_controller mbox;
	int num_chans;
	int irq;
};

static inline struct bflb_ipc *to_bflb_ipc(struct mbox_controller *mbox)
{
	return container_of(mbox, struct bflb_ipc, mbox);
}

static inline u32 bflb_ipc_get_hwirq(u16 source, u16 device)
{
	pr_debug("%s: source: %u, device: %u\n", __func__, source, device);

	return device;
}

#if 0
static void bflb_ipc_dump_regs(struct bflb_ipc *ipc)
{
	int i;
	for (i=0; i<4; i++) {
		dev_dbg(ipc->dev, "base %px\n", ipc->base[i]);
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

static irqreturn_t bflb_ipc_irq_fn(int irq, void *data)
{
	struct bflb_ipc *ipc = data;
	unsigned long stat;
	int pos;

	stat = readl(ipc->base[1] + IPC_REG_ISR);
	for_each_set_bit(pos, &stat, 32)
		generic_handle_domain_irq(ipc->irq_domain, pos);
	writel(stat, ipc->base[1] + IPC_REG_ICR);

	/* EOI the irqs */
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

static int bflb_ipc_mbox_send_data(struct mbox_chan *chan, void *data)
{
	struct bflb_ipc *ipc = to_bflb_ipc(chan->mbox);
	struct bflb_ipc_chan_info *mchan = chan->con_priv;
	u32 hwirq;

	hwirq = bflb_ipc_get_hwirq(mchan->client_id, mchan->signal_id);

	dev_dbg(ipc->dev, "%s: hwirq: %u\n", __func__, hwirq);

//	writel(hwirq, ipc->base + IPC_REG_SEND_ID);

	return 0;
}

static void bflb_ipc_mbox_shutdown(struct mbox_chan *chan)
{
	pr_debug("%s\n", __func__);
	chan->con_priv = NULL;
}

static struct mbox_chan *bflb_ipc_mbox_xlate(struct mbox_controller *mbox,
					const struct of_phandle_args *ph)
{
	struct bflb_ipc *ipc = to_bflb_ipc(mbox);
	struct bflb_ipc_chan_info *mchan;
	struct mbox_chan *chan;
	struct device *dev;
	int chan_id;

	dev = ipc->dev;

	dev_dbg(dev, "%s\n", __func__);

	if (ph->args_count != 2)
		return ERR_PTR(-EINVAL);

	for (chan_id = 0; chan_id < mbox->num_chans; chan_id++) {
		chan = &ipc->chans[chan_id];
		mchan = chan->con_priv;

		if (!mchan)
			break;
		else if (mchan->client_id == ph->args[0] &&
				mchan->signal_id == ph->args[1])
			return ERR_PTR(-EBUSY);
	}

	if (chan_id >= mbox->num_chans)
		return ERR_PTR(-EBUSY);

	mchan = devm_kzalloc(dev, sizeof(*mchan), GFP_KERNEL);
	if (!mchan)
		return ERR_PTR(-ENOMEM);

	mchan->client_id = ph->args[0];
	mchan->signal_id = ph->args[1];
	chan->con_priv = mchan;

	return chan;
}

static const struct mbox_chan_ops ipc_mbox_chan_ops = {
	.send_data = bflb_ipc_mbox_send_data,
	.shutdown = bflb_ipc_mbox_shutdown,
};

static int bflb_ipc_setup_mbox(struct bflb_ipc *ipc,
				struct device_node *controller_dn)
{
	struct of_phandle_args curr_ph;
	struct device_node *client_dn;
	struct mbox_controller *mbox;
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
				break;
			}
		}
	}

	/* If no clients are found, skip registering as a mbox controller */
	if (!ipc->num_chans)
		return 0;

	ipc->chans = devm_kcalloc(dev, ipc->num_chans,
					sizeof(struct mbox_chan), GFP_KERNEL);
	if (!ipc->chans)
		return -ENOMEM;

	mbox = &ipc->mbox;
	mbox->dev = dev;
	mbox->num_chans = ipc->num_chans;
	mbox->chans = ipc->chans;
	mbox->ops = &ipc_mbox_chan_ops;
	mbox->of_xlate = bflb_ipc_mbox_xlate;
	mbox->txdone_irq = false;
	mbox->txdone_poll = false;

	return devm_mbox_controller_register(dev, mbox);
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

	for (i=0; i<4; i++) {
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
		mbox_controller_unregister(&ipc->mbox);
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
