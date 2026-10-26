// SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note
/*
 *
 * (C) COPYRIGHT 2015-2024 ARM Limited. All rights reserved.
 *
 * This program is free software and is provided to you under the terms of the
 * GNU General Public License version 2 as published by the Free Software
 * Foundation, and any use by you of this program is subject to the terms
 * of such GNU license.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, you can access it online at
 * http://www.gnu.org/licenses/gpl-2.0.html.
 *
 */

#include <mali_kbase.h>
#include <mali_kbase_defs.h>
#include <device/mali_kbase_device.h>
#include <linux/pm_runtime.h>
#include <linux/reset.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/regulator/consumer.h>

#include "mali_kbase_config_platform.h"

static struct reset_control **resets;
static int nr_resets;

static void resets_init(struct kbase_device *kbdev)
{
	struct device_node *np;
	int i;

	np = kbdev->dev->of_node;

	nr_resets = of_count_phandle_with_args(np, "resets", "#reset-cells");
	if (nr_resets <= 0) {
		nr_resets = 0;
		return;
	}

	resets = devm_kcalloc(kbdev->dev, (size_t)nr_resets, sizeof(*resets),
			      GFP_KERNEL);
	if (!resets) {
		nr_resets = 0;
		return;
	}

	for (i = 0; i < nr_resets; i++) {
		resets[i] = devm_reset_control_get_exclusive_by_index(
				kbdev->dev, i);
		if (IS_ERR(resets[i])) {
			dev_warn(kbdev->dev,
				 "Failed to get GPU reset %d, skipping resets\n",
				 i);
			resets = NULL;
			nr_resets = 0;
			return;
		}
	}
}

static int pm_callback_soft_reset(struct kbase_device *kbdev)
{
	int i;

	if (resets) {
		for (i = 0; i < nr_resets; i++)
			reset_control_assert(resets[i]);

		udelay(10);

		for (i = 0; i < nr_resets; i++)
			reset_control_deassert(resets[i]);

		udelay(10);
	}

	/* Return 0 to let the normal soft reset sequence proceed */
	return 0;
}

/*
 * When CONFIG_PM_CLK is active, the genpd framework calls
 * pm_clk_suspend/pm_clk_resume which also manages device clocks.
 * The original code used __clk_is_enabled() to skip redundant
 * enable/disable, but this races with pm_clk and can corrupt the
 * ref-count, causing "already disabled" / "enabling unprepared"
 * WARNs and eventual crash.
 *
 * Fix: always call clk_prepare_enable/clk_disable_unprepare
 * (ref-counted) so the count stays balanced regardless of pm_clk
 * activity.  Same treatment for regulators.
 */
/*
 * Whether the GPU's clocks and regulators are held. The system suspend and
 * resume callbacks run whatever the runtime state: a suspend with the GPU
 * already runtime-suspended disabled nothing (the clock core only warns), and
 * the resume then took a fresh reference, one more per cycle. On ums512 a
 * held GPU clock keeps AON GPU_EB on, and with it the 26 MHz crystal through
 * deep sleep. Turn on and off only on a change of state.
 */
static bool gpu_power_control_on;

static void enable_gpu_power_control(struct kbase_device *kbdev)
{
	unsigned int i;

	if (gpu_power_control_on)
		return;

#if defined(CONFIG_REGULATOR)
	for (i = 0; i < kbdev->nr_regulators; i++) {
		if (kbdev->regulators[i] && regulator_enable(kbdev->regulators[i]))
			dev_warn(kbdev->dev, "failed to enable regulator %u\n", i);
	}
#endif

	for (i = 0; i < kbdev->nr_clocks; i++) {
		if (kbdev->clocks[i])
			clk_prepare_enable(kbdev->clocks[i]);
	}
	gpu_power_control_on = true;
}

static void disable_gpu_power_control_clocks(struct kbase_device *kbdev)
{
	unsigned int i;

	for (i = 0; i < kbdev->nr_clocks; i++) {
		if (kbdev->clocks[i])
			clk_disable_unprepare(kbdev->clocks[i]);
	}
}

static void disable_gpu_power_control(struct kbase_device *kbdev)
{
	unsigned int i;

	if (!gpu_power_control_on)
		return;

	disable_gpu_power_control_clocks(kbdev);

#if defined(CONFIG_REGULATOR)
	for (i = 0; i < kbdev->nr_regulators; i++) {
		if (kbdev->regulators[i])
			regulator_disable(kbdev->regulators[i]);
	}
#endif
	gpu_power_control_on = false;
}

