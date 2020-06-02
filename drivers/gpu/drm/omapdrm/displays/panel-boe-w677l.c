// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Driver for BOE W677L panel with Orise OTM1283A controller
 *
 * Copyright 2011 Texas Instruments, Inc.
 * Author: Archit Taneja <archit@ti.com>
 * Author: Tomi Valkeinen <tomi.valkeinen@ti.com>
 *
 * based on d2l panel driver by Jerry Alexander <x0135174@ti.com>
 *
 * Copyright (C) 2014-2019 Golden Delicious Computers
 * by H. Nikolaus Schaller <hns@goldelico.com>
 * based on lg4591 panel driver
 * harmonized with latest panel-dsi-cm.c
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#define LOG 0
#define OPTIONAL 0

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/fb.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

#include <drm/drm_connector.h>

#include <video/mipi_display.h>
#include <video/of_display_timing.h>

#include "../dss/omapdss.h"

#if LOG
#undef dev_dbg
#define dev_dbg dev_err
#endif

/* extended DCS commands (not defined in mipi_display.h) */
#define DCS_READ_DDB_START		0x02
#define DCS_READ_NUM_ERRORS		0x05
#define DCS_BRIGHTNESS			0x51	/* write brightness */
#define DCS_READ_BRIGHTNESS		0x52	/* read brightness */
#define DCS_CTRL_DISPLAY		0x53	/* enable backlight etc. */
#define DCS_READ_CTRL_DISPLAY	0x53	/* read control */
#define DCS_WRITE_CABC			0x55
#define DCS_READ_CABC			0x56
#define MCS_READID1		0xda
#define MCS_READID2		0xdb
#define MCS_READID3		0xdc

/* DCS NOP is 1-byte 0x00 while 2-byte 0x00 is address shift function of this controller */

#define IS_MCS(CMD, LEN) ((((CMD) == 0) && (LEN) == 2) || (((CMD) >= 0xb0 && (CMD) < 0xda)) || ((CMD) > 0xdc))

/* horizontal * vertical * refresh */
#define w677l_W				(720)
#define w677l_H				(1280)
#define w677l_WIDTH			(w677l_W+80+88)
#define w677l_HEIGHT			(w677l_H+160)
#define w677l_FPS				(60ll)
#define w677l_PIXELCLOCK		(w677l_WIDTH * w677l_HEIGHT * w677l_FPS)	/* Full HD * 60 fps */
/* panel has 16.7M colors = RGB888 = 3*8 bit per pixel */
#define w677l_PIXELFORMAT		OMAP_DSS_DSI_FMT_RGB888	/* 16.7M color = RGB888 */
#define w677l_BIT_PER_PIXEL	(3*8)
/* the panel can handle 4 lanes */
#define w677l_LANES			4
/* high speed clock is running at double data rate, i.e. half speed
 * (take care of integer overflows!)
 * hsck =  bit/pixel * 110% * pixel clock / lanes / 2 clock edges
 * real clock rate may be rounded up or down depending on divisors
 */
#define w677l_HS_CLOCK			(w677l_BIT_PER_PIXEL * (w677l_PIXELCLOCK / (w677l_LANES * 2)))
/* low power clock is quite arbitrarily choosen to be roughly 10 MHz */
#define w677l_LP_CLOCK			9200000	/* low power clock */

static struct videomode w677l_timings = {
	.hactive		= w677l_W,
	.vactive		= w677l_H,
	.pixelclock	= w677l_PIXELCLOCK,
	.hfront_porch		= 5,
	.hsync_len		= 5,
	.hback_porch		= w677l_WIDTH-w677l_W-5-5,
	.vfront_porch		= 50,
	.vsync_len		= w677l_HEIGHT-w677l_H-50-50,
	.vback_porch		= 50,
};

struct panel_drv_data {
	struct omap_dss_device dssdev;
	struct omap_dss_device *src;

	struct videomode vm;

	struct backlight_device *backlight;

	struct platform_device *pdev;

	struct mutex lock;

	int reset_gpio;
	int regulator_gpio;

	struct omap_dsi_pin_config pin_config;

	bool enabled;

	int config_channel;
	int pixel_channel;

};

#define to_panel_data(p) container_of(p, struct panel_drv_data, dssdev)

typedef u8 w677l_reg[20];	/* data[0] is length, data[1] is first byte */

