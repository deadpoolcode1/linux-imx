/****************************************************************************
*
*    The MIT License (MIT)
*
*    Copyright (c) 2014 - 2017 Vivante Corporation
*
*    Permission is hereby granted, free of charge, to any person obtaining a
*    copy of this software and associated documentation files (the "Software"),
*    to deal in the Software without restriction, including without limitation
*    the rights to use, copy, modify, merge, publish, distribute, sublicense,
*    and/or sell copies of the Software, and to permit persons to whom the
*    Software is furnished to do so, subject to the following conditions:
*
*    The above copyright notice and this permission notice shall be included in
*    all copies or substantial portions of the Software.
*
*    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
*    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
*    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
*    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
*    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
*    FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
*    DEALINGS IN THE SOFTWARE.
*
*****************************************************************************
*
*    The GPL License (GPL)
*
*    Copyright (C) 2014 - 2017 Vivante Corporation
*
*    This program is free software; you can redistribute it and/or
*    modify it under the terms of the GNU General Public License
*    as published by the Free Software Foundation; either version 2
*    of the License, or (at your option) any later version.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU General Public License for more details.
*
*    You should have received a copy of the GNU General Public License
*    along with this program; if not, write to the Free Software Foundation,
*    Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
*
*****************************************************************************
*
*    Note: This software is released under dual MIT and GPL licenses. A
*    recipient may use this file under the terms of either the MIT license or
*    GPL License. If you wish to use only one license not the other, you can
*    indicate your decision by deleting one of the above license notices in your
*    version of this file.
*
*****************************************************************************/


#include "gc_hal_kernel_linux.h"
#include "gc_hal_kernel_platform.h"
#include "gc_hal_kernel_device.h"
#include "gc_hal_driver.h"
#include <linux/slab.h>
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,10,0)
#include <linux/of_platform.h>
#include <linux/of_gpio.h>
#include <linux/of_address.h>
#endif

#if USE_PLATFORM_DRIVER
#   include <linux/platform_device.h>
#endif
#include <linux/component.h>

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(4,9,0)) || defined(IMX8_SCU_CONTROL)
#define IMX_GPU_SUBSYSTEM   1
#else
#define IMX_GPU_SUBSYSTEM   0
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(3,5,0)
#include <mach/viv_gpu.h>
#else
#include <linux/pm_runtime.h>
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 10, 0)
#include <mach/busfreq.h>
#elif LINUX_VERSION_CODE < KERNEL_VERSION(3, 14, 29)
#include <linux/busfreq-imx6.h>
#include <linux/reset.h>
#else
#if !defined(IMX8_SCU_CONTROL)
#include <linux/busfreq-imx.h>
#endif
#include <linux/reset.h>
#endif
#endif

#include <linux/clk.h>

#if defined(IMX8_SCU_CONTROL)
#include <soc/imx8/sc/sci.h>
static sc_ipc_t gpu_ipcHandle;
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(3,10,0)
#include <mach/hardware.h>
#endif
#include <linux/pm_runtime.h>

#include <linux/regulator/consumer.h>

#ifdef CONFIG_DEVICE_THERMAL
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,10,0)
#include <linux/device_cooling.h>
#define REG_THERMAL_NOTIFIER(a) register_devfreq_cooling_notifier(a);
#define UNREG_THERMAL_NOTIFIER(a) unregister_devfreq_cooling_notifier(a);
#else
extern int register_thermal_notifier(struct notifier_block *nb);
extern int unregister_thermal_notifier(struct notifier_block *nb);
#define REG_THERMAL_NOTIFIER(a) register_thermal_notifier(a);
#define UNREG_THERMAL_NOTIFIER(a) unregister_thermal_notifier(a);
#endif
#endif

#ifndef gcdFSL_CONTIGUOUS_SIZE
#define gcdFSL_CONTIGUOUS_SIZE (4 << 20)
#endif

static int initgpu3DMinClock = 1;
module_param(initgpu3DMinClock, int, 0644);

struct platform_device *pdevice;

#ifdef CONFIG_GPU_LOW_MEMORY_KILLER
#    include <linux/kernel.h>
#    include <linux/mm.h>
#    include <linux/oom.h>
#    include <linux/sched.h>
#    include <linux/profile.h>

struct task_struct *lowmem_deathpending;

static int
task_notify_func(struct notifier_block *self, unsigned long val, void *data);

static struct notifier_block task_nb = {
    .notifier_call  = task_notify_func,
};

static int
task_notify_func(struct notifier_block *self, unsigned long val, void *data)
{
    struct task_struct *task = data;

    if (task == lowmem_deathpending)
        lowmem_deathpending = NULL;

    return NOTIFY_DONE;
}

extern struct task_struct *lowmem_deathpending;
static unsigned long lowmem_deathpending_timeout;

