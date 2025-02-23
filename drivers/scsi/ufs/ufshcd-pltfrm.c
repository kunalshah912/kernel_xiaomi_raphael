/*
 * Universal Flash Storage Host controller Platform bus based glue driver
 *
 * This code is based on drivers/scsi/ufs/ufshcd-pltfrm.c
 * Copyright (C) 2011-2013 Samsung India Software Operations
 *
 * Authors:
 *	Santosh Yaraganavi <santosh.sy@samsung.com>
 *	Vinayak Holikatti <h.vinayak@samsung.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * See the COPYING file in the top-level directory or visit
 * <http://www.gnu.org/licenses/gpl-2.0.html>
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * This program is provided "AS IS" and "WITH ALL FAULTS" and
 * without warranty of any kind. You are solely responsible for
 * determining the appropriateness of using and distributing
 * the program and assume all risks associated with your exercise
 * of rights with respect to the program, including but not limited
 * to infringement of third party rights, the risks and costs of
 * program errors, damage to or loss of data, programs or equipment,
 * and unavailability or interruption of operations. Under no
 * circumstances will the contributor of this Program be liable for
 * any damages of any kind arising from your use or distribution of
 * this program.
 */

#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/of.h>

#include "ufshcd.h"
#include "ufshcd-pltfrm.h"

#define UFSHCD_DEFAULT_LANES_PER_DIRECTION		2

static int ufshcd_parse_reset_info(struct ufs_hba *hba)
{
	int ret = 0;

	hba->core_reset = devm_reset_control_get(hba->dev,
				"core_reset");
	if (IS_ERR(hba->core_reset)) {
		ret = PTR_ERR(hba->core_reset);
		dev_err(hba->dev, "core_reset unavailable,err = %d\n",
				ret);
		hba->core_reset = NULL;
	}

	return ret;
}

static int ufshcd_parse_clock_info(struct ufs_hba *hba)
{
	int ret = 0;
	int cnt;
	int i;
	struct device *dev = hba->dev;
	struct device_node *np = dev->of_node;
	char *name;
	u32 *clkfreq = NULL;
	struct ufs_clk_info *clki;
	int len = 0;
	size_t sz = 0;

	if (!np)
		goto out;

	cnt = of_property_count_strings(np, "clock-names");
	if (!cnt || (cnt == -EINVAL)) {
		dev_info(dev, "%s: Unable to find clocks, assuming enabled\n",
				__func__);
	} else if (cnt < 0) {
		dev_err(dev, "%s: count clock strings failed, err %d\n",
				__func__, cnt);
		ret = cnt;
	}

	if (cnt <= 0)
		goto out;

	if (!of_get_property(np, "freq-table-hz", &len)) {
		dev_info(dev, "freq-table-hz property not specified\n");
		goto out;
	}

	if (len <= 0)
		goto out;

	sz = len / sizeof(*clkfreq);
	if (sz != 2 * cnt) {
		dev_err(dev, "%s len mismatch\n", "freq-table-hz");
		ret = -EINVAL;
		goto out;
	}

	clkfreq = devm_kzalloc(dev, sz * sizeof(*clkfreq),
			GFP_KERNEL);
	if (!clkfreq) {
		ret = -ENOMEM;
		goto out;
	}

	ret = of_property_read_u32_array(np, "freq-table-hz",
			clkfreq, sz);
	if (ret && (ret != -EINVAL)) {
		dev_err(dev, "%s: error reading array %d\n",
				"freq-table-hz", ret);
		return ret;
	}

	for (i = 0; i < sz; i += 2) {
		ret = of_property_read_string_index(np,
				"clock-names", i/2, (const char **)&name);
		if (ret)
			goto out;

		clki = devm_kzalloc(dev, sizeof(*clki), GFP_KERNEL);
		if (!clki) {
			ret = -ENOMEM;
			goto out;
		}

		clki->min_freq = clkfreq[i];
		clki->max_freq = clkfreq[i+1];
		clki->name = kstrdup(name, GFP_KERNEL);
		dev_dbg(dev, "%s: min %u max %u name %s\n", "freq-table-hz",
				clki->min_freq, clki->max_freq, clki->name);
		list_add_tail(&clki->list, &hba->clk_list_head);
	}
out:
	return ret;
}

