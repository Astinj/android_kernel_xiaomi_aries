/* Copyright (c) 2013, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#define pr_fmt(fmt) "MSM-CPP %s:%d " fmt, __func__, __LINE__


#include <linux/delay.h>
#include <linux/clk.h>
#include <linux/firmware.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/module.h>
#include <linux/regulator/consumer.h>
#include <linux/ion.h>
#include <linux/proc_fs.h>
#include <linux/debugfs.h>
#include <linux/msm_ion.h>
#include <linux/iommu.h>
#include <mach/iommu_domains.h>
#include <mach/iommu.h>
#include <mach/vreg.h>
#include <media/msm_isp.h>
#include <media/v4l2-event.h>
#include <media/v4l2-ioctl.h>
#include <media/msmb_camera.h>
#include <media/msmb_pproc.h>
#include "msm_cpp.h"
#include "msm_camera_io_util.h"

#define MSM_CPP_DRV_NAME "msm_cpp"

#define CONFIG_MSM_CPP_DBG 0

#if CONFIG_MSM_CPP_DBG
#define CPP_DBG(fmt, args...) pr_err(fmt, ##args)
#else
#define CPP_DBG(fmt, args...) pr_debug(fmt, ##args)
#endif

#define ERR_USER_COPY(to) pr_err("copy %s user\n", \
			((to) ? "to" : "from"))
#define ERR_COPY_FROM_USER() ERR_USER_COPY(0)

#define msm_dequeue(queue, member) ({	   \
	unsigned long flags;		  \
	struct msm_device_queue *__q = (queue);	 \
	struct msm_queue_cmd *qcmd = 0;	   \
	spin_lock_irqsave(&__q->lock, flags);	 \
	if (!list_empty(&__q->list)) {		\
		__q->len--;		 \
		qcmd = list_first_entry(&__q->list,   \
		struct msm_queue_cmd, member);  \
		list_del_init(&qcmd->member);	 \
	}			 \
	spin_unlock_irqrestore(&__q->lock, flags);  \
	qcmd;			 \
})

static void msm_queue_init(struct msm_device_queue *queue, const char *name)
{
	CPP_DBG("E\n");
	spin_lock_init(&queue->lock);
	queue->len = 0;
	queue->max = 0;
	queue->name = name;
	INIT_LIST_HEAD(&queue->list);
	init_waitqueue_head(&queue->wait);
}

static void msm_enqueue(struct msm_device_queue *queue,
			struct list_head *entry)
{
	unsigned long flags;
	spin_lock_irqsave(&queue->lock, flags);
	queue->len++;
	if (queue->len > queue->max) {
		queue->max = queue->len;
		pr_info("queue %s new max is %d\n", queue->name, queue->max);
	}
	list_add_tail(entry, &queue->list);
	wake_up(&queue->wait);
	CPP_DBG("woke up %s\n", queue->name);
	spin_unlock_irqrestore(&queue->lock, flags);
}

static struct msm_cam_clk_info cpp_clk_info[] = {
	{"camss_top_ahb_clk", -1},
	{"vfe_clk_src", 266670000},
	{"camss_vfe_vfe_clk", -1},
	{"iface_clk", -1},
	{"cpp_core_clk", 266670000},
	{"cpp_iface_clk", -1},
	{"cpp_bus_clk", -1},
	{"micro_iface_clk", -1},
};
static int msm_cpp_notify_frame_done(struct cpp_device *cpp_dev);

static void msm_cpp_write(u32 data, void __iomem *cpp_base)
{
	writel_relaxed((data), cpp_base + MSM_CPP_MICRO_FIFO_RX_DATA);
}

static uint32_t msm_cpp_read(void __iomem *cpp_base)
{
	uint32_t tmp, retry = 0;
	do {
		tmp = msm_camera_io_r(cpp_base + MSM_CPP_MICRO_FIFO_TX_STAT);
	} while (((tmp & 0x2) == 0x0) && (retry++ < 10)) ;
	if (retry < 10) {
		tmp = msm_camera_io_r(cpp_base + MSM_CPP_MICRO_FIFO_TX_DATA);
		CPP_DBG("Read data: 0%x\n", tmp);
	} else {
		CPP_DBG("Read failed\n");
		tmp = 0xDEADBEEF;
	}

	return tmp;
}

static void msm_cpp_poll(void __iomem *cpp_base, u32 val)
{
	uint32_t tmp, retry = 0;
	do {
		usleep_range(1000, 2000);
		tmp = msm_cpp_read(cpp_base);
		if (tmp != 0xDEADBEEF)
			CPP_DBG("poll: 0%x\n", tmp);
	} while ((tmp != val) && (retry++ < MSM_CPP_POLL_RETRIES));
	if (retry < MSM_CPP_POLL_RETRIES)
		CPP_DBG("Poll finished\n");
	else
		pr_err("Poll failed: expect: 0x%x\n", val);
}

void cpp_release_ion_client(struct kref *ref)
{
	struct cpp_device *cpp_dev = container_of(ref,
		struct cpp_device, refcount);
	pr_err("Calling ion_client_destroy\n");
	ion_client_destroy(cpp_dev->client);
}

static int cpp_init_mem(struct cpp_device *cpp_dev)
{
	int rc = 0;

	kref_init(&cpp_dev->refcount);
	kref_get(&cpp_dev->refcount);
	cpp_dev->client = msm_ion_client_create(-1, "cpp");

	CPP_DBG("E\n");
	if (!cpp_dev->domain) {
		pr_err("domain / iommu context not found\n");
		return  -ENODEV;
	}

	CPP_DBG("X\n");
	return rc;
}

static void cpp_deinit_mem(struct cpp_device *cpp_dev)
{
	CPP_DBG("E\n");
	kref_put(&cpp_dev->refcount, cpp_release_ion_client);
	CPP_DBG("X\n");
}

static irqreturn_t msm_cpp_irq(int irq_num, void *data)
{
	uint32_t tx_level;
	uint32_t irq_status;
	uint32_t msg_id, cmd_len;
	uint32_t i;
	uint32_t tx_fifo[16];
	struct cpp_device *cpp_dev = data;
	irq_status = msm_camera_io_r(cpp_dev->base + MSM_CPP_MICRO_IRQGEN_STAT);
	CPP_DBG("status: 0x%x\n", irq_status);
	if (irq_status & 0x8) {
		tx_level = msm_camera_io_r(cpp_dev->base +
			MSM_CPP_MICRO_FIFO_TX_STAT) >> 2;
		for (i = 0; i < tx_level; i++) {
			tx_fifo[i] = msm_camera_io_r(cpp_dev->base +
				MSM_CPP_MICRO_FIFO_TX_DATA);
		}

		for (i = 0; i < tx_level; i++) {
			if (tx_fifo[i] == MSM_CPP_MSG_ID_CMD) {
				cmd_len = tx_fifo[i+1];
				msg_id = tx_fifo[i+2];
				if (msg_id == MSM_CPP_MSG_ID_FRAME_ACK) {
					CPP_DBG("Frame done!!\n");
					msm_cpp_notify_frame_done(cpp_dev);
				}
				i += cmd_len + 2;
			}
		}
	}
	msm_camera_io_w(irq_status, cpp_dev->base + MSM_CPP_MICRO_IRQGEN_CLR);
	return IRQ_HANDLED;
}

static void msm_cpp_boot_hw(struct cpp_device *cpp_dev)
{
	disable_irq(cpp_dev->irq->start);

	msm_camera_io_w(0x1, cpp_dev->base + MSM_CPP_MICRO_CLKEN_CTL);
	msm_camera_io_w(0x1, cpp_dev->base +
				 MSM_CPP_MICRO_BOOT_START);
	msm_cpp_poll(cpp_dev->base, MSM_CPP_MSG_ID_CMD);

	/*Trigger MC to jump to start address*/
	msm_cpp_write(MSM_CPP_CMD_EXEC_JUMP, cpp_dev->base);
	msm_cpp_write(MSM_CPP_JUMP_ADDRESS, cpp_dev->base);

	msm_cpp_poll(cpp_dev->base, MSM_CPP_MSG_ID_CMD);
	msm_cpp_poll(cpp_dev->base, 0x1);
	msm_cpp_poll(cpp_dev->base, MSM_CPP_MSG_ID_JUMP_ACK);
	msm_cpp_poll(cpp_dev->base, MSM_CPP_MSG_ID_TRAILER);

	/*Get Bootloader Version*/
	msm_cpp_write(MSM_CPP_CMD_GET_BOOTLOADER_VER, cpp_dev->base);
	pr_info("MC Bootloader Version: 0x%x\n",
		   msm_cpp_read(cpp_dev->base));

	/*Get Firmware Version*/
	msm_cpp_write(MSM_CPP_CMD_GET_FW_VER, cpp_dev->base);
	msm_cpp_write(MSM_CPP_MSG_ID_CMD, cpp_dev->base);
	msm_cpp_write(0x1, cpp_dev->base);
	msm_cpp_write(MSM_CPP_CMD_GET_FW_VER, cpp_dev->base);
	msm_cpp_write(MSM_CPP_MSG_ID_TRAILER, cpp_dev->base);

	msm_cpp_poll(cpp_dev->base, MSM_CPP_MSG_ID_CMD);
	msm_cpp_poll(cpp_dev->base, 0x2);
	msm_cpp_poll(cpp_dev->base, MSM_CPP_MSG_ID_FW_VER);
	pr_info("CPP FW Version: 0x%x\n", msm_cpp_read(cpp_dev->base));
	msm_cpp_poll(cpp_dev->base, MSM_CPP_MSG_ID_TRAILER);
	enable_irq(cpp_dev->irq->start);
	msm_camera_io_w_mb(0x8, cpp_dev->base +
		MSM_CPP_MICRO_IRQGEN_MASK);
	msm_camera_io_w_mb(0xFFFF, cpp_dev->base +
		MSM_CPP_MICRO_IRQGEN_CLR);
}