static int force_contiguous_lowmem_shrink(IN gckKERNEL Kernel)
{
    struct task_struct *p;
    struct task_struct *selected = NULL;
    int tasksize;
        int ret = -1;
    int min_adj = 0;
    int selected_tasksize = 0;
    int selected_oom_adj;
    /*
     * If we already have a death outstanding, then
     * bail out right away; indicating to vmscan
     * that we have nothing further to offer on
     * this pass.
     *
     */
    if (lowmem_deathpending &&
        time_before_eq(jiffies, lowmem_deathpending_timeout))
        return 0;
    selected_oom_adj = min_adj;

       rcu_read_lock();
    for_each_process(p) {
        struct mm_struct *mm;
        struct signal_struct *sig;
                gcuDATABASE_INFO info;
        int oom_adj;

        task_lock(p);
        mm = p->mm;
        sig = p->signal;
        if (!mm || !sig) {
            task_unlock(p);
            continue;
        }
        oom_adj = sig->oom_score_adj;
        if (oom_adj < min_adj) {
            task_unlock(p);
            continue;
        }

        tasksize = 0;
        task_unlock(p);
               rcu_read_unlock();

        if (gckKERNEL_QueryProcessDB(Kernel, p->pid, gcvFALSE, gcvDB_VIDEO_MEMORY, &info) == gcvSTATUS_OK){
            tasksize += info.counters.bytes / PAGE_SIZE;
        }
        if (gckKERNEL_QueryProcessDB(Kernel, p->pid, gcvFALSE, gcvDB_CONTIGUOUS, &info) == gcvSTATUS_OK){
            tasksize += info.counters.bytes / PAGE_SIZE;
        }

               rcu_read_lock();

        if (tasksize <= 0)
            continue;

        gckOS_Print("<gpu> pid %d (%s), adj %d, size %d \n", p->pid, p->comm, oom_adj, tasksize);

        if (selected) {
            if (oom_adj < selected_oom_adj)
                continue;
            if (oom_adj == selected_oom_adj &&
                tasksize <= selected_tasksize)
                continue;
        }
        selected = p;
        selected_tasksize = tasksize;
        selected_oom_adj = oom_adj;
    }
    if (selected && selected_oom_adj > 0) {
        gckOS_Print("<gpu> send sigkill to %d (%s), adj %d, size %d\n",
                 selected->pid, selected->comm,
                 selected_oom_adj, selected_tasksize);
        lowmem_deathpending = selected;
        lowmem_deathpending_timeout = jiffies + HZ;
        force_sig(SIGKILL, selected);
        ret = 0;
    }
       rcu_read_unlock();
    return ret;
}

extern gckKERNEL
_GetValidKernel(
  gckGALDEVICE Device
  );

gceSTATUS
_ShrinkMemory(
    IN gckPLATFORM Platform
    )
{
    struct platform_device *pdev;
    gckGALDEVICE galDevice;
    gckKERNEL kernel;
    gceSTATUS status = gcvSTATUS_OK;

    pdev = Platform->device;

    galDevice = platform_get_drvdata(pdev);

    kernel = _GetValidKernel(galDevice);

    if (kernel != gcvNULL)
    {
        if (force_contiguous_lowmem_shrink(kernel) != 0)
            status = gcvSTATUS_OUT_OF_MEMORY;
    }
    else
    {
        gcmkPRINT("%s(%d) can't find kernel! ", __FUNCTION__, __LINE__);
    }

    return status;
}
#endif

#if gcdENABLE_FSCALE_VAL_ADJUST && (defined(CONFIG_DEVICE_THERMAL) || defined(CONFIG_DEVICE_THERMAL_MODULE))
static int thermal_hot_pm_notify(struct notifier_block *nb, unsigned long event,
       void *dummy)
{
    static gctUINT orgFscale, minFscale, maxFscale;
    static gctBOOL bAlreadyTooHot = gcvFALSE;
    gckHARDWARE hardware;
    gckGALDEVICE galDevice;

    galDevice = platform_get_drvdata(pdevice);
    if (!galDevice)
    {
        /* GPU is not ready, so it is meaningless to change GPU freq. */
        return NOTIFY_OK;
    }

    if (!galDevice->kernels[gcvCORE_MAJOR])
    {
        return NOTIFY_OK;
    }

    hardware = galDevice->kernels[gcvCORE_MAJOR]->hardware;

    if (!hardware)
    {
        return NOTIFY_OK;
    }

    if (event && !bAlreadyTooHot) {
        gckHARDWARE_GetFscaleValue(hardware,&orgFscale,&minFscale, &maxFscale);
        gckHARDWARE_SetFscaleValue(hardware, minFscale);
        bAlreadyTooHot = gcvTRUE;
        gckOS_Print("System is too hot. GPU3D will work at %d/64 clock.\n", minFscale);
    } else if (!event && bAlreadyTooHot) {
        gckHARDWARE_SetFscaleValue(hardware, orgFscale);
        gckOS_Print("Hot alarm is canceled. GPU3D clock will return to %d/64\n", orgFscale);
        bAlreadyTooHot = gcvFALSE;
    }
    return NOTIFY_OK;
}

