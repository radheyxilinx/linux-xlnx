/*
 * Xilinx VPHY driver
 *
 * The Video Phy is a high-level wrapper around the GT to configure it
 * for video applications. The driver also provides common functionality
 * for its tightly-bound video protocol drivers such as HDMI RX/TX.
 *
 * Copyright (C) 2016, 2017 Leon Woestenberg <leon@sidebranch.com>
 * Copyright (C) 2014, 2015, 2017 Xilinx, Inc.
 *
 * Authors: Leon Woestenberg <leon@sidebranch.com>
 *          Rohit Consul <rohitco@xilinx.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

/* if both both DEBUG and DEBUG_TRACE are defined, trace_printk() is used */
#define DEBUG
//#define DEBUG_TRACE

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/of_address.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/delay.h>
#include <dt-bindings/phy/phy.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/interrupt.h>

#include "linux/phy/phy-vphy.h"

/* baseline driver includes */
#include "phy-xilinx-vphy/xvphy.h"
#include "phy-xilinx-vphy/xvphy_i.h"
#include "phy-xilinx-vphy/xil_printf.h"
#include "phy-xilinx-vphy/xstatus.h"

/* common RX/TX */
#include "phy-xilinx-vphy/xvidc.h"
#include "phy-xilinx-vphy/xvidc_edid.h"

/* either comment-out, or define as 1. Adapt Makefile also, see HDCP section */
//#define USE_HDCP 1

#if (defined(USE_HDCP) && USE_HDCP) /* WIP HDCP */
#include "phy-xilinx-vphy/bigdigits.h"
#include "phy-xilinx-vphy/xhdcp22_cipher.h"
#include "phy-xilinx-vphy/xhdcp22_mmult.h"
#include "phy-xilinx-vphy/xhdcp22_rng.h"
#include "phy-xilinx-vphy/xhdcp22_common.h"
#include "phy-xilinx-vphy/xtmrctr.h"
#endif

/* select either trace or printk logging */
#ifdef DEBUG_TRACE
#define do_hdmi_dbg(format, ...) do { \
  trace_printk("xlnx-hdmi-vphy: " format, ##__VA_ARGS__); \
} while(0)
#else
#define do_hdmi_dbg(format, ...) do { \
  printk(KERN_DEBUG "xlnx-hdmi-vphy: " format, ##__VA_ARGS__); \
} while(0)
#endif

/* either enable or disable debugging */
#ifdef DEBUG
#  define hdmi_dbg(x...) do_hdmi_dbg(x)
#else
#  define hdmi_dbg(x...)
#endif

/**
 * struct xvphy_lane - representation of a lane
 * @phy: pointer to the kernel PHY device
 *
 * @type: controller which uses this lane
 * @lane: lane number
 * @protocol: protocol in which the lane operates
 * @ref_clk: enum of allowed ref clock rates for this lane PLL
 * @pll_lock: PLL status
 * @data: pointer to hold private data
 * @refclk_rate: PLL reference clock frequency
 * @share_laneclk: lane number of the clock to be shared
 */
struct xvphy_lane {
	struct phy *phy;
	u8 type;
	u8 lane;
	u8 protocol;
	bool pll_lock;
	/* data is pointer to parent xvphy_dev */
	void *data;
	u32 refclk_rate;
	u32 share_laneclk;
};

/**
 * struct xvphy_dev - representation of a Xilinx Video PHY
 * @dev: pointer to device
 * @iomem: serdes base address
 */
struct xvphy_dev {
	struct device *dev;
	/* virtual remapped I/O memory */
	void __iomem *iomem;
	int irq;
	/* protects the XVphy baseline against concurrent access */
	struct mutex xvphy_mutex;
	struct xvphy_lane *lanes[4];
	/* bookkeeping for the baseline subsystem driver instance */
	XVphy xvphy;
	/* AXI Lite clock drives the clock detector */
	struct clk *axi_lite_clk;
};