static int cpp_init_hardware(struct cpp_device *cpp_dev)
{
	int rc = 0;

	if (cpp_dev->fs_cpp == NULL) {
		cpp_dev->fs_cpp =
			regulator_get(&cpp_dev->pdev->dev, "vdd");
		if (IS_ERR(cpp_dev->fs_cpp)) {
			pr_err("Regulator cpp vdd get failed %ld\n",
				PTR_ERR(cpp_dev->fs_cpp));
			cpp_dev->fs_cpp = NULL;
			goto fs_failed;
		} else if (regulator_enable(cpp_dev->fs_cpp)) {
			pr_err("Regulator cpp vdd enable failed\n");
			regulator_put(cpp_dev->fs_cpp);
			cpp_dev->fs_cpp = NULL;
			goto fs_failed;
		}
	}

	rc = msm_cam_clk_enable(&cpp_dev->pdev->dev, cpp_clk_info,
			cpp_dev->cpp_clk, ARRAY_SIZE(cpp_clk_info), 1);
	if (rc < 0) {
		pr_err("clk enable failed\n");
		goto clk_failed;
	}

	cpp_dev->base = ioremap(cpp_dev->mem->start,
		resource_size(cpp_dev->mem));
	if (!cpp_dev->base) {
		rc = -ENOMEM;
		pr_err("ioremap failed\n");
		goto remap_failed;
	}

	cpp_dev->vbif_base = ioremap(cpp_dev->vbif_mem->start,
		resource_size(cpp_dev->vbif_mem));
	if (!cpp_dev->vbif_base) {
		rc = -ENOMEM;
		pr_err("ioremap failed\n");
		goto vbif_remap_failed;
	}

	cpp_dev->cpp_hw_base = ioremap(cpp_dev->cpp_hw_mem->start,
		resource_size(cpp_dev->cpp_hw_mem));
	if (!cpp_dev->cpp_hw_base) {
		rc = -ENOMEM;
		pr_err("ioremap failed\n");
		goto cpp_hw_remap_failed;
	}

	if (cpp_dev->state != CPP_STATE_BOOT) {
		rc = request_irq(cpp_dev->irq->start, msm_cpp_irq,
			IRQF_TRIGGER_RISING, "cpp", cpp_dev);
		if (rc < 0) {
			pr_err("irq request fail\n");
			rc = -EBUSY;
			goto req_irq_fail;
		}
	}

	cpp_dev->hw_info.cpp_hw_version =
		msm_camera_io_r(cpp_dev->cpp_hw_base);
	pr_debug("CPP HW Version: 0x%x\n", cpp_dev->hw_info.cpp_hw_version);
	cpp_dev->hw_info.cpp_hw_caps =
		msm_camera_io_r(cpp_dev->cpp_hw_base + 0x4);
	pr_debug("CPP HW Caps: 0x%x\n", cpp_dev->hw_info.cpp_hw_caps);
	msm_camera_io_w(0x1, cpp_dev->vbif_base + 0x4);
	if (cpp_dev->is_firmware_loaded == 1)
		msm_cpp_boot_hw(cpp_dev);
	return rc;
req_irq_fail:
	iounmap(cpp_dev->cpp_hw_base);
cpp_hw_remap_failed:
	iounmap(cpp_dev->vbif_base);
vbif_remap_failed:
	iounmap(cpp_dev->base);
remap_failed:
	msm_cam_clk_enable(&cpp_dev->pdev->dev, cpp_clk_info,
		cpp_dev->cpp_clk, ARRAY_SIZE(cpp_clk_info), 0);
clk_failed:
	regulator_disable(cpp_dev->fs_cpp);
	regulator_put(cpp_dev->fs_cpp);
fs_failed:
	return rc;
}

