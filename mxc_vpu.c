/*
 * Copyright 2006-2013 Freescale Semiconductor, Inc. All Rights Reserved.
 */

/*
 * The code contained herein is licensed under the GNU General Public
 * License. You may obtain a copy of the GNU General Public License
 * Version 2 or later at the following locations:
 *
 * http://www.opensource.org/licenses/gpl-license.html
 * http://www.gnu.org/copyleft/gpl.html
 */

/*!
 * @file mxc_vpu.c
 *
 * @brief VPU system initialization and file operation implementation
 *
 * @ingroup VPU
 */

#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/interrupt.h>
#include <linux/ioport.h>
#include <linux/stat.h>
#include <linux/platform_device.h>
#include <linux/kdev_t.h>
#include <linux/dma-mapping.h>
#include <linux/wait.h>
#include <linux/list.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/fsl_devices.h>
#include <linux/uaccess.h>
#include <linux/io.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/sched.h>
#include <linux/vmalloc.h>
#include <linux/regulator/consumer.h>
#include <linux/page-flags.h>
#include <linux/of.h>
#include <linux/mm_types.h>
#include <linux/types.h>
#include <linux/memblock.h>
#include <linux/memory.h>
#include <linux/version.h>
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 5, 0)
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/of_address.h>
#include <linux/mfd/syscon.h>
#include <linux/regmap.h>
#endif
#include <asm/page.h>
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 5, 0)
#include <linux/sizes.h>
#else
#include <asm/sizes.h>
#endif
#include "mxc_vpu.h"
#include "iram_alloc.h"


/* Define one new pgprot which combined uncached and XN(never executable) */
#define pgprot_noncachedxn(prot) \
	__pgprot_modify(prot, L_PTE_MT_MASK, L_PTE_MT_UNCACHED | L_PTE_XN)

struct vpu_priv {
	struct fasync_struct *async_queue;
	struct work_struct work;
	struct workqueue_struct *workqueue;
	struct mutex lock;
};

/* To track the allocated memory buffer */
struct memalloc_record {
	struct list_head list;
	struct vpu_mem_desc mem;
};

struct iram_setting {
	u32 start;
	u32 end;
};

static LIST_HEAD(head);

static int vpu_major;
static int vpu_clk_usercount;
static struct class *vpu_class;
static struct vpu_priv vpu_data;
static u8 open_count;
static struct clk *vpu_clk;
static struct vpu_mem_desc bitwork_mem = { 0 };
static struct vpu_mem_desc pic_para_mem = { 0 };
static struct vpu_mem_desc user_data_mem = { 0 };
static struct vpu_mem_desc share_mem = { 0 };
static struct vpu_mem_desc vshare_mem = { 0 };

static void __iomem *vpu_base;
static int vpu_ipi_irq;
static u32 phy_vpu_base_addr;
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 5, 0)
static phys_addr_t top_address_DRAM;
static struct mxc_vpu_platform_data *vpu_plat;
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 5, 0)
static struct platform_device *vpu_pdev;
#endif

/* IRAM setting */
static struct iram_setting iram;

/* implement the blocking ioctl */
static int irq_status;
static int codec_done;
static wait_queue_head_t vpu_queue;

#ifdef CONFIG_SOC_IMX6Q
#define MXC_VPU_HAS_JPU
#endif

#ifdef MXC_VPU_HAS_JPU
static int vpu_jpu_irq;
#endif

static unsigned int regBk[64];
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 5, 0)
static struct regulator *vpu_regulator;
#endif
static unsigned int pc_before_suspend;
static atomic_t clk_cnt_from_ioc = ATOMIC_INIT(0);

#define	READ_REG(x)		readl_relaxed(vpu_base + x)
#define	WRITE_REG(val, x)	writel_relaxed(val, vpu_base + x)

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 5, 0)
/* redirect to static functions */
static int cpu_is_mx6dl(void)
{
	int ret;
	ret = of_machine_is_compatible("fsl,imx6dl");
	return ret;
}

static int cpu_is_mx6q(void)
{
	int ret;
	ret = of_machine_is_compatible("fsl,imx6q");
	return ret;
}
#endif

#define GPC_CNTR                0x0
#define GPC_IMR1                0x008
#define GPC_PGC_GPU_PGCR        0x260
#define GPC_PGC_CPU_PDN         0x2a0
#define GPC_PGC_CPU_PUPSCR      0x2a4
#define GPC_PGC_CPU_PDNSCR      0x2a8

#define IMR_NUM                 4

#define REG_SET         0x4
#define REG_CLR         0x8
#define ANA_MISC2       0x170
#define ANA_MISC0       0x150
#define ANA_REG_CORE    0x140
#define ANA_REG_2P5             0x130

#define ANA_USB1_CHRG_DETECT    0x1b0
#define ANA_USB2_CHRG_DETECT    0x210

#define BM_ANADIG_USB_CHRG_DETECT_EN_B  0x100000
#define BM_ANADIG_USB_CHRG_DETECT_CHK_CHRG_B    0x80000

#define ANA_REG_CORE_REG2_OFFSET        18/* VDDSOC */
#define ANA_REG_CORE_REG1_OFFSET        9 /* VDDPU */
#define ANA_REG_CORE_REG0_OFFSET        0 /* VDDARM */
#define ANA_REG_CORE_REG_MASK           0x1f

#define ANA_MISC2_REG1_STEP_OFFSET      26
#define ANA_MISC2_REG_STEP_MASK         0x3

#define LDO_RAMP_UP_UNIT_IN_CYCLES      64 /* 64 cycles per step */
#define LDO_RAMP_UP_FREQ_IN_MHZ         24 /* time base on 24M OSC */

#define ANA_MISC0_CLK_DELAY             26 /* bit26 - bit28 clkgate delay */

#define SRC_SCR                         0x000
#define SRC_SIMR                        0x018
#define SRC_GPR1                        0x020
#define BP_SRC_SCR_WARM_RESET_ENABLE    0
#define BP_SRC_SCR_GPU3D_RST            1
#define BP_SRC_SCR_VPU_RST              2
#define BP_SRC_SCR_GPU2D_RST            4
#define BP_SRC_SCR_CORE1_RST            14
#define BP_SRC_SCR_CORE1_ENABLE         22
#define BP_SRC_SIMR_MASK_VPU            1
#define SRC_IPU1_SWRST                  0x0080
#define SRC_IPU2_SWRST                  0x1000

