// SPDX-License-Identifier: GPL-2.0+
/*
 * Based on bflb_uart.c, by Bouffalolab team
 *
 * Copyright (C) 2022 Jisheng Zhang <jszhang@kernel.org>
 */

#include <linux/clk.h>
#include <linux/console.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/serial.h>
#include <linux/serial_core.h>
#include <linux/tty.h>
#include <linux/tty_flip.h>

#define UART_UTX_CONFIG			0x00
#define  UART_CR_UTX_EN			BIT(0)
#define  UART_CR_UTX_CTS_EN		BIT(1)
#define  UART_CR_UTX_FRM_EN		BIT(2)
#define  UART_CR_UTX_PRT_EN		BIT(4)
#define  UART_CR_UTX_PRT_SEL		BIT(5)
#define  UART_CR_UTX_BIT_CNT_D_SFT	8
#define  UART_CR_UTX_BIT_CNT_D_MSK	GENMASK(10, 8)
#define  UART_CR_UTX_BIT_CNT_P_SFT	11
#define  UART_CR_UTX_BIT_CNT_P_MSK	GENMASK(12, 11)
#define UART_URX_CONFIG			0x04
#define  UART_CR_URX_EN			BIT(0)
#define  UART_CR_URX_PRT_EN		BIT(4)
#define  UART_CR_URX_PRT_SEL		BIT(5)
#define  UART_CR_URX_BIT_CNT_D_SFT	8
#define  UART_CR_URX_BIT_CNT_D_MSK	GENMASK(10, 8)
#define UART_BIT_PRD			0x08
#define  UART_CR_UTX_BIT_PRD		GENMASK(15, 0)
#define  UART_CR_URX_BIT_PRD		GENMASK(31, 16)
#define UART_DATA_CONFIG		0x0c
#define  UART_CR_UART_BIT_INV		BIT(0)
#define UART_URX_RTO_TIMER		0x18
#define  UART_CR_URX_RTO_VALUE_MSK	GENMASK(7, 0)
#define UART_SW_MODE			0x1c
#define UART_INT_STS			(0x20)
#define  UART_UTX_END_INT		BIT(0)
#define  UART_URX_END_INT		BIT(1)
#define  UART_UTX_FIFO_INT		BIT(2)
#define  UART_URX_FIFO_INT		BIT(3)
#define  UART_URX_RTO_INT		BIT(4)
#define  UART_URX_PCE_INT		BIT(5)
#define  UART_UTX_FER_INT		BIT(6)
#define  UART_URX_FER_INT		BIT(7)
#define  UART_URX_LSE_INT		BIT(8)
#define UART_INT_MASK			0x24
#define UART_INT_CLEAR			0x28
#define UART_INT_EN			0x2c
#define UART_STATUS			0x30
#define  UART_STS_UTX_BUS_BUSY		BIT(0)
#define UART_FIFO_CONFIG_0		(0x80)
#define  UART_DMA_TX_EN			BIT(0)
#define  UART_DMA_RX_EN			BIT(1)
#define  UART_TX_FIFO_CLR		BIT(2)
#define  UART_RX_FIFO_CLR		BIT(3)
#define  UART_TX_FIFO_OVERFLOW		BIT(4)
#define  UART_TX_FIFO_UNDERFLOW		BIT(5)
#define  UART_RX_FIFO_OVERFLOW		BIT(6)
#define  UART_RX_FIFO_UNDERFLOW		BIT(7)
#define UART_FIFO_CONFIG_1		(0x84)
#define  UART_TX_FIFO_CNT_SFT		0
#define  UART_TX_FIFO_CNT_MSK		GENMASK(5, 0)
#define  UART_RX_FIFO_CNT_MSK		GENMASK(13, 8)
#define  UART_TX_FIFO_TH_SFT		16
#define  UART_TX_FIFO_TH_MSK		GENMASK(20, 16)
#define  UART_RX_FIFO_TH_SFT		24
#define  UART_RX_FIFO_TH_MSK		GENMASK(28, 24)
#define UART_FIFO_WDATA			0x88
#define UART_FIFO_RDATA			0x8c
#define  UART_FIFO_RDATA_MSK		GENMASK(7, 0)