static void cpp_release_hardware(struct cpp_device *cpp_dev)
{
	if (cpp_dev->state != CPP_STATE_BOOT)
		free_irq(cpp_dev->irq->start, cpp_dev);

	iounmap(cpp_dev->base);
	iounmap(cpp_dev->vbif_base);
	iounmap(cpp_dev->cpp_hw_base);
	msm_cam_clk_enable(&cpp_dev->pdev->dev, cpp_clk_info,
		cpp_dev->cpp_clk, ARRAY_SIZE(cpp_clk_info), 0);
	if (0) {
		regulator_disable(cpp_dev->fs_cpp);
		regulator_put(cpp_dev->fs_cpp);
		cpp_dev->fs_cpp = NULL;
	}
}

static void cpp_load_fw(struct cpp_device *cpp_dev, char *fw_name_bin)
{
	uint32_t i;
	uint32_t *ptr_bin = NULL;
	int32_t rc = -EFAULT;
	const struct firmware *fw = NULL;
	struct device *dev = &cpp_dev->pdev->dev;

	pr_debug("%s: FW file: %s\n", __func__, fw_name_bin);
	rc = request_firmware(&fw, fw_name_bin, dev);
	if (rc) {
		dev_err(dev, "Failed to locate blob %s from device %p, Error: %d\n",
				fw_name_bin, dev, rc);
	}

	CPP_DBG("HW Ver:0x%x\n",
		msm_camera_io_r(cpp_dev->base +
		MSM_CPP_MICRO_HW_VERSION));

	msm_camera_io_w(0x1, cpp_dev->base +
					   MSM_CPP_MICRO_BOOT_START);
	/*Enable MC clock*/
	msm_camera_io_w(0x1, cpp_dev->base +
					   MSM_CPP_MICRO_CLKEN_CTL);

	msm_cpp_poll(cpp_dev->base, MSM_CPP_MSG_ID_CMD);

	/*Start firmware loading*/
	msm_cpp_write(MSM_CPP_CMD_FW_LOAD, cpp_dev->base);
	msm_cpp_write(MSM_CPP_END_ADDRESS, cpp_dev->base);
	msm_cpp_write(MSM_CPP_START_ADDRESS, cpp_dev->base);
	if (NULL != fw)
		ptr_bin = (uint32_t *)fw->data;

	for (i = 0; i < fw->size/4; i++) {
		if (ptr_bin) {
			msm_cpp_write(*ptr_bin, cpp_dev->base);
			ptr_bin++;
		}
	}
	if (fw)
		release_firmware(fw);

	msm_cpp_poll(cpp_dev->base, MSM_CPP_MSG_ID_OK);
	msm_cpp_poll(cpp_dev->base, MSM_CPP_MSG_ID_CMD);

	/*Trigger MC to jump to start address*/
	msm_cpp_write(MSM_CPP_CMD_EXEC_JUMP, cpp_dev->base);
	msm_cpp_write(MSM_CPP_JUMP_ADDRESS, cpp_dev->base);

	msm_cpp_poll(cpp_dev->base, MSM_CPP_MSG_ID_CMD);
	msm_cpp_poll(cpp_dev->base, 0x1);
	msm_cpp_poll(cpp_dev->base, MSM_CPP_MSG_ID_JUMP_ACK);
	msm_cpp_poll(cpp_dev->base, MSM_CPP_MSG_ID_TRAILER);

	/*Get Bootloader Version*/
	msm_cpp_write(MSM_CPP_CMD_GET_BOOTLOADER_VER, cpp_dev->base);
	pr_info("MC Bootloader Version: 0x%x\n",
		   msm_cpp_read(cpp_dev->base));

	/*Get Firmware Version*/
	msm_cpp_write(MSM_CPP_CMD_GET_FW_VER, cpp_dev->base);
	msm_cpp_write(MSM_CPP_MSG_ID_CMD, cpp_dev->base);
	msm_cpp_write(0x1, cpp_dev->base);
	msm_cpp_write(MSM_CPP_CMD_GET_FW_VER, cpp_dev->base);
	msm_cpp_write(MSM_CPP_MSG_ID_TRAILER, cpp_dev->base);

	msm_cpp_poll(cpp_dev->base, MSM_CPP_MSG_ID_CMD);
	msm_cpp_poll(cpp_dev->base, 0x2);
	msm_cpp_poll(cpp_dev->base, MSM_CPP_MSG_ID_FW_VER);
	pr_info("CPP FW Version: 0x%x\n", msm_cpp_read(cpp_dev->base));
	msm_cpp_poll(cpp_dev->base, MSM_CPP_MSG_ID_TRAILER);

	/*Disable MC clock*/
	/*msm_camera_io_w(0x0, cpp_dev->base +
					   MSM_CPP_MICRO_CLKEN_CTL);*/
}