static DEFINE_SPINLOCK(scr_lock);


static void __iomem *gpc_base;
static u32 gpc_wake_irqs[IMR_NUM];
static u32 gpc_saved_imrs[IMR_NUM];
static u32 gpc_pu_count;
static bool pu_clk_get;
static struct mutex pu_lock;
static struct clk *gpu3d_clk, *gpu3d_shader_clk, *gpu2d_clk, *gpu2d_axi_clk;
static struct clk *openvg_axi_clk, *vpu_clk;

void imx_anatop_pu_vol(bool enable)
{
	struct regmap *anatop;
        unsigned int val;
        unsigned int vddsoc;

	anatop = syscon_regmap_lookup_by_compatible("fsl,imx6q-anatop");

        regmap_read(anatop, ANA_REG_CORE, &val);
        if (enable) {
                /* VDDPU track with VDDSOC if enable */
                val &= ~(ANA_REG_CORE_REG_MASK << ANA_REG_CORE_REG1_OFFSET);
                vddsoc = val & (ANA_REG_CORE_REG_MASK <<                        ANA_REG_CORE_REG2_OFFSET);
                val |= vddsoc >> (ANA_REG_CORE_REG2_OFFSET -
                        ANA_REG_CORE_REG1_OFFSET);
        } else
                /* power off pu */                val &= ~(ANA_REG_CORE_REG_MASK << ANA_REG_CORE_REG1_OFFSET);
        regmap_write(anatop, ANA_REG_CORE, val);

        if (enable) {                /*
                 * the delay time for LDO ramp up time is
                 * based on the register setting, we need
                 * to calculate how many steps LDO need to
                 * ramp up, and how much delay needed. (us)
                 */
                static unsigned int new_vol, delay_u;
                regmap_read(anatop, ANA_MISC2, &val);
                val = (val >> ANA_MISC2_REG1_STEP_OFFSET) &
                        ANA_MISC2_REG_STEP_MASK;
                new_vol = (vddsoc >> ANA_REG_CORE_REG2_OFFSET) &
                                ANA_REG_CORE_REG_MASK;
                /*
                 * Because we can't know what the exact voltage of PU here if
                 * use bypass mode(0x1f), the delay time for LDO ramp up should
                 * rely on external pmic ,here we validate on pfuze. On Sabre
                 * board VDDPU_IN and VDDSOC_IN connected, so max steps on
                 * VDDPU_IN in bypass mode is  4 steps of 25mv(1.175V->1.275V),
                 * and the delay time is 4us*4=16us. You may fine tune it,
                 * especially, you disconnect VDDPU_IN and VDDSOC_IN.
                 * if you want to power on/off external PU regulator to minimal
                 * power leakage, you can add your specific code here.
                 * But in our case, VDDPU_IN and VDDSOC_IN share the same input
                 * from pfuze, and VDDSOC_IN can't be turned off, then the
                 * delay time is decided by VDDPU switch from 0 to bypass, seem
                 * fixed on 50us, enlarge to 70us for safe.
                 */
                if (new_vol == 0x1f)
                        delay_u = 70;
                else
                        delay_u = new_vol  * ((LDO_RAMP_UP_UNIT_IN_CYCLES <<
                                        val) / LDO_RAMP_UP_FREQ_IN_MHZ + 1);
                udelay(delay_u);
        }
}

static void imx_pu_clk(bool enable)
{
        if (enable) {
                clk_prepare(gpu3d_clk);
                clk_enable(gpu3d_clk);
                clk_prepare(gpu3d_shader_clk);
                clk_enable(gpu3d_shader_clk);
                clk_prepare(vpu_clk);
                clk_enable(vpu_clk);
                clk_prepare(gpu2d_clk);
                clk_enable(gpu2d_clk);
                clk_prepare(gpu2d_axi_clk);
                clk_enable(gpu2d_axi_clk);
                clk_prepare(openvg_axi_clk);
                clk_enable(openvg_axi_clk);
        } else {
                clk_disable(gpu3d_clk);
                clk_unprepare(gpu3d_clk);
                clk_disable(gpu3d_shader_clk);
                clk_unprepare(gpu3d_shader_clk);
                clk_disable(vpu_clk);
                clk_unprepare(vpu_clk);
                clk_disable(gpu2d_clk);
                clk_unprepare(gpu2d_clk);
                clk_disable(gpu2d_axi_clk);
                clk_unprepare(gpu2d_axi_clk);
                clk_disable(openvg_axi_clk);
                clk_unprepare(openvg_axi_clk);
        }
}

static void imx_gpc_pu_clk_init(void)
{
        /* Get gpu&vpu clk for power up PU by GPC */
        gpu3d_clk = clk_get(NULL, "gpu3d_core");
        if (IS_ERR(gpu3d_clk))
                printk(KERN_ERR "%s: failed to get gpu3d_clk!\n" , __func__);
        gpu3d_shader_clk = clk_get(NULL, "gpu3d_shader");
        if (IS_ERR(gpu3d_shader_clk))
                printk(KERN_ERR "%s: failed to get shade_clk!\n", __func__);
        vpu_clk = clk_get(NULL, "vpu_axi");
        if (IS_ERR(vpu_clk))
                printk(KERN_ERR "%s: failed to get vpu_clk!\n", __func__);

        gpu2d_clk = clk_get(NULL, "gpu2d_core");
        if (IS_ERR(gpu2d_clk))
                printk(KERN_ERR "%s: failed to get gpu2d_clk!\n" , __func__);
        gpu2d_axi_clk = clk_get(NULL, "gpu2d_axi");
        if (IS_ERR(gpu2d_axi_clk))
                printk(KERN_ERR "%s: failed to get gpu2d_axi_clk!\n", __func__);
        openvg_axi_clk = clk_get(NULL, "openvg_axi");
        if (IS_ERR(openvg_axi_clk))
                printk(KERN_ERR "%s: failed to get openvg_clk!\n", __func__);
}