static w677l_reg init_seq[] = {
	{ 2, 0x00, 0x00, },
	{ 4, 0xff, 0x12, 0x83, 0x01, },	//EXTC=1
	{ 2, 0x00, 0x80, },	            //Orise mode enable
	{ 3, 0xff, 0x12, 0x83, },


//-------------------- panel setting --------------------//
	{ 2, 0x00, 0x80, },              //TCON Setting
	{ 10, 0xc0, 0x00, 0x64, 0x00, 0x0f, 0x11, 0x00, 0x64, 0x0f, 0x11, },

	{ 2, 0x00, 0x90, },             //Panel Timing Setting
	{ 7, 0xc0, 0x00, 0x5c, 0x00, 0x01, 0x00, 0x04, },

	{ 2, 0x00, 0x87, },
	{ 2, 0xc4, 0x18, },

	{ 2, 0x00, 0xb3, },            //Interval Scan Frame: 0 frame,  column inversion
	{ 3, 0xc0, 0x00, 0x50, },

	{ 2, 0x00, 0x81, },              //frame rate:60Hz
	{ 2, 0xc1, 0x66, },

	{ 2, 0x00, 0x81, },
	{ 3, 0xc4, 0x82, 0x02, },

	{ 2, 0x00, 0x90, },
	{ 2, 0xc4, 0x49, },

	{ 2, 0x00, 0xc6, },
	{ 2, 0xb0, 0x03, },

	{ 2, 0x00, 0x90, },             //Mode-3
	{ 5, 0xf5, 0x02, 0x11, 0x02, 0x11, },

	{ 2, 0x00, 0x90, },		//2xVPNL,  1.5*=00,  2*=50,  3*=a0
	{ 2, 0xc5, 0x50, },

	{ 2, 0x00, 0x94, },		//Frequency
	{ 2, 0xc5, 0x66, },

	{ 2, 0x00, 0xb2, },		//VGLO1 setting
	{ 3, 0xf5, 0x00, 0x00, },

	{ 2, 0x00, 0xb4, },		//VGLO1_S setting
	{ 3, 0xf5, 0x00, 0x00, },

	{ 2, 0x00, 0xb6, },		//VGLO2 setting
	{ 3, 0xf5, 0x00, 0x00, },

	{ 2, 0x00, 0xb8, },		//VGLO2_S setting
	{ 3, 0xf5, 0x00, 0x00, },

	{ 2, 0x00, 0x94, },		//VCL ON
	{ 2, 0xf5, 0x02, },

	{ 2, 0x00, 0xBA, },		//VSP ON
	{ 2, 0xf5, 0x03, },

	{ 2, 0x00, 0xb2, },		//VGHO Option
	{ 2, 0xc5, 0x40, },

	{ 2, 0x00, 0xb4, },		//VGLO Option
	{ 2, 0xc5, 0xC0, },

//-------------------- power setting --------------------//
	{ 2, 0x00, 0xa0, },		//dcdc setting
	{ 15, 0xc4, 0x05, 0x10, 0x06, 0x02, 0x05, 0x15, 0x10, 0x05, 0x10, 0x07, 0x02, 0x05, 0x15, 0x10, },

	{ 2, 0x00, 0xb0, },		//clamp voltage setting
	{ 3, 0xc4, 0x00, 0x00, },

	{ 2, 0x00, 0x91, },		//VGH=13V,  VGL=-12V,  pump ratio:VGH=6x,  VGL=-5x
	{ 3, 0xc5, 0x19, 0x50, },

	{ 2, 0x00, 0x00, },		//GVDD=4.87V,  NGVDD=-4.87V
	{ 3, 0xd8, 0xbc, 0xbc, },

	{ 2, 0x00, 0x00, },		//VCOMDC=-1.1
	{ 2, 0xd9, 0x5a, },  		//5d  6f

	{ 2, 0x00, 0x00, },
	{ 17, 0xE1, 0x01, 0x07, 0x0b, 0x0d, 0x06, 0x0d, 0x0b, 0x0a, 0x04, 0x07, 0x10, 0x08, 0x0f, 0x11, 0x0a, 0x01, },

	{ 2, 0x00, 0x00, },
	{ 17, 0xE2, 0x01, 0x07, 0x0b, 0x0d, 0x06, 0x0d, 0x0b, 0x0a, 0x04, 0x07, 0x10, 0x08, 0x0f, 0x11, 0x0a, 0x01, },

	{ 2, 0x00, 0xb0, },		//VDD_18V=1.7V,  LVDSVDD=1.55V
	{ 3, 0xc5, 0x04, 0xB8, },

	{ 2, 0x00, 0xbb, },		//LVD voltage level setting
	{ 2, 0xc5, 0x80, },

//-------------------- panel timing state control --------------------//
	{ 2, 0x00, 0x80, },		//panel timing state control
	{ 12, 0xcb, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, },

	{ 2, 0x00, 0x90, },		//panel timing state control
	{ 16, 0xcb, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, },

	{ 2, 0x00, 0xa0, },		//panel timing state control
	{ 16, 0xcb, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, },

	{ 2, 0x00, 0xb0, },		//panel timing state control
	{ 16, 0xcb, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, },

	{ 2, 0x00, 0xc0, },		//panel timing state control
	{ 16, 0xcb, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, },

	{ 2, 0x00, 0xd0, },		//panel timing state control
	{ 16, 0xcb, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x00, 0x00, },

	{ 2, 0x00, 0xe0, },		//panel timing state control
	{ 15, 0xcb, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x05, },

	{ 2, 0x00, 0xf0, },		//panel timing state control
	{ 12, 0xcb, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, },

//-------------------- panel pad mapping control --------------------//
	{ 2, 0x00, 0x80, },		//panel pad mapping control
	{ 16, 0xcc, 0x0a, 0x0c, 0x0e, 0x10, 0x02, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, },

	{ 2, 0x00, 0x90, },		//panel pad mapping control
	{ 16, 0xcc, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2e, 0x2d, 0x09, 0x0b, 0x0d, 0x0f, 0x01, 0x03, 0x00, 0x00, },

	{ 2, 0x00, 0xa0, },		//panel pad mapping control
	{ 15, 0xcc, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2e, 0x2d, },

	{ 2, 0x00, 0xb0, },		//panel pad mapping control
	{ 16, 0xcc, 0x0F, 0x0D, 0x0B, 0x09, 0x03, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, },

	{ 2, 0x00, 0xc0, },		//panel pad mapping control
	{ 16, 0xcc, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2d, 0x2e, 0x10, 0x0E, 0x0C, 0x0A, 0x04, 0x02, 0x00, 0x00, },

	{ 2, 0x00, 0xd0, },		//panel pad mapping control
	{ 15, 0xcc, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2d, 0x2e, },

//-------------------- panel timing setting --------------------//
	{ 2, 0x00, 0x80, },		//panel VST setting
	{ 13, 0xce, 0x8D, 0x03, 0x00, 0x8C, 0x03, 0x00, 0x8B, 0x03, 0x00, 0x8A, 0x03, 0x00, },

	{ 2, 0x00, 0x90, },		//panel VEND setting
	{ 15, 0xce, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, },

	{ 2, 0x00, 0xa0, },		//panel CLKA1/2 setting
	{ 15, 0xce, 0x38, 0x0B, 0x04, 0xFC, 0x00, 0x00, 0x00, 0x38, 0x0A, 0x04, 0xFD, 0x00, 0x00, 0x00, },

	{ 2, 0x00, 0xb0, },		//panel CLKA3/4 setting
	{ 15, 0xce, 0x38,  0x09,  0x04, 0xFE,  0x00, 0x00,  0x00,  0x38, 0x08,  0x04, 0xFF,  0x00,  0x00, 0x00, },

	{ 2, 0x00, 0xc0, },		//panel CLKb1/2 setting
	{ 15, 0xce, 0x38,  0x07,  0x05, 0x00,  0x00, 0x00,  0x00,  0x38, 0x06,  0x05, 0x01,  0x00,  0x00, 0x00, },

	{ 2, 0x00, 0xd0, },		//panel CLKb3/4 setting
	{ 15, 0xce, 0x38,  0x05,  0x05, 0x02,  0x00, 0x00,  0x00,  0x38, 0x04,  0x05, 0x03,  0x00,  0x00, 0x00, },

	{ 2, 0x00, 0x80, },		//panel CLKc1/2 setting
	{ 15, 0xcf, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, },

	{ 2, 0x00, 0x90, },		//panel CLKc3/4 setting
	{ 15, 0xcf, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, },

	{ 2, 0x00, 0xa0, },		//panel CLKd1/2 setting
	{ 15, 0xcf, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, },

	{ 2, 0x00, 0xb0, },		//panel CLKd3/4 setting
	{ 15, 0xcf, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, },

	{ 2, 0x00, 0xc0, },		//panel ECLK setting
	{ 12, 0xcf, 0x01, 0x01, 0x20, 0x20, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00, 0x08, },

	{ 2, 0x00, 0xb5, },             //TCON_GOA_OUT Setting
	{ 7, 0xc5, 0x33, 0xf1, 0xff, 0x33, 0xf1, 0xff, },  //normal output with VGH/VGL

	{ 2, 0x00, 0xa0, },
	{ 2, 0xc1, 0x02, },

	{ 2, 0x00, 0xb1, },
	{ 2, 0xc6, 0x04, },

	{ 2, 0x00, 0x00, },             //Orise mode disable
	/* this might not go through the SSD2858! */
	{ 3, 0xff, 0xff, 0xff, 0xff, },

#if 0
	{ 2, DCS_CTRL_DISPLAY, 0x24, },	// LEDPWM ON
	{ 2, DCS_WRITE_CABC, 0x00, },	// CABC off
#endif
};

