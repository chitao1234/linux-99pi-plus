/*
 * LOONGSON I2C controller
 *
 * Copyright (C) 2024 Loongson Technology Corporation Limited
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/reset.h>

/* Loongson I2C offset registers */
#define I2C_CR1		0x00
#define I2C_CR2		0x04
#define I2C_DR		0x10
#define I2C_SR1		0x14
#define I2C_SR2		0x18
#define I2C_CCR		0x1C
#define I2C_TRISE		0x20
#define I2C_FLTR		0x24

/* Loongson I2C control 1*/
#define I2C_CR1_POS		BIT(11)
#define I2C_CR1_ACK		BIT(10)
#define I2C_CR1_STOP		BIT(9)
#define I2C_CR1_START	BIT(8)
#define I2C_CR1_PE		BIT(0)

/* Loongson I2C control 2 */
#define I2C_CR2_FREQ_MASK	GENMASK(5, 0)
#define I2C_CR2_FREQ(n)	((n) & I2C_CR2_FREQ_MASK)
#define I2C_CR2_ITBUFEN	BIT(10)
#define I2C_CR2_ITEVTEN	BIT(9)
#define I2C_CR2_ITERREN	BIT(8)
#define I2C_CR2_IRQ_MASK	(I2C_CR2_ITBUFEN | \
				 I2C_CR2_ITEVTEN | \
				 I2C_CR2_ITERREN)

/* Loongson I2C Status 1 */
#define I2C_SR1_AF		BIT(10)
#define I2C_SR1_ARLO		BIT(9)
#define I2C_SR1_BERR		BIT(8)
#define I2C_SR1_TXE		BIT(7)
#define I2C_SR1_RXNE		BIT(6)
#define I2C_SR1_BTF		BIT(2)
#define I2C_SR1_ADDR		BIT(1)
#define I2C_SR1_SB		BIT(0)
#define I2C_SR1_ITEVTEN_MASK	(I2C_SR1_BTF | \
				 I2C_SR1_ADDR | \
				 I2C_SR1_SB)
#define I2C_SR1_ITBUFEN_MASK	(I2C_SR1_TXE | \
				 I2C_SR1_RXNE)
#define I2C_SR1_ITERREN_MASK	(I2C_SR1_AF | \
				 I2C_SR1_ARLO | \
				 I2C_SR1_BERR)

/* Loongson I2C Status 2 */
#define I2C_SR2_BUSY		BIT(1)

/* Loongson I2C Control Clock */
#define I2C_CCR_FS		BIT(15)
#define I2C_CCR_DUTY		BIT(14)

enum ls_i2c_speed {
	LS_I2C_SPEED_STANDARD,	/* 100 kHz */
	LS_I2C_SPEED_FAST,	/* 400 kHz */
};

/**
 * struct priv_msg - client specific data
 * @addr: 8-bit slave addr, including r/w bit
 * @count: number of bytes to be transferred
 * @buf: data buffer
 * @stop: last I2C msg to be sent, i.e. STOP to be generated
 * @result: result of the transfer
 */
struct priv_msg {
	u8 addr;
	u32 count;
	u8 *buf;
	bool stop;
	int result;
};

/**
 * struct ls_i2c_dev - private data of the controller
 * @adap: I2C adapter for this controller
 * @dev: device for this controller
 * @base: virtual memory area
 * @complete: completion of I2C message
 * @speed: Standard or Fast are supported
 * @msg: I2C transfer information
 */
struct ls_i2c_dev {
	struct i2c_adapter adap;
	struct device *dev;
	void __iomem *base;
	struct completion complete;
	int speed;
	struct priv_msg msg;
};

static inline void i2c_set_bits(void __iomem *reg, u32 mask)
{
	writel(readl(reg) | mask, reg);
}

static inline void i2c_clr_bits(void __iomem *reg, u32 mask)
{
	writel(readl(reg) & ~mask, reg);
}

static void ls_i2c_disable_irq(struct ls_i2c_dev *i2c_dev)
{
	void __iomem *reg = i2c_dev->base + I2C_CR2;

	i2c_clr_bits(reg, I2C_CR2_IRQ_MASK);
}

#define INPUT_REF_CLK 100000000

static int ls_i2c_hw_config(struct ls_i2c_dev *i2c_dev)
{
	u32 val;
	u32 ccr = 0;

	/* reference clock determination the cnfigure val(0x3f) */
	i2c_set_bits(i2c_dev->base + I2C_CR2, 0x3f);
	i2c_set_bits(i2c_dev->base + I2C_TRISE, 0x3f);

	if (i2c_dev->speed == LS_I2C_SPEED_STANDARD) {
		val = DIV_ROUND_UP(INPUT_REF_CLK, 100000 * 2);
	} else {
		val = DIV_ROUND_UP(INPUT_REF_CLK, 400000 * 3);

		/* Select Fast mode */
		ccr |= I2C_CCR_FS;
	}
	ccr |= val & 0xfff;
	writel(ccr, i2c_dev->base + I2C_CCR);

	/* Enable I2C */
	writel(I2C_CR1_PE, i2c_dev->base + I2C_CR1);

	return 0;
}