static struct notifier_block thermal_hot_pm_notifier = {
    .notifier_call = thermal_hot_pm_notify,
    };

static ssize_t show_gpu3DMinClock(struct device_driver *dev, char *buf)
{
    gctUINT currentf,minf,maxf;
    gckGALDEVICE galDevice;

    galDevice = platform_get_drvdata(pdevice);
    if(galDevice->kernels[gcvCORE_MAJOR])
    {
         gckHARDWARE_GetFscaleValue(galDevice->kernels[gcvCORE_MAJOR]->hardware,
            &currentf, &minf, &maxf);
    }
    snprintf(buf, PAGE_SIZE, "%d\n", minf);
    return strlen(buf);
}

static ssize_t update_gpu3DMinClock(struct device_driver *dev, const char *buf, size_t count)
{

    gctINT fields;
    gctUINT MinFscaleValue;
    gckGALDEVICE galDevice;

    galDevice = platform_get_drvdata(pdevice);
    if(galDevice->kernels[gcvCORE_MAJOR])
    {
         fields = sscanf(buf, "%d", &MinFscaleValue);
         if (fields < 1)
             return -EINVAL;

         gckHARDWARE_SetMinFscaleValue(galDevice->kernels[gcvCORE_MAJOR]->hardware,MinFscaleValue);
    }

    return count;
}

static DRIVER_ATTR(gpu3DMinClock, S_IRUGO | S_IWUSR, show_gpu3DMinClock, update_gpu3DMinClock);
#endif




#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,5,0)
static const struct of_device_id mxs_gpu_dt_ids[] = {
#if IMX_GPU_SUBSYSTEM
    { .compatible = "fsl,imx8-gpu-ss", },
#endif
    { .compatible = "fsl,imx6q-gpu", }, /*Backward Compatiblity */
    {/* sentinel */}
};
MODULE_DEVICE_TABLE(of, mxs_gpu_dt_ids);
#endif

struct gpu_clk {
    struct clk     *clk_core;
    struct clk     *clk_shader;
    struct clk     *clk_axi;
    struct clk     *clk_ahb;
};

struct imx_priv {

    struct gpu_clk     imx_gpu_clks[gcdMAX_GPU_COUNT];

#if LINUX_VERSION_CODE < KERNEL_VERSION(3,14,0)
#if LINUX_VERSION_CODE < KERNEL_VERSION(3,5,0) || LINUX_VERSION_CODE >= KERNEL_VERSION(3,10,0)
    /*Power management.*/
    struct regulator      *gpu_regulator;
#endif
#endif
       /*Run time pm*/
    struct device         *pmdev[gcdMAX_GPU_COUNT];

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,10,0)
    struct reset_control *rstc[gcdMAX_GPU_COUNT];
#endif
    int gpu3dCount;
};

static struct imx_priv imxPriv;

#if IMX_GPU_SUBSYSTEM
static int gpu_device_bind(struct device *dev, struct device *master,
	void *data)
{
    return 0;
}

static void gpu_device_unbind(struct device *dev, struct device *master,
	void *data)
{
}

static const struct component_ops gpu_ops = {
    .bind = gpu_device_bind,
    .unbind = gpu_device_unbind,
};

static const struct of_device_id gpu_match[] = {
    { .compatible = "fsl,imx8-gpu"},
    { /* sentinel */ }
};

static int gpu_device_probe(struct platform_device *pdev)
{
    return component_add(&pdev->dev, &gpu_ops);
}

static int gpu_device_remove(struct platform_device *pdev)
{
    component_del(&pdev->dev, &gpu_ops);
    return 0;
}
struct platform_driver mxc_gpu_driver = {
    .driver = {
        .name = "mxc-gpu",
        .owner = THIS_MODULE,
        .of_match_table = gpu_match,
    },
    .probe = gpu_device_probe,
    .remove = gpu_device_remove,
};
gceSTATUS
gckPLATFORM_RegisterDevice(
    IN gckPLATFORM Platform
    )
{
    return platform_driver_register(&mxc_gpu_driver);
}

gceSTATUS
gckPLATFORM_UnRegisterDevice(
    IN gckPLATFORM Platform
    )
{
    platform_driver_unregister(&mxc_gpu_driver);
    return gcvSTATUS_OK;
}

static int compare_of(struct device *dev, void *data)
{
    struct device_node *np = data;

    return dev->of_node == np;
}
#endif
/*TODO: Fix */
struct component_match *match = NULL;

gceSTATUS
gckPLATFORM_AdjustParam(
    IN gckPLATFORM Platform,
    OUT gcsMODULE_PARAMETERS *Args
    )
{
    struct resource* res;
    struct platform_device* pdev = Platform->device;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,10,0)
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(3,5,0)
    struct device_node *dn =pdev->dev.of_node;
    const u32 *prop;
#else
    struct viv_gpu_platform_data *pdata;