/* given the (Linux) phy handle, return the xvphy */
XVphy *xvphy_get_xvphy(struct phy *phy)
{
	struct xvphy_lane *vphy_lane = phy_get_drvdata(phy);
	struct xvphy_dev *vphy_dev = vphy_lane->data;
	return &vphy_dev->xvphy;
}
EXPORT_SYMBOL_GPL(xvphy_get_xvphy);

/* given the (Linux) phy handle, enter critical section of xvphy baseline code
 * XVphy functions must be called with mutex acquired to prevent concurrent access
 * by XVphy and upper-layer video protocol drivers */
void xvphy_mutex_lock(struct phy *phy)
{
	struct xvphy_lane *vphy_lane = phy_get_drvdata(phy);
	struct xvphy_dev *vphy_dev = vphy_lane->data;
	mutex_lock(&vphy_dev->xvphy_mutex);
}
EXPORT_SYMBOL_GPL(xvphy_mutex_lock);

void xvphy_mutex_unlock(struct phy *phy)
{
	struct xvphy_lane *vphy_lane = phy_get_drvdata(phy);
	struct xvphy_dev *vphy_dev = vphy_lane->data;
	mutex_unlock(&vphy_dev->xvphy_mutex);
}
EXPORT_SYMBOL_GPL(xvphy_mutex_unlock);

/* XVphy functions must be called with mutex acquired to prevent concurrent access
 * by XVphy and upper-layer video protocol drivers */
EXPORT_SYMBOL_GPL(XVphy_GetPllType);
EXPORT_SYMBOL_GPL(XVphy_IBufDsEnable);
EXPORT_SYMBOL_GPL(XVphy_SetHdmiCallback);
EXPORT_SYMBOL_GPL(XVphy_HdmiCfgCalcMmcmParam);
EXPORT_SYMBOL_GPL(XVphy_MmcmStart);

/* exclusively required by TX */
EXPORT_SYMBOL_GPL(XVphy_Clkout1OBufTdsEnable);
EXPORT_SYMBOL_GPL(XVphy_SetHdmiTxParam);
EXPORT_SYMBOL_GPL(XVphy_IsBonded);

static irqreturn_t xvphy_irq_handler(int irq, void *dev_id)
{
	struct xvphy_dev *vphydev;
	u32 IntrStatus;
	BUG_ON(!dev_id);
	vphydev = (struct xvphy_dev *)dev_id;
	BUG_ON(!vphydev);
	if (!vphydev)
		return IRQ_NONE;
	//printk(KERN_DEBUG "xvphy_irq_handler()\n");

	/* disable interrupts in the VPHY, they are re-enabled once serviced */
	XVphy_IntrDisable(&vphydev->xvphy, XVPHY_INTR_HANDLER_TYPE_TXRESET_DONE |
			XVPHY_INTR_HANDLER_TYPE_RXRESET_DONE |
			XVPHY_INTR_HANDLER_TYPE_CPLL_LOCK |
			XVPHY_INTR_HANDLER_TYPE_QPLL0_LOCK |
			XVPHY_INTR_HANDLER_TYPE_TXALIGN_DONE |
			XVPHY_INTR_HANDLER_TYPE_QPLL1_LOCK |
			XVPHY_INTR_HANDLER_TYPE_TX_CLKDET_FREQ_CHANGE |
			XVPHY_INTR_HANDLER_TYPE_RX_CLKDET_FREQ_CHANGE |
			XVPHY_INTR_HANDLER_TYPE_TX_TMR_TIMEOUT |
			XVPHY_INTR_HANDLER_TYPE_RX_TMR_TIMEOUT);

	return IRQ_WAKE_THREAD;
}