static int ls_i2c_wait_free_bus(struct ls_i2c_dev *i2c_dev)
{
	u32 status;
	int ret;

	ret = readl_poll_timeout(i2c_dev->base + I2C_SR2,
					 status,
					 !(status & I2C_SR2_BUSY),
					 10, 1000);
	if (ret) {
		dev_dbg(i2c_dev->dev, "bus not free\n");
		ret = -EBUSY;
	}

	return ret;
}

static void ls_i2c_write_byte(struct ls_i2c_dev *i2c_dev, u8 byte)
{
	writel(byte, i2c_dev->base + I2C_DR);
}

static void ls_i2c_read_msg(struct ls_i2c_dev *i2c_dev)
{
	struct priv_msg *msg = &i2c_dev->msg;

	*msg->buf++ = readl(i2c_dev->base + I2C_DR);
	msg->count--;
}

static void ls_i2c_terminate_xfer(struct ls_i2c_dev *i2c_dev)
{
	struct priv_msg *msg = &i2c_dev->msg;

	ls_i2c_disable_irq(i2c_dev);
	i2c_set_bits(i2c_dev->base + I2C_CR1, (1 << (8 + msg->stop)));
	complete(&i2c_dev->complete);
}

static void ls_i2c_handle_write(struct ls_i2c_dev *i2c_dev)
{
	struct priv_msg *msg = &i2c_dev->msg;
	void __iomem *reg = i2c_dev->base + I2C_CR2;

	if (msg->count) {
		ls_i2c_write_byte(i2c_dev, *msg->buf++);
		msg->count--;
		if (!msg->count)
			i2c_clr_bits(reg, I2C_CR2_ITBUFEN);
	} else {
		ls_i2c_terminate_xfer(i2c_dev);
	}
}

static void ls_i2c_handle_read(struct ls_i2c_dev *i2c_dev, int flag)
{
	struct priv_msg *msg = &i2c_dev->msg;
	void __iomem *reg = i2c_dev->base + I2C_CR2;
	int i;

	switch (msg->count) {
	case 1:
		/* only transmit 1 bytes condition */
		ls_i2c_disable_irq(i2c_dev);
		ls_i2c_read_msg(i2c_dev);
		complete(&i2c_dev->complete);
		break;
	case 2:
		if (flag != 1) {
			/* ensure only transmit 2 bytes condition */
			break;
		}
		i2c_set_bits(i2c_dev->base + I2C_CR1, (1 << (8 + msg->stop)));

		ls_i2c_disable_irq(i2c_dev);

		for (i = 2; i > 0; i--)
			ls_i2c_read_msg(i2c_dev);

		i2c_clr_bits(i2c_dev->base + I2C_CR1, I2C_CR1_POS);
		complete(&i2c_dev->complete);
		break;
	case 3:
		if (flag != 1)
			break;
		reg = i2c_dev->base + I2C_CR1;
		i2c_clr_bits(reg, I2C_CR1_ACK);
	default:
		ls_i2c_read_msg(i2c_dev);
	}
}

/**
 * ls_i2c_handle_rx_addr()
 * master receiver
 * @i2c_dev: Controller's private data
 */
static void ls_i2c_handle_rx_addr(struct ls_i2c_dev *i2c_dev)
{
	struct priv_msg *msg = &i2c_dev->msg;

	switch (msg->count) {
	case 0:
		ls_i2c_terminate_xfer(i2c_dev);
		break;
	case 1:
		i2c_clr_bits(i2c_dev->base + I2C_CR1,
				(I2C_CR1_ACK | I2C_CR1_POS));
		/* start or stop */
		i2c_set_bits(i2c_dev->base + I2C_CR1, (1 << (8 + msg->stop)));
		break;
	case 2:
		i2c_clr_bits(i2c_dev->base + I2C_CR1, I2C_CR1_ACK);
		i2c_set_bits(i2c_dev->base + I2C_CR1, I2C_CR1_POS);
		break;

	default:
		i2c_set_bits(i2c_dev->base + I2C_CR1, I2C_CR1_ACK);
		i2c_clr_bits(i2c_dev->base + I2C_CR1, I2C_CR1_POS);
		break;
	}
}