static w677l_reg sleep_out[] = {
//	{ 1, MIPI_DCS_SET_DISPLAY_ON, },
	{ 1, MIPI_DCS_EXIT_SLEEP_MODE, },
};

static w677l_reg display_on[] = {
	{ 1, MIPI_DCS_SET_DISPLAY_ON, },
//	{ 1, MIPI_DCS_EXIT_SLEEP_MODE, },
};

static int w677l_write(struct omap_dss_device *dssdev, u8 *buf, int len)
{
	struct panel_drv_data *ddata = to_panel_data(dssdev);
	struct omap_dss_device *src = ddata->src;
	int r;

#if LOG
	int i;
	static char logbuf[256];
	sprintf(logbuf, "%s", IS_MCS(buf[0], len) ? "gen" : "dcs");
	for (i = 0; i < len; i++)
		sprintf(logbuf + strlen(logbuf), " %02x", buf[i]);
	printk("dsi: %s(%s)", __func__, logbuf);
#endif

	if(IS_MCS(buf[0], len)) {
		/* this is a "manufacturer command" that must be sent as a "generic write command" */
		r = src->ops->dsi.gen_write(src, ddata->config_channel, buf, len);
	} else {
		/* this is a "user command" that must be sent as "DCS command" */
		r = src->ops->dsi.dcs_write_nosync(src, ddata->config_channel, buf, len);
	}

	if (r)
		dev_err(&ddata->pdev->dev, "write cmd/reg(%x) failed: %d\n",
				buf[0], r);

	return r;
}

