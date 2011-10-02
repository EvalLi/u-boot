/*
 * Basic I2C functions
 *
 * Copyright (c) 2004 Texas Instruments
 *
 * This package is free software;  you can redistribute it and/or
 * modify it under the terms of the license found in the file
 * named COPYING that should have accompanied this file.
 *
 * THIS PACKAGE IS PROVIDED ``AS IS'' AND WITHOUT ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, WITHOUT LIMITATION, THE IMPLIED
 * WARRANTIES OF MERCHANTIBILITY AND FITNESS FOR A PARTICULAR PURPOSE.
 *
 * Author: Jian Zhang jzhang@ti.com, Texas Instruments
 *
 * Copyright (c) 2003 Wolfgang Denk, wd@denx.de
 * Rewritten to fit into the current U-Boot framework
 *
 * Adapted for OMAP2420 I2C, r-woodruff2@ti.com
 *
 */

#include <common.h>

#include <asm/arch/i2c.h>
#include <asm/io.h>

#include "omap24xx_i2c.h"

DECLARE_GLOBAL_DATA_PTR;

#define I2C_TIMEOUT	1000

static void wait_for_bb(void);
static u16 wait_for_pin(void);
static void flush_fifo(void);

static struct i2c *i2c_base = (struct i2c *)I2C_DEFAULT_BASE;

static unsigned int bus_initialized[I2C_BUS_MAX];
static unsigned int current_bus;

void i2c_init(int speed, int slaveadd)
{
	int psc, fsscll, fssclh;
	int hsscll = 0, hssclh = 0;
	u32 scll, sclh;
	int timeout = I2C_TIMEOUT;

	/* Only handle standard, fast and high speeds */
	if ((speed != OMAP_I2C_STANDARD) &&
	    (speed != OMAP_I2C_FAST_MODE) &&
	    (speed != OMAP_I2C_HIGH_SPEED)) {
		printf("Error : I2C unsupported speed %d\n", speed);
		return;
	}

	psc = I2C_IP_CLK / I2C_INTERNAL_SAMPLING_CLK;
	psc -= 1;
	if (psc < I2C_PSC_MIN) {
		printf("Error : I2C unsupported prescalar %d\n", psc);
		return;
	}

	if (speed == OMAP_I2C_HIGH_SPEED) {
		/* High speed */

		/* For first phase of HS mode */
		fsscll = fssclh = I2C_INTERNAL_SAMPLING_CLK /
			(2 * OMAP_I2C_FAST_MODE);

		fsscll -= I2C_HIGHSPEED_PHASE_ONE_SCLL_TRIM;
		fssclh -= I2C_HIGHSPEED_PHASE_ONE_SCLH_TRIM;
		if (((fsscll < 0) || (fssclh < 0)) ||
		    ((fsscll > 255) || (fssclh > 255))) {
			printf("Error : I2C initializing first phase clock\n");
			return;
		}

		/* For second phase of HS mode */
		hsscll = hssclh = I2C_INTERNAL_SAMPLING_CLK / (2 * speed);

		hsscll -= I2C_HIGHSPEED_PHASE_TWO_SCLL_TRIM;
		hssclh -= I2C_HIGHSPEED_PHASE_TWO_SCLH_TRIM;
		if (((fsscll < 0) || (fssclh < 0)) ||
		    ((fsscll > 255) || (fssclh > 255))) {
			printf("Error : I2C initializing second phase clock\n");
			return;
		}

		scll = (unsigned int)hsscll << 8 | (unsigned int)fsscll;
		sclh = (unsigned int)hssclh << 8 | (unsigned int)fssclh;

	} else {
		/* Standard and fast speed */
		fsscll = fssclh = I2C_INTERNAL_SAMPLING_CLK / (2 * speed);

		fsscll -= I2C_FASTSPEED_SCLL_TRIM;
		fssclh -= I2C_FASTSPEED_SCLH_TRIM;
		if (((fsscll < 0) || (fssclh < 0)) ||
		    ((fsscll > 255) || (fssclh > 255))) {
			printf("Error : I2C initializing clock\n");
			return;
		}

		scll = (unsigned int)fsscll;
		sclh = (unsigned int)fssclh;
	}

	if (readw(I2C_CON) & I2C_CON_EN) {
		writew(0, I2C_CON);
		udelay(50000);
	}

	writew(0x2, I2C_SYSC); /* for ES2 after soft reset */
	udelay(1000);

	writew(I2C_CON_EN, I2C_CON);
	while (!(readw(I2C_SYSS) & I2C_SYSS_RDONE) && timeout--) {
		if (timeout <= 0) {
			printf("ERROR: Timeout in soft-reset\n");
			return;
		}
		udelay(1000);
	}

	writew(psc, I2C_PSC);
	writew(scll, I2C_SCLL);
	writew(sclh, I2C_SCLH);

	/* own address */
	writew(slaveadd, I2C_OA);
	writew(I2C_CON_EN, I2C_CON);

	/* have to enable intrrupts or OMAP i2c module doesn't work */
	writew(I2C_IE_XRDY_IE | I2C_IE_RRDY_IE | I2C_IE_ARDY_IE |
		I2C_IE_NACK_IE | I2C_IE_AL_IE, I2C_IE);
	udelay(1000);
	flush_fifo();
	writew(0xFFFF, I2C_STAT);
	writew(0, I2C_CNT);

	if (gd->flags & GD_FLG_RELOC)
		bus_initialized[current_bus] = 1;
}