static irqreturn_t ls_i2c_isr_error(u32 status, void *data)
{
	struct ls_i2c_dev *i2c_dev = data;
	struct priv_msg *msg = &i2c_dev->msg;

	/* Arbitration lost */
	if (status & I2C_SR1_ARLO) {
		i2c_clr_bits(i2c_dev->base + I2C_SR1, I2C_SR1_ARLO);
		msg->result = -EAGAIN;
	}

	/*
	 * Acknowledge failure:
	 * In master transmitter mode a Stop must be generated by software
	 */
	if (status & I2C_SR1_AF) {
		i2c_set_bits(i2c_dev->base + I2C_CR1, I2C_CR1_STOP);
		i2c_clr_bits(i2c_dev->base + I2C_SR1, I2C_SR1_AF);
		msg->result = -EIO;
	}

	/* Bus error */
	if (status & I2C_SR1_BERR) {
		i2c_clr_bits(i2c_dev->base + I2C_SR1, I2C_SR1_BERR);
		msg->result = -EIO;
	}

	ls_i2c_disable_irq(i2c_dev);
	complete(&i2c_dev->complete);

	return IRQ_HANDLED;
}

/**
 * ls_i2c_isr_event() - Interrupt routine for I2C bus event
 * @irq: interrupt number
 * @data: Controller's private data
 */
static irqreturn_t ls_i2c_isr_event(int irq, void *data)
{
	struct ls_i2c_dev *i2c_dev = data;
	struct priv_msg *msg = &i2c_dev->msg;
	u32 status, ien, event, cr2;
	u32 possible_status = I2C_SR1_ITEVTEN_MASK;
	int i;

	status = readl(i2c_dev->base + I2C_SR1);
	if (status & (I2C_SR1_ARLO | I2C_SR1_AF | I2C_SR1_BERR))
		return ls_i2c_isr_error(status, data);

	cr2 = readl(i2c_dev->base + I2C_CR2);
	ien = cr2 & I2C_CR2_IRQ_MASK;

	/* Update possible_status if buffer interrupt is enabled */
	if (ien & I2C_CR2_ITBUFEN)
		possible_status |= I2C_SR1_ITBUFEN_MASK;
	event = status & possible_status;

	if (!event) {
		dev_dbg(i2c_dev->dev,
			"spurious evt irq (status=0x%08x, ien=0x%08x)\n",
			status, ien);
		return IRQ_NONE;
	}

	/* Start condition generated */
	if (event & I2C_SR1_SB)
		ls_i2c_write_byte(i2c_dev, msg->addr);

	/* I2C Address sent */
	if (event & I2C_SR1_ADDR) {
		if (msg->addr & I2C_M_RD)
			ls_i2c_handle_rx_addr(i2c_dev);
		/* Clear ADDR flag */
		readl(i2c_dev->base + I2C_SR2);
		/* Enable buffer interrupts for RX/TX not empty events */
		i2c_set_bits(i2c_dev->base + I2C_CR2, I2C_CR2_ITBUFEN);
	}

	if (msg->addr & I2C_M_RD) {
		/* RX */
		if (event & 0x4)
			ls_i2c_handle_read(i2c_dev, 1);
		 if (event & 0x40)
			 ls_i2c_handle_read(i2c_dev, 0);
	} else {
		/* TX */
		if (event & 0x84)
			for (i = 0; i < 1 + !!((event & 0x84) == 0x84); i++)
				ls_i2c_handle_write(i2c_dev);
	}

	return IRQ_HANDLED;
}

static int ls_i2c_xfer_msg(struct ls_i2c_dev *i2c_dev,
				struct i2c_msg *msg, bool is_stop)
{
	struct priv_msg *priv_msg = &i2c_dev->msg;
	unsigned long timeout;
	int ret;

	priv_msg->addr   = i2c_8bit_addr_from_msg(msg);
	priv_msg->buf    = msg->buf;
	priv_msg->count  = msg->len;
	priv_msg->stop   = is_stop;
	priv_msg->result = 0;

	reinit_completion(&i2c_dev->complete);

	/* Enable events and errors interrupts */
	i2c_set_bits(i2c_dev->base + I2C_CR2,
			I2C_CR2_ITEVTEN | I2C_CR2_ITERREN);

	timeout = wait_for_completion_timeout(&i2c_dev->complete,
					      i2c_dev->adap.timeout);
	ret = priv_msg->result;

	if (!timeout)
		ret = -ETIMEDOUT;

	return ret;
}

/**
 * ls_i2c_xfer() - Transfer combined I2C message
 * @i2c_adap: Adapter pointer to the controller
 * @msgs: Pointer to data to be written.
 * @num: Number of messages to be executed
 */