#define BFLB_UART_MAXPORTS 		8
#define BFLB_UART_BAUD			2000000
#define BFLB_UART_RX_FIFO_TH		7
#define BFLB_UART_TX_FIFO_DEPTH		32

struct bflb_uart_port {
	struct uart_port port;
	struct clk *clk;
};

#define to_bflb_uart_port(p) (container_of((p), \
			      struct bflb_uart_port, \
			      port))

static struct bflb_uart_port *bflb_uart_ports[BFLB_UART_MAXPORTS];

static inline u32 rdl(struct uart_port *port, u32 reg)
{
	return readl_relaxed(port->membase + reg);
}

static inline void wrl(struct uart_port *port, u32 reg, u32 value)
{
	writel_relaxed(value, port->membase + reg);
}

static inline void wrb(struct uart_port *port, u32 reg, u8 value)
{
	writeb_relaxed(value, port->membase + reg);
}

static unsigned int bflb_uart_tx_empty(struct uart_port *port)
{
	return (rdl(port, UART_FIFO_CONFIG_1) & UART_TX_FIFO_CNT_MSK) ? TIOCSER_TEMT : 0;
}

static unsigned int bflb_uart_get_mctrl(struct uart_port *port)
{
	return TIOCM_CAR | TIOCM_DSR | TIOCM_CTS;
}

static void bflb_uart_set_mctrl(struct uart_port *port, unsigned int sigs)
{
}

static void bflb_uart_start_tx(struct uart_port *port)
{
	u32 val;

	val = rdl(port, UART_UTX_CONFIG);
	val |= UART_CR_UTX_EN;
	wrl(port, UART_UTX_CONFIG, val);

	val = rdl(port, UART_INT_MASK);
	val &= ~UART_UTX_END_INT;
	wrl(port, UART_INT_MASK, val);

	val = rdl(port, UART_FIFO_CONFIG_1);
	val &= ~UART_TX_FIFO_TH_MSK;
	val |= 15 << UART_TX_FIFO_TH_SFT;
	wrl(port, UART_FIFO_CONFIG_1, val);

	val = rdl(port, UART_INT_MASK);
	val &= ~UART_UTX_FIFO_INT;
	wrl(port, UART_INT_MASK, val);
}

static void bflb_uart_stop_tx(struct uart_port *port)
{
	u32 val;

	val = rdl(port, UART_INT_MASK);
	val |= UART_UTX_END_INT | UART_UTX_FIFO_INT;
	wrl(port, UART_INT_MASK, val);
}

static void bflb_uart_stop_rx(struct uart_port *port)
{
	u32 val;

	val = rdl(port, UART_URX_CONFIG);
	val &= ~UART_CR_URX_EN;
	wrl(port, UART_URX_CONFIG, val);

	val = rdl(port, UART_INT_MASK);
	val |= UART_URX_FIFO_INT | UART_URX_RTO_INT |
	       UART_URX_FER_INT;
	wrl(port, UART_INT_MASK, val);
}

static void bflb_uart_break_ctl(struct uart_port *port, int break_state)
{
}