static int cpp_open_node(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	uint32_t i;
	struct cpp_device *cpp_dev = v4l2_get_subdevdata(sd);
	CPP_DBG("E\n");

	mutex_lock(&cpp_dev->mutex);
	if (cpp_dev->cpp_open_cnt == MAX_ACTIVE_CPP_INSTANCE) {
		pr_err("No free CPP instance\n");
		mutex_unlock(&cpp_dev->mutex);
		return -ENODEV;
	}

	for (i = 0; i < MAX_ACTIVE_CPP_INSTANCE; i++) {
		if (cpp_dev->cpp_subscribe_list[i].active == 0) {
			cpp_dev->cpp_subscribe_list[i].active = 1;
			cpp_dev->cpp_subscribe_list[i].vfh = &fh->vfh;
			break;
		}
	}
	if (i == MAX_ACTIVE_CPP_INSTANCE) {
		pr_err("No free instance\n");
		mutex_unlock(&cpp_dev->mutex);
		return -ENODEV;
	}

	CPP_DBG("open %d %p\n", i, &fh->vfh);
	cpp_dev->cpp_open_cnt++;
	if (cpp_dev->cpp_open_cnt == 1) {
		cpp_init_hardware(cpp_dev);
		cpp_init_mem(cpp_dev);
		cpp_dev->state = CPP_STATE_IDLE;
	}
	mutex_unlock(&cpp_dev->mutex);
	return 0;
}

static int cpp_close_node(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	uint32_t i;
	struct cpp_device *cpp_dev = v4l2_get_subdevdata(sd);
	mutex_lock(&cpp_dev->mutex);
	for (i = 0; i < MAX_ACTIVE_CPP_INSTANCE; i++) {
		if (cpp_dev->cpp_subscribe_list[i].vfh == &fh->vfh) {
			cpp_dev->cpp_subscribe_list[i].active = 0;
			cpp_dev->cpp_subscribe_list[i].vfh = NULL;
			break;
		}
	}
	if (i == MAX_ACTIVE_CPP_INSTANCE) {
		pr_err("Invalid close\n");
		mutex_unlock(&cpp_dev->mutex);
		return -ENODEV;
	}

	CPP_DBG("close %d %p\n", i, &fh->vfh);
	cpp_dev->cpp_open_cnt--;
	if (cpp_dev->cpp_open_cnt == 0) {
		msm_camera_io_w(0x0, cpp_dev->base + MSM_CPP_MICRO_CLKEN_CTL);
		cpp_deinit_mem(cpp_dev);
		cpp_release_hardware(cpp_dev);
		cpp_dev->state = CPP_STATE_OFF;
	}
	mutex_unlock(&cpp_dev->mutex);
	return 0;
}

static const struct v4l2_subdev_internal_ops msm_cpp_internal_ops = {
	.open = cpp_open_node,
	.close = cpp_close_node,
};

static int msm_cpp_notify_frame_done(struct cpp_device *cpp_dev)
{
	struct v4l2_event v4l2_evt;
	struct msm_queue_cmd *frame_qcmd;
	struct msm_queue_cmd *event_qcmd;
	struct msm_cpp_frame_info_t *processed_frame;
	struct msm_device_queue *queue = &cpp_dev->processing_q;

	if (queue->len > 0) {
		frame_qcmd = msm_dequeue(queue, list_frame);
		processed_frame = frame_qcmd->command;
		kfree(frame_qcmd);
		event_qcmd = kzalloc(sizeof(struct msm_queue_cmd), GFP_ATOMIC);
		if (!event_qcmd) {
			pr_err("Insufficient memory. return");
			return -ENOMEM;
		}
		atomic_set(&event_qcmd->on_heap, 1);
		event_qcmd->command = processed_frame;
		CPP_DBG("fid %d\n", processed_frame->frame_id);
		msm_enqueue(&cpp_dev->eventData_q, &event_qcmd->list_eventdata);

		v4l2_evt.id = processed_frame->inst_id;
		v4l2_evt.type = V4L2_EVENT_CPP_FRAME_DONE;
		v4l2_event_queue(cpp_dev->msm_sd.sd.devnode, &v4l2_evt);
	}
	return 0;
}

static int msm_cpp_send_frame_to_hardware(struct cpp_device *cpp_dev)
{
	uint32_t i;
	struct msm_queue_cmd *frame_qcmd;
	struct msm_cpp_frame_info_t *process_frame;
	struct msm_device_queue *queue;

	if (cpp_dev->processing_q.len < MAX_CPP_PROCESSING_FRAME) {
		while (cpp_dev->processing_q.len < MAX_CPP_PROCESSING_FRAME) {
			if (cpp_dev->realtime_q.len != 0) {
				queue = &cpp_dev->realtime_q;
			} else if (cpp_dev->offline_q.len != 0) {
				queue = &cpp_dev->offline_q;
			} else {
				pr_debug("All frames queued\n");
				break;
			}
			frame_qcmd = msm_dequeue(queue, list_frame);
			process_frame = frame_qcmd->command;
			msm_enqueue(&cpp_dev->processing_q,
						&frame_qcmd->list_frame);
			msm_cpp_write(0x6, cpp_dev->base);
			for (i = 0; i < process_frame->msg_len; i++)
				msm_cpp_write(process_frame->cpp_cmd_msg[i],
					cpp_dev->base);
		}
	}
	return 0;
}