static irqreturn_t xvphy_irq_thread(int irq, void *dev_id)
{
	struct xvphy_dev *vphydev;
	u32 IntrStatus;
	BUG_ON(!dev_id);
	vphydev = (struct xvphy_dev *)dev_id;
	BUG_ON(!vphydev);
	if (!vphydev)
		return IRQ_NONE;

	//printk(KERN_DEBUG "xvphy_irq_thread()\n");
	/* call baremetal interrupt handler with mutex locked */
	mutex_lock(&vphydev->xvphy_mutex);

	IntrStatus = XVphy_ReadReg(vphydev->xvphy.Config.BaseAddr, XVPHY_INTR_STS_REG);
	printk(KERN_DEBUG "XVphy IntrStatus = 0x%08x\n", IntrStatus);

	/* handle pending interrupts */
	XVphy_InterruptHandler(&vphydev->xvphy);
	mutex_unlock(&vphydev->xvphy_mutex);

	/* enable interrupt requesting in the VPHY */
	XVphy_IntrEnable(&vphydev->xvphy, XVPHY_INTR_HANDLER_TYPE_TXRESET_DONE |
		XVPHY_INTR_HANDLER_TYPE_RXRESET_DONE |
		XVPHY_INTR_HANDLER_TYPE_CPLL_LOCK |
		XVPHY_INTR_HANDLER_TYPE_QPLL0_LOCK |
		XVPHY_INTR_HANDLER_TYPE_TXALIGN_DONE |
		XVPHY_INTR_HANDLER_TYPE_QPLL1_LOCK |
		XVPHY_INTR_HANDLER_TYPE_TX_CLKDET_FREQ_CHANGE |
		XVPHY_INTR_HANDLER_TYPE_RX_CLKDET_FREQ_CHANGE |
		XVPHY_INTR_HANDLER_TYPE_TX_TMR_TIMEOUT |
		XVPHY_INTR_HANDLER_TYPE_RX_TMR_TIMEOUT);

	XVphy_LogDisplay(&vphydev->xvphy);
	//XVphy_HdmiDebugInfo(&vphydev->xvphy, 0, XVPHY_CHANNEL_ID_CH1);
	return IRQ_HANDLED;
}

/**
 * xvphy_phy_init - initializes a lane
 * @phy: pointer to kernel PHY device
 *
 * Return: 0 on success or error on failure
 */
static int xvphy_phy_init(struct phy *phy)
{
	BUG_ON(!phy);
	//struct xvphy_lane *vphy_lane = NULL;
	printk(KERN_INFO "xvphy_phy_init(%p).\n", phy);

	//printk(KERN_INFO "xvphy_probe() found %d phy lanes from device-tree configuration.\n", index);
	//printk(KERN_INFO "xvphy_probe() found %d phy lanes from device-tree configuration.\n", index);

	return 0;
}

/**
 * xvphy_xlate - provides a PHY specific to a controller
 * @dev: pointer to device
 * @args: arguments from dts
 *
 * Return: pointer to kernel PHY device or error on failure
 *
 *
 */
static struct phy *xvphy_xlate(struct device *dev,
				   struct of_phandle_args *args)
{
	struct xvphy_dev *vphydev = dev_get_drvdata(dev);
	struct xvphy_lane *vphy_lane = NULL;
	struct device_node *phynode = args->np;
	int index;
	u8 controller;
	u8 instance_num;

	if (args->args_count != 4) {
		dev_err(dev, "Invalid number of cells in 'phy' property\n");
		return ERR_PTR(-EINVAL);
	}
	if (!of_device_is_available(phynode)) {
		dev_warn(dev, "requested PHY is disabled\n");
		return ERR_PTR(-ENODEV);
	}
	for (index = 0; index < of_get_child_count(dev->of_node); index++) {
		if (phynode == vphydev->lanes[index]->phy->dev.of_node) {
			//dev_info(dev, "xvphy_xlate() matched with phy index %d\n", index);
			vphy_lane = vphydev->lanes[index];
			break;
		}
	}
	if (!vphy_lane) {
		dev_err(dev, "failed to find appropriate phy\n");
		return ERR_PTR(-EINVAL);
	}

	/* get type of controller from lanes */
	controller = args->args[0];

	/* get controller instance number */
	instance_num = args->args[1];

