/* SPDX-License-Identifier: GPL-2.0+ OR MIT */
/*
 * This header provides constants for Bouffalo Lab pinctrl bindings.
 */

#ifndef _DT_BINDINGS_PINCTRL_BFLB_H
#define _DT_BINDINGS_PINCTRL_BFLB_H

#define BFLB_PINMUX(pin, func) ((pin) | ((func) << 16))
#define BFLB_PIN(pinmux) ((pinmux) & 0xffff)
#define BFLB_FUNC(pinmux) ((pinmux) >> 16)

#define BFLB_FUNC_SWGPIO 11

#endif /* _DT_BINDINGS_PINCTRL_BFLB_H */
