// SPDX-License-Identifier: GPL-2.0-only
/*
 * Bouffalo Lab SoC pinctrl+GPIO+external IRQ driver
 *
 * Based on: pinctrl-apple-gpio.c
 * Copyright (C) The Asahi Linux Contributors
 * Copyright (C) 2020 Corellium LLC
 *
 * Based on: pinctrl-pistachio.c
 * Copyright (C) 2014 Imagination Technologies Ltd.
 * Copyright (C) 2014 Google, Inc.
 */

#include <dt-bindings/pinctrl/bflb.h>

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/gpio/driver.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#include <linux/pinctrl/pinctrl.h>
#include <linux/pinctrl/pinmux.h>

#include "pinctrl-utils.h"
#include "core.h"
#include "pinmux.h"

struct bflb_gpio_pinctrl
{
	struct device *dev;
	struct pinctrl_dev *pctldev;

	void __iomem *base;
	struct regmap *map;

	struct pinctrl_desc pinctrl_desc;
	struct gpio_chip gpio_chip;
	u8 irqgrps[];
};

//Register indexing
#define REG_GPIO(x)              (4 * (x))

//Register map
#define REG_GPIOx_MODE           GENMASK(31, 30) //Bits 31 and 30 rw - Software Control Mode (only applicable when REG_GPIOx_FUNC_SEL is SWGPIO)
#define REG_GPIOx_I              BIT(28)         //Bit 28 ro - Boolean for GPIO input (only applicable when REG_GPIOx_FUNC_SEL is SWGPIO, and REG_GPIOx_MODE is Input mode, see RM for quirks)
#define REG_GPIOx_CLR            BIT(26)         //Bit 26 wo 1 pass - Clear/Reset GPIO output (SET and CLR together ignores CLR) (only applicable when REG_GPIOx_FUNC_SEL is SWGPIO and REG_GPIOx_MODE is Set/Clear mode)
#define REG_GPIOx_SET            BIT(25)         //Bit 25 wo 1 pass - Put/Set GPIO output (SET and CLR together ignores CLR) (only applicable when REG_GPIOx_FUNC_SEL is SWGPIO and REG_GPIOx_MODE is Set/Clear mode)
#define REG_GPIOx_O              BIT(24)         //Bit 24 rw - Boolean set/reset for GPIO output (only applicable when REG_GPIOx_FUNC_SEL is SWGPIO, and REG_GPIOx_MODE is Output Value mode, see RM for quirks)
#define REG_GPIOx_INT_MASK       BIT(22)         //Bit 22 rw - Mask interrupts from this GPIO
#define REG_GPIOx_INT_STAT       BIT(21)         //Bit 21 ro - Current interrupt status for this GPIO
#define REG_GPIOx_INT_CLR        BIT(20)         //Bit 20 rw - Clear interrupt status for this GPIO
#define REG_GPIOx_INT_MODE_SET   GENMASK(29, 16) //Bits 19 to 16 rw - Trigger mode for interrupting on this GPIO
#define REG_GPIOx_FUNC_SEL       GENMASK(12, 8)  //Bits 12 to 8 rw - Function assigned to this GPIO
#define REG_GPIOx_OE             BIT(6)          //Bit 6 rw - Output Enable for this GPIO (only applicable when REG_GPIOx_FUNC_SEL is Register Control GPIO)
#define REG_GPIOx_PD             BIT(5)          //Bit 5 rw - Pulldown Enable
#define REG_GPIOx_PU             BIT(4)          //Bit 4 rw - Pullup Enable
#define REG_GPIOx_DRV            GENMASK(3, 2)   //Bits 3 and 2 rw - Drive/Bias? Enable
#define REG_GPIOx_SMT            BIT(1)          //Bit 1 rw - Schmitt Trigger Enable
#define REG_GPIOx_IE             BIT(0)          //Bit 0 rw - Input Enable for this GPIO

//Interrupt trigger modes
#define BFLB_IRQ_MODE_SYNC_EDGE_FALLING 0
#define BFLB_IRQ_MODE_SYNC_EDGE_RISING 1
#define BFLB_IRQ_MODE_SYNC_LEVEL_LOW 2
#define BFLB_IRQ_MODE_SYNC_LEVEL_HIGH 3
#define BFLB_IRQ_MODE_SYNC_EDGE_BOTH 4