	/* Check if lane sharing is required */
	vphy_lane->share_laneclk = args->args[2];

	/* get the required clk rate for controller from lanes */
	vphy_lane->refclk_rate = args->args[3];

	//dev_info(dev, "xvphy_xlate() returns phy %p\n", vphy_lane->phy);
	BUG_ON(!vphy_lane->phy);
	return vphy_lane->phy;
}

/* @TODO allocate dynamically, inside vphydev struct, once the internal TODOs are resolved
 * required to support multiple vphy's in the driver
 */
static XVphy_Config config = {
	.DeviceId = 0,
	.BaseAddr = 0/* filled during probe*/,
	.ErrIrq = 0 /* ERR IRQ disabled by default */
};

static struct phy_ops xvphy_phyops = {
	.init		= xvphy_phy_init,
	.owner		= THIS_MODULE,
};

static int vphy_parse_of(struct xvphy_dev *vphydev, XVphy_Config *c)
{
	struct device *dev = vphydev->dev;
	struct device_node *node = dev->of_node;
	int rc;
	u32 val;
	bool has_err_irq;

	/* @TODO property name/value unknown, TODO xlnx,xcvrtype?? */
	rc = of_property_read_u32(node, "xlnx,transceiver-type", &val);
	if (rc < 0)
		goto error_dt;
	c->XcvrType = val;

	/* @TODO property name/value unknown, @TODO xlnx,tx-buffer-bypass?? */
	rc = of_property_read_u32(node, "xlnx,tx-buffer-bypass", &val);
	if (rc < 0)
		goto error_dt;
	c->TxBufferBypass = val;

	rc = of_property_read_u32(node, "xlnx,input-pixels-per-clock", &val);
	if (rc < 0)
		goto error_dt;
	c->Ppc = val;

	rc = of_property_read_u32(node, "xlnx,nidru", &val);
	if (rc < 0)
		goto error_dt;
	c->DruIsPresent = val;

	rc = of_property_read_u32(node, "xlnx,nidru-refclk-sel", &val);
	if (rc < 0)
		goto error_dt;
	c->DruRefClkSel = val;

	rc = of_property_read_u32(node, "xlnx,rx-no-of-channels", &val);
	if (rc < 0)
		goto error_dt;
	c->RxChannels = val;

	rc = of_property_read_u32(node, "xlnx,tx-no-of-channels", &val);
	if (rc < 0)
		goto error_dt;
	c->TxChannels = val;

	rc = of_property_read_u32(node, "xlnx,rx-protocol", &val);
	if (rc < 0)
		goto error_dt;
	c->RxProtocol = val;

	rc = of_property_read_u32(node, "xlnx,tx-protocol", &val);
	if (rc < 0)
		goto error_dt;
	c->TxProtocol = val;

	rc = of_property_read_u32(node, "xlnx,rx-refclk-sel", &val);
	if (rc < 0)
		goto error_dt;
	c->RxRefClkSel = val;

	rc = of_property_read_u32(node, "xlnx,tx-refclk-sel", &val);
	if (rc < 0)
		goto error_dt;
	c->TxRefClkSel = val;

	rc = of_property_read_u32(node, "xlnx,rx-pll-selection", &val);
	if (rc < 0)
		goto error_dt;
	c->RxSysPllClkSel = val;

	rc = of_property_read_u32(node, "xlnx,tx-pll-selection", &val);
	if (rc < 0)
		goto error_dt;
	c->TxSysPllClkSel = val;

	rc = of_property_read_u32(node, "xlnx,hdmi-fast-switch", &val);
	if (rc < 0)
		goto error_dt;
	c->HdmiFastSwitch = val;

	rc = of_property_read_u32(node, "xlnx,transceiver-width", &val);
	if (rc < 0)
		goto error_dt;
	c->TransceiverWidth = val;

	has_err_irq = false;
	has_err_irq = of_property_read_bool(node, "xlnx,err-irq-en");
	c->ErrIrq = has_err_irq;
	return 0;

#if 0 /* example bool */
	bool has_dre = false;
	has_dre = of_property_read_bool(node, "xlnx,include-dre");
#endif
error_dt:
	dev_err(vphydev->dev, "Error parsing device tree");
	return -EINVAL;
}