#define MAX_PROP_SIZE 32
static int ufshcd_populate_vreg(struct device *dev, const char *name,
		struct ufs_vreg **out_vreg)
{
	int len, ret = 0;
	char prop_name[MAX_PROP_SIZE];
	struct ufs_vreg *vreg = NULL;
	struct device_node *np = dev->of_node;
	const __be32 *prop;

	if (!np) {
		dev_err(dev, "%s: non DT initialization\n", __func__);
		goto out;
	}

	snprintf(prop_name, MAX_PROP_SIZE, "%s-supply", name);
	if (!of_parse_phandle(np, prop_name, 0)) {
		dev_info(dev, "%s: Unable to find %s regulator, assuming enabled\n",
				__func__, prop_name);
		goto out;
	}

	vreg = devm_kzalloc(dev, sizeof(*vreg), GFP_KERNEL);
	if (!vreg)
		return -ENOMEM;

	vreg->name = kstrdup(name, GFP_KERNEL);

	/* if fixed regulator no need further initialization */
	snprintf(prop_name, MAX_PROP_SIZE, "%s-fixed-regulator", name);
	if (of_property_read_bool(np, prop_name))
		goto out;

	snprintf(prop_name, MAX_PROP_SIZE, "%s-max-microamp", name);
	ret = of_property_read_u32(np, prop_name, &vreg->max_uA);
	if (ret) {
		dev_err(dev, "%s: unable to find %s err %d\n",
				__func__, prop_name, ret);
		goto out;
	}

	snprintf(prop_name, MAX_PROP_SIZE, "%s-min-microamp", name);
	if (of_property_read_u32(np, prop_name, &vreg->min_uA))
		vreg->min_uA = UFS_VREG_LPM_LOAD_UA;

	if (!strcmp(name, "vcc")) {
		if (of_property_read_bool(np, "vcc-supply-1p8")) {
			vreg->min_uV = UFS_VREG_VCC_1P8_MIN_UV;
			vreg->max_uV = UFS_VREG_VCC_1P8_MAX_UV;
		} else {
			prop = of_get_property(np, "vcc-voltage-level", &len);
			if (!prop || (len != (2 * sizeof(__be32)))) {
				dev_warn(dev, "%s vcc-voltage-level property.\n",
					prop ? "invalid format" : "no");
				vreg->min_uV = UFS_VREG_VCC_MIN_UV;
				vreg->max_uV = UFS_VREG_VCC_MAX_UV;
			} else {
				vreg->min_uV = be32_to_cpup(&prop[0]);
				vreg->max_uV = be32_to_cpup(&prop[1]);
			}

			if (of_property_read_bool(np, "vcc-low-voltage-sup"))
				vreg->low_voltage_sup = true;
		}
	} else if (!strcmp(name, "vccq")) {
		vreg->min_uV = UFS_VREG_VCCQ_MIN_UV;
		vreg->max_uV = UFS_VREG_VCCQ_MAX_UV;
	} else if (!strcmp(name, "vccq2")) {
		prop = of_get_property(np, "vccq2-voltage-level", &len);
		if (!prop || (len != (2 * sizeof(__be32)))) {
			dev_warn(dev, "%s vccq2-voltage-level property.\n",
				prop ? "invalid format" : "no");
			vreg->min_uV = UFS_VREG_VCCQ2_MIN_UV;
			vreg->max_uV = UFS_VREG_VCCQ2_MAX_UV;
		} else {
			vreg->min_uV = be32_to_cpup(&prop[0]);
			vreg->max_uV = be32_to_cpup(&prop[1]);
		}
	}