static int pm_callback_power_on(struct kbase_device *kbdev)
{
	int ret = 1; /* Assume GPU has been powered off */
	int error;
	unsigned long flags;

	dev_dbg(kbdev->dev, "%s %pK\n", __func__, (void *)kbdev->dev->pm_domain);

	spin_lock_irqsave(&kbdev->hwaccess_lock, flags);
	WARN_ON(kbdev->pm.backend.gpu_powered);
#if MALI_USE_CSF
	if (likely(kbdev->csf.firmware_inited)) {
		WARN_ON(!kbdev->pm.active_count);
		WARN_ON(kbdev->pm.runtime_active);
	}
	spin_unlock_irqrestore(&kbdev->hwaccess_lock, flags);

	enable_gpu_power_control(kbdev);
	CSTD_UNUSED(error);
#else
	spin_unlock_irqrestore(&kbdev->hwaccess_lock, flags);

#ifdef KBASE_PM_RUNTIME
	error = pm_runtime_get_sync(kbdev->dev);
	if (error == 1) {
		/*
		 * Let core know that the chip has not been
		 * powered off, so we can save on re-initialization.
		 */
		ret = 0;
	}
	dev_dbg(kbdev->dev, "pm_runtime_get_sync returned %d\n", error);
#else
	enable_gpu_power_control(kbdev);
#endif /* KBASE_PM_RUNTIME */

#endif /* MALI_USE_CSF */

	return ret;
}

static void pm_callback_power_off(struct kbase_device *kbdev)
{
	unsigned long flags;

	dev_dbg(kbdev->dev, "%s\n", __func__);

	spin_lock_irqsave(&kbdev->hwaccess_lock, flags);
	WARN_ON(kbdev->pm.backend.gpu_powered);
#if MALI_USE_CSF
	if (likely(kbdev->csf.firmware_inited)) {
#ifdef CONFIG_MALI_DEBUG
		WARN_ON(kbase_csf_scheduler_get_nr_active_csgs(kbdev));
#endif
		WARN_ON(kbdev->pm.backend.mcu_state != KBASE_MCU_OFF);
	}
	spin_unlock_irqrestore(&kbdev->hwaccess_lock, flags);

	/* Power down the GPU immediately */
	disable_gpu_power_control(kbdev);
#else /* MALI_USE_CSF */
	spin_unlock_irqrestore(&kbdev->hwaccess_lock, flags);

#ifdef KBASE_PM_RUNTIME
	pm_runtime_mark_last_busy(kbdev->dev);
	pm_runtime_put_autosuspend(kbdev->dev);
#else
	/* Power down the GPU immediately as runtime PM is disabled */
	disable_gpu_power_control(kbdev);
#endif
#endif /* MALI_USE_CSF */
}

#if MALI_USE_CSF && defined(KBASE_PM_RUNTIME)
static void pm_callback_runtime_gpu_active(struct kbase_device *kbdev)
{
	unsigned long flags;
	int error;

	lockdep_assert_held(&kbdev->pm.lock);

	spin_lock_irqsave(&kbdev->hwaccess_lock, flags);
	WARN_ON(!kbdev->pm.backend.gpu_powered);
	WARN_ON(!kbdev->pm.active_count);
	WARN_ON(kbdev->pm.runtime_active);
	spin_unlock_irqrestore(&kbdev->hwaccess_lock, flags);

	if (pm_runtime_status_suspended(kbdev->dev)) {
		error = pm_runtime_get_sync(kbdev->dev);
		dev_dbg(kbdev->dev, "pm_runtime_get_sync returned %d", error);
	} else {
		/* Call the async version here, otherwise there could be
		 * a deadlock if the runtime suspend operation is ongoing.
		 * Caller would have taken the kbdev->pm.lock and/or the
		 * scheduler lock, and the runtime suspend callback function
		 * will also try to acquire the same lock(s).
		 */
		error = pm_runtime_get(kbdev->dev);
		dev_dbg(kbdev->dev, "pm_runtime_get returned %d", error);
	}

	kbdev->pm.runtime_active = true;
}

static void pm_callback_runtime_gpu_idle(struct kbase_device *kbdev)
{
	unsigned long flags;

	lockdep_assert_held(&kbdev->pm.lock);

	dev_dbg(kbdev->dev, "%s", __func__);

	spin_lock_irqsave(&kbdev->hwaccess_lock, flags);
	WARN_ON(!kbdev->pm.backend.gpu_powered);
	WARN_ON(kbdev->pm.backend.l2_state != KBASE_L2_OFF);
	WARN_ON(kbdev->pm.active_count);
	WARN_ON(!kbdev->pm.runtime_active);
	spin_unlock_irqrestore(&kbdev->hwaccess_lock, flags);

	pm_runtime_mark_last_busy(kbdev->dev);
	pm_runtime_put_autosuspend(kbdev->dev);
	kbdev->pm.runtime_active = false;
}
#endif