#if (defined(USE_HDCP) && USE_HDCP) /* WIP HDCP */
extern XHdcp22_Cipher_Config XHdcp22_Cipher_ConfigTable[];
extern XHdcp22_mmult_Config XHdcp22_mmult_ConfigTable[];
extern XHdcp22_Rng_Config XHdcp22_Rng_ConfigTable[];
#endif

/**
 * xvphy_probe - The device probe function for driver initialization.
 * @pdev: pointer to the platform device structure.
 *
 * Return: 0 for success and error value on failure
 */
static int xvphy_probe(struct platform_device *pdev)
{
	struct device_node *child, *np = pdev->dev.of_node;
	struct xvphy_dev *vphydev;
	struct phy_provider *provider;
	struct phy *phy;
	unsigned long axi_lite_rate;

	struct resource *res;
	int lanecount, port = 0, index = 0;
	int ret;
	int i;
	u32 Status;
	u32 Data;
	u16 DrpVal;

	hdmi_dbg("xvphy probed\n");
	vphydev = devm_kzalloc(&pdev->dev, sizeof(*vphydev), GFP_KERNEL);
	if (!vphydev)
		return -ENOMEM;

	/* mutex that protects against concurrent access */
	mutex_init(&vphydev->xvphy_mutex);

	vphydev->dev = &pdev->dev;
	/* set a pointer to our driver data */
	platform_set_drvdata(pdev, vphydev);

	BUG_ON(!np);
	hdmi_dbg("xvphy_probe DT parse start\n");
	ret = vphy_parse_of(vphydev, &config);
	if (ret) return ret;
	hdmi_dbg("xvphy_probe DT parse done\n");

	for_each_child_of_node(np, child) {
		struct xvphy_lane *vphy_lane;

		vphy_lane = devm_kzalloc(&pdev->dev, sizeof(*vphy_lane),
					 GFP_KERNEL);
		if (!vphy_lane)
			return -ENOMEM;

		/* Assign lane number to gtr_phy instance */
		vphy_lane->lane = index;

		/* Disable lane sharing as default */
		vphy_lane->share_laneclk = -1;

		BUG_ON(port >= 4);
		/* array of pointer to vphy_lane structs */
		vphydev->lanes[port] = vphy_lane;

		/* create phy device for each lane */
		phy = devm_phy_create(&pdev->dev, child, &xvphy_phyops);
		if (IS_ERR(phy)) {
			ret = PTR_ERR(phy);
			if (ret != -EPROBE_DEFER)
				dev_err(&pdev->dev, "failed to create PHY\n");
			hdmi_dbg("xvphy probe deferred\n");
			return ret;
		}
		/* array of pointer to phy */
		vphydev->lanes[port]->phy = phy;
		/* where each phy device has vphy_lane as driver data */
		phy_set_drvdata(phy, vphydev->lanes[port]);
		/* and each vphy_lane points back to parent device */
		vphy_lane->data = vphydev;
		port++;
		index++;
	}

	//printk(KERN_INFO "xvphy_probe() found %d phy lanes from device-tree configuration.\n", index);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	vphydev->iomem = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(vphydev->iomem))
		return PTR_ERR(vphydev->iomem);

	/* set address in configuration data */
	config.BaseAddr = (uintptr_t)vphydev->iomem;

	vphydev->irq = platform_get_irq(pdev, 0);
	if (vphydev->irq <= 0) {
		dev_err(&pdev->dev, "platform_get_irq() failed\n");
		return vphydev->irq;
	}

	/* the AXI lite clock is used for the clock rate detector */
	vphydev->axi_lite_clk = devm_clk_get(&pdev->dev, "axi-lite");
	if (IS_ERR(vphydev->axi_lite_clk)) {
		ret = PTR_ERR(vphydev->axi_lite_clk);
		vphydev->axi_lite_clk = NULL;
		if (ret != -EPROBE_DEFER)
			dev_err(&pdev->dev, "failed to get the axi lite clk.\n");
		return ret;
	}

	ret = clk_prepare_enable(vphydev->axi_lite_clk);
	if (ret) {
		dev_err(&pdev->dev, "failed to enable axi-lite clk\n");
		return ret;
	}
	axi_lite_rate = clk_get_rate(vphydev->axi_lite_clk);
	hdmi_dbg("AXI Lite clock rate = %lu Hz\n", axi_lite_rate);

	/* set axi-lite clk in configuration data */
	config.AxiLiteClkFreq = axi_lite_rate;

	provider = devm_of_phy_provider_register(&pdev->dev, xvphy_xlate);
	if (IS_ERR(provider)) {
		dev_err(&pdev->dev, "registering provider failed\n");
			return PTR_ERR(provider);
	}

	/* Initialize HDMI VPHY */
	Status = XVphy_HdmiInitialize(&vphydev->xvphy, 0/*QuadID*/,
		&config, axi_lite_rate);
	if (Status != XST_SUCCESS) {
		printk(KERN_INFO "HDMI VPHY initialization error\n");
		return XST_FAILURE;
	}

	Data = XVphy_GetVersion(&vphydev->xvphy);
	printk(KERN_INFO "VPhy version : %02d.%02d (%04x)\n", ((Data >> 24) & 0xFF), ((Data >> 16) & 0xFF), (Data & 0xFFFF));

	DrpVal = XVphy_DrpRead(&vphydev->xvphy, 0/*QuadId*/, 1/*ChId*/, 0x7C);
	hdmi_dbg("DrpVal @0x7C : 0x%08x%s\n", DrpVal, DrpVal & 0x2000?" GEARBOX ENABLED(?!)":" GEARBOX DISABLED");

	ret = devm_request_threaded_irq(&pdev->dev, vphydev->irq, xvphy_irq_handler, xvphy_irq_thread,
			IRQF_TRIGGER_HIGH /*IRQF_SHARED*/, "xilinx-vphy", vphydev/*dev_id*/);

	if (ret) {
		dev_err(&pdev->dev, "unable to request IRQ %d\n", vphydev->irq);
		return ret;
	}

	hdmi_dbg("config.DruIsPresent = %d\n", config.DruIsPresent);
	if (vphydev->xvphy.Config.DruIsPresent == (TRUE)) {
		hdmi_dbg("DRU reference clock frequency %0d Hz\n\r",
						XVphy_DruGetRefClkFreqHz(&vphydev->xvphy));
	}

	hdmi_dbg("HDMI VPHY initialization completed\n");
	return 0;
}