#define BFLB_IRQ_MODE_ASYNC_EDGE_FALLING 8
#define BFLB_IRQ_MODE_ASYNC_EDGE_RISING 9
#define BFLB_IRQ_MODE_ASYNC_LEVEL_LOW 10
#define BFLB_IRQ_MODE_ASYNC_LEVEL_HIGH 11

struct regmap_config regmap_config = {
	.reg_bits = 32,
	.val_bits = 32,
	.reg_stride = 4,
	.cache_type = REGCACHE_FLAT,
	.max_register = 512 * sizeof(u32),
	.num_reg_defaults_raw = 512,
	.use_relaxed_mmio = true,
	.use_raw_spinlock = true,
};

/* No locking needed to mask/unmask IRQs as the interrupt mode is per pin-register. */

//AH: Set raw gpio config register bits based on mask
static void bflb_gpio_set_reg(struct bflb_gpio_pinctrl *pctl, unsigned int pin, u32 mask, u32 value)
{
	regmap_update_bits(pctl->map, REG_GPIO(pin), mask, value);
}

//AH: Get raw gpio config register bits
static u32 bflb_gpio_get_reg(struct bflb_gpio_pinctrl *pctl, unsigned int pin)
{
	int ret;
	u32 val;

	ret = regmap_read(pctl->map, REG_GPIO(pin), &val);

	if (ret) return 0;

	return val;
}

/* Pin controller functions */

//AH: Map DT to internal
static int bflb_gpio_dt_node_to_map(struct pinctrl_dev *pctldev, struct device_node *node, struct pinctrl_map **map, unsigned *num_maps)
{
	unsigned reserved_maps;
	struct bflb_gpio_pinctrl *pctl;
	u32 pinfunc, pin, func;
	int num_pins, i, ret;
	const char *group_name;
	const char *function_name;

	*map = NULL;
	*num_maps = 0;
	reserved_maps = 0;

	pctl = pinctrl_dev_get_drvdata(pctldev);

	ret = of_property_count_u32_elems(node, "pinmux");

	if (ret <= 0)
	{
		dev_err(pctl->dev, "missing or empty pinmux property in node %pOFn.\n", node);

		return ret ? ret : -EINVAL;
	}

	num_pins = ret;

	ret = pinctrl_utils_reserve_map(pctldev, map, &reserved_maps, num_maps, num_pins);

	if (ret) return ret;

	for (i = 0; i < num_pins; i++)
	{
		ret = of_property_read_u32_index(node, "pinmux", i, &pinfunc);

		if (ret) goto free_map;

		pin = BFLB_PIN(pinfunc);
		func = BFLB_FUNC(pinfunc);

		if (func >= pinmux_generic_get_function_count(pctldev))
		{
			ret = -EINVAL;
			goto free_map;
		}

		group_name = pinctrl_generic_get_group_name(pctldev, pin);
		function_name = pinmux_generic_get_function_name(pctl->pctldev, func);
		ret = pinctrl_utils_add_map_mux(pctl->pctldev, map, &reserved_maps, num_maps, group_name, function_name);

		if (ret) goto free_map;
	}

free_map:
	if (ret < 0) pinctrl_utils_free_map(pctldev, *map, *num_maps);

	return ret;
}

static const struct pinctrl_ops bflb_gpio_pinctrl_ops = {
	.get_groups_count = pinctrl_generic_get_group_count,
	.get_group_name   = pinctrl_generic_get_group_name,
	.get_group_pins   = pinctrl_generic_get_group_pins,
	.dt_node_to_map   = bflb_gpio_dt_node_to_map,
	.dt_free_map      = pinctrl_utils_free_map,
};

/* Pin multiplexer functions */

//AH: Configure gpio modes and features
static int bflb_gpio_pinmux_set(struct pinctrl_dev *pctldev, unsigned func, unsigned group)
{
	struct bflb_gpio_pinctrl *pctl = pinctrl_dev_get_drvdata(pctldev);

	bflb_gpio_set_reg
	(
		pctl, //Driver
		group, //Pin
		REG_GPIOx_FUNC_SEL, //Mask - Setting Function
		FIELD_PREP(REG_GPIOx_FUNC_SEL, func) //Value - Setting to specified func
	);

	return 0;
}

static const struct pinmux_ops bflb_gpio_pinmux_ops = {
	.get_functions_count = pinmux_generic_get_function_count,
	.get_function_name = pinmux_generic_get_function_name,
	.get_function_groups = pinmux_generic_get_function_groups,
	.set_mux = bflb_gpio_pinmux_set,
	.strict = true,
};

/* GPIO chip functions */