void imx_gpc_power_up_pu(bool flag)
{
        unsigned int reg;

        mutex_lock(&pu_lock);

        if (!pu_clk_get) {
                imx_gpc_pu_clk_init();
                pu_clk_get = true;
        }

        if (gpc_pu_count && flag) {
                gpc_pu_count++;
                mutex_unlock(&pu_lock);
                return;
        }
        if ((gpc_pu_count > 1) && !flag) {
                gpc_pu_count--;
                mutex_unlock(&pu_lock);
                return;
        }

        if (!gpc_pu_count && flag) {
                /* turn on vddpu */
                imx_anatop_pu_vol(true);
                /* enable pu clock */
                imx_pu_clk(true);
                /* enable power up request */
                reg = __raw_readl(gpc_base + GPC_PGC_GPU_PGCR);
                __raw_writel(reg | 0x1, gpc_base + GPC_PGC_GPU_PGCR);
                /* power up request */
                reg = __raw_readl(gpc_base + GPC_CNTR);
                __raw_writel(reg | 0x2, gpc_base + GPC_CNTR);
                /* Wait for the power up bit to clear */
                while (__raw_readl(gpc_base + GPC_CNTR) & 0x2)
                        ;
                /* disable pu clock */
                imx_pu_clk(false);
                gpc_pu_count++;
        } else if ((gpc_pu_count == 1) && !flag) {
                /* enable power down request */
                reg = __raw_readl(gpc_base + GPC_PGC_GPU_PGCR);
                __raw_writel(reg | 0x1, gpc_base + GPC_PGC_GPU_PGCR);
                /* power down request */
                reg = __raw_readl(gpc_base + GPC_CNTR);
                __raw_writel(reg | 0x1, gpc_base + GPC_CNTR);
                /* Wait for power down to complete */
                while (__raw_readl(gpc_base + GPC_CNTR) & 0x1)
                        ;
                /* turn off vddpu */
                imx_anatop_pu_vol(false);
                gpc_pu_count--;
        }
        mutex_unlock(&pu_lock);
        return;
}

int imx_src_reset_vpu(void)
{
        u32 val;
        unsigned long flags;
	void __iomem *src_base;
	struct device_node *np;

        np = of_find_compatible_node(NULL, NULL, "fsl,imx6q-src");
        src_base = of_iomap(np, 0);
        WARN_ON(!src_base);

        /* mask interrupt due to vpu passed reset */
        val = readl_relaxed(src_base + SRC_SIMR);
        val |= (1 << BP_SRC_SIMR_MASK_VPU);
        writel_relaxed(val, src_base + SRC_SIMR);

        spin_lock_irqsave(&scr_lock, flags);
        val = readl_relaxed(src_base + SRC_SCR);
        val |= (1 << BP_SRC_SCR_VPU_RST);        writel_relaxed(val, src_base + SRC_SCR);
        spin_unlock_irqrestore(&scr_lock, flags);

        while (readl_relaxed(src_base + SRC_SCR) & (1 << BP_SRC_SCR_VPU_RST))
                ;
        return 0;
}


/*!
 * Private function to alloc dma buffer
 * @return status  0 success.
 */
static int vpu_alloc_dma_buffer(struct vpu_mem_desc *mem)
{
	mem->cpu_addr = (unsigned long)
	    dma_alloc_coherent(NULL, PAGE_ALIGN(mem->size),
			       (dma_addr_t *) (&mem->phy_addr),
			       GFP_DMA | GFP_KERNEL);
	pr_debug("[ALLOC] mem alloc cpu_addr = 0x%x\n", mem->cpu_addr);
	if ((void *)(mem->cpu_addr) == NULL) {
		printk(KERN_ERR "Physical memory allocation error!\n");
		return -1;
	}
	return 0;
}

/*!
 * Private function to free dma buffer
 */
static void vpu_free_dma_buffer(struct vpu_mem_desc *mem)
{
	if (mem->cpu_addr != 0) {
		dma_free_coherent(0, PAGE_ALIGN(mem->size),
				  (void *)mem->cpu_addr, mem->phy_addr);
	}
}

/*!
 * Private function to free buffers
 * @return status  0 success.
 */
static int vpu_free_buffers(void)
{
	struct memalloc_record *rec, *n;
	struct vpu_mem_desc mem;

	list_for_each_entry_safe(rec, n, &head, list) {
		mem = rec->mem;
		if (mem.cpu_addr != 0) {
			vpu_free_dma_buffer(&mem);
			pr_debug("[FREE] freed paddr=0x%08X\n", mem.phy_addr);
			/* delete from list */
			list_del(&rec->list);
			kfree(rec);
		}
	}

	return 0;
}

static inline void vpu_worker_callback(struct work_struct *w)
{
	struct vpu_priv *dev = container_of(w, struct vpu_priv,
				work);

	if (dev->async_queue)
		kill_fasync(&dev->async_queue, SIGIO, POLL_IN);

	irq_status = 1;
	/*
	 * Clock is gated on when dec/enc started, gate it off when
	 * codec is done.
	 */
	if (codec_done)
		codec_done = 0;

	wake_up_interruptible(&vpu_queue);
}

/*!
 * @brief vpu interrupt handler
 */
static irqreturn_t vpu_ipi_irq_handler(int irq, void *dev_id)
{
	struct vpu_priv *dev = dev_id;
	unsigned long reg;

	reg = READ_REG(BIT_INT_REASON);
	if (reg & 0x8)
		codec_done = 1;
	WRITE_REG(0x1, BIT_INT_CLEAR);

	queue_work(dev->workqueue, &dev->work);

	return IRQ_HANDLED;
}

/*!
 * @brief vpu jpu interrupt handler
 */
#ifdef MXC_VPU_HAS_JPU
static irqreturn_t vpu_jpu_irq_handler(int irq, void *dev_id)
{
	struct vpu_priv *dev = dev_id;
	unsigned long reg;

	reg = READ_REG(MJPEG_PIC_STATUS_REG);
	if (reg & 0x3)
		codec_done = 1;

	queue_work(dev->workqueue, &dev->work);

	return IRQ_HANDLED;
}
#endif

/*!
 * @brief check phy memory prepare to pass to vpu is valid or not, we
 * already address some issue that if pass a wrong address to vpu
 * (like virtual address), system will hang.
 *
 * @return true return is a valid phy memory address, false return not.
 */
bool vpu_is_valid_phy_memory(u32 paddr)
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 5, 0)
	if (paddr > top_address_DRAM)
		return false;
#endif

	return true;
}