static int w677l_read(struct omap_dss_device *dssdev, u8 dcs_cmd, u8 *buf, int len)
{
	struct panel_drv_data *ddata = to_panel_data(dssdev);
	struct omap_dss_device *src = ddata->src;
	int r;

	r = src->ops->dsi.set_max_rx_packet_size(src, ddata->config_channel, len);	/* tell panel how much we expect */
	if (r) {
		dev_err(&ddata->pdev->dev, "can't set max rx packet size\n");
		return -EIO;
	}

	if(IS_MCS(dcs_cmd, len)) {
		/* this is a "manufacturer command" that must be sent as a "generic read command" */
		r = src->ops->dsi.gen_read(src, ddata->config_channel, &dcs_cmd, 1, buf, len);
	} else {
		/* this is a "user command" that must be sent as "DCS command" */
		r = src->ops->dsi.dcs_read(src, ddata->config_channel, dcs_cmd, buf, len);
	}

	if (r)
		dev_err(&ddata->pdev->dev, "read cmd/reg(%02x, %d) failed: %d\n",
				dcs_cmd, len, r);

#if LOG
	{
	int i;
	static char logbuf[256];

	sprintf(logbuf, "%02x:", dcs_cmd);
	for (i = 0; i < len; i++)
		sprintf(logbuf + strlen(logbuf), " %02x", buf[i]);
	printk("dsi: %s(%s) -> %d\n", __func__, logbuf, r);
	}
#endif

	return r;
}

static int w677l_write_sequence(struct omap_dss_device *dssdev,
		w677l_reg *seq, int len)
{
	int r, i;
	struct panel_drv_data *ddata = to_panel_data(dssdev);

	for (i = 0; i < len; i++) {
		r = w677l_write(dssdev, &seq[i][1], seq[i][0]);
		if (r) {
			dev_err(&ddata->pdev->dev, "sequence failed: %d\n", i);
			return -EINVAL;
		}
	}

	return 0;
}