//AH: Get the current gpio direction
static int bflb_gpio_get_direction(struct gpio_chip *chip, unsigned int offset)
{
	struct bflb_gpio_pinctrl *pctl = gpiochip_get_data(chip);
	unsigned int reg = bflb_gpio_get_reg(pctl, offset);

	if (FIELD_GET(REG_GPIOx_OE, reg) == 1 && FIELD_GET(REG_GPIOx_IE, reg) == 0)
	{
		return GPIO_LINE_DIRECTION_OUT;
	}
	else if (FIELD_GET(REG_GPIOx_IE, reg) == 1 && FIELD_GET(REG_GPIOx_OE, reg) == 0)
	{
		return GPIO_LINE_DIRECTION_IN;
	}

	return -EIO;
}

//AH: Get the incoming input or outgoing output value for the specified GPIO
static int bflb_gpio_get(struct gpio_chip *chip, unsigned offset) //TODO AH
{
	struct bflb_gpio_pinctrl *pctl = gpiochip_get_data(chip);
	unsigned int reg = bflb_gpio_get_reg(pctl, offset);
	
	if (FIELD_GET(REG_GPIOx_OE, reg) == 1 && FIELD_GET(REG_GPIOx_IE, reg) == 0)
	{
		reg = readl_relaxed(pctl->base + REG_GPIO(offset)); //Read w/ cache ignore
		return !!(reg & REG_GPIOx_O);
	}
	else if (FIELD_GET(REG_GPIOx_IE, reg) == 1 && FIELD_GET(REG_GPIOx_OE, reg) == 0)
	{
		reg = readl_relaxed(pctl->base + REG_GPIO(offset)); //Read w/ cache ignore
		return !!(reg & REG_GPIOx_I);
	}

	return -EIO;
}

//AH: Set the specified GPIO's output state
static void bflb_gpio_set(struct gpio_chip *chip, unsigned int offset, int value)
{
	struct bflb_gpio_pinctrl *pctl = gpiochip_get_data(chip);

	bflb_gpio_set_reg(pctl, offset, REG_GPIOx_O, value ? FIELD_PREP(REG_GPIOx_O, 1) : 0);
}

//AH: Set the specified gpio direction to input
static int bflb_gpio_direction_input(struct gpio_chip *chip, unsigned int offset)
{
	struct bflb_gpio_pinctrl *pctl = gpiochip_get_data(chip);

	bflb_gpio_set_reg
	(
		pctl, //Driver
		offset, //Pin
		REG_GPIOx_PD | REG_GPIOx_PU | REG_GPIOx_OE | REG_GPIOx_IE | REG_GPIOx_SMT, //Mask
		FIELD_PREP(REG_GPIOx_PD, 1) | FIELD_PREP(REG_GPIOx_PU, 0) | FIELD_PREP(REG_GPIOx_OE, 0) | FIELD_PREP(REG_GPIOx_IE, 1) | FIELD_PREP(REG_GPIOx_SMT, 1) //Value
	);

	return 0;
}

//AH: Set the specified gpio direction to output
static int bflb_gpio_direction_output(struct gpio_chip *chip, unsigned int offset, int value)
{
	struct bflb_gpio_pinctrl *pctl = gpiochip_get_data(chip);

	bflb_gpio_set_reg
	(
		pctl, //Driver
		offset, //Pin
		REG_GPIOx_PD | REG_GPIOx_PU | REG_GPIOx_OE | REG_GPIOx_IE | REG_GPIOx_SMT | REG_GPIOx_MODE, //Mask
		FIELD_PREP(REG_GPIOx_PD, 1) | FIELD_PREP(REG_GPIOx_PU, 0) | FIELD_PREP(REG_GPIOx_OE, 1) | FIELD_PREP(REG_GPIOx_IE, 0) | FIELD_PREP(REG_GPIOx_SMT, 1) | FIELD_PREP(REG_GPIOx_MODE, 0) //Value
	);

	return 0;
}

/* IRQ chip functions */

//AH: Clear the interrupt for the specified GPIO
static void bflb_gpio_irq_ack(struct irq_data *data) //TODO AH
{
	struct bflb_gpio_pinctrl *pctl = gpiochip_get_data(irq_data_get_irq_chip_data(data));
	/*unsigned int irqgrp = FIELD_GET(REG_GPIOx_GRP, bflb_gpio_get_reg(pctl, data->hwirq));

	writel(BIT(data->hwirq % 32), pctl->base + REG_IRQ(irqgrp, data->hwirq));*/

	bflb_gpio_set_reg
	(
		pctl, //Driver
		data->hwirq, //Ping
		REG_GPIOx_INT_CLR, //Mask
	    FIELD_PREP(REG_GPIOx_INT_CLR, 1) //Value
	);
}