/* Match table for of_platform binding */
static const struct of_device_id xvphy_of_match[] = {
	{ .compatible = "xlnx,vid-phy-controller-2.0" },
	{},
};
MODULE_DEVICE_TABLE(of, xvphy_of_match);

static struct platform_driver xvphy_driver = {
	.probe = xvphy_probe,
	.driver = {
		.name = "xilinx-vphy",
		.of_match_table	= xvphy_of_match,
	},
};
module_platform_driver(xvphy_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Leon Woestenberg <leon@sidebranch.com>");
MODULE_DESCRIPTION("Xilinx Vphy driver");

/* common functionality shared between RX and TX */
EXPORT_SYMBOL_GPL(XVidC_ReportTiming);
EXPORT_SYMBOL_GPL(XVidC_SetVideoStream);
EXPORT_SYMBOL_GPL(XVidC_ReportStreamInfo);
EXPORT_SYMBOL_GPL(XVidC_EdidGetManName);
EXPORT_SYMBOL_GPL(XVidC_Set3DVideoStream);
EXPORT_SYMBOL_GPL(XVidC_GetPixelClockHzByVmId);
EXPORT_SYMBOL_GPL(XVidC_GetVideoModeId);
EXPORT_SYMBOL_GPL(XVidC_GetPixelClockHzByHVFr);

#if (defined(USE_HDCP) && USE_HDCP) /* WIP HDCP */
EXPORT_SYMBOL_GPL(mpAdd);
EXPORT_SYMBOL_GPL(mpConvFromOctets);
EXPORT_SYMBOL_GPL(mpConvToOctets);
EXPORT_SYMBOL_GPL(mpDivide);
EXPORT_SYMBOL_GPL(mpEqual);
EXPORT_SYMBOL_GPL(mpGetBit);
EXPORT_SYMBOL_GPL(mpModExp);
EXPORT_SYMBOL_GPL(mpModInv);
EXPORT_SYMBOL_GPL(mpModMult);
EXPORT_SYMBOL_GPL(mpModulo);
EXPORT_SYMBOL_GPL(mpMultiply);
EXPORT_SYMBOL_GPL(mpShiftLeft);
EXPORT_SYMBOL_GPL(mpSubtract);

EXPORT_SYMBOL_GPL(XHdcp22Cipher_CfgInitialize);
EXPORT_SYMBOL_GPL(XHdcp22Cipher_LookupConfig);
EXPORT_SYMBOL_GPL(XHdcp22Cipher_SetKs);
EXPORT_SYMBOL_GPL(XHdcp22Cipher_SetLc128);
EXPORT_SYMBOL_GPL(XHdcp22Cipher_SetRiv);
EXPORT_SYMBOL_GPL(XHdcp22Cmn_Aes128Encrypt);
EXPORT_SYMBOL_GPL(XHdcp22Cmn_HmacSha256Hash);
EXPORT_SYMBOL_GPL(XHdcp22Cmn_Sha256Hash);
EXPORT_SYMBOL_GPL(XHdcp22_mmult_CfgInitialize);
EXPORT_SYMBOL_GPL(XHdcp22_mmult_IsDone);
EXPORT_SYMBOL_GPL(XHdcp22_mmult_IsReady);
EXPORT_SYMBOL_GPL(XHdcp22_mmult_LookupConfig);
EXPORT_SYMBOL_GPL(XHdcp22_mmult_Read_U_Words);
EXPORT_SYMBOL_GPL(XHdcp22_mmult_Start);
EXPORT_SYMBOL_GPL(XHdcp22_mmult_Write_A_Words);
EXPORT_SYMBOL_GPL(XHdcp22_mmult_Write_B_Words);
EXPORT_SYMBOL_GPL(XHdcp22_mmult_Write_NPrime_Words);
EXPORT_SYMBOL_GPL(XHdcp22_mmult_Write_N_Words);
EXPORT_SYMBOL_GPL(XHdcp22Rng_CfgInitialize);
EXPORT_SYMBOL_GPL(XHdcp22Rng_GetRandom);
EXPORT_SYMBOL_GPL(XHdcp22Rng_LookupConfig);
EXPORT_SYMBOL_GPL(XTmrCtr_CfgInitialize);
EXPORT_SYMBOL_GPL(XTmrCtr_GetValue);
EXPORT_SYMBOL_GPL(XTmrCtr_LookupConfig);
EXPORT_SYMBOL_GPL(XTmrCtr_Reset);
EXPORT_SYMBOL_GPL(XTmrCtr_SetHandler);
EXPORT_SYMBOL_GPL(XTmrCtr_SetOptions);
EXPORT_SYMBOL_GPL(XTmrCtr_SetResetValue);
EXPORT_SYMBOL_GPL(XTmrCtr_Stop);
EXPORT_SYMBOL_GPL(XTmrCtr_Start);

EXPORT_SYMBOL_GPL(XHdcp22_Cipher_ConfigTable);
EXPORT_SYMBOL_GPL(XHdcp22_mmult_ConfigTable);
EXPORT_SYMBOL_GPL(XHdcp22_Rng_ConfigTable);
#endif