/*!
 * @brief open function for vpu file operation
 *
 * @return  0 on success or negative error code on error
 */
static int vpu_open(struct inode *inode, struct file *filp)
{

	mutex_lock(&vpu_data.lock);

	if (open_count++ == 0) {
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 5, 0)
		if (!IS_ERR(vpu_regulator))
			regulator_enable(vpu_regulator);
#else
		pm_runtime_get_sync(&vpu_pdev->dev);
		imx_gpc_power_up_pu(true);
#endif

#ifdef CONFIG_SOC_IMX6Q
		clk_prepare(vpu_clk);
		clk_enable(vpu_clk);
		if (READ_REG(BIT_CUR_PC))
			pr_debug("Not power off before vpu open!\n");
		clk_disable(vpu_clk);
		clk_unprepare(vpu_clk);
#endif
	}

	filp->private_data = (void *)(&vpu_data);
	mutex_unlock(&vpu_data.lock);
	return 0;
}

/*!
 * @brief IO ctrl function for vpu file operation
 * @param cmd IO ctrl command
 * @return  0 on success or negative error code on error
 */
static long vpu_ioctl(struct file *filp, u_int cmd,
		     u_long arg)
{
	int ret = 0;

	switch (cmd) {
	case VPU_IOC_PHYMEM_ALLOC:
		{
			struct memalloc_record *rec;

			rec = kzalloc(sizeof(*rec), GFP_KERNEL);
			if (!rec)
				return -ENOMEM;

			ret = copy_from_user(&(rec->mem),
					     (struct vpu_mem_desc *)arg,
					     sizeof(struct vpu_mem_desc));
			if (ret) {
				kfree(rec);
				return -EFAULT;
			}

			pr_debug("[ALLOC] mem alloc size = 0x%x\n",
				 rec->mem.size);

			ret = vpu_alloc_dma_buffer(&(rec->mem));
			if (ret == -1) {
				kfree(rec);
				printk(KERN_ERR
				       "Physical memory allocation error!\n");
				break;
			}
			ret = copy_to_user((void __user *)arg, &(rec->mem),
					   sizeof(struct vpu_mem_desc));
			if (ret) {
				kfree(rec);
				ret = -EFAULT;
				break;
			}

			mutex_lock(&vpu_data.lock);
			list_add(&rec->list, &head);
			mutex_unlock(&vpu_data.lock);

			break;
		}
	case VPU_IOC_PHYMEM_FREE:
		{
			struct memalloc_record *rec, *n;
			struct vpu_mem_desc vpu_mem;

			ret = copy_from_user(&vpu_mem,
					     (struct vpu_mem_desc *)arg,
					     sizeof(struct vpu_mem_desc));
			if (ret)
				return -EACCES;

			pr_debug("[FREE] mem freed cpu_addr = 0x%x\n",
				 vpu_mem.cpu_addr);
			if ((void *)vpu_mem.cpu_addr != NULL)
				vpu_free_dma_buffer(&vpu_mem);

			mutex_lock(&vpu_data.lock);
			list_for_each_entry_safe(rec, n, &head, list) {
				if (rec->mem.cpu_addr == vpu_mem.cpu_addr) {
					/* delete from list */
					list_del(&rec->list);
					kfree(rec);
					break;
				}
			}
			mutex_unlock(&vpu_data.lock);

			break;
		}
	case VPU_IOC_WAIT4INT:
		{
			u_long timeout = (u_long) arg;
			if (!wait_event_interruptible_timeout
			    (vpu_queue, irq_status != 0,
			     msecs_to_jiffies(timeout))) {
				printk(KERN_WARNING "VPU blocking: timeout.\n");
				ret = -ETIME;
			} else if (signal_pending(current)) {
				printk(KERN_WARNING
				       "VPU interrupt received.\n");
				ret = -ERESTARTSYS;
			} else
				irq_status = 0;
			break;
		}
	case VPU_IOC_IRAM_SETTING:
		{
			ret = copy_to_user((void __user *)arg, &iram,
					   sizeof(struct iram_setting));
			if (ret)
				ret = -EFAULT;

			break;
		}
	case VPU_IOC_CLKGATE_SETTING:
		{
			u32 clkgate_en;

			if (get_user(clkgate_en, (u32 __user *) arg))
				return -EFAULT;

			if (clkgate_en) {
				clk_prepare(vpu_clk);
				clk_enable(vpu_clk);
				atomic_inc(&clk_cnt_from_ioc);
			} else {
				clk_disable(vpu_clk);
				clk_unprepare(vpu_clk);
				atomic_dec(&clk_cnt_from_ioc);
			}

			break;
		}
	case VPU_IOC_GET_SHARE_MEM:
		{
			mutex_lock(&vpu_data.lock);
			if (share_mem.cpu_addr != 0) {
				ret = copy_to_user((void __user *)arg,
						   &share_mem,
						   sizeof(struct vpu_mem_desc));
				mutex_unlock(&vpu_data.lock);
				break;
			} else {
				if (copy_from_user(&share_mem,
						   (struct vpu_mem_desc *)arg,
						 sizeof(struct vpu_mem_desc))) {
					mutex_unlock(&vpu_data.lock);
					return -EFAULT;
				}
				if (vpu_alloc_dma_buffer(&share_mem) == -1)
					ret = -EFAULT;
				else {
					if (copy_to_user((void __user *)arg,
							 &share_mem,
							 sizeof(struct
								vpu_mem_desc)))
						ret = -EFAULT;
				}
			}
			mutex_unlock(&vpu_data.lock);
			break;
		}
	case VPU_IOC_REQ_VSHARE_MEM:
		{
			mutex_lock(&vpu_data.lock);
			if (vshare_mem.cpu_addr != 0) {
				ret = copy_to_user((void __user *)arg,
						   &vshare_mem,
						   sizeof(struct vpu_mem_desc));
				mutex_unlock(&vpu_data.lock);
				break;
			} else {
				if (copy_from_user(&vshare_mem,
						   (struct vpu_mem_desc *)arg,
						   sizeof(struct
							  vpu_mem_desc))) {
					mutex_unlock(&vpu_data.lock);
					return -EFAULT;
				}
				/* vmalloc shared memory if not allocated */
				if (!vshare_mem.cpu_addr)
					vshare_mem.cpu_addr =
					    (unsigned long)
					    vmalloc_user(vshare_mem.size);
				if (copy_to_user
				     ((void __user *)arg, &vshare_mem,
				     sizeof(struct vpu_mem_desc)))
					ret = -EFAULT;
			}
			mutex_unlock(&vpu_data.lock);
			break;
		}
	case VPU_IOC_GET_WORK_ADDR:
		{
			if (bitwork_mem.cpu_addr != 0) {
				ret =
				    copy_to_user((void __user *)arg,
						 &bitwork_mem,
						 sizeof(struct vpu_mem_desc));
				break;
			} else {
				if (copy_from_user(&bitwork_mem,
						   (struct vpu_mem_desc *)arg,
						   sizeof(struct vpu_mem_desc)))
					return -EFAULT;

				if (vpu_alloc_dma_buffer(&bitwork_mem) == -1)
					ret = -EFAULT;
				else if (copy_to_user((void __user *)arg,
						      &bitwork_mem,
						      sizeof(struct
							     vpu_mem_desc)))
					ret = -EFAULT;
			}
			break;
		}
	/*
	 * The following two ioctl is used when user allocates working buffer
	 * and register it to vpu driver.
	 */
	case VPU_IOC_QUERY_BITWORK_MEM:
		{
			if (copy_to_user((void __user *)arg,
					 &bitwork_mem,
					 sizeof(struct vpu_mem_desc)))
				ret = -EFAULT;
			break;
		}
	case VPU_IOC_SET_BITWORK_MEM:
		{
			if (copy_from_user(&bitwork_mem,
					   (struct vpu_mem_desc *)arg,
					   sizeof(struct vpu_mem_desc)))
				ret = -EFAULT;
			break;
		}
	case VPU_IOC_SYS_SW_RESET:
		{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 5, 0)
			imx_src_reset_vpu();
#else
			if (vpu_plat->reset)
				vpu_plat->reset();
#endif

			break;
		}
	case VPU_IOC_REG_DUMP:
		break;
	case VPU_IOC_PHYMEM_DUMP:
		break;
	case VPU_IOC_PHYMEM_CHECK:
	{
		struct vpu_mem_desc check_memory;
		ret = copy_from_user(&check_memory,
				     (void __user *)arg,
				     sizeof(struct vpu_mem_desc));
		if (ret != 0) {
			printk(KERN_ERR "copy from user failure:%d\n", ret);
			ret = -EFAULT;
			break;
		}
		ret = vpu_is_valid_phy_memory((u32)check_memory.phy_addr);

		pr_debug("vpu: memory phy:0x%x %s phy memory\n",
		       check_memory.phy_addr, (ret ? "is" : "isn't"));
		/* borrow .size to pass back the result. */
		check_memory.size = ret;
		ret = copy_to_user((void __user *)arg, &check_memory,
				   sizeof(struct vpu_mem_desc));
		if (ret) {
			ret = -EFAULT;
			break;
		}
		break;
	}
	case VPU_IOC_LOCK_DEV:
		{
			u32 lock_en;

			if (get_user(lock_en, (u32 __user *) arg))
				return -EFAULT;

			if (lock_en)
				mutex_lock(&vpu_data.lock);
			else
				mutex_unlock(&vpu_data.lock);

			break;
		}
	default:
		{
			printk(KERN_ERR "No such IOCTL, cmd is %d\n", cmd);
			ret = -EINVAL;
			break;
		}
	}
	return ret;
}