static int w677l_reset(struct omap_dss_device *dssdev, int activate)
{
	struct panel_drv_data *ddata = to_panel_data(dssdev);

	dev_dbg(&ddata->pdev->dev, "reset(%s)\n", activate?"active":"inactive");

	if (gpio_is_valid(ddata->reset_gpio))
		gpio_set_value(ddata->reset_gpio, !activate);
	return 0;
}

static int w677l_regulator(struct omap_dss_device *dssdev, int state)
{
	struct panel_drv_data *ddata = to_panel_data(dssdev);

	dev_dbg(&ddata->pdev->dev, "regulator(%d)\n", state);

	if (gpio_is_valid(ddata->regulator_gpio))
		gpio_set_value(ddata->regulator_gpio, state);	/* switch regulator */
	return 0;
}

static int w677l_update_brightness(struct omap_dss_device *dssdev, int level)
{
	int r;
#if 1
	u8 buf[2];

	buf[0] = DCS_BRIGHTNESS;
	buf[1] = level;
#else
	u8 buf[3];

	buf[0] = DCS_BRIGHTNESS;
	buf[1] = level >> 4;	/* 12 bit mode */
	buf[2] = buf[1] + ((level & 0x0f) << 4);
#endif
	r = w677l_write(dssdev, buf, sizeof(buf));
	if (r)
		return r;
	return 0;
}

static int w677l_set_brightness(struct backlight_device *bd)
{
	struct omap_dss_device *dssdev = dev_get_drvdata(&bd->dev);
	struct panel_drv_data *ddata = to_panel_data(dssdev);
//	struct omap_dss_device *src = ddata->src;
	int bl = bd->props.brightness;
	int r = 0;

	dev_dbg(&ddata->pdev->dev, "set_brightness(%d)\n", bl);

#if 0

	mutex_lock(&ddata->lock);

	src->ops->dsi.bus_lock(src);

	r = w677l_update_brightness(dssdev, bl);

	src->ops->dsi.bus_unlock(src);

	mutex_unlock(&ddata->lock);
#endif

	return r;
}

static int w677l_get_brightness(struct backlight_device *bd)
{
	struct omap_dss_device *dssdev = dev_get_drvdata(&bd->dev);
	struct panel_drv_data *ddata = to_panel_data(dssdev);
	struct omap_dss_device *src = ddata->src;
	u8 data[16];
	u16 brightness = 0;
	int r = 0;

	dev_dbg(&ddata->pdev->dev, "get_brightness()\n");

	mutex_lock(&ddata->lock);

	if (ddata->enabled) {
		src->ops->dsi.bus_lock(src);
		r = w677l_read(dssdev, DCS_READ_BRIGHTNESS, data, 2);
		brightness = (data[0]<<4) + (data[1]>>4);

		src->ops->dsi.bus_unlock(src);
	}

	mutex_unlock(&ddata->lock);

	if (r < 0) {
		dev_err(&ddata->pdev->dev, "get_brightness: read error\n");
		return bd->props.brightness;
	}

	dev_dbg(&ddata->pdev->dev, "get_brightness -> %d\n", brightness);

	return brightness>>4;	/* get into range 0..255 */
}

static const struct backlight_ops w677l_backlight_ops  = {
	.get_brightness = w677l_get_brightness,
	.update_status = w677l_set_brightness,
};

static int w677l_connect(struct omap_dss_device *src, struct omap_dss_device *dst)
{
	struct panel_drv_data *ddata = to_panel_data(dst);
	struct device *dev = &ddata->pdev->dev;
	int r;

	dev_dbg(&ddata->pdev->dev, "connect()\n");

	/* channel0 used for video packets */
	r = src->ops->dsi.request_vc(src, &ddata->pixel_channel);
	if (r) {
		dev_err(dev, "failed to get virtual channel\n");
		goto err_req_vc0;
	}

	r = src->ops->dsi.set_vc_id(src, ddata->pixel_channel, 0);
	if (r) {
		dev_err(dev, "failed to set VC_ID\n");
		goto err_vc_id0;
	}

	/* channel1 used for registers access in LP mode */
	r = src->ops->dsi.request_vc(src, &ddata->config_channel);
	if (r) {
		dev_err(dev, "failed to get virtual channel\n");
		goto err_req_vc1;
	}

	r = src->ops->dsi.set_vc_id(src, ddata->config_channel, 0);
	if (r) {
		dev_err(dev, "failed to set VC_ID\n");
		goto err_vc_id1;
	}

	dev_dbg(&ddata->pdev->dev, "connect() ok\n");

	ddata->src = src;

	return 0;

err_vc_id1:
	src->ops->dsi.release_vc(src, ddata->config_channel);
err_req_vc1:
err_vc_id0:
	src->ops->dsi.release_vc(src, ddata->pixel_channel);
err_req_vc0:
	return r;
}