static int i2c_read_byte(u8 devaddr, u8 regoffset, u8 *value)
{
	int i2c_error = 0;
	u16 status;

	/* wait until bus not busy */
	wait_for_bb();

	/* one byte only */
	writew(1, I2C_CNT);
	/* set slave address */
	writew (devaddr, I2C_SA);
	/* no stop bit needed here */
	writew(I2C_CON_EN | I2C_CON_MST | I2C_CON_STT |
	      I2C_CON_TRX, I2C_CON);

	/* send register offset */
	while (1) {
		status = wait_for_pin();
		if (status == 0 || status & I2C_STAT_NACK) {
			i2c_error = 1;
			goto read_exit;
		}
		if (status & I2C_STAT_XRDY) {
			/* Important: have to use byte access */
			writeb(regoffset, I2C_DATA);
			writew(I2C_STAT_XRDY, I2C_STAT);
		}
		if (status & I2C_STAT_ARDY) {
			writew(I2C_STAT_ARDY, I2C_STAT);
			break;
		}
	}

	/* set slave address */
	writew(devaddr, I2C_SA);
	/* read one byte from slave */
	writew(1, I2C_CNT);
	/* need stop bit here */
	writew(I2C_CON_EN | I2C_CON_MST |
		I2C_CON_STT | I2C_CON_STP,
		I2C_CON);

	/* receive data */
	while (1) {
		status = wait_for_pin();
		if (status == 0 || status & I2C_STAT_NACK) {
			i2c_error = 1;
			goto read_exit;
		}
		if (status & I2C_STAT_RRDY) {
#if defined(CONFIG_OMAP243X) || defined(CONFIG_OMAP34XX) || \
	defined(CONFIG_OMAP44XX)
			*value = readb(I2C_DATA);
#else
			*value = readw(I2C_DATA);
#endif
			writew(I2C_STAT_RRDY, I2C_STAT);
		}
		if (status & I2C_STAT_ARDY) {
			writew(I2C_STAT_ARDY, I2C_STAT);
			break;
		}
	}

read_exit:
	flush_fifo();
	writew(0xFFFF, I2C_STAT);
	writew(0, I2C_CNT);
	return i2c_error;
}

static void flush_fifo(void)
{	u16 stat;

	/* note: if you try and read data when its not there or ready
	 * you get a bus error
	 */
	while (1) {
		stat = readw(I2C_STAT);
		if (stat == I2C_STAT_RRDY) {
#if defined(CONFIG_OMAP243X) || defined(CONFIG_OMAP34XX) || \
	defined(CONFIG_OMAP44XX) || defined(CONFIG_TI81XX)
			readb(I2C_DATA);
#else
			readw(I2C_DATA);
#endif
			writew(I2C_STAT_RRDY, I2C_STAT);
			udelay(1000);
		} else
			break;
	}
}

int i2c_probe(uchar chip)
{
	u16 status;
	int res = 1; /* default = fail */

	if (chip == readw(I2C_OA))
		return res;

	/* wait until bus not busy */
	wait_for_bb();

	/* try to write one byte */
	writew(1, I2C_CNT);
	/* set slave address */
	writew(chip, I2C_SA);
	/* stop bit needed here */
	writew(I2C_CON_EN | I2C_CON_MST | I2C_CON_STT | I2C_CON_TRX |
	       I2C_CON_STP, I2C_CON);

	status = wait_for_pin();

	/* check for ACK (!NAK) */
	if (!(status & I2C_STAT_NACK))
		res = 0;

	/* abort transfer (force idle state) */
	writew(0, I2C_CON);

	flush_fifo();
	/* don't allow any more data in... we don't want it. */
	writew(0, I2C_CNT);
	writew(0xFFFF, I2C_STAT);
	return res;
}