//AH: Find the correct value for the type of interrupts we want to receive for a GPIO
static unsigned int bflb_gpio_irq_type(unsigned int type)
{
	switch (type & IRQ_TYPE_SENSE_MASK) {
		case IRQ_TYPE_EDGE_RISING: return BFLB_IRQ_MODE_SYNC_EDGE_RISING;

		case IRQ_TYPE_EDGE_FALLING: return BFLB_IRQ_MODE_SYNC_EDGE_FALLING;

		case IRQ_TYPE_EDGE_BOTH: return BFLB_IRQ_MODE_SYNC_EDGE_BOTH;

		case IRQ_TYPE_LEVEL_HIGH: return BFLB_IRQ_MODE_SYNC_LEVEL_HIGH;

		case IRQ_TYPE_LEVEL_LOW: return BFLB_IRQ_MODE_SYNC_LEVEL_LOW;

		default: return BFLB_IRQ_MODE_SYNC_EDGE_FALLING; //No "off" available on BL808, set to default IRQ_TYPE_EDGE_FALLING and then we'll need to mask
	}

	return BFLB_IRQ_MODE_SYNC_EDGE_FALLING;
}

//AH: Disable the specified GPIO's interrupt
static void bflb_gpio_irq_mask(struct irq_data *data) //TODO AH
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(data);
	struct bflb_gpio_pinctrl *pctl = gpiochip_get_data(gc);

	bflb_gpio_set_reg
	(
		pctl, //Driver
		data->hwirq, //Pin
		REG_GPIOx_INT_MASK, //Mask
	    FIELD_PREP(REG_GPIOx_INT_MASK, 1) //Value
	);

	gpiochip_disable_irq(gc, data->hwirq);
}

//AH: Enable the specified GPIO's interrupt
static void bflb_gpio_irq_unmask(struct irq_data *data) //TODO AH
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(data);
	struct bflb_gpio_pinctrl *pctl = gpiochip_get_data(gc);
	unsigned int irqtype = bflb_gpio_irq_type(irqd_get_trigger_type(data));

	gpiochip_enable_irq(gc, data->hwirq);

	bflb_gpio_set_reg
	(
		pctl, //Driver
		data->hwirq, //Pin
		REG_GPIOx_INT_MASK | REG_GPIOx_INT_MODE_SET, //Mask
	    FIELD_PREP(REG_GPIOx_INT_MASK, 0) | FIELD_PREP(REG_GPIOx_INT_MODE_SET, irqtype) //Value
	);
}

//AH: Initialise the specified GPIO's interrupt
static unsigned int bflb_gpio_irq_startup(struct irq_data *data) //TODO AH
{
	struct gpio_chip *chip = irq_data_get_irq_chip_data(data);
	struct bflb_gpio_pinctrl *pctl = gpiochip_get_data(chip);

	bflb_gpio_set_reg
	(
		pctl, //Driver
		data->hwirq, //Ping
		REG_GPIOx_INT_CLR, //Mask
	    FIELD_PREP(REG_GPIOx_INT_CLR, 1) //Value
	);

	bflb_gpio_direction_input(chip, data->hwirq);
	bflb_gpio_irq_unmask(data);

	return 0;
}

//AH: Set the specified GPIO's interrupt mode
static int bflb_gpio_irq_set_type(struct irq_data *data, unsigned int type) //TODO AH
{
	struct bflb_gpio_pinctrl *pctl = gpiochip_get_data(irq_data_get_irq_chip_data(data));
	unsigned int irqtype = bflb_gpio_irq_type(type);

	if (irqtype == 0) return -EINVAL;

	bflb_gpio_set_reg
	(
		pctl, //Driver
		data->hwirq, //Pin
		REG_GPIOx_INT_MODE_SET, //Mask
		FIELD_PREP(REG_GPIOx_INT_MODE_SET, irqtype) //Value
	);

	if (type & IRQ_TYPE_LEVEL_MASK)
	{
		irq_set_handler_locked(data, handle_level_irq);
	}
	else
	{
		irq_set_handler_locked(data, handle_edge_irq);
	}
		
	return 0;
}

