/*
 *  Copyright (C) 2015, Samsung Electronics Co. Ltd. All Rights Reserved.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 */

#include "ssp_sensorhub.h"

#include <linux/moduleparam.h>

static int wl_sensorhub = 1;
module_param(wl_sensorhub, int, 0644);

inline void ssp_sensorhub_log(const char *func_name,
				const char *data, int length)
{
}

static int ssp_sensorhub_send_cmd(struct ssp_sensorhub_data *hub_data,
					const char *buf, int count)
{
	int ret = 0;

	if (buf[2] < MSG2SSP_AP_STATUS_WAKEUP ||
		buf[2] >= MSG2SSP_AP_TEMPHUMIDITY_CAL_DONE) {
		sensorhub_err("MSG2SSP_INST_LIB_NOTI err(%d)", buf[2]);
		return -EINVAL;
	}

	ret = ssp_send_cmd(hub_data->ssp_data, buf[2], 0);

	if (buf[2] == MSG2SSP_AP_STATUS_WAKEUP ||
		buf[2] == MSG2SSP_AP_STATUS_SLEEP)
		hub_data->ssp_data->uLastAPState = buf[2];

	if (buf[2] == MSG2SSP_AP_STATUS_SUSPEND ||
		buf[2] == MSG2SSP_AP_STATUS_RESUME)
		hub_data->ssp_data->uLastResumeState = buf[2];

	return ret;
}

static int ssp_sensorhub_send_instruction(struct ssp_sensorhub_data *hub_data,
					const char *buf, int count)
{
	unsigned char instruction = buf[0];

	if (buf[0] == MSG2SSP_INST_LIBRARY_REMOVE)
		instruction = REMOVE_LIBRARY;
	else if (buf[0] == MSG2SSP_INST_LIBRARY_ADD)
		instruction = ADD_LIBRARY;

	return send_instruction(hub_data->ssp_data, instruction,
		(unsigned char)buf[1], (unsigned char *)(buf+2),
		(unsigned short)(count-2));
}

static ssize_t ssp_sensorhub_write(struct file *file, const char __user *buf,
				size_t count, loff_t *pos)
{
	struct ssp_sensorhub_data *hub_data
		= container_of(file->private_data,
			struct ssp_sensorhub_data, sensorhub_device);
	int ret = 0;
	char *buffer;

	if (unlikely(count <= 2)) {
		sensorhub_err("library data length err(%d)", (u32)count);
		return -EINVAL;
	}

	buffer = kcalloc(count, sizeof(char), GFP_KERNEL);
	if (unlikely(!buffer)) {
		sensorhub_err("allocate memory for kernel buffer err");
		return -ENOMEM;
	}

	ret = copy_from_user(buffer, buf, count);
	if (unlikely(ret)) {
		sensorhub_err("memcpy for kernel buffer err");
		ret = -EFAULT;
		goto exit;
	}

	ssp_sensorhub_log(__func__, buffer, count);

	if (unlikely(hub_data->ssp_data->bSspShutdown)) {
		sensorhub_err("stop sending library data(shutdown)");
		ret = -EBUSY;
		goto exit;
	}

	if (buffer[0] == MSG2SSP_INST_LIB_NOTI)
		ret = ssp_sensorhub_send_cmd(hub_data, buffer, count);
	else
		ret = ssp_sensorhub_send_instruction(hub_data, buffer, count);

	if (unlikely(ret <= 0)) {
		sensorhub_err("send library data err(%d)", ret);
		/* i2c transfer fail */
		if (ret == ERROR)
			ret = -EIO;
		/* i2c transfer done but no ack from MCU */
		else if (ret == FAIL)
			ret = -EAGAIN;

		goto exit;
	}

	ret = count;

exit:
	kfree(buffer);
	return ret;
}