#endif
#if IMX_GPU_SUBSYSTEM
    struct device_node *node =pdev->dev.of_node;
    if (node && Platform->ops->registerDevice) {
        int i=0;
        struct device_node *core_node;
        gctINT  core = gcvCORE_MAJOR;
        while ((core_node = of_parse_phandle(node, "cores", i++)) != NULL) {
            struct platform_device *pdev_gpu;
            gctINT  irqLine = -1;

            if(!of_device_is_available(core_node)){
                of_node_put(core_node);
                continue;
            }
            component_match_add(&pdev->dev, &match, compare_of, core_node);

            pdev_gpu = of_find_device_by_node(core_node);
            if (!pdev_gpu) {
                break;
            }
            irqLine = platform_get_irq(pdev_gpu, 0);
            if (irqLine < 0) {
                break;
            }
            res = platform_get_resource(pdev_gpu, IORESOURCE_MEM, 0);
            if (!res) {
                break;
            }

            Args->irqs[core] = irqLine;
            Args->registerBases[core] = res->start;
            Args->registerSizes[core] = res->end - res->start + 1;

            of_node_put(core_node);
            ++core;
        }
        if(core_node) {
            of_node_put(core_node);
        }
    }
    else
#endif
    {
        res = platform_get_resource_byname(pdev, IORESOURCE_IRQ, "irq_3d");
        if (res)
            Args->irqLine = res->start;

        res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "iobase_3d");
        if (res)
        {
            Args->registerMemBase = res->start;
            Args->registerMemSize = res->end - res->start + 1;
        }

        res = platform_get_resource_byname(pdev, IORESOURCE_IRQ, "irq_2d");
        if (res)
            Args->irqLine2D = res->start;

        res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "iobase_2d");
        if (res)
        {
            Args->registerMemBase2D = res->start;
            Args->registerMemSize2D = res->end - res->start + 1;
        }

        res = platform_get_resource_byname(pdev, IORESOURCE_IRQ, "irq_vg");
        if (res)
            Args->irqLineVG = res->start;

        res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "iobase_vg");
        if (res)
        {
            Args->registerMemBaseVG = res->start;
            Args->registerMemSizeVG = res->end - res->start + 1;
        }
    }

    res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "phys_baseaddr");
    if (res && !Args->baseAddress && !Args->physSize)
    {
        Args->baseAddress = res->start;
        Args->physSize = res->end - res->start + 1;
    }

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,1,0)
    res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "contiguous_mem");
    if (res)
    {
        if( Args->contiguousBase == 0 )
           Args->contiguousBase = res->start;
        if( Args->contiguousSize == ~0U )
           Args->contiguousSize = res->end - res->start + 1;
    }
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(3,10,0)
       Args->contiguousBase = 0;
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(3,5,0)
       prop = of_get_property(dn, "contiguousbase", NULL);
       if(prop)
               Args->contiguousBase = *prop;
       of_property_read_u32(dn,"contiguoussize", (u32 *)&contiguousSize);
#else
    pdata = pdev->dev.platform_data;
    if (pdata) {
        Args->contiguousBase = pdata->reserved_mem_base;
       Args->contiguousSize = pdata->reserved_mem_size;
     }
#endif
    if (Args->contiguousSize == ~0U)
    {
       gckOS_Print("Warning: No contiguous memory is reserverd for gpu.!\n ");
       gckOS_Print("Warning: Will use default value(%d) for the reserved memory!\n ",gcdFSL_CONTIGUOUS_SIZE);
       Args->contiguousSize = gcdFSL_CONTIGUOUS_SIZE;
    }

    Args->gpu3DMinClock = initgpu3DMinClock;

    if(Args->physSize == 0)
    {
#if defined(IMX8_PHYS_BASE)
        Args->baseAddress = IMX8_PHYS_BASE;
#endif

#if defined(IMX8_PHYS_SIZE)
        Args->physSize = IMX8_PHYS_SIZE;
#else
        Args->physSize = 0x80000000;
#endif
    }

    return gcvSTATUS_OK;
}

gceSTATUS
_AllocPriv(
    IN gckPLATFORM Platform
    )
{
    Platform->priv = &imxPriv;

    gckOS_ZeroMemory(Platform->priv, sizeof(imxPriv));

#ifdef CONFIG_GPU_LOW_MEMORY_KILLER
#if LINUX_VERSION_CODE < KERNEL_VERSION(4,1,0)
    task_free_register(&task_nb);
#else
    task_handoff_register(&task_nb);
#endif
#endif

    return gcvSTATUS_OK;
}

gceSTATUS
_FreePriv(
    IN gckPLATFORM Platform
    )
{
#ifdef CONFIG_GPU_LOW_MEMORY_KILLER
#if LINUX_VERSION_CODE < KERNEL_VERSION(4,1,0)
     task_free_unregister(&task_nb);
#else
    task_handoff_unregister(&task_nb);
#endif
#endif

    return gcvSTATUS_OK;
}