	goto out;

out:
	if (!ret)
		*out_vreg = vreg;
	return ret;
}

/**
 * ufshcd_parse_regulator_info - get regulator info from device tree
 * @hba: per adapter instance
 *
 * Get regulator info from device tree for vcc, vccq, vccq2 power supplies.
 * If any of the supplies are not defined it is assumed that they are always-on
 * and hence return zero. If the property is defined but parsing is failed
 * then return corresponding error.
 */
static int ufshcd_parse_regulator_info(struct ufs_hba *hba)
{
	int err;
	struct device *dev = hba->dev;
	struct ufs_vreg_info *info = &hba->vreg_info;

	err = ufshcd_populate_vreg(dev, "vdd-hba", &info->vdd_hba);
	if (err)
		goto out;

	err = ufshcd_populate_vreg(dev, "vcc", &info->vcc);
	if (err)
		goto out;

	err = ufshcd_populate_vreg(dev, "vccq", &info->vccq);
	if (err)
		goto out;

	err = ufshcd_populate_vreg(dev, "vccq2", &info->vccq2);
out:
	return err;
}

static void ufshcd_parse_pm_levels(struct ufs_hba *hba)
{
	struct device *dev = hba->dev;
	struct device_node *np = dev->of_node;

	if (np) {
		if (of_property_read_u32(np, "rpm-level", &hba->rpm_lvl))
			hba->rpm_lvl = -1;
		if (of_property_read_u32(np, "spm-level", &hba->spm_lvl))
			hba->spm_lvl = -1;
	}
}

static int ufshcd_parse_pinctrl_info(struct ufs_hba *hba)
{
	int ret = 0;

	/* Try to obtain pinctrl handle */
	hba->pctrl = devm_pinctrl_get(hba->dev);
	if (IS_ERR(hba->pctrl)) {
		ret = PTR_ERR(hba->pctrl);
		hba->pctrl = NULL;
	}

	return ret;
}

static int ufshcd_parse_extcon_info(struct ufs_hba *hba)
{
	struct extcon_dev *extcon;

	extcon = extcon_get_edev_by_phandle(hba->dev, 0);
	if (IS_ERR(extcon) && PTR_ERR(extcon) != -ENODEV)
		return PTR_ERR(extcon);

	if (!IS_ERR(extcon))
		hba->extcon = extcon;

	return 0;
}

static void ufshcd_parse_gear_limits(struct ufs_hba *hba)
{
	struct device *dev = hba->dev;
	struct device_node *np = dev->of_node;
	int ret;

	if (!np)
		return;

	ret = of_property_read_u32(np, "limit-tx-hs-gear",
		&hba->limit_tx_hs_gear);
	if (ret)
		hba->limit_tx_hs_gear = -1;

	ret = of_property_read_u32(np, "limit-rx-hs-gear",
		&hba->limit_rx_hs_gear);
	if (ret)
		hba->limit_rx_hs_gear = -1;

	ret = of_property_read_u32(np, "limit-tx-pwm-gear",
		&hba->limit_tx_pwm_gear);
	if (ret)
		hba->limit_tx_pwm_gear = -1;

	ret = of_property_read_u32(np, "limit-rx-pwm-gear",
		&hba->limit_rx_pwm_gear);
	if (ret)
		hba->limit_rx_pwm_gear = -1;
}

static void ufshcd_parse_cmd_timeout(struct ufs_hba *hba)
{
	struct device *dev = hba->dev;
	struct device_node *np = dev->of_node;
	int ret;

	if (!np)
		return;

	ret = of_property_read_u32(np, "scsi-cmd-timeout",
		&hba->scsi_cmd_timeout);
	if (ret)
		hba->scsi_cmd_timeout = 0;
}