static int msm_cpp_flush_frames(struct cpp_device *cpp_dev)
{
	struct v4l2_event v4l2_evt;
	struct msm_queue_cmd *frame_qcmd;
	struct msm_cpp_frame_info_t *process_frame;
	struct msm_device_queue *queue;
	struct msm_queue_cmd *event_qcmd;

	do {
		if (cpp_dev->realtime_q.len != 0) {
			queue = &cpp_dev->realtime_q;
		} else if (cpp_dev->offline_q.len != 0) {
			queue = &cpp_dev->offline_q;
		} else {
			pr_debug("All frames flushed\n");
			break;
		}
		frame_qcmd = msm_dequeue(queue, list_frame);
		process_frame = frame_qcmd->command;
		kfree(frame_qcmd);
		event_qcmd = kzalloc(sizeof(struct msm_queue_cmd), GFP_ATOMIC);
		if (!event_qcmd) {
			pr_err("Insufficient memory. return");
			return -ENOMEM;
		}
		atomic_set(&event_qcmd->on_heap, 1);
		event_qcmd->command = process_frame;
		CPP_DBG("fid %d\n", process_frame->frame_id);
		msm_enqueue(&cpp_dev->eventData_q, &event_qcmd->list_eventdata);

		v4l2_evt.id = process_frame->inst_id;
		v4l2_evt.type = V4L2_EVENT_CPP_FRAME_DONE;
		v4l2_event_queue(cpp_dev->msm_sd.sd.devnode, &v4l2_evt);
	} while ((cpp_dev->realtime_q.len != 0) ||
		(cpp_dev->offline_q.len != 0));
	return 0;
}

static int msm_cpp_cfg(struct cpp_device *cpp_dev,
	struct msm_camera_v4l2_ioctl_t *ioctl_ptr)
{
	int rc = 0;
	struct msm_queue_cmd *frame_qcmd = NULL;
	struct msm_cpp_frame_info_t *new_frame =
		kzalloc(sizeof(struct msm_cpp_frame_info_t), GFP_KERNEL);
	uint32_t *cpp_frame_msg;
	unsigned long len;
	unsigned long in_phyaddr, out_phyaddr;
	uint16_t num_stripes = 0;

	int i = 0;
	if (!new_frame) {
		pr_err("Insufficient memory. return\n");
		return -ENOMEM;
	}

	rc = (copy_from_user(new_frame, (void __user *)ioctl_ptr->ioctl_ptr,
		sizeof(struct msm_cpp_frame_info_t)) ? -EFAULT : 0);
	if (rc) {
		ERR_COPY_FROM_USER();
		rc = -EINVAL;
		goto ERROR1;
	}

	cpp_frame_msg = kzalloc(sizeof(uint32_t)*new_frame->msg_len,
		GFP_KERNEL);
	if (!cpp_frame_msg) {
		pr_err("Insufficient memory. return");
		rc = -ENOMEM;
		goto ERROR1;
	}

	rc = (copy_from_user(cpp_frame_msg,
		(void __user *)new_frame->cpp_cmd_msg,
		sizeof(uint32_t)*new_frame->msg_len) ? -EFAULT : 0);
	if (rc) {
		ERR_COPY_FROM_USER();
		rc = -EINVAL;
		goto ERROR2;
	}

	new_frame->cpp_cmd_msg = cpp_frame_msg;

	CPP_DBG("CPP in_fd: %d out_fd: %d\n", new_frame->src_fd,
		new_frame->dst_fd);

	new_frame->src_ion_handle = ion_import_dma_buf(cpp_dev->client,
		new_frame->src_fd);
	if (IS_ERR_OR_NULL(new_frame->src_ion_handle)) {
		pr_err("ION import failed\n");
		rc = PTR_ERR(new_frame->src_ion_handle);
		goto ERROR2;
	}

	rc = ion_map_iommu(cpp_dev->client, new_frame->src_ion_handle,
		cpp_dev->domain_num, 0, SZ_4K, 0,
		(unsigned long *)&in_phyaddr, &len, 0, 0);
	if (rc < 0) {
		pr_err("ION import failed\n");
		rc = PTR_ERR(new_frame->src_ion_handle);
		goto ERROR3;
	}

	CPP_DBG("in phy addr: 0x%x len: %ld\n", (uint32_t) in_phyaddr, len);
	new_frame->dest_ion_handle = ion_import_dma_buf(cpp_dev->client,
		new_frame->dst_fd);
	if (IS_ERR_OR_NULL(new_frame->dest_ion_handle)) {
		pr_err("ION import failed\n");
		rc = PTR_ERR(new_frame->dest_ion_handle);
		goto ERROR4;
	}

	rc = ion_map_iommu(cpp_dev->client, new_frame->dest_ion_handle,
		cpp_dev->domain_num, 0, SZ_4K, 0,
		(unsigned long *)&out_phyaddr, &len, 0, 0);
	if (rc < 0) {
		rc = PTR_ERR(new_frame->dest_ion_handle);
		goto ERROR5;
	}

	CPP_DBG("out phy addr: 0x%x len: %ld\n", (uint32_t)out_phyaddr, len);
	num_stripes = ((cpp_frame_msg[12] >> 20) & 0x3FF) +
		((cpp_frame_msg[12] >> 10) & 0x3FF) +
		(cpp_frame_msg[12] & 0x3FF);

	for (i = 0; i < num_stripes; i++) {
		cpp_frame_msg[133 + i * 27] += (uint32_t) in_phyaddr;
		cpp_frame_msg[139 + i * 27] += (uint32_t) out_phyaddr;
		cpp_frame_msg[140 + i * 27] += (uint32_t) out_phyaddr;
		cpp_frame_msg[141 + i * 27] += (uint32_t) out_phyaddr;
		cpp_frame_msg[142 + i * 27] += (uint32_t) out_phyaddr;
	}

	frame_qcmd = kzalloc(sizeof(struct msm_queue_cmd), GFP_KERNEL);
	if (!frame_qcmd) {
		pr_err("Insufficient memory. return\n");
		rc = -ENOMEM;
		goto ERROR6;
	}

	atomic_set(&frame_qcmd->on_heap, 1);
	frame_qcmd->command = new_frame;
	if (new_frame->frame_type == MSM_CPP_REALTIME_FRAME) {
		msm_enqueue(&cpp_dev->realtime_q,
					&frame_qcmd->list_frame);
	} else if (new_frame->frame_type == MSM_CPP_OFFLINE_FRAME) {
		msm_enqueue(&cpp_dev->offline_q,
					&frame_qcmd->list_frame);
	} else {
		pr_err("Invalid frame type\n");
		rc = -EINVAL;
		goto ERROR7;
	}
	msm_cpp_send_frame_to_hardware(cpp_dev);
	return rc;
ERROR7:
	kfree(frame_qcmd);
ERROR6:
	ion_unmap_iommu(cpp_dev->client, new_frame->dest_ion_handle,
		cpp_dev->domain_num, 0);
ERROR5:
	ion_free(cpp_dev->client, new_frame->dest_ion_handle);
	new_frame->dest_ion_handle = NULL;
ERROR4:
	ion_unmap_iommu(cpp_dev->client, new_frame->src_ion_handle,
		cpp_dev->domain_num, 0);
ERROR3:
	ion_free(cpp_dev->client, new_frame->src_ion_handle);
	new_frame->src_ion_handle = NULL;
ERROR2:
	kfree(cpp_frame_msg);
ERROR1:
	kfree(new_frame);
	return rc;

}