static void bflb_uart_set_termios(struct uart_port *port,
				  struct ktermios *termios,
				  const struct ktermios *old)
{
	unsigned long flags;
	u32 valt, valr, val;
	unsigned int baud, min;

	valt = valr = 0;

	spin_lock_irqsave(&port->lock, flags);

	/* set data length */
	val = tty_get_char_size(termios->c_cflag) - 1;
	valt |= (val << UART_CR_UTX_BIT_CNT_D_SFT);

	/* calculate parity */
	termios->c_cflag &= ~CMSPAR;	/* no support mark/space */
	if (termios->c_cflag & PARENB) {
		valt |= UART_CR_UTX_PRT_EN;
		if (termios->c_cflag & PARODD)
			valr |= UART_CR_UTX_PRT_SEL;
	}

	valr = valt;

	/* calculate stop bits */
	if (termios->c_cflag & CSTOPB)
		val = 2;
	else
		val = 1;
	valt |= (val << UART_CR_UTX_BIT_CNT_P_SFT);

	/* flow control */
	if (termios->c_cflag & CRTSCTS)
		valt |= UART_CR_UTX_CTS_EN;

	/* enable TX freerunning mode */
	valt |= UART_CR_UTX_FRM_EN;

	valt |= UART_CR_UTX_EN;
	valr |= UART_CR_URX_EN;

	wrl(port, UART_UTX_CONFIG, valt);
	wrl(port, UART_URX_CONFIG, valr);

	min = port->uartclk / (UART_CR_UTX_BIT_PRD + 1);
	baud = uart_get_baud_rate(port, termios, old, min, 4000000);

	val = DIV_ROUND_CLOSEST(port->uartclk, baud) - 1;
	val &= UART_CR_UTX_BIT_PRD;
	val |= (val << 16);
	wrl(port, UART_BIT_PRD, val);

	uart_update_timeout(port, termios->c_cflag, baud);

	spin_unlock_irqrestore(&port->lock, flags);
}

static void bflb_uart_rx_chars(struct bflb_uart_port *bp)
{
	unsigned char ch, flag;
	unsigned long status;

	while ((status = rdl(&bp->port, UART_FIFO_CONFIG_1)) & UART_RX_FIFO_CNT_MSK) {
		ch = rdl(&bp->port, UART_FIFO_RDATA) & UART_FIFO_RDATA_MSK;
		flag = TTY_NORMAL;
		bp->port.icount.rx++;

		if (uart_handle_sysrq_char(&bp->port, ch))
			continue;
		uart_insert_char(&bp->port, 0, 0, ch, flag);
	}

	spin_unlock(&bp->port.lock);
	tty_flip_buffer_push(&bp->port.state->port);
	spin_lock(&bp->port.lock);
}

/**
 * bflb_uart_txfifo_space() - How much space is left int the TX FIFO?
 * @bp: pointer to a struct bflb_uart_port
 *
 * Read the transmit FIFO count to find out how much space is left
 *
 * Returns: UART_TX_FIFO_CNT - count of space left in the TX FIFO
 */
static int bflb_uart_txfifo_space(struct bflb_uart_port *bp)
{
	return (rdl(&bp->port, UART_FIFO_CONFIG_1)
		& UART_TX_FIFO_CNT_MSK) >> UART_TX_FIFO_CNT_SFT;
}

/**
 * bflb_uart_tx_char() - enqueue a byte to transmit onto the TX FIFO
 * @bp: pointer to a struct bflb_uart_port
 * @ch: character to transmit
 *
 * Enqueue a byte @ch onto the transmit FIFO, given a pointer @bp to the
 * struct bflb_uart_port * to transmit on.
 *
 * Context: Any context.
 */
static void bflb_uart_tx_char(struct bflb_uart_port *bp, int ch)
{
	wrl(&bp->port, UART_FIFO_WDATA, ch);
}

/**
 * bflb_uart_tx_chars() - enqueue multiple bytes onto the TX FIFO
 * @bp: pointer to a struct bflb_uart_port
 *
 * Transfer up to a TX FIFO size's worth of characters from the Linux serial
 * transmit buffer to the BFLB UART TX FIFO.
 *
 * Context: Any context.  Expects @bp->port.lock to be held by caller.
 */
static void bflb_uart_tx_chars(struct bflb_uart_port *bp)
{
	u8 ch;

	uart_port_tx_limited(&bp->port, ch, BFLB_UART_TX_FIFO_DEPTH,
		bflb_uart_txfifo_space(bp),
		bflb_uart_tx_char(bp, ch),
		({}));
}