/*!
 * @brief Release function for vpu file operation
 * @return  0 on success or negative error code on error
 */
static int vpu_release(struct inode *inode, struct file *filp)
{
	int i;
	unsigned long timeout;

	mutex_lock(&vpu_data.lock);

	if (open_count > 0 && !(--open_count)) {

		/* Wait for vpu go to idle state */
		clk_prepare(vpu_clk);
		clk_enable(vpu_clk);
		if (READ_REG(BIT_CUR_PC)) {

			timeout = jiffies + HZ;
			while (READ_REG(BIT_BUSY_FLAG)) {
				msleep(1);
				if (time_after(jiffies, timeout)) {
					printk(KERN_WARNING "VPU timeout during release\n");
					break;
				}
			}
			clk_disable(vpu_clk);
			clk_unprepare(vpu_clk);

			/* Clean up interrupt */
			cancel_work_sync(&vpu_data.work);
			flush_workqueue(vpu_data.workqueue);
			irq_status = 0;

			clk_prepare(vpu_clk);
			clk_enable(vpu_clk);
			if (READ_REG(BIT_BUSY_FLAG)) {

				if (cpu_is_mx51() || cpu_is_mx53()) {
					printk(KERN_ERR
						"fatal error: can't gate/power off when VPU is busy\n");
					clk_disable(vpu_clk);
					clk_unprepare(vpu_clk);
					mutex_unlock(&vpu_data.lock);
					return -EFAULT;
				}

#ifdef CONFIG_SOC_IMX6Q
				if (cpu_is_mx6dl() || cpu_is_mx6q()) {
					WRITE_REG(0x11, 0x10F0);
					timeout = jiffies + HZ;
					while (READ_REG(0x10F4) != 0x77) {
						msleep(1);
						if (time_after(jiffies, timeout))
							break;
					}

					if (READ_REG(0x10F4) != 0x77) {
						printk(KERN_ERR
							"fatal error: can't gate/power off when VPU is busy\n");
						WRITE_REG(0x0, 0x10F0);
						clk_disable(vpu_clk);
						clk_unprepare(vpu_clk);
						mutex_unlock(&vpu_data.lock);
						return -EFAULT;
					} else {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 5, 0)
						imx_src_reset_vpu();
#else
						if (vpu_plat->reset)
							vpu_plat->reset();
#endif
					}
				}
#endif
			}
		}
		clk_disable(vpu_clk);
		clk_unprepare(vpu_clk);

		vpu_free_buffers();

		/* Free shared memory when vpu device is idle */
		vpu_free_dma_buffer(&share_mem);
		share_mem.cpu_addr = 0;
		vfree((void *)vshare_mem.cpu_addr);
		vshare_mem.cpu_addr = 0;

		vpu_clk_usercount = atomic_read(&clk_cnt_from_ioc);
		for (i = 0; i < vpu_clk_usercount; i++) {
			clk_disable(vpu_clk);
			clk_unprepare(vpu_clk);
			atomic_dec(&clk_cnt_from_ioc);
		}

#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 5, 0)
		if (!IS_ERR(vpu_regulator))
			regulator_disable(vpu_regulator);
#else
		imx_gpc_power_up_pu(false);
		pm_runtime_put_sync_suspend(&vpu_pdev->dev);
#endif

	}
	mutex_unlock(&vpu_data.lock);

	return 0;
}