long msm_cpp_subdev_ioctl(struct v4l2_subdev *sd,
			unsigned int cmd, void *arg)
{
	struct cpp_device *cpp_dev = v4l2_get_subdevdata(sd);
	struct msm_camera_v4l2_ioctl_t *ioctl_ptr = arg;
	int rc = 0;
	char *fw_name_bin;

	mutex_lock(&cpp_dev->mutex);
	CPP_DBG("E cmd: %d\n", cmd);
	switch (cmd) {
	case VIDIOC_MSM_CPP_GET_HW_INFO: {
		if (copy_to_user((void __user *)ioctl_ptr->ioctl_ptr,
			&cpp_dev->hw_info,
			sizeof(struct cpp_hw_info))) {
			mutex_unlock(&cpp_dev->mutex);
			return -EINVAL;
		}
		break;
	}

	case VIDIOC_MSM_CPP_LOAD_FIRMWARE: {
		if (cpp_dev->is_firmware_loaded == 0) {
			fw_name_bin = kzalloc(ioctl_ptr->len, GFP_KERNEL);
			if (!fw_name_bin) {
				pr_err("%s:%d: malloc error\n", __func__,
					__LINE__);
				mutex_unlock(&cpp_dev->mutex);
				return -EINVAL;
			}

			rc = (copy_from_user(fw_name_bin,
				(void __user *)ioctl_ptr->ioctl_ptr,
				ioctl_ptr->len) ? -EFAULT : 0);
			if (rc) {
				ERR_COPY_FROM_USER();
				kfree(fw_name_bin);
				mutex_unlock(&cpp_dev->mutex);
				return -EINVAL;
			}

			disable_irq(cpp_dev->irq->start);
			cpp_load_fw(cpp_dev, fw_name_bin);
			kfree(fw_name_bin);
			enable_irq(cpp_dev->irq->start);
			cpp_dev->is_firmware_loaded = 1;
		}
		break;
	}
	case VIDIOC_MSM_CPP_CFG:
		rc = msm_cpp_cfg(cpp_dev, ioctl_ptr);
		break;
	case VIDIOC_MSM_CPP_FLUSH_QUEUE:
		rc = msm_cpp_flush_frames(cpp_dev);
		break;
	case VIDIOC_MSM_CPP_GET_EVENTPAYLOAD: {
		struct msm_device_queue *queue = &cpp_dev->eventData_q;
		struct msm_queue_cmd *event_qcmd;
		struct msm_cpp_frame_info_t *process_frame;
		event_qcmd = msm_dequeue(queue, list_eventdata);
		process_frame = event_qcmd->command;
		CPP_DBG("fid %d\n", process_frame->frame_id);
		if (copy_to_user((void __user *)ioctl_ptr->ioctl_ptr,
				process_frame,
				sizeof(struct msm_cpp_frame_info_t))) {
					mutex_unlock(&cpp_dev->mutex);
					return -EINVAL;
		}
		if (process_frame->dest_ion_handle) {
			ion_unmap_iommu(cpp_dev->client,
				process_frame->dest_ion_handle,
				cpp_dev->domain_num, 0);
			ion_free(cpp_dev->client,
				process_frame->dest_ion_handle);
			process_frame->dest_ion_handle = NULL;
		}

		if (process_frame->src_ion_handle) {
			ion_unmap_iommu(cpp_dev->client,
				process_frame->src_ion_handle,
				cpp_dev->domain_num, 0);
			ion_free(cpp_dev->client,
				process_frame->src_ion_handle);
			process_frame->src_ion_handle = NULL;
		}

		kfree(process_frame->cpp_cmd_msg);
		kfree(process_frame);
		kfree(event_qcmd);
		break;
	}
	}
	mutex_unlock(&cpp_dev->mutex);
	CPP_DBG("X\n");
	return 0;
}

int msm_cpp_subscribe_event(struct v4l2_subdev *sd, struct v4l2_fh *fh,
	struct v4l2_event_subscription *sub)
{
	CPP_DBG("Called\n");
	return v4l2_event_subscribe(fh, sub, MAX_CPP_V4l2_EVENTS);
}

int msm_cpp_unsubscribe_event(struct v4l2_subdev *sd, struct v4l2_fh *fh,
	struct v4l2_event_subscription *sub)
{
	CPP_DBG("Called\n");
	return v4l2_event_unsubscribe(fh, sub);
}

static struct v4l2_subdev_core_ops msm_cpp_subdev_core_ops = {
	.ioctl = msm_cpp_subdev_ioctl,
	.subscribe_event = msm_cpp_subscribe_event,
	.unsubscribe_event = msm_cpp_unsubscribe_event,
};

static const struct v4l2_subdev_ops msm_cpp_subdev_ops = {
	.core = &msm_cpp_subdev_core_ops,
};

static int msm_cpp_enable_debugfs(struct cpp_device *cpp_dev);

static struct v4l2_file_operations msm_cpp_v4l2_subdev_fops;

static long msm_cpp_subdev_do_ioctl(
	struct file *file, unsigned int cmd, void *arg)
{
	struct video_device *vdev = video_devdata(file);
	struct v4l2_subdev *sd = vdev_to_v4l2_subdev(vdev);
	struct v4l2_fh *vfh = file->private_data;