static irqreturn_t bflb_uart_interrupt(int irq, void *data)
{
	struct bflb_uart_port *bp = data;
	u32 isr, val;

	isr = rdl(&bp->port, UART_INT_STS);
	wrl(&bp->port, UART_INT_CLEAR, isr);

	isr &= ~rdl(&bp->port, UART_INT_MASK);

	spin_lock(&bp->port.lock);

	if (isr & UART_URX_FER_INT) {
		/* RX FIFO error interrupt */
		val = rdl(&bp->port, UART_FIFO_CONFIG_0);
		if (val & UART_RX_FIFO_OVERFLOW)
			bp->port.icount.overrun++;

		val |= UART_RX_FIFO_CLR;
		wrl(&bp->port, UART_FIFO_CONFIG_0, val);
	}

	if (isr & (UART_URX_FIFO_INT | UART_URX_RTO_INT)) {
		bflb_uart_rx_chars(bp);
	}
	if (isr & (UART_UTX_FIFO_INT | UART_UTX_END_INT)) {
		bflb_uart_tx_chars(bp);
	}

	spin_unlock(&bp->port.lock);

	return IRQ_RETVAL(isr);
}

static void bflb_uart_config_port(struct uart_port *port, int flags)
{
	u32 val;

	port->type = PORT_BFLB;

	/* Clear mask, so no surprise interrupts. */
	val = rdl(port, UART_INT_MASK);
	val |= UART_UTX_END_INT;
	val |= UART_UTX_FIFO_INT;
	val |= UART_URX_FIFO_INT;
	val |= UART_URX_RTO_INT;
	val |= UART_URX_FER_INT;
	wrl(port, UART_INT_MASK, val);
}

static int bflb_uart_startup(struct uart_port *port)
{
	unsigned long flags;
	u32 val;
	struct bflb_uart_port *bp = to_bflb_uart_port(port);
	int ret;

	dev_dbg(port->dev, "startup %s\n", port->name);

	spin_lock_irqsave(&port->lock, flags);

	ret = devm_request_irq(port->dev, port->irq, bflb_uart_interrupt,
			       IRQF_SHARED, port->name, bp);
	if (ret) {
		dev_err(port->dev, "fail to request serial irq %d, ret=%d\n",
			port->irq, ret);
		return ret;
	}

	val = rdl(port, UART_INT_MASK);
	val |= 0xfff;
	wrl(port, UART_INT_MASK, val);

	wrl(port, UART_DATA_CONFIG, 0);
	wrl(port, UART_SW_MODE, 0);
	wrl(port, UART_URX_RTO_TIMER, 0x4f);

	val = rdl(port, UART_FIFO_CONFIG_1);
	val &= ~UART_RX_FIFO_TH_MSK;
	val |= BFLB_UART_RX_FIFO_TH << UART_RX_FIFO_TH_SFT;
	wrl(port, UART_FIFO_CONFIG_1, val);

	/* Unmask RX interrupts now */
	val = rdl(port, UART_INT_MASK);
	val &= ~UART_URX_FIFO_INT;
	val &= ~UART_URX_RTO_INT;
	val &= ~UART_URX_FER_INT;
	wrl(port, UART_INT_MASK, val);

	val = rdl(port, UART_UTX_CONFIG);
	val |= UART_CR_UTX_EN;
	wrl(port, UART_UTX_CONFIG, val);
	val = rdl(port, UART_URX_CONFIG);
	val |= UART_CR_URX_EN;
	wrl(port, UART_URX_CONFIG, val);

	spin_unlock_irqrestore(&port->lock, flags);

	return 0;
}

static void bflb_uart_shutdown(struct uart_port *port)
{
	unsigned long flags;
	struct bflb_uart_port *bp = to_bflb_uart_port(port);

	dev_dbg(port->dev, "shutdown %s\n", port->name);

	spin_lock_irqsave(&port->lock, flags);
	/* mask all interrupts now */
	wrl(port, UART_INT_MASK, UART_UTX_END_INT | UART_URX_END_INT);
	devm_free_irq(port->dev, port->irq, bp);
	spin_unlock_irqrestore(&port->lock, flags);
}

static const char *bflb_uart_type(struct uart_port *port)
{
	return (port->type == PORT_BFLB) ? "BFLB UART" : NULL;
}