static ssize_t ssp_sensorhub_read(struct file *file, char __user *buf,
				size_t count, loff_t *pos)
{
	struct ssp_sensorhub_data *hub_data
		= container_of(file->private_data,
			struct ssp_sensorhub_data, sensorhub_device);
	struct sensorhub_event *event;
	int retries = MAX_DATA_COPY_TRY;
	int length = 0;
	int ret = 0;

	spin_lock_bh(&hub_data->sensorhub_lock);
	if (unlikely(kfifo_is_empty(&hub_data->fifo))) {
		sensorhub_info("no library data");
		goto err;
	}

	/* first in first out */
	ret = kfifo_out_peek(&hub_data->fifo, &event, sizeof(void *));
	if (unlikely(!ret)) {
		sensorhub_err("kfifo out peek err(%d)", ret);
		ret = EIO;
		goto err;
	}

	length = event->library_length;

	while (retries--) {
		ret = copy_to_user(buf,
			event->library_data, event->library_length);
		if (likely(!ret))
			break;
	}
	if (unlikely(ret))
		sensorhub_err("read library data err(%d/%d/%d)", ret, length, event->library_event_number);
	else
		ssp_sensorhub_log(__func__, event->library_data, event->library_length);

	/* remove first event from the list */
	ret = kfifo_out(&hub_data->fifo, &event, sizeof(void *));
	if (unlikely(ret != sizeof(void *))) {
		sensorhub_err("kfifo out err(%d)", ret);
		ret = EIO;
		goto err;
	}

	complete(&hub_data->read_done);
	spin_unlock_bh(&hub_data->sensorhub_lock);

	return length;

err:
	spin_unlock_bh(&hub_data->sensorhub_lock);
	return ret ? -ret : 0;
}

static long ssp_sensorhub_ioctl(struct file *file, unsigned int cmd,
				unsigned long arg)
{
	int length = 0;

	switch (cmd) {
	case IOCTL_READ_BIG_CONTEXT_DATA:
		break;

	default:
		sensorhub_err("ioctl cmd err(%d)", cmd);
		return -EINVAL;
	}

	return length;
}

static struct file_operations const ssp_sensorhub_fops = {
	.owner = THIS_MODULE,
	.open = nonseekable_open,
	.write = ssp_sensorhub_write,
	.read = ssp_sensorhub_read,
	.unlocked_ioctl = ssp_sensorhub_ioctl,
};

void ssp_sensorhub_report_notice(struct ssp_data *ssp_data, char notice)
{
	struct sensor_value sensorsdata;
	int size = 0, index = 0;
	char reportData[4] = {0x02, 0x01, 0x00, 0x00};

	memset(&sensorsdata, 0, sizeof(struct sensor_value));

	if (notice == MSG2SSP_AP_STATUS_RESET) {
		reportData[2] = MSG2SSP_AP_STATUS_RESET;
		if (ssp_data->IsMcuCrashed == true) {
			reportData[3] = MCU_CRASHED;
			ssp_data->IsMcuCrashed = false;
		} else if (ssp_data->intendedMcuReset == true) {
			reportData[3] = MCU_INTENDED_RESET;
			ssp_data->intendedMcuReset = false;
		} else
			reportData[3] = KERNEL_RESET;
		size = 4;
	} else {
		reportData[2] = notice;
		size = 3;
	}
	memcpy(&sensorsdata.scontext_buf[index], &size, sizeof(int));
	index += sizeof(int);
	index += sizeof(short);//always start index is 0
	size--;
	memcpy(&sensorsdata.scontext_buf[index], &size, sizeof(short));
	index += sizeof(short);
	memcpy(&sensorsdata.scontext_buf[index], reportData, size + 1);

	report_scontext_data(ssp_data, &sensorsdata);

	if (notice == MSG2SSP_AP_STATUS_WAKEUP)
		sensorhub_info("wake up");
	else if (notice == MSG2SSP_AP_STATUS_SLEEP)
		sensorhub_info("sleep");
	else if (notice == MSG2SSP_AP_STATUS_RESET)
		sensorhub_info("reset");
	else
		sensorhub_err("invalid notice(0x%x)", notice);
}

inline int ssp_sensorhub_list(struct ssp_sensorhub_data *hub_data,
				char *dataframe, int length)
{
	return 0;
}

inline int ssp_sensorhub_handle_data(struct ssp_data *ssp_data, char *dataframe,
				int start, int end)
{
	return 0;
}

inline void ssp_read_big_library_task(struct work_struct *work)
{
}

inline void ssp_send_big_library_task(struct work_struct *work)
{
}

inline int ssp_sensorhub_pcm_dump(struct ssp_sensorhub_data *hub_data)
{
	return 0;
}