#ifdef KBASE_PM_RUNTIME
static int kbase_device_runtime_init(struct kbase_device *kbdev)
{
	int ret = 0;

	dev_dbg(kbdev->dev, "%s\n", __func__);

	pm_runtime_set_autosuspend_delay(kbdev->dev, AUTO_SUSPEND_DELAY);
	pm_runtime_use_autosuspend(kbdev->dev);

	/*
	 * Use set_suspended (not set_active) so the first pm_runtime_get_sync
	 * actually calls through to genpd and powers on the domain. set_active
	 * marks the device as already on, causing get_sync to return immediately
	 * before hardware is ready, leading to an SError on first register write.
	 */
	pm_runtime_set_suspended(kbdev->dev);

	/*
	 * power_control_init() enabled every clock at probe and keeps that
	 * reference for good; with the runtime callbacks counting their own
	 * on top, the clocks never reach zero. On ums512 three of them are
	 * children of AON GPU_EB, so the GPU_TOP domain's power-off could never
	 * gate it, and with GPU_EB on AP_SYS never reaches deep sleep. The
	 * device starts out suspended: hand the probe's references back, and
	 * the first pm_runtime_get_sync() takes its own.
	 */
	disable_gpu_power_control_clocks(kbdev);
	gpu_power_control_on = false;

	pm_runtime_enable(kbdev->dev);

	if (!pm_runtime_enabled(kbdev->dev)) {
		dev_warn(kbdev->dev, "pm_runtime not enabled");
		ret = -EINVAL;
	} else if (atomic_read(&kbdev->dev->power.usage_count)) {
		dev_warn(kbdev->dev, "%s: Device runtime usage count unexpectedly non zero %d",
			 __func__, atomic_read(&kbdev->dev->power.usage_count));
		ret = -EINVAL;
	}

	if (!ret)
		resets_init(kbdev);

	return ret;
}

static void kbase_device_runtime_disable(struct kbase_device *kbdev)
{
	dev_dbg(kbdev->dev, "%s\n", __func__);

	if (atomic_read(&kbdev->dev->power.usage_count))
		dev_warn(kbdev->dev, "%s: Device runtime usage count unexpectedly non zero %d",
			 __func__, atomic_read(&kbdev->dev->power.usage_count));

	pm_runtime_disable(kbdev->dev);
}
#endif /* KBASE_PM_RUNTIME */

static int pm_callback_runtime_on(struct kbase_device *kbdev)
{
	dev_dbg(kbdev->dev, "%s\n", __func__);

#if !MALI_USE_CSF
	enable_gpu_power_control(kbdev);
#endif
	return 0;
}

static void pm_callback_runtime_off(struct kbase_device *kbdev)
{
	dev_dbg(kbdev->dev, "%s\n", __func__);

#if !MALI_USE_CSF
	disable_gpu_power_control(kbdev);
#endif
}

static void pm_callback_resume(struct kbase_device *kbdev)
{
	int ret = pm_callback_runtime_on(kbdev);

	WARN_ON(ret);
}

static void pm_callback_suspend(struct kbase_device *kbdev)
{
	pm_callback_runtime_off(kbdev);
}


struct kbase_pm_callback_conf pm_callbacks = {
	.power_on_callback = pm_callback_power_on,
	.power_off_callback = pm_callback_power_off,
	.power_suspend_callback = pm_callback_suspend,
	.soft_reset_callback = pm_callback_soft_reset,
	.power_resume_callback = pm_callback_resume,
#ifdef KBASE_PM_RUNTIME
	.power_runtime_init_callback = kbase_device_runtime_init,
	.power_runtime_term_callback = kbase_device_runtime_disable,
	.power_runtime_on_callback = pm_callback_runtime_on,
	.power_runtime_off_callback = pm_callback_runtime_off,
#else /* KBASE_PM_RUNTIME */
	.power_runtime_init_callback = NULL,
	.power_runtime_term_callback = NULL,
	.power_runtime_on_callback = NULL,
	.power_runtime_off_callback = NULL,
#endif /* KBASE_PM_RUNTIME */

#if MALI_USE_CSF && defined(KBASE_PM_RUNTIME)
	.power_runtime_gpu_idle_callback = pm_callback_runtime_gpu_idle,
	.power_runtime_gpu_active_callback = pm_callback_runtime_gpu_active,
#else
	.power_runtime_gpu_idle_callback = NULL,
	.power_runtime_gpu_active_callback = NULL,
#endif
};