static void ufshcd_parse_force_g4_flag(struct ufs_hba *hba)
{
	if (device_property_read_bool(hba->dev, "force-g4"))
		hba->force_g4 = true;
	else
		hba->force_g4 = false;
}

static void ufshcd_parse_dev_ref_clk_freq(struct ufs_hba *hba)
{
	struct device *dev = hba->dev;
	struct device_node *np = dev->of_node;
	int ret;

	if (!np)
		return;

	ret = of_property_read_u32(np, "dev-ref-clk-freq",
				   &hba->dev_ref_clk_freq);
	if (ret ||
	    (hba->dev_ref_clk_freq < 0) ||
	    (hba->dev_ref_clk_freq > REF_CLK_FREQ_52_MHZ))
		/* default setting */
		hba->dev_ref_clk_freq = REF_CLK_FREQ_26_MHZ;
}

#ifdef CONFIG_SMP
/**
 * ufshcd_pltfrm_restore - restore power management function
 * @dev: pointer to device handle
 *
 * Returns 0 if successful
 * Returns non-zero otherwise
 */
int ufshcd_pltfrm_restore(struct device *dev)
{
	return ufshcd_system_restore(dev_get_drvdata(dev));
}
EXPORT_SYMBOL(ufshcd_pltfrm_restore);

/**
 * ufshcd_pltfrm_freeze - freeze power management function
 * @dev: pointer to device handle
 *
 * Returns 0 if successful
 * Returns non-zero otherwise
 */
int ufshcd_pltfrm_freeze(struct device *dev)
{
	return ufshcd_system_freeze(dev_get_drvdata(dev));
}
EXPORT_SYMBOL(ufshcd_pltfrm_freeze);

/**
 * ufshcd_pltfrm_thaw - freeze power management function
 * @dev: pointer to device handle
 *
 * Returns 0 if successful
 * Returns non-zero otherwise
 */
int ufshcd_pltfrm_thaw(struct device *dev)
{
	return ufshcd_system_thaw(dev_get_drvdata(dev));
}
EXPORT_SYMBOL(ufshcd_pltfrm_thaw);

/**
 * ufshcd_pltfrm_suspend - suspend power management function
 * @dev: pointer to device handle
 *
 * Returns 0 if successful
 * Returns non-zero otherwise
 */
int ufshcd_pltfrm_suspend(struct device *dev)
{
	return ufshcd_system_suspend(dev_get_drvdata(dev));
}
EXPORT_SYMBOL_GPL(ufshcd_pltfrm_suspend);

/**
 * ufshcd_pltfrm_resume - resume power management function
 * @dev: pointer to device handle
 *
 * Returns 0 if successful
 * Returns non-zero otherwise
 */
int ufshcd_pltfrm_resume(struct device *dev)
{
	return ufshcd_system_resume(dev_get_drvdata(dev));
}
EXPORT_SYMBOL_GPL(ufshcd_pltfrm_resume);

int ufshcd_pltfrm_runtime_suspend(struct device *dev)
{
	return ufshcd_runtime_suspend(dev_get_drvdata(dev));
}
EXPORT_SYMBOL_GPL(ufshcd_pltfrm_runtime_suspend);

int ufshcd_pltfrm_runtime_resume(struct device *dev)
{
	return ufshcd_runtime_resume(dev_get_drvdata(dev));
}
EXPORT_SYMBOL_GPL(ufshcd_pltfrm_runtime_resume);

int ufshcd_pltfrm_runtime_idle(struct device *dev)
{
	return ufshcd_runtime_idle(dev_get_drvdata(dev));
}
EXPORT_SYMBOL_GPL(ufshcd_pltfrm_runtime_idle);

#endif /* CONFIG_PM */

void ufshcd_pltfrm_shutdown(struct platform_device *pdev)
{
	ufshcd_shutdown((struct ufs_hba *)platform_get_drvdata(pdev));
}
EXPORT_SYMBOL_GPL(ufshcd_pltfrm_shutdown);