/*!
 * @brief fasync function for vpu file operation
 * @return  0 on success or negative error code on error
 */
static int vpu_fasync(int fd, struct file *filp, int mode)
{
	struct vpu_priv *dev = (struct vpu_priv *)filp->private_data;
	return fasync_helper(fd, filp, mode, &dev->async_queue);
}

/*!
 * @brief memory map function of harware registers for vpu file operation
 * @return  0 on success or negative error code on error
 */
static int vpu_map_hwregs(struct file *fp, struct vm_area_struct *vm)
{
	unsigned long pfn;

	vm->vm_flags |= VM_IO;
	/*
	 * Since vpu registers have been mapped with ioremap() at probe
	 * which L_PTE_XN is 1, and the same physical address must be
	 * mapped multiple times with same type, so set L_PTE_XN to 1 here.
	 * Otherwise, there may be unexpected result in video codec.
	 */
	vm->vm_page_prot = pgprot_noncachedxn(vm->vm_page_prot);
	pfn = phy_vpu_base_addr >> PAGE_SHIFT;
	pr_debug("size=0x%x,  page no.=0x%x\n",
		 (int)(vm->vm_end - vm->vm_start), (int)pfn);
	return remap_pfn_range(vm, vm->vm_start, pfn, vm->vm_end - vm->vm_start,
			       vm->vm_page_prot) ? -EAGAIN : 0;
}

/*!
 * @brief memory map function of memory for vpu file operation
 * @return  0 on success or negative error code on error
 */
static int vpu_map_dma_mem(struct file *fp, struct vm_area_struct *vm)
{
	int request_size;
	request_size = vm->vm_end - vm->vm_start;

	pr_debug(" start=0x%x, pgoff=0x%x, size=0x%x\n",
		 (unsigned int)(vm->vm_start), (unsigned int)(vm->vm_pgoff),
		 request_size);

	vm->vm_flags |= VM_IO;
	vm->vm_page_prot = pgprot_writecombine(vm->vm_page_prot);

	return remap_pfn_range(vm, vm->vm_start, vm->vm_pgoff,
			       request_size, vm->vm_page_prot) ? -EAGAIN : 0;

}

/* !
 * @brief memory map function of vmalloced share memory
 * @return  0 on success or negative error code on error
 */
static int vpu_map_vshare_mem(struct file *fp, struct vm_area_struct *vm)
{
	int ret = -EINVAL;

	ret = remap_vmalloc_range(vm, (void *)(vm->vm_pgoff << PAGE_SHIFT), 0);
	vm->vm_flags |= VM_IO;

	return ret;
}
/*!
 * @brief memory map interface for vpu file operation
 * @return  0 on success or negative error code on error
 */
static int vpu_mmap(struct file *fp, struct vm_area_struct *vm)
{
	unsigned long offset;

	offset = vshare_mem.cpu_addr >> PAGE_SHIFT;

	if (vm->vm_pgoff && (vm->vm_pgoff == offset))
		return vpu_map_vshare_mem(fp, vm);
	else if (vm->vm_pgoff)
		return vpu_map_dma_mem(fp, vm);
	else
		return vpu_map_hwregs(fp, vm);
}

const struct file_operations vpu_fops = {
	.owner = THIS_MODULE,
	.open = vpu_open,
	.unlocked_ioctl = vpu_ioctl,
	.release = vpu_release,
	.fasync = vpu_fasync,
	.mmap = vpu_mmap,
};

/*!
 * This function is called by the driver framework to initialize the vpu device.
 * @param   dev The device structure for the vpu passed in by the framework.
 * @return   0 on success or negative error code on error
 */
static int vpu_dev_probe(struct platform_device *pdev)
{
	int err = 0;
	struct device *temp_class;
	struct resource *res;
	unsigned long addr = 0;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 5, 0)
	struct device_node *np = pdev->dev.of_node;
	u32 iramsize;

	err = of_property_read_u32(np, "iramsize", (u32 *)&iramsize);
	if (!err && iramsize)
		iram_alloc(iramsize, &addr);
	if (addr == 0)
		iram.start = iram.end = 0;
	else {
		iram.start = addr;
		iram.end = addr + iramsize - 1;
	}

	vpu_pdev = pdev;
#else

	vpu_plat = pdev->dev.platform_data;

	if (vpu_plat && vpu_plat->iram_enable && vpu_plat->iram_size)
		iram_alloc(vpu_plat->iram_size, &addr);
	if (addr == 0)
		iram.start = iram.end = 0;
	else {
		iram.start = addr;
		iram.end = addr +  vpu_plat->iram_size - 1;
	}
#endif

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "vpu_regs");
	if (!res) {
		printk(KERN_ERR "vpu: unable to get vpu base addr\n");
		return -ENODEV;
	}
	phy_vpu_base_addr = res->start;
	vpu_base = ioremap(res->start, res->end - res->start);

	vpu_major = register_chrdev(vpu_major, "mxc_vpu", &vpu_fops);
	if (vpu_major < 0) {
		printk(KERN_ERR "vpu: unable to get a major for VPU\n");
		err = -EBUSY;
		goto error;
	}

	vpu_class = class_create(THIS_MODULE, "mxc_vpu");
	if (IS_ERR(vpu_class)) {
		err = PTR_ERR(vpu_class);
		goto err_out_chrdev;
	}

	temp_class = device_create(vpu_class, NULL, MKDEV(vpu_major, 0),
				   NULL, "mxc_vpu");
	if (IS_ERR(temp_class)) {
		err = PTR_ERR(temp_class);
		goto err_out_class;
	}

	vpu_clk = clk_get(&pdev->dev, "vpu_clk");
	if (IS_ERR(vpu_clk)) {
		err = -ENOENT;
		goto err_out_class;
	}

	vpu_ipi_irq = platform_get_irq_byname(pdev, "vpu_ipi_irq");
	if (vpu_ipi_irq < 0) {
		printk(KERN_ERR "vpu: unable to get vpu interrupt\n");
		err = -ENXIO;
		goto err_out_class;
	}
	err = request_irq(vpu_ipi_irq, vpu_ipi_irq_handler, 0, "VPU_CODEC_IRQ",
			  (void *)(&vpu_data));
	if (err)
		goto err_out_class;
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 5, 0)
	vpu_regulator = regulator_get(NULL, "cpu_vddvpu");
	if (IS_ERR(vpu_regulator)) {
		if (!(cpu_is_mx51() || cpu_is_mx53())) {
			printk(KERN_ERR
				"%s: failed to get vpu regulator\n", __func__);
			goto err_out_class;
		} else {
			/* regulator_get will return error on MX5x,
			 * just igore it everywhere*/
			printk(KERN_WARNING
				"%s: failed to get vpu regulator\n", __func__);
		}
	}