static int bflb_uart_request_port(struct uart_port *port)
{
	/* UARTs always present */
	return 0;
}

static void bflb_uart_release_port(struct uart_port *port)
{
	/* Nothing to release... */
}

static int bflb_uart_verify_port(struct uart_port *port,
				 struct serial_struct *ser)
{
	if (ser->type != PORT_UNKNOWN && ser->type != PORT_BFLB)
		return -EINVAL;
	return 0;
}

static const struct uart_ops bflb_uart_ops = {
	.tx_empty = bflb_uart_tx_empty,
	.get_mctrl = bflb_uart_get_mctrl,
	.set_mctrl = bflb_uart_set_mctrl,
	.start_tx = bflb_uart_start_tx,
	.stop_tx = bflb_uart_stop_tx,
	.stop_rx = bflb_uart_stop_rx,
	.break_ctl = bflb_uart_break_ctl,
	.startup = bflb_uart_startup,
	.shutdown = bflb_uart_shutdown,
	.set_termios = bflb_uart_set_termios,
	.type = bflb_uart_type,
	.request_port = bflb_uart_request_port,
	.release_port = bflb_uart_release_port,
	.config_port = bflb_uart_config_port,
	.verify_port = bflb_uart_verify_port,
};

#ifdef CONFIG_SERIAL_BFLB_CONSOLE
static void bflb_console_putchar(struct uart_port *port, unsigned char ch)
{
	while (!(rdl(port, UART_FIFO_CONFIG_1) & UART_TX_FIFO_CNT_MSK))
		cpu_relax();
	wrb(port, UART_FIFO_WDATA, ch);
}

/*
 * Interrupts are disabled on entering
 */
static void bflb_uart_console_write(struct console *co, const char *s,
				    u_int count)
{
	struct uart_port *port = &bflb_uart_ports[co->index]->port;
	u32 status, reg, mask;

	/* save then disable interrupts */
	mask = rdl(port, UART_INT_MASK);
	reg = -1;
	wrl(port, UART_INT_MASK, reg);

	/* Make sure that tx is enabled */
	reg = rdl(port, UART_UTX_CONFIG);
	reg |= UART_CR_UTX_EN;
	wrl(port, UART_UTX_CONFIG, reg);

	uart_console_write(port, s, count, bflb_console_putchar);

	/* wait for TX done */
	do {
		status = rdl(port, UART_STATUS);
	} while ((status & UART_STS_UTX_BUS_BUSY));

	/* restore IRQ mask */
	wrl(port, UART_INT_MASK, mask);
}

static int bflb_uart_console_setup(struct console *co, char *options)
{
	struct uart_port *port;
	struct bflb_uart_port *bp;
	int baud = BFLB_UART_BAUD;
	int bits = 8;
	int parity = 'n';
	int flow = 'n';
	u32 val;

	if (co->index >= BFLB_UART_MAXPORTS || co->index < 0)
		return -EINVAL;

	bp = bflb_uart_ports[co->index];
	if (!bp)
		/* Port not initialized yet - delay setup */
		return -ENODEV;

	port = &bp->port;

	val = rdl(port, UART_UTX_CONFIG);
	val |= UART_CR_UTX_EN;
	wrl(port, UART_UTX_CONFIG, val);

	if (options)
		uart_parse_options(options, &baud, &parity, &bits, &flow);

	return uart_set_options(port, co, baud, parity, bits, flow);
}

static struct uart_driver bflb_uart_driver;
static struct console bflb_uart_console = {
	.name = "ttyS",
	.write = bflb_uart_console_write,
	.device = uart_console_device,
	.setup = bflb_uart_console_setup,
	.flags = CON_PRINTBUFFER,
	.index = -1,
	.data = &bflb_uart_driver,
};

static int __init bflb_uart_console_init(void)
{
	register_console(&bflb_uart_console);
	return 0;
}
console_initcall(bflb_uart_console_init);

#define BFLB_UART_CONSOLE (&bflb_uart_console)