gceSTATUS
_SetClock(
    IN gckPLATFORM Platform,
    IN gceCORE GPU,
    IN gctBOOL Enable
    );

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,10,0)
static void imx6sx_optimize_qosc_for_GPU(IN gckPLATFORM Platform)
{
    struct device_node *np;
    void __iomem *src_base;

    np = of_find_compatible_node(NULL, NULL, "fsl,imx6sx-qosc");
    if (!np)
        return;

    src_base = of_iomap(np, 0);
    WARN_ON(!src_base);
    _SetClock(Platform, gcvCORE_MAJOR, gcvTRUE);
    writel_relaxed(0, src_base); /* Disable clkgate & soft_rst */
    writel_relaxed(0, src_base+0x60); /* Enable all masters */
    writel_relaxed(0, src_base+0x1400); /* Disable clkgate & soft_rst for gpu */
    writel_relaxed(0x0f000222, src_base+0x1400+0xd0); /* Set Write QoS 2 for gpu */
    writel_relaxed(0x0f000822, src_base+0x1400+0xe0); /* Set Read QoS 8 for gpu */
    _SetClock(Platform, gcvCORE_MAJOR, gcvFALSE);
    return;
}
#endif

gceSTATUS
_GetPower(
    IN gckPLATFORM Platform
    )
{
    struct device* pdev = &Platform->device->dev;
    struct imx_priv *priv = Platform->priv;
#if IMX_GPU_SUBSYSTEM
    struct device_node *node = pdev->of_node;
#endif
    struct clk *clk_core = NULL;
    struct clk *clk_shader = NULL;
    struct clk *clk_axi = NULL;

    /*Initialize the clock structure*/
#if IMX_GPU_SUBSYSTEM
    if (node && Platform->ops->registerDevice) {
        int i=0;
        struct device_node *core_node;
        gctINT  core = gcvCORE_MAJOR;
#if defined(IMX8_SCU_CONTROL)
        sc_err_t sciErr;
        uint32_t mu_id;
        sc_rsrc_t  sc_gpu_pid[gcdMAX_GPU_COUNT];

        sciErr = sc_ipc_getMuID(&mu_id);
        if (sciErr != SC_ERR_NONE) {
            gckOS_Print("galcore; cannot obtain mu id\n");
            return gcvSTATUS_FALSE;
        }

        sciErr = sc_ipc_open(&gpu_ipcHandle, mu_id);
        if (sciErr != SC_ERR_NONE) {
            gckOS_Print("galcore: cannot open MU channel to SCU\n");
            return gcvSTATUS_FALSE;
        };
#endif
        while ((core_node = of_parse_phandle(node, "cores", i++)) != NULL) {
            struct platform_device *pdev_gpu = NULL;
            clk_shader = NULL;
            clk_core = NULL;
            clk_axi = NULL;

            if(!of_device_is_available(core_node)){
                of_node_put(core_node);
                continue;
            }

            pdev_gpu = of_find_device_by_node(core_node);
            if (!pdev_gpu) {
                break;
            }
            clk_core = clk_get(&pdev_gpu->dev, "core");
            if (IS_ERR(clk_core)) {
                gckOS_Print("galcore: clk_get clk_core failed\n");
                break;
            }
            clk_axi = clk_get(&pdev_gpu->dev, "bus");
            if (IS_ERR(clk_axi)) {
                clk_axi = NULL;
            }

            clk_shader = clk_get(&pdev_gpu->dev, "shader");
            if (IS_ERR(clk_shader)) {
                gckOS_Print("galcore: clk_get clk_3d_shader failed\n");
                continue;
            }

            priv->imx_gpu_clks[core].clk_shader = clk_shader;
            priv->imx_gpu_clks[core].clk_core = clk_core;
            priv->imx_gpu_clks[core].clk_axi = clk_axi;

#if defined(IMX8_SCU_CONTROL)
            if(!of_property_read_u32(core_node, "fsl,sc_gpu_pid", &sc_gpu_pid[core]))
            {
                sciErr = sc_misc_set_control(gpu_ipcHandle, sc_gpu_pid[core], SC_C_ID, core);
                if (sciErr != SC_ERR_NONE)
                    gckOS_Print("galcore: failed to set gpu id for 3d_%d\n", core);

                /* set single mode by default to avoid the potential impact by inter-signals */
                sciErr = sc_misc_set_control(gpu_ipcHandle, sc_gpu_pid[core], SC_C_SINGLE_MODE, 1);
                if (sciErr != SC_ERR_NONE)
                    gckOS_Print("galcore: failed to set gpu single mode for 3d_%d\n", core);
            }
#endif

#ifdef CONFIG_PM
            pm_runtime_enable(&pdev_gpu->dev);
            priv->pmdev[core] = &pdev_gpu->dev;
#endif
            of_node_put(core_node);
            core++;
        }
        priv->gpu3dCount = core;

        if(core_node) {
            of_node_put(core_node);
        }
#if defined(IMX8_SCU_CONTROL)
        if(priv->gpu3dCount > 1) {
            for (core=gcvCORE_MAJOR; core <priv->gpu3dCount; core++)
            {
                if (sc_gpu_pid[core])
                {
                    sciErr = sc_misc_set_control(gpu_ipcHandle, sc_gpu_pid[core], SC_C_SINGLE_MODE, 0);
                    if (sciErr != SC_ERR_NONE)
                        gckOS_Print("galcore: failed to set gpu dual mode for 3d_%d\n", core);
                }
            }
        }
#endif
    }
    else
#endif
    {
#ifdef CONFIG_RESET_CONTROLLER
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,10,0)
        struct reset_control *rstc;
        rstc = devm_reset_control_get(pdev, "gpu3d");
        priv->rstc[gcvCORE_MAJOR] = IS_ERR(rstc) ? NULL : rstc;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,9,0)
        rstc = devm_reset_control_get_shared(pdev, "gpu2d");
        priv->rstc[gcvCORE_2D] = IS_ERR(rstc) ? NULL : rstc;
        rstc = devm_reset_control_get_shared(pdev, "gpuvg");