#endif

#ifdef MXC_VPU_HAS_JPU
	vpu_jpu_irq = platform_get_irq_byname(pdev, "vpu_jpu_irq");
	if (vpu_jpu_irq < 0) {
		printk(KERN_ERR "vpu: unable to get vpu jpu interrupt\n");
		err = -ENXIO;
		free_irq(vpu_ipi_irq, &vpu_data);
		goto err_out_class;
	}
	err = request_irq(vpu_jpu_irq, vpu_jpu_irq_handler, IRQF_TRIGGER_RISING,
			  "VPU_JPG_IRQ", (void *)(&vpu_data));
	if (err) {
		free_irq(vpu_ipi_irq, &vpu_data);
		goto err_out_class;
	}
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 5, 0)
	pm_runtime_enable(&pdev->dev);
#endif

	vpu_data.workqueue = create_workqueue("vpu_wq");
	INIT_WORK(&vpu_data.work, vpu_worker_callback);
	mutex_init(&vpu_data.lock);
	printk(KERN_INFO "VPU initialized\n");
	goto out;

err_out_class:
	device_destroy(vpu_class, MKDEV(vpu_major, 0));
	class_destroy(vpu_class);
err_out_chrdev:
	unregister_chrdev(vpu_major, "mxc_vpu");
error:
	iounmap(vpu_base);
out:
	return err;
}

static int vpu_dev_remove(struct platform_device *pdev)
{
	free_irq(vpu_ipi_irq, &vpu_data);
#ifdef MXC_VPU_HAS_JPU
	free_irq(vpu_jpu_irq, &vpu_data);
#endif
	cancel_work_sync(&vpu_data.work);
	flush_workqueue(vpu_data.workqueue);
	destroy_workqueue(vpu_data.workqueue);

	iounmap(vpu_base);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 5, 0)
	if (iram.start)
		iram_free(iram.start, iram.end-iram.start+1);
#else
	if (vpu_plat && vpu_plat->iram_enable && vpu_plat->iram_size)
		iram_free(iram.start,  vpu_plat->iram_size);
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 5, 0)
	if (!IS_ERR(vpu_regulator))
		regulator_put(vpu_regulator);
#endif
	return 0;
}

#ifdef CONFIG_PM
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 5, 0)
static int vpu_suspend(struct device *dev)
#else
static int vpu_suspend(struct platform_device *pdev, pm_message_t state)
#endif
{
	int i;
	unsigned long timeout;

	mutex_lock(&vpu_data.lock);
	if (open_count == 0) {
		/* VPU is released (all instances are freed),
		 * clock is already off, context is no longer needed,
		 * power is already off on MX6,
		 * gate power on MX51 */
		if (cpu_is_mx51()) {
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 5, 0)
			if (vpu_plat->pg)
				vpu_plat->pg(1);
#endif
		}
	} else {
		/* Wait for vpu go to idle state, suspect vpu cannot be changed
		   to idle state after about 1 sec */
		timeout = jiffies + HZ;
		clk_prepare(vpu_clk);
		clk_enable(vpu_clk);
		while (READ_REG(BIT_BUSY_FLAG)) {
			msleep(1);
			if (time_after(jiffies, timeout)) {
				clk_disable(vpu_clk);
				clk_unprepare(vpu_clk);
				mutex_unlock(&vpu_data.lock);
				return -EAGAIN;
			}
		}
		clk_disable(vpu_clk);
		clk_unprepare(vpu_clk);

		/* Make sure clock is disabled before suspend */
		vpu_clk_usercount = atomic_read(&clk_cnt_from_ioc);
		for (i = 0; i < vpu_clk_usercount; i++) {
			clk_disable(vpu_clk);
			clk_unprepare(vpu_clk);
		}

		if (cpu_is_mx53()) {
			mutex_unlock(&vpu_data.lock);
			return 0;
		}

		if (bitwork_mem.cpu_addr != 0) {
			clk_prepare(vpu_clk);
			clk_enable(vpu_clk);
			/* Save 64 registers from BIT_CODE_BUF_ADDR */
			for (i = 0; i < 64; i++)
				regBk[i] = READ_REG(BIT_CODE_BUF_ADDR + (i * 4));
			pc_before_suspend = READ_REG(BIT_CUR_PC);
			clk_disable(vpu_clk);
			clk_unprepare(vpu_clk);
		}

#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 5, 0)
		if (vpu_plat->pg)
			vpu_plat->pg(1);
#endif

		/* If VPU is working before suspend, disable
		 * regulator to make usecount right. */
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 5, 0)
		if (!IS_ERR(vpu_regulator))
			regulator_disable(vpu_regulator);
#else
		imx_gpc_power_up_pu(false);
#endif
	}

	mutex_unlock(&vpu_data.lock);
	return 0;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 5, 0)