//AH: Handle GPIO interrupts on this controller
static void bflb_gpio_irq_handler(struct irq_desc *desc)
{
	struct irq_chip *chip = irq_desc_get_chip(desc);
	u8 *grpp = irq_desc_get_handler_data(desc);
	struct bflb_gpio_pinctrl *pctl;
	unsigned int pinh, pinl;
	//unsigned long pending;
	unsigned long reg;
	struct gpio_chip *gc;

	pctl = container_of(grpp - *grpp, typeof(*pctl), irqgrps[0]);
	gc = &pctl->gpio_chip;

	chained_irq_enter(chip, desc);

	//Old code was reading in chunks of 32 bits and parsing. We cannot do this as we don't have one block of bits for IRQs
	/*for (pinh = 0; pinh < gc->ngpio; pinh += 32)
	{
		pending = readl_relaxed(pctl->base + REG_IRQ(*grpp, pinh));

		for_each_set_bit(pinl, &pending, 32) generic_handle_domain_irq(gc->irq.domain, pinh + pinl);
	}*/

	//New code, we must go through each individual GPIO register to read its interrupt status. There is no gpio_cfg128+ helper register for interrupts (looking at BL808 RM)
	for (pinh = 0; pinh < gc->ngpio; pinh += 1)
	{
		reg = readl_relaxed(pctl->base + REG_GPIO(pinh)); //Read w/ cache ignore
		
		if (reg & REG_GPIOx_INT_STAT) generic_handle_domain_irq(gc->irq.domain, pinh);
	}

	chained_irq_exit(chip, desc);
}

static const struct irq_chip bflb_gpio_irqchip = {
	.name			= "bflb-GPIO",
	.irq_startup    = bflb_gpio_irq_startup,
	.irq_ack		= bflb_gpio_irq_ack,
	.irq_mask		= bflb_gpio_irq_mask,
	.irq_unmask		= bflb_gpio_irq_unmask,
	.irq_set_type	= bflb_gpio_irq_set_type,
	.flags			= IRQCHIP_IMMUTABLE,
	GPIOCHIP_IRQ_RESOURCE_HELPERS,
};

/* Probe & register */

static int bflb_gpio_register(struct bflb_gpio_pinctrl *pctl)
{
	struct gpio_irq_chip *girq = &pctl->gpio_chip.irq;
	void **irq_data = NULL;
	int ret;

	pctl->gpio_chip.label = dev_name(pctl->dev);
	pctl->gpio_chip.request = gpiochip_generic_request;
	pctl->gpio_chip.free = gpiochip_generic_free;
	pctl->gpio_chip.get_direction = bflb_gpio_get_direction;
	pctl->gpio_chip.direction_input = bflb_gpio_direction_input;
	pctl->gpio_chip.direction_output = bflb_gpio_direction_output;
	pctl->gpio_chip.get = bflb_gpio_get;
	pctl->gpio_chip.set = bflb_gpio_set;
	pctl->gpio_chip.base = -1;
	pctl->gpio_chip.ngpio = pctl->pinctrl_desc.npins;
	pctl->gpio_chip.parent = pctl->dev;

	if (girq->num_parents)
	{
		int i;

		gpio_irq_chip_set_chip(girq, &bflb_gpio_irqchip);

		girq->parent_handler = bflb_gpio_irq_handler;

		girq->parents = kmalloc_array(girq->num_parents, sizeof(*girq->parents), GFP_KERNEL);
		irq_data = kmalloc_array(girq->num_parents, sizeof(*irq_data), GFP_KERNEL);

		if (!girq->parents || !irq_data)
		{
			ret = -ENOMEM;
			goto out_free_irq_data;
		}

		for (i = 0; i < girq->num_parents; i++)
		{
			ret = platform_get_irq(to_platform_device(pctl->dev), i);

			if (ret < 0) goto out_free_irq_data;

			girq->parents[i] = ret;
			pctl->irqgrps[i] = i;
			irq_data[i] = &pctl->irqgrps[i];
		}

		girq->parent_handler_data_array = irq_data;
		girq->per_parent_data = true;
		girq->default_type = IRQ_TYPE_NONE;
		girq->handler = handle_level_irq;
	}

	ret = devm_gpiochip_add_data(pctl->dev, &pctl->gpio_chip, pctl);

out_free_irq_data:
	kfree(girq->parents);
	kfree(irq_data);

	return ret;
}