#else
        rstc = devm_reset_control_get(pdev, "gpu2d");
        priv->rstc[gcvCORE_2D] = IS_ERR(rstc) ? NULL : rstc;
        rstc = devm_reset_control_get(pdev, "gpuvg");
#endif
        priv->rstc[gcvCORE_VG] = IS_ERR(rstc) ? NULL : rstc;
#endif
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(3,14,0)
#if LINUX_VERSION_CODE < KERNEL_VERSION(3,5,0)
        /*get gpu regulator*/
        priv->gpu_regulator = regulator_get(pdev, "cpu_vddgpu");
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(3,10,0)
        priv->gpu_regulator = devm_regulator_get(pdev, "pu");
#endif
#if LINUX_VERSION_CODE < KERNEL_VERSION(3,5,0) || LINUX_VERSION_CODE >= KERNEL_VERSION(3,10,0)
        if (IS_ERR(priv->gpu_regulator)) {
           gcmkTRACE_ZONE(gcvLEVEL_ERROR, gcvZONE_DRIVER,
                   "%s(%d): Failed to get gpu regulator \n",
                   __FUNCTION__, __LINE__);
           return gcvSTATUS_NOT_FOUND;
        }
#endif
#endif
        clk_core = clk_get(pdev, "gpu3d_clk");
        if (!IS_ERR(clk_core)) {
            clk_axi = clk_get(pdev, "gpu3d_axi_clk");
            clk_shader = clk_get(pdev, "gpu3d_shader_clk");
            if (IS_ERR(clk_shader)) {
                clk_put(clk_core);
                clk_core = NULL;
                clk_shader = NULL;
                gckOS_Print("galcore: clk_get gpu3d_shader_clk failed, disable 3d!\n");
            }
        } else {
            clk_core = NULL;
            gckOS_Print("galcore: clk_get gpu3d_clk failed, disable 3d!\n");
        }

        priv->imx_gpu_clks[gcvCORE_MAJOR].clk_core = clk_core;
        priv->imx_gpu_clks[gcvCORE_MAJOR].clk_shader = clk_shader;
        priv->imx_gpu_clks[gcvCORE_MAJOR].clk_axi = clk_axi;

        clk_core = clk_get(pdev, "gpu2d_clk");
        if (IS_ERR(clk_core)) {
            clk_core = NULL;
            gckOS_Print("galcore: clk_get 2d core clock failed, disable 2d/vg!\n");
        } else {
            clk_axi = clk_get(pdev, "gpu2d_axi_clk");
            if (IS_ERR(clk_axi)) {
                clk_axi = NULL;
                gckOS_Print("galcore: clk_get 2d axi clock failed, disable 2d\n");
            }

            priv->imx_gpu_clks[gcvCORE_2D].clk_core = clk_core;
            priv->imx_gpu_clks[gcvCORE_2D].clk_axi = clk_axi;

            clk_axi = clk_get(pdev, "openvg_axi_clk");
            if (IS_ERR(clk_axi)) {
                clk_axi = NULL;
                gckOS_Print("galcore: clk_get vg clock failed, disable vg!\n");
            }

            priv->imx_gpu_clks[gcvCORE_VG].clk_core = clk_core;
            priv->imx_gpu_clks[gcvCORE_VG].clk_axi = clk_axi;
        }

#ifdef CONFIG_PM
        pm_runtime_enable(pdev);
        priv->pmdev[gcvCORE_MAJOR] = pdev;
        priv->pmdev[gcvCORE_2D] = pdev;
        priv->pmdev[gcvCORE_VG] = pdev;
#endif
    }