static int vpu_resume(struct device *dev)
#else
static int vpu_resume(struct platform_device *pdev)
#endif
{
	int i;

	mutex_lock(&vpu_data.lock);
	if (open_count == 0) {
		/* VPU is released (all instances are freed),
		 * clock should be kept off, context is no longer needed,
		 * power should be kept off on MX6,
		 * disable power gating on MX51 */
		if (cpu_is_mx51()) {
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 5, 0)
			if (vpu_plat->pg)
				vpu_plat->pg(0);
#endif
		}
	} else {
		if (cpu_is_mx53())
			goto recover_clk;

#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 5, 0)
		/* If VPU is working before suspend, enable
		 * regulator to make usecount right. */
		if (!IS_ERR(vpu_regulator))
			regulator_enable(vpu_regulator);

		if (vpu_plat->pg)
			vpu_plat->pg(0);
#else
		imx_gpc_power_up_pu(true);
#endif

		if (bitwork_mem.cpu_addr != 0) {
			u32 *p = (u32 *) bitwork_mem.cpu_addr;
			u32 data, pc;
			u16 data_hi;
			u16 data_lo;

			clk_prepare(vpu_clk);
			clk_enable(vpu_clk);

			pc = READ_REG(BIT_CUR_PC);
			if (pc) {
				printk(KERN_WARNING "Not power off after suspend (PC=0x%x)\n", pc);
				clk_disable(vpu_clk);
				clk_unprepare(vpu_clk);
				goto recover_clk;
			}

			/* Restore registers */
			for (i = 0; i < 64; i++)
				WRITE_REG(regBk[i], BIT_CODE_BUF_ADDR + (i * 4));

			WRITE_REG(0x0, BIT_RESET_CTRL);
			WRITE_REG(0x0, BIT_CODE_RUN);
			/* MX6 RTL has a bug not to init MBC_SET_SUBBLK_EN on reset */
#ifdef CONFIG_SOC_IMX6Q
			WRITE_REG(0x0, MBC_SET_SUBBLK_EN);
#endif

			/*
			 * Re-load boot code, from the codebuffer in external RAM.
			 * Thankfully, we only need 4096 bytes, same for all platforms.
			 */
			for (i = 0; i < 2048; i += 4) {
				data = p[(i / 2) + 1];
				data_hi = (data >> 16) & 0xFFFF;
				data_lo = data & 0xFFFF;
				WRITE_REG((i << 16) | data_hi, BIT_CODE_DOWN);
				WRITE_REG(((i + 1) << 16) | data_lo,
						BIT_CODE_DOWN);

				data = p[i / 2];
				data_hi = (data >> 16) & 0xFFFF;
				data_lo = data & 0xFFFF;
				WRITE_REG(((i + 2) << 16) | data_hi,
						BIT_CODE_DOWN);
				WRITE_REG(((i + 3) << 16) | data_lo,
						BIT_CODE_DOWN);
			}

			if (pc_before_suspend) {
				WRITE_REG(0x1, BIT_BUSY_FLAG);
				WRITE_REG(0x1, BIT_CODE_RUN);
				while (READ_REG(BIT_BUSY_FLAG))
					;
			} else {
				printk(KERN_WARNING "PC=0 before suspend\n");
			}
			clk_disable(vpu_clk);
			clk_unprepare(vpu_clk);
		}

recover_clk:
		/* Recover vpu clock */
		for (i = 0; i < vpu_clk_usercount; i++) {
			clk_prepare(vpu_clk);
			clk_enable(vpu_clk);
		}
	}

	mutex_unlock(&vpu_data.lock);
	return 0;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 5, 0)
static int vpu_runtime_suspend(struct device *dev)
{
	//release_bus_freq(BUS_FREQ_HIGH);
	return 0;
}

static int vpu_runtime_resume(struct device *dev)
{
	//request_bus_freq(BUS_FREQ_HIGH);
	return 0;
}

static const struct dev_pm_ops vpu_pm_ops = {
	SET_RUNTIME_PM_OPS(vpu_runtime_suspend, vpu_runtime_resume, NULL)
	SET_SYSTEM_SLEEP_PM_OPS(vpu_suspend, vpu_resume)
};
#endif

#else
#define	vpu_suspend	NULL
#define	vpu_resume	NULL
#endif				/* !CONFIG_PM */

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 5, 0)
static const struct of_device_id vpu_of_match[] = {
	{ .compatible = "fsl,imx6q-vpu", },
	{/* sentinel */}
};
MODULE_DEVICE_TABLE(of, vpu_of_match);
#endif

/*! Driver definition
 *
 */
static struct platform_driver mxcvpu_driver = {
	.driver = {
		   .name = "mxc_vpu",
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 5, 0)
		   .of_match_table = vpu_of_match,
#ifdef CONFIG_PM
		   .pm = &vpu_pm_ops,
#endif
#endif
		   },
	.probe = vpu_dev_probe,
	.remove = vpu_dev_remove,
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 5, 0)
	.suspend = vpu_suspend,
	.resume = vpu_resume,
#endif
};

static int __init vpu_init(void)
{
	int ret = platform_driver_register(&mxcvpu_driver);

	init_waitqueue_head(&vpu_queue);


#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 5, 0)
	memblock_analyze();
	top_address_DRAM = memblock_end_of_DRAM_with_reserved();
#endif

	return ret;
}

static void __exit vpu_exit(void)
{
	if (vpu_major > 0) {
		device_destroy(vpu_class, MKDEV(vpu_major, 0));
		class_destroy(vpu_class);
		unregister_chrdev(vpu_major, "mxc_vpu");
		vpu_major = 0;
	}

	vpu_free_dma_buffer(&bitwork_mem);
	vpu_free_dma_buffer(&pic_para_mem);
	vpu_free_dma_buffer(&user_data_mem);

	/* reset VPU state */
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 5, 0)
	if (!IS_ERR(vpu_regulator))
		regulator_enable(vpu_regulator);
	clk_prepare(vpu_clk);
	clk_enable(vpu_clk);
	if (vpu_plat->reset)
		vpu_plat->reset();
	clk_disable(vpu_clk);
	clk_unprepare(vpu_clk);
	if (!IS_ERR(vpu_regulator))
		regulator_disable(vpu_regulator);
#else
	imx_gpc_power_up_pu(true);
	clk_prepare(vpu_clk);
	clk_enable(vpu_clk);
	imx_src_reset_vpu();
	clk_disable(vpu_clk);
	clk_unprepare(vpu_clk);
	imx_gpc_power_up_pu(false);
#endif

	clk_put(vpu_clk);

	platform_driver_unregister(&mxcvpu_driver);
	return;
}

MODULE_AUTHOR("Freescale Semiconductor, Inc.");
MODULE_DESCRIPTION("Linux VPU driver for Freescale i.MX/MXC");
MODULE_LICENSE("GPL");

module_init(vpu_init);
module_exit(vpu_exit);