static int bflb_gpio_pinctrl_probe(struct platform_device *pdev)
{
	struct bflb_gpio_pinctrl *pctl;
	struct pinctrl_pin_desc *pins;

	unsigned int npins;
	const char **pin_names;
	unsigned int *pin_nums;
	unsigned int i, nirqs = 0;
	int res;

	static const char* pinmux_functions[] = {
		"gpio", "periph1", "periph2", "periph3"
	};

	if (of_property_read_bool(pdev->dev.of_node, "interrupt-controller"))
	{
		res = platform_irq_count(pdev);
		if (res > 0) nirqs = res;
	}

	pctl = devm_kzalloc(&pdev->dev, struct_size(pctl, irqgrps, nirqs), GFP_KERNEL);

	if (!pctl) return -ENOMEM;

	pctl->dev = &pdev->dev;
	pctl->gpio_chip.irq.num_parents = nirqs;

	dev_set_drvdata(&pdev->dev, pctl);

	if (of_property_read_u32(pdev->dev.of_node, "bflb,npins", &npins)) return dev_err_probe(&pdev->dev, -EINVAL, "bflb,npins property not found\n");

	pins = devm_kmalloc_array(&pdev->dev, npins, sizeof(pins[0]), GFP_KERNEL);
	pin_names = devm_kmalloc_array(&pdev->dev, npins, sizeof(pin_names[0]), GFP_KERNEL);
	pin_nums = devm_kmalloc_array(&pdev->dev, npins, sizeof(pin_nums[0]), GFP_KERNEL);

	if (!pins || !pin_names || !pin_nums) return -ENOMEM;

	pctl->base = devm_platform_ioremap_resource(pdev, 0);

	if (IS_ERR(pctl->base)) return PTR_ERR(pctl->base);

	pctl->map = devm_regmap_init_mmio(&pdev->dev, pctl->base, &regmap_config);

	if (IS_ERR(pctl->map)) return dev_err_probe(&pdev->dev, PTR_ERR(pctl->map), "Failed to create regmap\n");

	for (i = 0; i < npins; i++)
	{
		pins[i].number = i;
		pins[i].name = devm_kasprintf(&pdev->dev, GFP_KERNEL, "PIN%u", i);
		pins[i].drv_data = pctl;
		pin_names[i] = pins[i].name;
		pin_nums[i] = i;
	}

	pctl->pinctrl_desc.name = dev_name(pctl->dev);
	pctl->pinctrl_desc.pins = pins;
	pctl->pinctrl_desc.npins = npins;
	pctl->pinctrl_desc.pctlops = &bflb_gpio_pinctrl_ops;
	pctl->pinctrl_desc.pmxops = &bflb_gpio_pinmux_ops;

	pctl->pctldev =	devm_pinctrl_register(&pdev->dev, &pctl->pinctrl_desc, pctl);

	if (IS_ERR(pctl->pctldev)) return dev_err_probe(&pdev->dev, PTR_ERR(pctl->pctldev), "Failed to register pinctrl device.\n");

	for (i = 0; i < npins; i++)
	{
		res = pinctrl_generic_add_group(pctl->pctldev, pins[i].name, pin_nums + i, 1, pctl);

		if (res < 0) return dev_err_probe(pctl->dev, res, "Failed to register group");
	}

	for (i = 0; i < ARRAY_SIZE(pinmux_functions); ++i)
	{
		res = pinmux_generic_add_function(pctl->pctldev, pinmux_functions[i], pin_names, npins, pctl);

		if (res < 0) return dev_err_probe(pctl->dev, res, "Failed to register function.");
	}

	return bflb_gpio_register(pctl);
}

static const struct of_device_id bflb_gpio_pinctrl_of_match[] = {
	{ .compatible = "bflb,pinctrl", },
	{ }
};

MODULE_DEVICE_TABLE(of, bflb_gpio_pinctrl_of_match);

static struct platform_driver bflb_gpio_pinctrl_driver = {
	.driver = {
		.name = "bflb-gpio-pinctrl",
		.of_match_table = bflb_gpio_pinctrl_of_match,
		.suppress_bind_attrs = true,
	},
	.probe = bflb_gpio_pinctrl_probe,
};

module_platform_driver(bflb_gpio_pinctrl_driver);

MODULE_DESCRIPTION("Bouffalo BL808 pinctrl/GPIO driver");
MODULE_AUTHOR("Stan Skowronek <stan@corellium.com>");
MODULE_AUTHOR("Joey Gouly <joey.gouly@arm.com>");
MODULE_AUTHOR("Alexander Horner <contact@alexhorner.cc>");
MODULE_LICENSE("GPL v2");