	switch (cmd) {
	case VIDIOC_DQEVENT:
		if (!(sd->flags & V4L2_SUBDEV_FL_HAS_EVENTS))
			return -ENOIOCTLCMD;

		return v4l2_event_dequeue(vfh, arg, file->f_flags & O_NONBLOCK);

	case VIDIOC_SUBSCRIBE_EVENT:
		return v4l2_subdev_call(sd, core, subscribe_event, vfh, arg);

	case VIDIOC_UNSUBSCRIBE_EVENT:
		return v4l2_subdev_call(sd, core, unsubscribe_event, vfh, arg);

	case VIDIOC_MSM_CPP_GET_INST_INFO: {
		uint32_t i;
		struct cpp_device *cpp_dev = v4l2_get_subdevdata(sd);
		struct msm_camera_v4l2_ioctl_t *ioctl_ptr = arg;
		struct msm_cpp_frame_info_t inst_info;
		for (i = 0; i < MAX_ACTIVE_CPP_INSTANCE; i++) {
			if (cpp_dev->cpp_subscribe_list[i].vfh == vfh) {
				inst_info.inst_id = i;
				break;
			}
		}
		if (copy_to_user(
				(void __user *)ioctl_ptr->ioctl_ptr, &inst_info,
				sizeof(struct msm_cpp_frame_info_t))) {
			return -EINVAL;
		}
	}
	break;
	default:
		return v4l2_subdev_call(sd, core, ioctl, cmd, arg);
	}

	return 0;
}

static long msm_cpp_subdev_fops_ioctl(struct file *file, unsigned int cmd,
	unsigned long arg)
{
	return video_usercopy(file, cmd, arg, msm_cpp_subdev_do_ioctl);
}

static int cpp_register_domain(void)
{
	struct msm_iova_partition cpp_fw_partition = {
		.start = SZ_128K,
		.size = SZ_2G - SZ_128K,
	};
	struct msm_iova_layout cpp_fw_layout = {
		.partitions = &cpp_fw_partition,
		.npartitions = 1,
		.client_name = "camera_cpp",
		.domain_flags = 0,
	};

	return msm_register_domain(&cpp_fw_layout);
}

static int __devinit cpp_probe(struct platform_device *pdev)
{
	struct cpp_device *cpp_dev;
	int rc = 0;

	cpp_dev = kzalloc(sizeof(struct cpp_device), GFP_KERNEL);
	if (!cpp_dev) {
		pr_err("no enough memory\n");
		return -ENOMEM;
	}

	cpp_dev->cpp_clk = kzalloc(sizeof(struct clk *) *
		ARRAY_SIZE(cpp_clk_info), GFP_KERNEL);
	if (!cpp_dev->cpp_clk) {
		pr_err("no enough memory\n");
		rc = -ENOMEM;
		goto ERROR1;
	}

	v4l2_subdev_init(&cpp_dev->msm_sd.sd, &msm_cpp_subdev_ops);
	cpp_dev->msm_sd.sd.internal_ops = &msm_cpp_internal_ops;
	snprintf(cpp_dev->msm_sd.sd.name, ARRAY_SIZE(cpp_dev->msm_sd.sd.name),
		 "cpp");
	cpp_dev->msm_sd.sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	cpp_dev->msm_sd.sd.flags |= V4L2_SUBDEV_FL_HAS_EVENTS;
	v4l2_set_subdevdata(&cpp_dev->msm_sd.sd, cpp_dev);
	platform_set_drvdata(pdev, &cpp_dev->msm_sd.sd);
	mutex_init(&cpp_dev->mutex);

	if (pdev->dev.of_node)
		of_property_read_u32((&pdev->dev)->of_node,
					"cell-index", &pdev->id);

	cpp_dev->pdev = pdev;

	cpp_dev->mem = platform_get_resource_byname(pdev,
					IORESOURCE_MEM, "cpp");
	if (!cpp_dev->mem) {
		pr_err("no mem resource?\n");
		rc = -ENODEV;
		goto ERROR2;
	}

	cpp_dev->vbif_mem = platform_get_resource_byname(pdev,
					IORESOURCE_MEM, "cpp_vbif");
	if (!cpp_dev->vbif_mem) {
		pr_err("no mem resource?\n");
		rc = -ENODEV;
		goto ERROR2;
	}

	cpp_dev->cpp_hw_mem = platform_get_resource_byname(pdev,
					IORESOURCE_MEM, "cpp_hw");
	if (!cpp_dev->cpp_hw_mem) {
		pr_err("no mem resource?\n");
		rc = -ENODEV;
		goto ERROR2;
	}

	cpp_dev->irq = platform_get_resource_byname(pdev,
					IORESOURCE_IRQ, "cpp");
	if (!cpp_dev->irq) {
		pr_err("%s: no irq resource?\n", __func__);
		rc = -ENODEV;
		goto ERROR2;
	}

	cpp_dev->io = request_mem_region(cpp_dev->mem->start,
		resource_size(cpp_dev->mem), pdev->name);
	if (!cpp_dev->io) {
		pr_err("%s: no valid mem region\n", __func__);
		rc = -EBUSY;
		goto ERROR2;
	}

	cpp_dev->domain_num = cpp_register_domain();
	if (cpp_dev->domain_num < 0) {
		pr_err("%s: could not register domain\n", __func__);
		rc = -ENODEV;
		goto ERROR3;
	}

	cpp_dev->domain =
		msm_get_iommu_domain(cpp_dev->domain_num);
	if (!cpp_dev->domain) {
		pr_err("%s: cannot find domain\n", __func__);
		rc = -ENODEV;
		goto ERROR3;
	}

	cpp_dev->iommu_ctx = msm_iommu_get_ctx("cpp");
	if (!cpp_dev->iommu_ctx) {
		pr_err("%s: cannot get iommu_ctx\n", __func__);
		rc = -ENODEV;
		goto ERROR3;
	}

	media_entity_init(&cpp_dev->msm_sd.sd.entity, 0, NULL, 0);
	cpp_dev->msm_sd.sd.entity.type = MEDIA_ENT_T_V4L2_SUBDEV;
	cpp_dev->msm_sd.sd.entity.group_id = MSM_CAMERA_SUBDEV_CPP;
	cpp_dev->msm_sd.sd.entity.name = pdev->name;
	msm_sd_register(&cpp_dev->msm_sd);
	msm_cpp_v4l2_subdev_fops.owner = v4l2_subdev_fops.owner;
	msm_cpp_v4l2_subdev_fops.open = v4l2_subdev_fops.open;
	msm_cpp_v4l2_subdev_fops.unlocked_ioctl = msm_cpp_subdev_fops_ioctl;
	msm_cpp_v4l2_subdev_fops.release = v4l2_subdev_fops.release;
	msm_cpp_v4l2_subdev_fops.poll = v4l2_subdev_fops.poll;

	cpp_dev->msm_sd.sd.devnode->fops = &msm_cpp_v4l2_subdev_fops;
	cpp_dev->msm_sd.sd.entity.revision = cpp_dev->msm_sd.sd.devnode->num;
	cpp_dev->state = CPP_STATE_BOOT;
	cpp_init_hardware(cpp_dev);
	iommu_attach_device(cpp_dev->domain, cpp_dev->iommu_ctx);

	msm_camera_io_w(0x0, cpp_dev->base +
					   MSM_CPP_MICRO_IRQGEN_MASK);
	msm_camera_io_w(0xFFFF, cpp_dev->base +
					   MSM_CPP_MICRO_IRQGEN_CLR);

	cpp_release_hardware(cpp_dev);
	cpp_dev->state = CPP_STATE_OFF;

	msm_cpp_enable_debugfs(cpp_dev);
	msm_queue_init(&cpp_dev->eventData_q, "eventdata");
	msm_queue_init(&cpp_dev->offline_q, "frame");
	msm_queue_init(&cpp_dev->realtime_q, "frame");
	msm_queue_init(&cpp_dev->processing_q, "frame");
	cpp_dev->cpp_open_cnt = 0;
	cpp_dev->is_firmware_loaded = 0;

	return rc;

ERROR3:
	release_mem_region(cpp_dev->mem->start, resource_size(cpp_dev->mem));
ERROR2:
	kfree(cpp_dev->cpp_clk);
ERROR1:
	kfree(cpp_dev);
	return rc;
}