#if gcdENABLE_FSCALE_VAL_ADJUST && (defined(CONFIG_DEVICE_THERMAL) || defined(CONFIG_DEVICE_THERMAL_MODULE))
    pdevice = Platform->device;
    REG_THERMAL_NOTIFIER(&thermal_hot_pm_notifier);
    {
        int ret = 0;
        ret = driver_create_file(pdevice->dev.driver, &driver_attr_gpu3DMinClock);
        if(ret)
            dev_err(&pdevice->dev, "create gpu3DMinClock attr failed (%d)\n", ret);
    }
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,10,0)
    imx6sx_optimize_qosc_for_GPU(Platform);
#endif

    return gcvSTATUS_OK;
}

gceSTATUS
_PutPower(
    IN gckPLATFORM Platform
    )
{
    int core=0;
    struct gpu_clk *imx_clk = NULL;
    struct imx_priv *priv = Platform->priv;
    struct device *pmdev_last = NULL;/*legacy gpu device entry for imx6*/
    struct clk *clk_core_last = NULL;/*vg has same core clk as 2d */

    for (core =0; core < gcdMAX_GPU_COUNT; core++)
    {
        imx_clk = &priv->imx_gpu_clks[core];

        if(imx_clk->clk_core && imx_clk->clk_core != clk_core_last) {
             clk_put(imx_clk->clk_core);
             clk_core_last = imx_clk->clk_core;
             imx_clk->clk_core = NULL;
        }

        if(imx_clk->clk_shader) {
             clk_put(imx_clk->clk_shader);
             imx_clk->clk_shader = NULL;
        }

        if(imx_clk->clk_axi) {
             clk_put(imx_clk->clk_axi);
             imx_clk->clk_axi = NULL;
        }

#ifdef CONFIG_PM
        if(priv->pmdev[core] && priv->pmdev[core] != pmdev_last){
            pm_runtime_disable(priv->pmdev[core]);
            pmdev_last = priv->pmdev[core];
        }
#endif
    }

#if LINUX_VERSION_CODE < KERNEL_VERSION(3,5,0)
    if (priv->gpu_regulator) {
       regulator_put(priv->gpu_regulator);
       priv->gpu_regulator = NULL;
    }
#endif

#if gcdENABLE_FSCALE_VAL_ADJUST && (defined(CONFIG_DEVICE_THERMAL) || defined(CONFIG_DEVICE_THERMAL_MODULE))
    UNREG_THERMAL_NOTIFIER(&thermal_hot_pm_notifier);

    driver_remove_file(pdevice->dev.driver, &driver_attr_gpu3DMinClock);
#endif

#if defined(IMX8_SCU_CONTROL)
    if (gpu_ipcHandle)
        sc_ipc_close(gpu_ipcHandle);
#endif

    return gcvSTATUS_OK;
}

gceSTATUS
_SetPower(
    IN gckPLATFORM Platform,
    IN gceCORE GPU,
    IN gctBOOL Enable
    )
{
#ifdef CONFIG_PM
    struct imx_priv* priv = Platform->priv;
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(3,14,0)
#if LINUX_VERSION_CODE < KERNEL_VERSION(3,5,0) || LINUX_VERSION_CODE >= KERNEL_VERSION(3,10,0)
    int ret;
#endif
#endif

    if (Enable)
    {
#if LINUX_VERSION_CODE < KERNEL_VERSION(3,14,0)
#if LINUX_VERSION_CODE < KERNEL_VERSION(3,5,0) || LINUX_VERSION_CODE >= KERNEL_VERSION(3,10,0)
        if(!IS_ERR(priv->gpu_regulator)) {
            ret = regulator_enable(priv->gpu_regulator);
            if (ret != 0)
                gckOS_Print("%s(%d): fail to enable pu regulator %d!\n",
                    __FUNCTION__, __LINE__, ret);
        }
#else
        imx_gpc_power_up_pu(true);
#endif
#endif

#ifdef CONFIG_PM
        pm_runtime_get_sync(priv->pmdev[GPU]);
#endif
    }
    else
    {
#ifdef CONFIG_PM
        pm_runtime_put_sync(priv->pmdev[GPU]);
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(3,14,0)
#if LINUX_VERSION_CODE < KERNEL_VERSION(3,5,0) || LINUX_VERSION_CODE >= KERNEL_VERSION(3,10,0)
        if(!IS_ERR(priv->gpu_regulator))
            regulator_disable(priv->gpu_regulator);
#else
        imx_gpc_power_up_pu(false);
#endif
#endif

    }

    return gcvSTATUS_OK;
}

gceSTATUS
_SetClock(
    IN gckPLATFORM Platform,
    IN gceCORE GPU,
    IN gctBOOL Enable
    )
{
    struct imx_priv* priv = Platform->priv;
    struct clk *clk_core = priv->imx_gpu_clks[GPU].clk_core;
    struct clk *clk_shader = priv->imx_gpu_clks[GPU].clk_shader;
    struct clk *clk_axi = priv->imx_gpu_clks[GPU].clk_axi;

    if (Enable) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,5,0)
        if(clk_core) clk_prepare(clk_core);
        if(clk_shader) clk_prepare(clk_shader);
        if(clk_axi) clk_prepare(clk_axi);