static void w677l_disconnect(struct omap_dss_device *src, struct omap_dss_device *dst)
{
	struct panel_drv_data *ddata = to_panel_data(dst);

	dev_dbg(&ddata->pdev->dev, "disconnect()\n");

	src->ops->dsi.release_vc(src, ddata->pixel_channel);
	src->ops->dsi.release_vc(src, ddata->config_channel);

	ddata->src = NULL;
}

static int w677l_check_timings(struct omap_dss_device *dssdev,
		struct drm_display_mode *timings)
{
	struct panel_drv_data *ddata = to_panel_data(dssdev);
	struct omap_dss_device *src = ddata->src;

	dev_dbg(&ddata->pdev->dev, "check_timings() in = %s %p\n", src->name, src->ops);

#if 0
	/* if we are connected to the ssd2858 driver src->ops provides check_timings() */

	if (src->ops && src->ops->check_timings) {
		dev_dbg(&ddata->pdev->dev, "check_timings with source\n");
		return src->ops->check_timings(src, (struct videomode *) timings);
	}
#endif

	return 0;
}

static void w677l_enable(struct omap_dss_device *dssdev)
{
	struct panel_drv_data *ddata = to_panel_data(dssdev);
	struct device *dev = &ddata->pdev->dev;
	struct omap_dss_device *src = ddata->src;
	int r = 0;
	struct omap_dss_dsi_config w677l_dsi_config = {
		.mode = OMAP_DSS_DSI_VIDEO_MODE,
		.pixel_format = w677l_PIXELFORMAT,
		.vm = &ddata->vm,
		.hs_clk_min = 125000000 /*w677l_HS_CLOCK*/,
		.hs_clk_max = 450000000 /*(12*w677l_HS_CLOCK)/10*/,
		.lp_clk_min = (7*w677l_LP_CLOCK)/10,	/* 70% */
		.lp_clk_max = w677l_LP_CLOCK,
		.ddr_clk_always_on = true,
		.trans_mode = OMAP_DSS_DSI_BURST_MODE,
	};

	dev_dbg(&ddata->pdev->dev, "enable\n");

	if (omapdss_device_is_enabled(dssdev))
		return;

	dev_dbg(&ddata->pdev->dev, "start()\n");

	mutex_lock(&ddata->lock);

	src->ops->dsi.bus_lock(src);

	dev_dbg(&ddata->pdev->dev, "hs_clk_min=%lu\n", w677l_dsi_config.hs_clk_min);
	dev_dbg(&ddata->pdev->dev, "power_on()\n");

	w677l_reset(dssdev, true);	/* activate reset */

#if 0
	if (ddata->pin_config.num_pins > 0) {
		r = src->ops->dsi.configure_pins(src, &ddata->pin_config);
		if (r) {
			dev_err(&ddata->pdev->dev,
					"failed to configure DSI pins\n");
			goto err;
		}
	}
#endif

	// CHECKME: this might be the first place where timings are really important
	// or is this too late?

	r = src->ops->dsi.set_config(src, &w677l_dsi_config);
	if (r) {
		dev_err(dev, "failed to configure DSI\n");
		goto err;
	}

	src->ops->enable(src);

#if 0
	r = src->ops->enable(src);
	if (r) {
		dev_err(dev, "failed to enable DSI\n");
		goto err;
	}
#endif

	w677l_regulator(dssdev, true);	/* switch power on */
	msleep(50);

	w677l_reset(dssdev, false);	/* release reset */
	msleep(10);

	src->ops->dsi.enable_hs(src, ddata->pixel_channel, true);

	r = w677l_write_sequence(dssdev, sleep_out, ARRAY_SIZE(sleep_out));
	if (r)
		goto cleanup;

	msleep(10);

	r = w677l_write_sequence(dssdev, init_seq, ARRAY_SIZE(init_seq));
	if (r) {
		dev_err(dev, "failed to configure panel\n");
// can fail if the ssd can't forward long commands
//		goto cleanup;
	}

	r = w677l_update_brightness(dssdev, 255);
	if (r)
		goto cleanup;

#if LOG
	{
		u8 ret[8];
		/* read some registers through DCS commands */
		r = w677l_read(dssdev, 0x05, ret, 1);
		r = w677l_read(dssdev, 0x0a, ret, 1);  // power mode 0x10=sleep off; 0x04=display on
		r = w677l_read(dssdev, 0x0b, ret, 1);  // address mode
		r = w677l_read(dssdev, MIPI_DCS_GET_PIXEL_FORMAT, ret, 1);     // pixel format 0x70 = RGB888
		r = w677l_read(dssdev, 0x0d, ret, 1);  // display mode 0x80 = command 0x34/0x35
		r = w677l_read(dssdev, 0x0e, ret, 1);  // signal mode
		r = w677l_read(dssdev, MIPI_DCS_GET_DIAGNOSTIC_RESULT, ret, 1);        // diagnostic 0x40 = functional
		r = w677l_read(dssdev, 0x45, ret, 2);  // get scanline
	}
#endif
	r = src->ops->dsi.enable_video_output(src, ddata->pixel_channel);
	if (r)
		goto cleanup;

	msleep(10);

#if 1	/* this is recommended by the latest data sheet */
	r = w677l_write_sequence(dssdev, display_on, ARRAY_SIZE(display_on));
	if (r)
		goto cleanup;
#endif

	// not really used...
	ddata->enabled = true;

	dev_dbg(&ddata->pdev->dev, "enable() powered on()\n");

	goto ok;

cleanup:
	dev_err(dev, "error while enabling panel, issuing HW reset\n");

	src->ops->disable(src);
	mdelay(10);
//	w677l_reset(dssdev, true);	/* activate reset */
	w677l_regulator(dssdev, false);	/* switch power off */
	mdelay(20);

err:
ok:

	src->ops->dsi.bus_unlock(src);

	if (r)
		dev_err(&ddata->pdev->dev, "enable failed\n");

	mutex_unlock(&ddata->lock);

	backlight_enable(ddata->backlight);
}