static int ls_i2c_xfer(struct i2c_adapter *i2c_adap, struct i2c_msg msgs[],
			    int num)
{
	struct ls_i2c_dev *i2c_dev = i2c_get_adapdata(i2c_adap);
	int ret = 0;
	int i;
#if 0
	u32 sr1;
	sr1 = readl(i2c_dev->base + I2C_SR1);
	if(sr1){
		printk("i2c sr1 has error :%x\n",sr1);
		/* Enable events and errors interrupts */
		i2c_set_bits(i2c_dev->base + I2C_CR2,
			I2C_CR2_ITEVTEN | I2C_CR2_ITERREN);
		msleep(10);
		sr1 = readl(i2c_dev->base + I2C_SR1);
		printk("i2c sr1 now is :%x\n",sr1);
	}
#endif
	ret = ls_i2c_wait_free_bus(i2c_dev);
	if (ret)
		return ret;
	/* START generation */
	i2c_set_bits(i2c_dev->base + I2C_CR1, I2C_CR1_START);

	for (i = 0; i < num && !ret; i++)
		ret = ls_i2c_xfer_msg(i2c_dev, &msgs[i], i == num - 1);

	return (ret < 0) ? ret : num;
}

static u32 ls_i2c_func(struct i2c_adapter *adap)
{
	return I2C_FUNC_I2C | I2C_FUNC_SMBUS_EMUL;
}

static const struct i2c_algorithm ls_i2c_algo = {
	.master_xfer = ls_i2c_xfer,
	.functionality = ls_i2c_func,
};

static int ls_i2c_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct ls_i2c_dev *i2c_dev;
	struct resource *res;
	u32 irq_event, clk_rate;
	struct i2c_adapter *adap;
	int ret;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		dev_err(&pdev->dev, "no mem resource?\n");
		return -ENODEV;
	}

	irq_event = platform_get_irq(pdev, 0);
	if (irq_event <= 0) {
		dev_err(&pdev->dev, "IRQ event missing or invalid\n");
		return -EINVAL;
	}

	i2c_dev = devm_kzalloc(&pdev->dev, sizeof(*i2c_dev), GFP_KERNEL);
	if (!i2c_dev)
		return -ENOMEM;

	i2c_dev->base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(i2c_dev->base)) {
		ret = PTR_ERR(i2c_dev->base);
		goto free_i2c_mem;
	}

	i2c_dev->speed = LS_I2C_SPEED_STANDARD;
	of_property_read_u32(np, "i2c-speed", &clk_rate);
	if (!ret && clk_rate >= 400000)
		i2c_dev->speed = LS_I2C_SPEED_FAST;

	init_completion(&i2c_dev->complete);

	i2c_dev->dev = &pdev->dev;

	ret = request_irq(irq_event,  ls_i2c_isr_event, IRQF_SHARED,
			       pdev->name, i2c_dev);
	if (ret) {
		dev_err(&pdev->dev, "Failed to request irq event %i\n",
			irq_event);
		goto free_i2c_ioremap;
	}

	ls_i2c_hw_config(i2c_dev);

	adap = &i2c_dev->adap;
	i2c_set_adapdata(adap, i2c_dev);
	adap->nr = pdev->id;
	strscpy(adap->name, pdev->name, sizeof(adap->name));
	adap->owner = THIS_MODULE;
	adap->retries = 5;
	adap->algo = &ls_i2c_algo;
	adap->dev.parent = &pdev->dev;
	adap->dev.of_node = pdev->dev.of_node;
	adap->timeout = 2 * HZ;

	ret = i2c_add_adapter(adap);
	if (ret) {
		dev_err(&pdev->dev, "failure adding adapter\n");
		goto free_i2c_ioremap;
	}

	platform_set_drvdata(pdev, i2c_dev);

	dev_info(&pdev->dev,"driver initialized\n");

	return 0;

free_i2c_ioremap:
	devm_iounmap(&pdev->dev, i2c_dev->base);
free_i2c_mem:
	kfree(i2c_dev);

	return ret;
}

static int ls_i2c_remove(struct platform_device *pdev)
{
	struct ls_i2c_dev *i2c_dev = platform_get_drvdata(pdev);

	platform_set_drvdata(pdev, NULL);
	i2c_del_adapter(&i2c_dev->adap);
	iounmap(i2c_dev->base);
	kfree(i2c_dev);

	return 0;
}

static const struct of_device_id ls_i2c_match[] = {
	{.compatible = "loongson,ls-new-i2c"},
	{},
};
MODULE_DEVICE_TABLE(of, ls_i2c_match);

static struct platform_driver ls_i2c_driver = {
	.driver = {
		.name = "ls-i2c",
		.of_match_table = ls_i2c_match,
	},
	.probe = ls_i2c_probe,
	.remove = ls_i2c_remove,
};

module_platform_driver(ls_i2c_driver);

MODULE_AUTHOR("Loongson Technology Corporation Limited");
MODULE_DESCRIPTION("Loongson fast I2C bus adapter");
MODULE_LICENSE("GPL");