#endif
        if(clk_core) clk_enable(clk_core);
        if(clk_shader) clk_enable(clk_shader);
        if(clk_axi) clk_enable(clk_axi);
    } else {
        if(clk_core) clk_disable(clk_core);
        if(clk_shader) clk_disable(clk_shader);
        if(clk_axi) clk_disable(clk_axi);
 #if LINUX_VERSION_CODE >= KERNEL_VERSION(3,5,0)
        if(clk_core) clk_unprepare(clk_core);
        if(clk_shader) clk_unprepare(clk_shader);
        if(clk_axi) clk_unprepare(clk_axi);
#endif
    }

    return gcvSTATUS_OK;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,5,0)
#ifdef CONFIG_PM
#ifdef CONFIG_PM_RUNTIME
static int gpu_runtime_suspend(struct device *dev)
{
    release_bus_freq(BUS_FREQ_HIGH);
    return 0;
}

static int gpu_runtime_resume(struct device *dev)
{
    request_bus_freq(BUS_FREQ_HIGH);
    return 0;
}
#endif

static struct dev_pm_ops gpu_pm_ops;
#endif
#endif

gceSTATUS
_AdjustDriver(
    IN gckPLATFORM Platform
    )
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,5,0)
    struct platform_driver * driver = Platform->driver;
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,5,0)
    driver->driver.of_match_table = mxs_gpu_dt_ids;
#endif

#ifdef CONFIG_PM
    /* Override PM callbacks to add runtime PM callbacks. */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,5,0)
    /* Fill local structure with original value. */
    memcpy(&gpu_pm_ops, driver->driver.pm, sizeof(struct dev_pm_ops));

    /* Add runtime PM callback. */
#ifdef CONFIG_PM_RUNTIME
    gpu_pm_ops.runtime_suspend = gpu_runtime_suspend;
    gpu_pm_ops.runtime_resume = gpu_runtime_resume;
    gpu_pm_ops.runtime_idle = NULL;
#endif

    /* Replace callbacks. */
    driver->driver.pm = &gpu_pm_ops;
#endif
#endif

    return gcvSTATUS_OK;
}

gceSTATUS
_Reset(
    IN gckPLATFORM Platform,
    gceCORE GPU
    )
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(3,5,0)
#define SRC_SCR_OFFSET 0
#define BP_SRC_SCR_GPU3D_RST 1
#define BP_SRC_SCR_GPU2D_RST 4
    void __iomem *src_base = IO_ADDRESS(SRC_BASE_ADDR);
    gctUINT32 bit_offset,val;

    if(GPU == gcvCORE_MAJOR) {
        bit_offset = BP_SRC_SCR_GPU3D_RST;
    } else if((GPU == gcvCORE_VG)
            ||(GPU == gcvCORE_2D)) {
        bit_offset = BP_SRC_SCR_GPU2D_RST;
    } else {
        return gcvSTATUS_INVALID_CONFIG;
    }
    val = __raw_readl(src_base + SRC_SCR_OFFSET);
    val &= ~(1 << (bit_offset));
    val |= (1 << (bit_offset));
    __raw_writel(val, src_base + SRC_SCR_OFFSET);

    while ((__raw_readl(src_base + SRC_SCR_OFFSET) &
                (1 << (bit_offset))) != 0) {
    }

    return gcvSTATUS_NOT_SUPPORTED;
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(3,10,0)
    struct imx_priv* priv = Platform->priv;
    struct reset_control *rstc = priv->rstc[GPU];
    if (rstc)
        reset_control_reset(rstc);
#else
    imx_src_reset_gpu((int)GPU);
#endif
    return gcvSTATUS_OK;
}

gcmkPLATFROM_Name

gcsPLATFORM_OPERATIONS platformOperations = {
    .adjustParam  = gckPLATFORM_AdjustParam,
    .allocPriv    = _AllocPriv,
    .freePriv     = _FreePriv,
    .getPower     = _GetPower,
    .putPower     = _PutPower,
    .setPower     = _SetPower,
    .setClock     = _SetClock,
    .reset        = _Reset,
    .adjustDriver = _AdjustDriver,
#ifdef CONFIG_GPU_LOW_MEMORY_KILLER
    .shrinkMemory = _ShrinkMemory,
#endif
#if IMX_GPU_SUBSYSTEM
    .registerDevice  = gckPLATFORM_RegisterDevice,
    .unRegisterDevice  = gckPLATFORM_UnRegisterDevice,
#endif
    .name          = _Name,
};

void
gckPLATFORM_QueryOperations(
    IN gcsPLATFORM_OPERATIONS ** Operations
    )
{
#if IMX_GPU_SUBSYSTEM
    if (!of_find_compatible_node(NULL, NULL, "fsl,imx8-gpu-ss")) {
        platformOperations.registerDevice = NULL;
        platformOperations.unRegisterDevice = NULL;
    }
#endif

    *Operations = &platformOperations;
}