static void w677l_disable(struct omap_dss_device *dssdev)
{
	struct panel_drv_data *ddata = to_panel_data(dssdev);
	struct omap_dss_device *src = ddata->src;

	dev_dbg(&ddata->pdev->dev, "disable\n");

	if (!omapdss_device_is_enabled(dssdev))
		return;

	backlight_disable(ddata->backlight);

	dev_dbg(&ddata->pdev->dev, "stop()\n");

	mutex_lock(&ddata->lock);

	src->ops->dsi.bus_lock(src);

	dev_dbg(&ddata->pdev->dev, "power_off()\n");

	ddata->enabled = 0;
	src->ops->dsi.disable_video_output(src, ddata->pixel_channel);
	dev_dbg(&ddata->pdev->dev, "disabled video()\n");
	if (src->ops->disable)
		src->ops->disable(src);
	else
		dev_dbg(&ddata->pdev->dev, "no disable op?\n");
	dev_dbg(&ddata->pdev->dev, "disabled in()\n");
	mdelay(10);
	w677l_reset(dssdev, true);	/* activate reset */
	w677l_regulator(dssdev, false);	/* switch power off - after stopping video stream */
	mdelay(20);
	/* here we can also power off IOVCC */

	dev_dbg(&ddata->pdev->dev, "unlock bus()\n");
	src->ops->dsi.bus_unlock(src);

	mutex_unlock(&ddata->lock);
	dev_dbg(&ddata->pdev->dev, "disable finished)\n");
}

static int w677l__panel_get_modes(struct omap_dss_device *dssdev,
				      struct drm_connector *connector)
{
	struct panel_drv_data *ddata = to_panel_data(dssdev);

	connector->display_info.width_mm = 63;
	connector->display_info.height_mm = 112;
	connector->display_info.panel_orientation = DRM_MODE_PANEL_ORIENTATION_RIGHT_UP;

	return omapdss_display_get_modes(connector, &ddata->vm);
}

static struct omap_dss_device_ops w677l_dss_ops = {
	.connect	= w677l_connect,
	.disconnect	= w677l_disconnect,

	.enable		= w677l_enable,
	.disable	= w677l_disable,

	.check_timings	= w677l_check_timings,

	.get_modes	= w677l__panel_get_modes,

};

static struct omap_dss_driver w677l_dss_driver = {
};