int ssp_sensorhub_initialize(struct ssp_data *ssp_data)
{
	struct ssp_sensorhub_data *hub_data;
	int ret;

	/* allocate memory for sensorhub data */
	hub_data = kzalloc(sizeof(*hub_data), GFP_KERNEL);
	hub_data->ssp_data = ssp_data;
	ssp_data->hub_data = hub_data;

	/* init wakelock, list, waitqueue, completion and spinlock */
	wake_lock_init(&hub_data->sensorhub_wake_lock, WAKE_LOCK_SUSPEND,
			"ssp_sensorhub_wake_lock");
	init_waitqueue_head(&hub_data->sensorhub_wq);
	init_completion(&hub_data->read_done);
	init_completion(&hub_data->big_read_done);
	init_completion(&hub_data->big_write_done);
	init_completion(&hub_data->mcu_init_done);
	spin_lock_init(&hub_data->sensorhub_lock);
	mutex_init(&hub_data->big_events_lock);

	/* allocate sensorhub input device */
	hub_data->sensorhub_input_dev = input_allocate_device();
	if (!hub_data->sensorhub_input_dev) {
		sensorhub_err("allocate sensorhub input device err");
		ret = -ENOMEM;
		goto err_input_allocate_device_sensorhub;
	}

	/* set sensorhub input device */
	input_set_drvdata(hub_data->sensorhub_input_dev, hub_data);
	hub_data->sensorhub_input_dev->name = "ssp_context";
	input_set_capability(hub_data->sensorhub_input_dev, EV_REL, DATA);
	input_set_capability(hub_data->sensorhub_input_dev, EV_REL, BIG_DATA);
	input_set_capability(hub_data->sensorhub_input_dev, EV_REL, NOTICE);

	/* register sensorhub input device */
	ret = input_register_device(hub_data->sensorhub_input_dev);
	if (ret < 0) {
		sensorhub_err("register sensorhub input device err(%d)", ret);
		input_free_device(hub_data->sensorhub_input_dev);
		goto err_input_register_device_sensorhub;
	}

	/* register sensorhub misc device */
	hub_data->sensorhub_device.minor = MISC_DYNAMIC_MINOR;
	hub_data->sensorhub_device.name = "ssp_sensorhub";
	hub_data->sensorhub_device.fops = &ssp_sensorhub_fops;

	ret = misc_register(&hub_data->sensorhub_device);
	if (ret < 0) {
		sensorhub_err("register sensorhub misc device err(%d)", ret);
		goto err_misc_register;
	}

	/* allocate fifo */
	ret = kfifo_alloc(&hub_data->fifo,
		sizeof(void *) * LIST_SIZE, GFP_KERNEL);
	if (ret) {
		sensorhub_err("kfifo allocate err(%d)", ret);
		goto err_kfifo_alloc;
	}

	return 0;

err_kfifo_alloc:
	misc_deregister(&hub_data->sensorhub_device);
err_misc_register:
	input_unregister_device(hub_data->sensorhub_input_dev);
err_input_register_device_sensorhub:
err_input_allocate_device_sensorhub:
	complete_all(&hub_data->big_write_done);
	complete_all(&hub_data->big_read_done);
	complete_all(&hub_data->read_done);
	wake_lock_destroy(&hub_data->sensorhub_wake_lock);
	kfree(hub_data);

	return ret;
}

void ssp_sensorhub_remove(struct ssp_data *ssp_data)
{
	struct ssp_sensorhub_data *hub_data = ssp_data->hub_data;

	kfifo_free(&hub_data->fifo);
	misc_deregister(&hub_data->sensorhub_device);
	input_unregister_device(hub_data->sensorhub_input_dev);
	complete_all(&hub_data->big_write_done);
	mutex_destroy(&hub_data->big_events_lock);
	complete_all(&hub_data->big_read_done);
	complete_all(&hub_data->read_done);
	wake_lock_destroy(&hub_data->sensorhub_wake_lock);
	kfree(hub_data);
}

MODULE_DESCRIPTION("Seamless Sensor Platform(SSP) sensorhub driver");
MODULE_AUTHOR("Samsung Electronics");
MODULE_LICENSE("GPL");
