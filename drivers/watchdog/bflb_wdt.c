// SPDX-License-Identifier: GPL-2.0

#include <linux/err.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/utsname.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/watchdog.h>
#include <linux/bits.h>

// Settings here get us the slowest possible clock speed on the hardware
// which is the closest we can get to the linux watchdog's resolution of
// 1 second.
#define BFLB_INT_TICKS_PER_SEC 1024
#define BFLB_TICK_CLKDIV 256
#define BFLB_TICKS_PER_SEC ( BFLB_INT_TICKS_PER_SEC / BFLB_TICK_CLKDIV )
#define BFLB_MAX_SECS ( 65535 / BFLB_TICKS_PER_SEC )

#define BFLB_DEFAULT_TIMEOUT 60

#define BFLB_REG_BASE 0x2000A500

#define BFLB_REG_WFAR 0x9C
#define BFLB_WFAR_MAGIC 0xBABA

#define BFLB_REG_WSAR 0xA0
#define BFLB_WSAR_MAGIC 0xEB10

#define BFLB_REG_WVR 0x6C
#define BFLB_REG_WCR 0x98
#define BFLB_WCR_RESET_COUNTER BIT(0)

#define BFLB_REG_WMER 0x64
#define BFLB_WMER_WATCHDOG_ENABLE BIT(0)
#define BFLB_WMER_RESET_SOURCE BIT(1)

#define BFLB_REG_TCCR 0x00
#define BFLB_SHIFT_CS_WDT 8
#define BFLB_MASK_CS_WDT GENMASK(BFLB_SHIFT_CS_WDT+3,BFLB_SHIFT_CS_WDT)
#define BFLB_TCCR_CS_FCLK (0x0 << BFLB_SHIFT_CS_WDT)
#define BFLB_TCCR_CS_32K (0x1 << BFLB_SHIFT_CS_WDT)
#define BFLB_TCCR_CS_1K (0x2 << BFLB_SHIFT_CS_WDT)
#define BFLB_TCCR_CS_32M (0x3 << BFLB_SHIFT_CS_WDT)
#define BFLB_TCCR_CS_GPIO (0x4 << BFLB_SHIFT_CS_WDT)

#define BFLB_REG_TCDR 0xbc
#define BFLB_SHIFT_WDT_CLKDIV 24
#define BFLB_MASK_TCDR GENMASK(BFLB_SHIFT_WDT_CLKDIV+7,BFLB_SHIFT_WDT_CLKDIV)

// CLKDIV is an eight bit counter, but the counter is
// zero indexed, so subtract 1 before setting
#define BFLB_TCDR_CLKDIV  ((BFLB_TICK_CLKDIV - 1) << BFLB_SHIFT_WDT_CLKDIV)

#define BFLB_REG_WMR 0x68
#define BFLB_MASK_WMR GENMASK(15,0)

struct bflb_watchdog_device {
	struct watchdog_device wdd;
	struct device *dev;
	void __iomem *regs;
};

static inline
struct bflb_watchdog_device *to_bflb_wdd(struct watchdog_device *wdd)
{
	return container_of(wdd, struct bflb_watchdog_device, wdd);
}

// Access key registers must be written before write
// operations presumably to prevent accidentally enabling
// the watchdog and killing the machine.
static inline int bflb_unlock_watchdog(struct bflb_watchdog_device *bflb_wdd)
{
	writew(BFLB_WFAR_MAGIC, bflb_wdd->regs + BFLB_REG_WFAR);
	writew(BFLB_WSAR_MAGIC, bflb_wdd->regs + BFLB_REG_WSAR);

	return 0;
}

static int bflb_wdt_ping(struct watchdog_device *wdd)
{
	uint32_t reg_val;
	struct bflb_watchdog_device *bflb_wdd = to_bflb_wdd(wdd);

	dev_dbg(wdd->parent, "bflb_wdt_ping");
	bflb_unlock_watchdog(bflb_wdd);
	reg_val = readl(bflb_wdd->regs + BFLB_REG_WCR);
	reg_val |= BFLB_WCR_RESET_COUNTER;
	writel(reg_val, bflb_wdd->regs + BFLB_REG_WCR);

	return 0;
};

static inline void bflb_wdt_update_timeout_reg(struct watchdog_device *wdd)
{
	unsigned int timeout_ticks;
	struct bflb_watchdog_device *bflb_wdd = to_bflb_wdd(wdd);

	bflb_unlock_watchdog(bflb_wdd);
	timeout_ticks = wdd->timeout * BFLB_TICKS_PER_SEC;
	writew(timeout_ticks, bflb_wdd->regs + BFLB_REG_WMR);
}

static int bflb_wdt_set_timeout(struct watchdog_device *wdd,
				unsigned int timeout)
{
	if (timeout >= wdd->max_timeout) {
		dev_warn(wdd->parent,
			"timeout %i > max_timeout %i, using max_timeout...",
			timeout, wdd->max_timeout);
		timeout = wdd->max_timeout;
	}

	wdd->timeout = timeout;

	bflb_wdt_update_timeout_reg(wdd);

	dev_dbg(wdd->parent, "bflb_wdt_set_timeout (s=%i tps=%i)",
		timeout, BFLB_TICKS_PER_SEC);

	return 0;
}

static int bflb_wdt_start(struct watchdog_device *wdd)
{
	uint32_t reg_val;
	struct bflb_watchdog_device *bflb_wdd = to_bflb_wdd(wdd);


	// And enable the watchdog
	bflb_unlock_watchdog(bflb_wdd);
	reg_val = readl(bflb_wdd->regs + BFLB_REG_WMER);
	reg_val |= BFLB_WMER_WATCHDOG_ENABLE;
	writel(reg_val, bflb_wdd->regs + BFLB_REG_WMER);

	dev_info(wdd->parent, "bflb_wdt_start started...");

	return 0;
}