static int w677l_probe_of(struct platform_device *pdev)
{
	struct device_node *node = pdev->dev.of_node;
	struct panel_drv_data *ddata = platform_get_drvdata(pdev);
	int gpio;

	dev_dbg(&ddata->pdev->dev, "probe_of()\n");

	if (!node)
		return -ENODEV;

	gpio = of_get_gpio(node, 0);
	if (gpio == -EPROBE_DEFER)
		return -EPROBE_DEFER;
	if (!gpio_is_valid(gpio)) {
		dev_err(&pdev->dev, "failed to parse reset gpio (err=%d)\n", gpio);
		return gpio;
	}
	ddata->reset_gpio = gpio;

#if OPTIONAL
	gpio = of_get_gpio(node, 1);
	if (!gpio_is_valid(gpio)) {
		dev_err(&pdev->dev, "failed to parse regulator gpio (err=%d)\n", gpio);
		return gpio;
	}

	if (gpio == -EPROBE_DEFER)
		return -EPROBE_DEFER;
	ddata->regulator_gpio = gpio;
#endif

	ddata->backlight = devm_of_find_backlight(&pdev->dev);
	if (IS_ERR(ddata->backlight))
		return PTR_ERR(ddata->backlight);

	return 0;
}

static int w677l_probe(struct platform_device *pdev)
{
	struct panel_drv_data *ddata;
	struct device *dev = &pdev->dev;
	struct omap_dss_device *dssdev;
	int r;

	dev_dbg(dev, "%s\n", __func__);

	ddata = devm_kzalloc(dev, sizeof(*ddata), GFP_KERNEL);
	if (!ddata)
		return -ENOMEM;

	platform_set_drvdata(pdev, ddata);
	ddata->pdev = pdev;

	ddata->vm = w677l_timings;

	r = w677l_probe_of(pdev);
	if (r) {
		dev_err(dev, "Failed to probe %d\n", r);
		return r;
	}

	dssdev = &ddata->dssdev;
	dssdev->dev = dev;
	dssdev->ops = &w677l_dss_ops;
	dssdev->driver = &w677l_dss_driver;
	dssdev->type = OMAP_DISPLAY_TYPE_DSI;
	dssdev->display = true;
	dssdev->owner = THIS_MODULE;
	dssdev->of_ports = BIT(0);
	dssdev->ops_flags = OMAP_DSS_DEVICE_OP_MODES;

#if 0
	dssdev->caps = OMAP_DSS_DISPLAY_CAP_MANUAL_UPDATE |
		OMAP_DSS_DISPLAY_CAP_TEAR_ELIM;
#endif

	omapdss_display_init(dssdev);
	omapdss_device_register(dssdev);

	mutex_init(&ddata->lock);

	if (gpio_is_valid(ddata->reset_gpio)) {
		r = devm_gpio_request_one(dev, ddata->reset_gpio,
					  GPIOF_DIR_OUT, "panel reset");
		if (r) {
			dev_err(dev, "failed to request reset gpio (%d err=%d)\n", ddata->reset_gpio, r);
			return r;
		}
	}

#if OPTIONAL
	if (gpio_is_valid(ddata->regulator_gpio)) {
		r = devm_gpio_request_one(dev, ddata->regulator_gpio,
					  GPIOF_DIR_OUT, "panel DC/DC regulator");
		if (r) {
			dev_err(dev, "failed to request regulator gpio (%d err=%d)\n", ddata->regulator_gpio, r);
			return r;
		}
	}
#endif

	dev_dbg(dev, "probe ok\n");

	return 0;
}


static int __exit w677l_remove(struct platform_device *pdev)
{
	struct panel_drv_data *ddata = platform_get_drvdata(pdev);
	struct omap_dss_device *dssdev = &ddata->dssdev;

	dev_dbg(&pdev->dev, "%s\n", __func__);

	omapdss_device_unregister(dssdev);

	w677l_disable(dssdev);

	mutex_destroy(&ddata->lock);

	return 0;
}

static const struct of_device_id w677l_of_match[] = {
	{ .compatible = "omapdss,boe,btl507212-w677l", },
	{ .compatible = "boe,btl507212-w677l", },
	{},
};

MODULE_DEVICE_TABLE(of, w677l_of_match);

static struct platform_driver w677l_driver = {
	.probe = w677l_probe,
	.remove = __exit_p(w677l_remove),
	.driver = {
		.name = "panel-btl507212-w677l",
		.of_match_table = w677l_of_match,
		.suppress_bind_attrs = true,
	},
};

module_platform_driver(w677l_driver);

MODULE_AUTHOR("H. Nikolaus Schaller <hns@goldelico.com>");
MODULE_DESCRIPTION("btl507212-w677l driver");
MODULE_LICENSE("GPL");