static const struct of_device_id msm_cpp_dt_match[] = {
	{.compatible = "qcom,cpp"},
	{}
};

static int cpp_device_remove(struct platform_device *dev)
{
	struct v4l2_subdev *sd = platform_get_drvdata(dev);
	struct cpp_device  *cpp_dev;
	if (!sd) {
		pr_err("%s: Subdevice is NULL\n", __func__);
		return 0;
	}

	cpp_dev = (struct cpp_device *)v4l2_get_subdevdata(sd);
	if (!cpp_dev) {
		pr_err("%s: cpp device is NULL\n", __func__);
		return 0;
	}

	iommu_detach_device(cpp_dev->domain, cpp_dev->iommu_ctx);
	msm_sd_unregister(&cpp_dev->msm_sd);
	release_mem_region(cpp_dev->mem->start, resource_size(cpp_dev->mem));
	release_mem_region(cpp_dev->vbif_mem->start,
		resource_size(cpp_dev->vbif_mem));
	release_mem_region(cpp_dev->cpp_hw_mem->start,
		resource_size(cpp_dev->cpp_hw_mem));
	mutex_destroy(&cpp_dev->mutex);
	kfree(cpp_dev->cpp_clk);
	kfree(cpp_dev);
	return 0;
}

static struct platform_driver cpp_driver = {
	.probe = cpp_probe,
	.remove = cpp_device_remove,
	.driver = {
		.name = MSM_CPP_DRV_NAME,
		.owner = THIS_MODULE,
		.of_match_table = msm_cpp_dt_match,
	},
};

static int __init msm_cpp_init_module(void)
{
	return platform_driver_register(&cpp_driver);
}

static void __exit msm_cpp_exit_module(void)
{
	platform_driver_unregister(&cpp_driver);
}

static int msm_cpp_debugfs_stream_s(void *data, u64 val)
{
	struct cpp_device *cpp_dev = data;
	CPP_DBG("CPP processing frame E\n");
	while (1) {
		mutex_lock(&cpp_dev->mutex);
		msm_cpp_notify_frame_done(cpp_dev);
		msm_cpp_send_frame_to_hardware(cpp_dev);
		mutex_unlock(&cpp_dev->mutex);
		msleep(20);
	}
	CPP_DBG("CPP processing frame X\n");
	return 0;
}

static int msm_cpp_debugfs_load_fw(void *data, u64 val)
{
	const struct firmware *fw = NULL;
	struct cpp_device *cpp_dev = data;
	int rc = 0;
	CPP_DBG("%s\n", __func__);
	rc = request_firmware(&fw, "FIRMWARE.bin", &cpp_dev->pdev->dev);
	if (rc) {
		pr_err("request_fw failed\n");
	} else {
		CPP_DBG("request ok\n");
		release_firmware(fw);
	}
	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(cpp_debugfs_stream, NULL,
			msm_cpp_debugfs_stream_s, "%llu\n");
DEFINE_SIMPLE_ATTRIBUTE(cpp_debugfs_fw, NULL,
			msm_cpp_debugfs_load_fw, "%llu\n");

static int msm_cpp_enable_debugfs(struct cpp_device *cpp_dev)
{
	struct dentry *debugfs_base, *debugfs_test;
	debugfs_base = debugfs_create_dir("msm_camera", NULL);
	if (!debugfs_base)
		return -ENOMEM;

	debugfs_test = debugfs_create_file("test", S_IRUGO | S_IWUSR,
		debugfs_base, (void *)cpp_dev, &cpp_debugfs_stream);
	if (!debugfs_test) {
		debugfs_remove(debugfs_base);
		return -ENOMEM;
	}

	if (!debugfs_create_file("fw", S_IRUGO | S_IWUSR, debugfs_base,
			(void *)cpp_dev, &cpp_debugfs_fw)) {
		debugfs_remove(debugfs_test);
		debugfs_remove(debugfs_base);
		return -ENOMEM;
	}
	return 0;
}

module_init(msm_cpp_init_module);
module_exit(msm_cpp_exit_module);
MODULE_DESCRIPTION("MSM CPP driver");
MODULE_LICENSE("GPL v2");