static int bflb_wdt_stop(struct watchdog_device *wdd)
{
	uint32_t reg_val;
	struct bflb_watchdog_device *bflb_wdd = to_bflb_wdd(wdd);

	// disable
	bflb_unlock_watchdog(bflb_wdd);
	reg_val = readl(bflb_wdd->regs + BFLB_REG_WMER);
	reg_val &= ~BFLB_WMER_WATCHDOG_ENABLE;
	writel(reg_val, bflb_wdd->regs + BFLB_REG_WMER);

	dev_info(wdd->parent, "bflb_wdt_stopped...");

	return 0;
};


static unsigned int bflb_wdt_timeleft(struct watchdog_device *wdd)
{
	unsigned int used_seconds;
	unsigned int remaining_seconds;
	unsigned int ticks;
	struct bflb_watchdog_device *bflb_wdd = to_bflb_wdd(wdd);

	ticks = readw(bflb_wdd->regs + BFLB_REG_WVR);

	used_seconds = ticks / BFLB_TICKS_PER_SEC;
	remaining_seconds = wdd->max_timeout - used_seconds;
	dev_dbg(wdd->parent, "bflb_wdt_time left %i (elapsed tick %i, sec %i)",
		remaining_seconds, ticks, used_seconds);

	return remaining_seconds;
};

static const struct watchdog_info bflb_wdt_info = {
	.identity = "bflb_wdt",
	.options = WDIOF_SETTIMEOUT |
	WDIOF_KEEPALIVEPING |
	WDIOF_MAGICCLOSE,
};

static const struct watchdog_ops bflb_wdt_ops = {
	.start = bflb_wdt_start,
	.stop = bflb_wdt_stop,
	.ping = bflb_wdt_ping,
	.set_timeout = bflb_wdt_set_timeout,
	.get_timeleft = bflb_wdt_timeleft,
};


static int __init bflb_wdt_probe(struct platform_device *pdev)
{
	struct bflb_watchdog_device *bflb_wdd;
	struct watchdog_device *wdd;
	int err;
	uint32_t reg_val;

	dev_dbg(&pdev->dev, "bflb_wdt_probe started");

	bflb_wdd = devm_kzalloc(&pdev->dev, sizeof(*bflb_wdd), GFP_KERNEL);
	if (!bflb_wdd) return -ENOMEM;

	bflb_wdd->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(bflb_wdd->regs))
		return PTR_ERR(bflb_wdd->regs);

	wdd = &bflb_wdd->wdd;

	wdd->info = &bflb_wdt_info;
	wdd->ops = &bflb_wdt_ops;

	wdd->timeout = BFLB_DEFAULT_TIMEOUT;
	wdd->max_timeout = BFLB_MAX_SECS;
	wdd->min_timeout = 1;
	wdd->parent = &pdev->dev;

	watchdog_stop_on_reboot(wdd);
	watchdog_stop_on_unregister(wdd);
	watchdog_set_nowayout(wdd, WATCHDOG_NOWAYOUT);
	watchdog_init_timeout(wdd, BFLB_DEFAULT_TIMEOUT, &pdev->dev);

	// Setup registers

	// Set to reboot on watchdog, disable until we start
	bflb_unlock_watchdog(bflb_wdd);
	reg_val = readl(bflb_wdd->regs + BFLB_REG_WMER);
	reg_val &= ~BFLB_WMER_WATCHDOG_ENABLE;
	reg_val |= BFLB_WMER_RESET_SOURCE;
	writel(reg_val, bflb_wdd->regs + BFLB_REG_WMER);

	// Set to 1K per second clock
	reg_val = readl(bflb_wdd->regs + BFLB_REG_TCCR);
	reg_val &= ~BFLB_MASK_CS_WDT;
	reg_val |= BFLB_TCCR_CS_1K;
	writel(reg_val, bflb_wdd->regs + BFLB_REG_TCCR);

	reg_val = readl(bflb_wdd->regs + BFLB_REG_TCDR);
	reg_val &= ~BFLB_MASK_TCDR;
	reg_val |= BFLB_TCDR_CLKDIV;
	writel(reg_val, bflb_wdd->regs + BFLB_REG_TCDR);

	// Set last valid timeout value
	bflb_wdt_update_timeout_reg(wdd);

	err = devm_watchdog_register_device(&pdev->dev, wdd);
	if (err) return err;

	platform_set_drvdata(pdev, bflb_wdd);

	dev_info(&pdev->dev, "bflb_wdt_probe completed...");

	return 0;
}

static int __exit bflb_wdt_remove(struct platform_device *pdev)
{
	dev_info(&pdev->dev, "bflb_wdt_remove removed...");

	return 0;
}

static const struct of_device_id bflb_wdt_match[] = {
	{
		.compatible = "bflb,bl808-wdt",
	},
	{},
};

MODULE_DEVICE_TABLE(of, bflb_wdt_match);

static struct platform_driver bflb_wdt_driver = {
	.probe = bflb_wdt_probe,
	.remove = bflb_wdt_remove,
	.driver = {
		.name = "bflb_wdt",
		.owner = THIS_MODULE,
		.of_match_table = bflb_wdt_match,
		.suppress_bind_attrs = true,
  },
};


module_platform_driver(bflb_wdt_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("BL808 Watchdog support");
MODULE_AUTHOR("Grant Olson <kgo@grant-olson.net");