static void ufshcd_init_lanes_per_dir(struct ufs_hba *hba)
{
	struct device *dev = hba->dev;
	int ret;

	ret = of_property_read_u32(dev->of_node, "lanes-per-direction",
		&hba->lanes_per_direction);
	if (ret) {
		dev_dbg(hba->dev,
			"%s: failed to read lanes-per-direction, ret=%d\n",
			__func__, ret);
		hba->lanes_per_direction = UFSHCD_DEFAULT_LANES_PER_DIRECTION;
	}
}

/**
 * ufshcd_pltfrm_init - probe routine of the driver
 * @pdev: pointer to Platform device handle
 * @var: pointer to variant specific data
 *
 * Returns 0 on success, non-zero value on failure
 */
int ufshcd_pltfrm_init(struct platform_device *pdev,
		       struct ufs_hba_variant *var)
{
	struct ufs_hba *hba;
	void __iomem *mmio_base;
	struct resource *mem_res;
	int irq, err;
	struct device *dev = &pdev->dev;

	mem_res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	mmio_base = devm_ioremap_resource(dev, mem_res);
	if (IS_ERR(mmio_base)) {
		err = PTR_ERR(mmio_base);
		goto out;
	}

	irq = platform_get_irq(pdev, 0);
	if (irq < 0) {
		dev_err(dev, "IRQ resource not available\n");
		err = -ENODEV;
		goto out;
	}

	err = ufshcd_alloc_host(dev, &hba);
	if (err) {
		dev_err(&pdev->dev, "Allocation failed\n");
		goto out;
	}

	hba->var = var;

	err = ufshcd_parse_clock_info(hba);
	if (err) {
		dev_err(&pdev->dev, "%s: clock parse failed %d\n",
				__func__, err);
		goto dealloc_host;
	}
	err = ufshcd_parse_regulator_info(hba);
	if (err) {
		dev_err(&pdev->dev, "%s: regulator init failed %d\n",
				__func__, err);
		goto dealloc_host;
	}

	err = ufshcd_parse_reset_info(hba);
	if (err) {
		dev_err(&pdev->dev, "%s: reset parse failed %d\n",
				__func__, err);
		goto dealloc_host;
	}

	err = ufshcd_parse_pinctrl_info(hba);
	if (err) {
		dev_dbg(&pdev->dev, "%s: unable to parse pinctrl data %d\n",
				__func__, err);
		/* let's not fail the probe */
	}

	ufshcd_parse_dev_ref_clk_freq(hba);
	ufshcd_parse_pm_levels(hba);
	ufshcd_parse_gear_limits(hba);
	ufshcd_parse_cmd_timeout(hba);
	ufshcd_parse_force_g4_flag(hba);
	err = ufshcd_parse_extcon_info(hba);
	if (err)
		goto dealloc_host;

	if (!dev->dma_mask)
		dev->dma_mask = &dev->coherent_dma_mask;

	ufshcd_init_lanes_per_dir(hba);

	err = ufshcd_init(hba, mmio_base, irq);
	if (err) {
		dev_err(dev, "Initialization failed\n");
		goto dealloc_host;
	}

	platform_set_drvdata(pdev, hba);

	pm_runtime_set_active(&pdev->dev);
	pm_runtime_enable(&pdev->dev);

	return 0;

dealloc_host:
	ufshcd_dealloc_host(hba);
out:
	return err;
}
EXPORT_SYMBOL_GPL(ufshcd_pltfrm_init);

MODULE_AUTHOR("Santosh Yaragnavi <santosh.sy@samsung.com>");
MODULE_AUTHOR("Vinayak Holikatti <h.vinayak@samsung.com>");
MODULE_DESCRIPTION("UFS host controller Platform bus based glue driver");
MODULE_LICENSE("GPL");
MODULE_VERSION(UFSHCD_DRIVER_VERSION);