static void bflb_uart_earlycon_write(struct console *co, const char *s,
				     unsigned int count)
{
	struct earlycon_device *dev = co->data;

	uart_console_write(&dev->port, s, count, bflb_console_putchar);
}

static int __init bflb_uart_earlycon_setup(struct earlycon_device *dev,
					   const char *options)
{
	if (!dev->port.membase)
		return -ENODEV;

	dev->con->write = bflb_uart_earlycon_write;

	return 0;
}
OF_EARLYCON_DECLARE(bflb_uart, "bflb,bl808-uart", bflb_uart_earlycon_setup);

#else

#define BFLB_UART_CONSOLE NULL

#endif /* CONFIG_SERIAL_BFLB_CONSOLE */

static struct uart_driver bflb_uart_driver = {
	.owner = THIS_MODULE,
	.driver_name = "bflb_uart",
	.dev_name = "ttyS",
	.nr = BFLB_UART_MAXPORTS,
	.cons = BFLB_UART_CONSOLE,
};

static int bflb_uart_probe(struct platform_device *pdev)
{
	struct uart_port *port;
	struct bflb_uart_port *bp;
	struct resource *res;
	int index, irq;

	index = of_alias_get_id(pdev->dev.of_node, "serial");
	if (unlikely(index < 0 || index >= BFLB_UART_MAXPORTS)) {
		dev_err(&pdev->dev, "got a wrong serial alias id %d\n", index);
		return -EINVAL;
	}

	bp = devm_kzalloc(&pdev->dev, sizeof(*bp), GFP_KERNEL);
	if (!bp)
		return -ENOMEM;

	bflb_uart_ports[index] = bp;
	platform_set_drvdata(pdev, bp);
	port = &bp->port;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	port->membase = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(port->membase))
		return PTR_ERR(port->membase);

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	port->mapbase = res->start;
	port->irq = irq;
	port->line = index;
	port->type = PORT_BFLB;
	port->iotype = UPIO_MEM;
	port->fifosize = BFLB_UART_TX_FIFO_DEPTH;
	port->ops = &bflb_uart_ops;
	port->flags = UPF_BOOT_AUTOCONF;
	port->dev = &pdev->dev;
	port->has_sysrq = IS_ENABLED(CONFIG_SERIAL_BFLB_CONSOLE);

	bp->clk = devm_clk_get_enabled(&pdev->dev, NULL);
	if (IS_ERR(bp->clk))
		return PTR_ERR(bp->clk);
	port->uartclk = clk_get_rate(bp->clk);

	return uart_add_one_port(&bflb_uart_driver, port);
}

static int bflb_uart_remove(struct platform_device *pdev)
{
	struct bflb_uart_port *bp = platform_get_drvdata(pdev);

	uart_remove_one_port(&bflb_uart_driver, &bp->port);
	bflb_uart_ports[bp->port.line] = NULL;

	return 0;
}

static const struct of_device_id bflb_uart_match[] = {
	{
		.compatible = "bflb,bl808-uart",
	},
	{},
};
MODULE_DEVICE_TABLE(of, bflb_uart_match);

static struct platform_driver bflb_uart_platform_driver = {
	.probe	= bflb_uart_probe,
	.remove	= bflb_uart_remove,
	.driver	= {
		.name		= "bflb_uart",
		.of_match_table	= of_match_ptr(bflb_uart_match),
	},
};

static int __init bflb_uart_init(void)
{
	int ret;

	ret = uart_register_driver(&bflb_uart_driver);
	if (ret)
		return ret;

	ret = platform_driver_register(&bflb_uart_platform_driver);
	if (ret)
		uart_unregister_driver(&bflb_uart_driver);

	return ret;
}

static void __exit bflb_uart_exit(void)
{
	platform_driver_unregister(&bflb_uart_platform_driver);
	uart_unregister_driver(&bflb_uart_driver);
}

module_init(bflb_uart_init);
module_exit(bflb_uart_exit);

MODULE_DESCRIPTION("Bouffalolab UART driver");
MODULE_AUTHOR("Jisheng Zhang <jszhang@kernel.org>");
MODULE_LICENSE("GPL");
