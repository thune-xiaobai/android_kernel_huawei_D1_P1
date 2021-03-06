/*
 * twl6040-irq.c  --  Interrupt controller support for TWL6040
 *
 * Copyright 2010 Texas Instruments Inc.
 *
 * Author: Misael Lopez Cruz <misael.lopez@ti.com>
 *
 *  This program is free software; you can redistribute  it and/or modify it
 *  under  the terms of  the GNU General  Public License as published by the
 *  Free Software Foundation;  either version 2 of the  License, or (at your
 *  option) any later version.
 *
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/irq.h>
#include <linux/interrupt.h>
#include <linux/mfd/core.h>
#include <linux/mfd/twl6040-codec.h>
#include "../../../sound/soc/codecs/twl6040.h"
struct twl6040_irq_data {
	int mask;
	int status;
};

static struct twl6040_irq_data twl6040_irqs[] = {
	{
		.mask = TWL6040_THMSK,
		.status = TWL6040_THINT,
	},
	{
		.mask = TWL6040_PLUGMSK,
		.status = TWL6040_PLUGINT | TWL6040_UNPLUGINT,
	},
	{
		.mask = TWL6040_HOOKMSK,
		.status = TWL6040_HOOKINT,
	},
	{
		.mask = TWL6040_HFMSK,
		.status = TWL6040_HFINT,
	},
	{
		.mask = TWL6040_VIBMSK,
		.status = TWL6040_VIBINT,
	},
	{
		.mask = TWL6040_READYMSK,
		.status = TWL6040_READYINT,
	},
};

static inline struct twl6040_irq_data *irq_to_twl6040_irq(struct twl6040 *twl6040,
							  int irq)
{
	return &twl6040_irqs[irq - twl6040->irq_base];
}

static void twl6040_irq_lock(struct irq_data *data)
{
	struct twl6040 *twl6040 = irq_data_get_irq_chip_data(data);

	mutex_lock(&twl6040->irq_mutex);
}

static void twl6040_irq_sync_unlock(struct irq_data *data)
{
	struct twl6040 *twl6040 = irq_data_get_irq_chip_data(data);

	/* write back to hardware any change in irq mask */
	if (twl6040->irq_masks_cur != twl6040->irq_masks_cache) {
		twl6040->irq_masks_cache = twl6040->irq_masks_cur;
		twl6040_reg_write(twl6040, TWL6040_REG_INTMR,
				  twl6040->irq_masks_cur);
	}

	mutex_unlock(&twl6040->irq_mutex);
}

static void twl6040_irq_unmask(struct irq_data *data)
{
	struct twl6040 *twl6040 = irq_data_get_irq_chip_data(data);
	struct twl6040_irq_data *irq_data = irq_to_twl6040_irq(twl6040, data->irq);

	twl6040->irq_masks_cur &= ~irq_data->mask;
}

static void twl6040_irq_mask(struct irq_data *data)
{
	struct twl6040 *twl6040 = irq_data_get_irq_chip_data(data);
	struct twl6040_irq_data *irq_data = irq_to_twl6040_irq(twl6040, data->irq);

	twl6040->irq_masks_cur |= irq_data->mask;
}

static struct irq_chip twl6040_irq_chip = {
	.name = "twl6040",
	.irq_bus_lock = twl6040_irq_lock,
	.irq_bus_sync_unlock = twl6040_irq_sync_unlock,
	.irq_mask = twl6040_irq_mask,
	.irq_unmask = twl6040_irq_unmask,
};

static irqreturn_t twl6040_irq_thread(int irq, void *data)
{
	struct twl6040 *twl6040 = data;
	u8 intid;
	int i;
        int val;
        val = twl6040_reg_read(twl6040, TWL6040_REG_STATUS);
	intid = twl6040_reg_read(twl6040, TWL6040_REG_INTID);

	/* apply masking and report (backwards to handle READYINT first) */
	for (i = ARRAY_SIZE(twl6040_irqs) - 1; i >= 0; i--) {
		if (twl6040->irq_masks_cur & twl6040_irqs[i].mask)
			intid &= ~twl6040_irqs[i].status;
		if (intid & twl6040_irqs[i].status)
			handle_nested_irq(twl6040->irq_base + i);
	}

	/* ack unmasked irqs */
	twl6040_reg_write(twl6040, TWL6040_REG_INTID, intid);

	return IRQ_HANDLED;
}

int twl6040_irq_init(struct twl6040 *twl6040)
{
	int cur_irq, ret;
	u8 val;

	mutex_init(&twl6040->irq_mutex);

	/* mask the individual interrupt sources */
	twl6040->irq_masks_cur = TWL6040_ALLINT_MSK;
	twl6040->irq_masks_cache = TWL6040_ALLINT_MSK;
	twl6040_reg_write(twl6040, TWL6040_REG_INTMR, TWL6040_ALLINT_MSK);

	if (!twl6040->irq) {
		dev_warn(twl6040->dev,
			 "no interrupt specified, no interrupts\n");
		twl6040->irq_base = 0;
		return 0;
	}

	if (!twl6040->irq_base) {
		dev_err(twl6040->dev,
			"no interrupt base specified, no interrupts\n");
		return 0;
	}

	/* Register them with genirq */
	for (cur_irq = twl6040->irq_base;
	     cur_irq < twl6040->irq_base + ARRAY_SIZE(twl6040_irqs);
	     cur_irq++) {
		irq_set_chip_data(cur_irq, twl6040);
		irq_set_chip_and_handler(cur_irq, &twl6040_irq_chip,
					 handle_level_irq);
		irq_set_nested_thread(cur_irq, 1);

		/* ARM needs us to explicitly flag the IRQ as valid
		 * and will set them noprobe when we do so. */
#ifdef CONFIG_ARM
		set_irq_flags(cur_irq, IRQF_VALID);
#else
		set_irq_noprobe(cur_irq);
#endif
	}

	ret = request_threaded_irq(twl6040->irq, NULL, twl6040_irq_thread,
				   IRQF_ONESHOT,
				   "twl6040", twl6040);
	if (ret) {
		dev_err(twl6040->dev, "failed to request IRQ %d: %d\n",
			twl6040->irq, ret);
		return ret;
	}

	/* reset interrupts */
	val = twl6040_reg_read(twl6040, TWL6040_REG_INTID);

	/* interrupts cleared on write */
	val = twl6040_reg_read(twl6040, TWL6040_REG_ACCCTL)
		& ~TWL6040_INTCLRMODE;
	twl6040_reg_write(twl6040, TWL6040_REG_ACCCTL, val);

	return 0;
}
EXPORT_SYMBOL(twl6040_irq_init);

void twl6040_irq_exit(struct twl6040 *twl6040)
{
	if (twl6040->irq)
		free_irq(twl6040->irq, twl6040);
}
EXPORT_SYMBOL(twl6040_irq_exit);