int i2c_read(uchar chip, uint addr, int alen, uchar *buffer, int len)
{
	int i;

	if (alen > 1) {
		printf("I2C read: addr len %d not supported\n", alen);
		return 1;
	}

	if (addr + len > 256) {
		printf("I2C read: address out of range\n");
		return 1;
	}

	for (i = 0; i < len; i++) {
		if (i2c_read_byte(chip, addr + i, &buffer[i])) {
			printf("I2C read: I/O error\n");
			i2c_init(CONFIG_SYS_I2C_SPEED, CONFIG_SYS_I2C_SLAVE);
			return 1;
		}
	}

	return 0;
}

int i2c_write(uchar chip, uint addr, int alen, uchar *buffer, int len)
{
	int i;
	u16 status;
	int i2c_error = 0;

	if (alen > 1) {
		printf("I2C write: addr len %d not supported\n", alen);
		return 1;
	}

	if (addr + len > 256) {
		printf("I2C write: address 0x%x + 0x%x out of range\n",
				addr, len);
		return 1;
	}

	/* wait until bus not busy */
	wait_for_bb();

	/* start address phase - will write regoffset + len bytes data */
	/* TODO consider case when !CONFIG_OMAP243X/34XX/44XX */
	writew(alen + len, I2C_CNT);
	/* set slave address */
	writew(chip, I2C_SA);
	/* stop bit needed here */
	writew(I2C_CON_EN | I2C_CON_MST | I2C_CON_STT | I2C_CON_TRX |
		I2C_CON_STP, I2C_CON);

	/* Send address byte */
	status = wait_for_pin();

	if (status == 0 || status & I2C_STAT_NACK) {
		i2c_error = 1;
		printf("error waiting for i2c address ACK (status=0x%x)\n",
		      status);
		goto write_exit;
	}

	if (status & I2C_STAT_XRDY) {
		writeb(addr & 0xFF, I2C_DATA);
		writew(I2C_STAT_XRDY, I2C_STAT);
	} else {
		i2c_error = 1;
		printf("i2c bus not ready for transmit (status=0x%x)\n",
		      status);
		goto write_exit;
	}

	/* address phase is over, now write data */
	for (i = 0; i < len; i++) {
		status = wait_for_pin();

		if (status == 0 || status & I2C_STAT_NACK) {
			i2c_error = 1;
			printf("i2c error waiting for data ACK (status=0x%x)\n",
					status);
			goto write_exit;
		}

		if (status & I2C_STAT_XRDY) {
			writeb(buffer[i], I2C_DATA);
			writew(I2C_STAT_XRDY, I2C_STAT);
		} else {
			i2c_error = 1;
			printf("i2c bus not ready for Tx (i=%d)\n", i);
			goto write_exit;
		}
	}

write_exit:
	flush_fifo();
	writew(0xFFFF, I2C_STAT);
	return i2c_error;
}

static void wait_for_bb(void)
{
	int timeout = I2C_TIMEOUT;
	u16 stat;

	writew(0xFFFF, I2C_STAT);	/* clear current interrupts...*/
	while ((stat = readw(I2C_STAT) & I2C_STAT_BB) && timeout--) {
		writew(stat, I2C_STAT);
		udelay(50000);
	}

	if (timeout <= 0) {
		printf("timed out in wait_for_bb: I2C_STAT=%x\n",
			readw(I2C_STAT));
	}
	writew(0xFFFF, I2C_STAT);	 /* clear delayed stuff*/
}

static u16 wait_for_pin(void)
{
	u16 status;
	int timeout = I2C_TIMEOUT;

	do {
		udelay(1000);
		status = readw(I2C_STAT);
	} while (!(status &
		   (I2C_STAT_ROVR | I2C_STAT_XUDF | I2C_STAT_XRDY |
		    I2C_STAT_RRDY | I2C_STAT_ARDY | I2C_STAT_NACK |
		    I2C_STAT_AL)) && timeout--);

	if (timeout <= 0) {
		printf("timed out in wait_for_pin: I2C_STAT=%x\n",
			readw(I2C_STAT));
		writew(0xFFFF, I2C_STAT);
		status = 0;
	}

	return status;
}

int i2c_set_bus_num(unsigned int bus)
{
	if ((bus < 0) || (bus >= I2C_BUS_MAX)) {
		printf("Bad bus: %d\n", bus);
		return -1;
	}

#if I2C_BUS_MAX == 3
	if (bus == 2)
		i2c_base = (struct i2c *)I2C_BASE3;
	else
#endif
	if (bus == 1)
		i2c_base = (struct i2c *)I2C_BASE2;
	else
		i2c_base = (struct i2c *)I2C_BASE1;

	current_bus = bus;

	if (!bus_initialized[current_bus])
		i2c_init(CONFIG_SYS_I2C_SPEED, CONFIG_SYS_I2C_SLAVE);

	return 0;
}

int i2c_get_bus_num(void)
{
	return (int) current_bus;
}
