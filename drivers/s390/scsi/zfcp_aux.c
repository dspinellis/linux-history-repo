/*
 * This file is part of the zfcp device driver for
 * FCP adapters for IBM System z9 and zSeries.
 *
 * (C) Copyright IBM Corp. 2002, 2006
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

/*
 * Driver authors:
 *            Martin Peschke (originator of the driver)
 *            Raimund Schroeder
 *            Aron Zeh
 *            Wolfgang Taphorn
 *            Stefan Bader
 *            Heiko Carstens (kernel 2.6 port of the driver)
 *            Andreas Herrmann
 *            Maxim Shchetynin
 *            Volker Sameske
 *            Ralph Wuerthner
 */

#include "zfcp_ext.h"

/* accumulated log level (module parameter) */
static u32 loglevel = ZFCP_LOG_LEVEL_DEFAULTS;
static char *device;
/*********************** FUNCTION PROTOTYPES *********************************/

/* written against the module interface */
static int __init  zfcp_module_init(void);

/* miscellaneous */
static int zfcp_sg_list_alloc(struct zfcp_sg_list *, size_t);
static void zfcp_sg_list_free(struct zfcp_sg_list *);
static int zfcp_sg_list_copy_from_user(struct zfcp_sg_list *,
				       void __user *, size_t);
static int zfcp_sg_list_copy_to_user(void __user *,
				     struct zfcp_sg_list *, size_t);
static long zfcp_cfdc_dev_ioctl(struct file *, unsigned int, unsigned long);

#define ZFCP_CFDC_IOC_MAGIC                     0xDD
#define ZFCP_CFDC_IOC \
	_IOWR(ZFCP_CFDC_IOC_MAGIC, 0, struct zfcp_cfdc_sense_data)


static const struct file_operations zfcp_cfdc_fops = {
	.unlocked_ioctl = zfcp_cfdc_dev_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = zfcp_cfdc_dev_ioctl
#endif
};

static struct miscdevice zfcp_cfdc_misc = {
	.minor = ZFCP_CFDC_DEV_MINOR,
	.name = ZFCP_CFDC_DEV_NAME,
	.fops = &zfcp_cfdc_fops
};

/*********************** KERNEL/MODULE PARAMETERS  ***************************/

/* declare driver module init/cleanup functions */
module_init(zfcp_module_init);

MODULE_AUTHOR("IBM Deutschland Entwicklung GmbH - linux390@de.ibm.com");
MODULE_DESCRIPTION
    ("FCP (SCSI over Fibre Channel) HBA driver for IBM System z9 and zSeries");
MODULE_LICENSE("GPL");

module_param(device, charp, 0400);
MODULE_PARM_DESC(device, "specify initial device");

module_param(loglevel, uint, 0400);
MODULE_PARM_DESC(loglevel,
		 "log levels, 8 nibbles: "
		 "FC ERP QDIO CIO Config FSF SCSI Other, "
		 "levels: 0=none 1=normal 2=devel 3=trace");

/****************************************************************/
/************** Functions without logging ***********************/
/****************************************************************/

void
_zfcp_hex_dump(char *addr, int count)
{
	int i;
	for (i = 0; i < count; i++) {
		printk("%02x", addr[i]);
		if ((i % 4) == 3)
			printk(" ");
		if ((i % 32) == 31)
			printk("\n");
	}
	if (((i-1) % 32) != 31)
		printk("\n");
}


/****************************************************************/
/****** Functions to handle the request ID hash table    ********/
/****************************************************************/

#define ZFCP_LOG_AREA			ZFCP_LOG_AREA_FSF

static int zfcp_reqlist_alloc(struct zfcp_adapter *adapter)
{
	int idx;

	adapter->req_list = kcalloc(REQUEST_LIST_SIZE, sizeof(struct list_head),
				    GFP_KERNEL);
	if (!adapter->req_list)
		return -ENOMEM;

	for (idx = 0; idx < REQUEST_LIST_SIZE; idx++)
		INIT_LIST_HEAD(&adapter->req_list[idx]);
	return 0;
}

static void zfcp_reqlist_free(struct zfcp_adapter *adapter)
{
	kfree(adapter->req_list);
}

int zfcp_reqlist_isempty(struct zfcp_adapter *adapter)
{
	unsigned int idx;

	for (idx = 0; idx < REQUEST_LIST_SIZE; idx++)
		if (!list_empty(&adapter->req_list[idx]))
			return 0;
	return 1;
}

#undef ZFCP_LOG_AREA

/****************************************************************/
/************** Uncategorised Functions *************************/
/****************************************************************/

#define ZFCP_LOG_AREA			ZFCP_LOG_AREA_OTHER

/**
 * zfcp_device_setup - setup function
 * @str: pointer to parameter string
 *
 * Parse "device=..." parameter string.
 */
static int __init
zfcp_device_setup(char *devstr)
{
	char *tmp, *str;
	size_t len;

	if (!devstr)
		return 0;

	len = strlen(devstr) + 1;
	str = kmalloc(len, GFP_KERNEL);
	if (!str)
		goto err_out;
	memcpy(str, devstr, len);

	tmp = strchr(str, ',');
	if (!tmp)
		goto err_out;
	*tmp++ = '\0';
	strncpy(zfcp_data.init_busid, str, BUS_ID_SIZE);
	zfcp_data.init_busid[BUS_ID_SIZE-1] = '\0';

	zfcp_data.init_wwpn = simple_strtoull(tmp, &tmp, 0);
	if (*tmp++ != ',')
		goto err_out;
	if (*tmp == '\0')
		goto err_out;

	zfcp_data.init_fcp_lun = simple_strtoull(tmp, &tmp, 0);
	if (*tmp != '\0')
		goto err_out;
	kfree(str);
	return 1;

 err_out:
	ZFCP_LOG_NORMAL("Parse error for device parameter string %s\n", str);
	kfree(str);
	return 0;
}

static void __init
zfcp_init_device_configure(void)
{
	struct zfcp_adapter *adapter;
	struct zfcp_port *port;
	struct zfcp_unit *unit;

	down(&zfcp_data.config_sema);
	read_lock_irq(&zfcp_data.config_lock);
	adapter = zfcp_get_adapter_by_busid(zfcp_data.init_busid);
	if (adapter)
		zfcp_adapter_get(adapter);
	read_unlock_irq(&zfcp_data.config_lock);

	if (adapter == NULL)
		goto out_adapter;
	port = zfcp_port_enqueue(adapter, zfcp_data.init_wwpn, 0, 0);
	if (!port)
		goto out_port;
	unit = zfcp_unit_enqueue(port, zfcp_data.init_fcp_lun);
	if (!unit)
		goto out_unit;
	up(&zfcp_data.config_sema);
	ccw_device_set_online(adapter->ccw_device);
	zfcp_erp_wait(adapter);
	down(&zfcp_data.config_sema);
	zfcp_unit_put(unit);
 out_unit:
	zfcp_port_put(port);
 out_port:
	zfcp_adapter_put(adapter);
 out_adapter:
	up(&zfcp_data.config_sema);
	return;
}

static int calc_alignment(int size)
{
	int align = 1;

	if (!size)
		return 0;

	while ((size - align) > 0)
		align <<= 1;

	return align;
}

static int __init
zfcp_module_init(void)
{
	int retval = -ENOMEM;
	int size, align;

	size = sizeof(struct zfcp_fsf_req_qtcb);
	align = calc_alignment(size);
	zfcp_data.fsf_req_qtcb_cache =
		kmem_cache_create("zfcp_fsf", size, align, 0, NULL);
	if (!zfcp_data.fsf_req_qtcb_cache)
		goto out;

	size = sizeof(struct fsf_status_read_buffer);
	align = calc_alignment(size);
	zfcp_data.sr_buffer_cache =
		kmem_cache_create("zfcp_sr", size, align, 0, NULL);
	if (!zfcp_data.sr_buffer_cache)
		goto out_sr_cache;

	size = sizeof(struct zfcp_gid_pn_data);
	align = calc_alignment(size);
	zfcp_data.gid_pn_cache =
		kmem_cache_create("zfcp_gid", size, align, 0, NULL);
	if (!zfcp_data.gid_pn_cache)
		goto out_gid_cache;

	atomic_set(&zfcp_data.loglevel, loglevel);

	/* initialize adapter list */
	INIT_LIST_HEAD(&zfcp_data.adapter_list_head);

	/* initialize adapters to be removed list head */
	INIT_LIST_HEAD(&zfcp_data.adapter_remove_lh);

	zfcp_data.scsi_transport_template =
		fc_attach_transport(&zfcp_transport_functions);
	if (!zfcp_data.scsi_transport_template)
		goto out_transport;

	retval = misc_register(&zfcp_cfdc_misc);
	if (retval != 0) {
		ZFCP_LOG_INFO("registration of misc device "
			      "zfcp_cfdc failed\n");
		goto out_misc;
	}

	ZFCP_LOG_TRACE("major/minor for zfcp_cfdc: %d/%d\n",
		       ZFCP_CFDC_DEV_MAJOR, zfcp_cfdc_misc.minor);

	/* Initialise proc semaphores */
	sema_init(&zfcp_data.config_sema, 1);

	/* initialise configuration rw lock */
	rwlock_init(&zfcp_data.config_lock);

	/* setup dynamic I/O */
	retval = zfcp_ccw_register();
	if (retval) {
		ZFCP_LOG_NORMAL("registration with common I/O layer failed\n");
		goto out_ccw_register;
	}

	if (zfcp_device_setup(device))
		zfcp_init_device_configure();

	goto out;

 out_ccw_register:
	misc_deregister(&zfcp_cfdc_misc);
 out_misc:
	fc_release_transport(zfcp_data.scsi_transport_template);
 out_transport:
	kmem_cache_destroy(zfcp_data.gid_pn_cache);
 out_gid_cache:
	kmem_cache_destroy(zfcp_data.sr_buffer_cache);
 out_sr_cache:
	kmem_cache_destroy(zfcp_data.fsf_req_qtcb_cache);
 out:
	return retval;
}

/*
 * function:    zfcp_cfdc_dev_ioctl
 *
 * purpose:     Handle control file upload/download transaction via IOCTL
 *		interface
 *
 * returns:     0           - Operation completed successfuly
 *              -ENOTTY     - Unknown IOCTL command
 *              -EINVAL     - Invalid sense data record
 *              -ENXIO      - The FCP adapter is not available
 *              -EOPNOTSUPP - The FCP adapter does not have CFDC support
 *              -ENOMEM     - Insufficient memory
 *              -EFAULT     - User space memory I/O operation fault
 *              -EPERM      - Cannot create or queue FSF request or create SBALs
 *              -ERESTARTSYS- Received signal (is mapped to EAGAIN by VFS)
 */
static long
zfcp_cfdc_dev_ioctl(struct file *file, unsigned int command,
		    unsigned long buffer)
{
	struct zfcp_cfdc_sense_data *sense_data, __user *sense_data_user;
	struct zfcp_adapter *adapter = NULL;
	struct zfcp_fsf_req *fsf_req = NULL;
	struct zfcp_sg_list *sg_list = NULL;
	u32 fsf_command, option;
	char *bus_id = NULL;
	int retval = 0;

	sense_data = kmalloc(sizeof(struct zfcp_cfdc_sense_data), GFP_KERNEL);
	if (sense_data == NULL) {
		retval = -ENOMEM;
		goto out;
	}

	sg_list = kzalloc(sizeof(struct zfcp_sg_list), GFP_KERNEL);
	if (sg_list == NULL) {
		retval = -ENOMEM;
		goto out;
	}

	if (command != ZFCP_CFDC_IOC) {
		ZFCP_LOG_INFO("IOC request code 0x%x invalid\n", command);
		retval = -ENOTTY;
		goto out;
	}

	if ((sense_data_user = (void __user *) buffer) == NULL) {
		ZFCP_LOG_INFO("sense data record is required\n");
		retval = -EINVAL;
		goto out;
	}

	retval = copy_from_user(sense_data, sense_data_user,
				sizeof(struct zfcp_cfdc_sense_data));
	if (retval) {
		retval = -EFAULT;
		goto out;
	}

	if (sense_data->signature != ZFCP_CFDC_SIGNATURE) {
		ZFCP_LOG_INFO("invalid sense data request signature 0x%08x\n",
			      ZFCP_CFDC_SIGNATURE);
		retval = -EINVAL;
		goto out;
	}

	switch (sense_data->command) {

	case ZFCP_CFDC_CMND_DOWNLOAD_NORMAL:
		fsf_command = FSF_QTCB_DOWNLOAD_CONTROL_FILE;
		option = FSF_CFDC_OPTION_NORMAL_MODE;
		break;

	case ZFCP_CFDC_CMND_DOWNLOAD_FORCE:
		fsf_command = FSF_QTCB_DOWNLOAD_CONTROL_FILE;
		option = FSF_CFDC_OPTION_FORCE;
		break;

	case ZFCP_CFDC_CMND_FULL_ACCESS:
		fsf_command = FSF_QTCB_DOWNLOAD_CONTROL_FILE;
		option = FSF_CFDC_OPTION_FULL_ACCESS;
		break;

	case ZFCP_CFDC_CMND_RESTRICTED_ACCESS:
		fsf_command = FSF_QTCB_DOWNLOAD_CONTROL_FILE;
		option = FSF_CFDC_OPTION_RESTRICTED_ACCESS;
		break;

	case ZFCP_CFDC_CMND_UPLOAD:
		fsf_command = FSF_QTCB_UPLOAD_CONTROL_FILE;
		option = 0;
		break;

	default:
		ZFCP_LOG_INFO("invalid command code 0x%08x\n",
			      sense_data->command);
		retval = -EINVAL;
		goto out;
	}

	bus_id = kmalloc(BUS_ID_SIZE, GFP_KERNEL);
	if (bus_id == NULL) {
		retval = -ENOMEM;
		goto out;
	}
	snprintf(bus_id, BUS_ID_SIZE, "%d.%d.%04x",
		(sense_data->devno >> 24),
		(sense_data->devno >> 16) & 0xFF,
		(sense_data->devno & 0xFFFF));

	read_lock_irq(&zfcp_data.config_lock);
	adapter = zfcp_get_adapter_by_busid(bus_id);
	if (adapter)
		zfcp_adapter_get(adapter);
	read_unlock_irq(&zfcp_data.config_lock);

	kfree(bus_id);

	if (adapter == NULL) {
		ZFCP_LOG_INFO("invalid adapter\n");
		retval = -ENXIO;
		goto out;
	}

	if (sense_data->command & ZFCP_CFDC_WITH_CONTROL_FILE) {
		retval = zfcp_sg_list_alloc(sg_list,
					    ZFCP_CFDC_MAX_CONTROL_FILE_SIZE);
		if (retval) {
			retval = -ENOMEM;
			goto out;
		}
	}

	if ((sense_data->command & ZFCP_CFDC_DOWNLOAD) &&
	    (sense_data->command & ZFCP_CFDC_WITH_CONTROL_FILE)) {
		retval = zfcp_sg_list_copy_from_user(
			sg_list, &sense_data_user->control_file,
			ZFCP_CFDC_MAX_CONTROL_FILE_SIZE);
		if (retval) {
			retval = -EFAULT;
			goto out;
		}
	}

	retval = zfcp_fsf_control_file(adapter, &fsf_req, fsf_command,
				       option, sg_list);
	if (retval)
		goto out;

	if ((fsf_req->qtcb->prefix.prot_status != FSF_PROT_GOOD) &&
	    (fsf_req->qtcb->prefix.prot_status != FSF_PROT_FSF_STATUS_PRESENTED)) {
		retval = -ENXIO;
		goto out;
	}

	sense_data->fsf_status = fsf_req->qtcb->header.fsf_status;
	memcpy(&sense_data->fsf_status_qual,
	       &fsf_req->qtcb->header.fsf_status_qual,
	       sizeof(union fsf_status_qual));
	memcpy(&sense_data->payloads, &fsf_req->qtcb->bottom.support.els, 256);

	retval = copy_to_user(sense_data_user, sense_data,
		sizeof(struct zfcp_cfdc_sense_data));
	if (retval) {
		retval = -EFAULT;
		goto out;
	}

	if (sense_data->command & ZFCP_CFDC_UPLOAD) {
		retval = zfcp_sg_list_copy_to_user(
			&sense_data_user->control_file, sg_list,
			ZFCP_CFDC_MAX_CONTROL_FILE_SIZE);
		if (retval) {
			retval = -EFAULT;
			goto out;
		}
	}

 out:
	if (fsf_req != NULL)
		zfcp_fsf_req_free(fsf_req);

	if ((adapter != NULL) && (retval != -ENXIO))
		zfcp_adapter_put(adapter);

	if (sg_list != NULL) {
		zfcp_sg_list_free(sg_list);
		kfree(sg_list);
	}

	kfree(sense_data);

	return retval;
}


/**
 * zfcp_sg_list_alloc - create a scatter-gather list of the specified size
 * @sg_list: structure describing a scatter gather list
 * @size: size of scatter-gather list
 * Return: 0 on success, else -ENOMEM
 *
 * In sg_list->sg a pointer to the created scatter-gather list is returned,
 * or NULL if we run out of memory. sg_list->count specifies the number of
 * elements of the scatter-gather list. The maximum size of a single element
 * in the scatter-gather list is PAGE_SIZE.
 */
static int
zfcp_sg_list_alloc(struct zfcp_sg_list *sg_list, size_t size)
{
	struct scatterlist *sg;
	unsigned int i;
	int retval = 0;
	void *address;

	BUG_ON(sg_list == NULL);

	sg_list->count = size >> PAGE_SHIFT;
	if (size & ~PAGE_MASK)
		sg_list->count++;
	sg_list->sg = kcalloc(sg_list->count, sizeof(struct scatterlist),
			      GFP_KERNEL);
	if (sg_list->sg == NULL) {
		sg_list->count = 0;
		retval = -ENOMEM;
		goto out;
	}
	sg_init_table(sg_list->sg, sg_list->count);

	for (i = 0, sg = sg_list->sg; i < sg_list->count; i++, sg++) {
		address = (void *) get_zeroed_page(GFP_KERNEL);
		if (address == NULL) {
			sg_list->count = i;
			zfcp_sg_list_free(sg_list);
			retval = -ENOMEM;
			goto out;
		}
		zfcp_address_to_sg(address, sg, min(size, PAGE_SIZE));
		size -= sg->length;
	}

 out:
	return retval;
}


/**
 * zfcp_sg_list_free - free memory of a scatter-gather list
 * @sg_list: structure describing a scatter-gather list
 *
 * Memory for each element in the scatter-gather list is freed.
 * Finally sg_list->sg is freed itself and sg_list->count is reset.
 */
static void
zfcp_sg_list_free(struct zfcp_sg_list *sg_list)
{
	struct scatterlist *sg;
	unsigned int i;

	BUG_ON(sg_list == NULL);

	for (i = 0, sg = sg_list->sg; i < sg_list->count; i++, sg++)
		free_page((unsigned long) zfcp_sg_to_address(sg));

	sg_list->count = 0;
	kfree(sg_list->sg);
}

/**
 * zfcp_sg_size - determine size of a scatter-gather list
 * @sg: array of (struct scatterlist)
 * @sg_count: elements in array
 * Return: size of entire scatter-gather list
 */
static size_t zfcp_sg_size(struct scatterlist *sg, unsigned int sg_count)
{
	unsigned int i;
	struct scatterlist *p;
	size_t size;

	size = 0;
	for (i = 0, p = sg; i < sg_count; i++, p++) {
		BUG_ON(p == NULL);
		size += p->length;
	}

	return size;
}


/**
 * zfcp_sg_list_copy_from_user -copy data from user space to scatter-gather list
 * @sg_list: structure describing a scatter-gather list
 * @user_buffer: pointer to buffer in user space
 * @size: number of bytes to be copied
 * Return: 0 on success, -EFAULT if copy_from_user fails.
 */
static int
zfcp_sg_list_copy_from_user(struct zfcp_sg_list *sg_list,
			    void __user *user_buffer,
                            size_t size)
{
	struct scatterlist *sg;
	unsigned int length;
	void *zfcp_buffer;
	int retval = 0;

	BUG_ON(sg_list == NULL);

	if (zfcp_sg_size(sg_list->sg, sg_list->count) < size)
		return -EFAULT;

	for (sg = sg_list->sg; size > 0; sg++) {
		length = min((unsigned int)size, sg->length);
		zfcp_buffer = zfcp_sg_to_address(sg);
		if (copy_from_user(zfcp_buffer, user_buffer, length)) {
			retval = -EFAULT;
			goto out;
		}
		user_buffer += length;
		size -= length;
	}

 out:
	return retval;
}


/**
 * zfcp_sg_list_copy_to_user - copy data from scatter-gather list to user space
 * @user_buffer: pointer to buffer in user space
 * @sg_list: structure describing a scatter-gather list
 * @size: number of bytes to be copied
 * Return: 0 on success, -EFAULT if copy_to_user fails
 */
static int
zfcp_sg_list_copy_to_user(void __user  *user_buffer,
			  struct zfcp_sg_list *sg_list,
                          size_t size)
{
	struct scatterlist *sg;
	unsigned int length;
	void *zfcp_buffer;
	int retval = 0;

	BUG_ON(sg_list == NULL);

	if (zfcp_sg_size(sg_list->sg, sg_list->count) < size)
		return -EFAULT;

	for (sg = sg_list->sg; size > 0; sg++) {
		length = min((unsigned int) size, sg->length);
		zfcp_buffer = zfcp_sg_to_address(sg);
		if (copy_to_user(user_buffer, zfcp_buffer, length)) {
			retval = -EFAULT;
			goto out;
		}
		user_buffer += length;
		size -= length;
	}

 out:
	return retval;
}


#undef ZFCP_LOG_AREA

/****************************************************************/
/****** Functions for configuration/set-up of structures ********/
/****************************************************************/

#define ZFCP_LOG_AREA			ZFCP_LOG_AREA_CONFIG

/**
 * zfcp_get_unit_by_lun - find unit in unit list of port by FCP LUN
 * @port: pointer to port to search for unit
 * @fcp_lun: FCP LUN to search for
 * Traverse list of all units of a port and return pointer to a unit
 * with the given FCP LUN.
 */
struct zfcp_unit *
zfcp_get_unit_by_lun(struct zfcp_port *port, fcp_lun_t fcp_lun)
{
	struct zfcp_unit *unit;
	int found = 0;

	list_for_each_entry(unit, &port->unit_list_head, list) {
		if ((unit->fcp_lun == fcp_lun) &&
		    !atomic_test_mask(ZFCP_STATUS_COMMON_REMOVE, &unit->status))
		{
			found = 1;
			break;
		}
	}
	return found ? unit : NULL;
}

/**
 * zfcp_get_port_by_wwpn - find port in port list of adapter by wwpn
 * @adapter: pointer to adapter to search for port
 * @wwpn: wwpn to search for
 * Traverse list of all ports of an adapter and return pointer to a port
 * with the given wwpn.
 */
struct zfcp_port *
zfcp_get_port_by_wwpn(struct zfcp_adapter *adapter, wwn_t wwpn)
{
	struct zfcp_port *port;
	int found = 0;

	list_for_each_entry(port, &adapter->port_list_head, list) {
		if ((port->wwpn == wwpn) &&
		    !(atomic_read(&port->status) &
		      (ZFCP_STATUS_PORT_NO_WWPN | ZFCP_STATUS_COMMON_REMOVE))) {
			found = 1;
			break;
		}
	}
	return found ? port : NULL;
}

/**
 * zfcp_get_port_by_did - find port in port list of adapter by d_id
 * @adapter: pointer to adapter to search for port
 * @d_id: d_id to search for
 * Traverse list of all ports of an adapter and return pointer to a port
 * with the given d_id.
 */
struct zfcp_port *
zfcp_get_port_by_did(struct zfcp_adapter *adapter, u32 d_id)
{
	struct zfcp_port *port;
	int found = 0;

	list_for_each_entry(port, &adapter->port_list_head, list) {
		if ((port->d_id == d_id) &&
		    !atomic_test_mask(ZFCP_STATUS_COMMON_REMOVE, &port->status))
		{
			found = 1;
			break;
		}
	}
	return found ? port : NULL;
}

/**
 * zfcp_get_adapter_by_busid - find adpater in adapter list by bus_id
 * @bus_id: bus_id to search for
 * Traverse list of all adapters and return pointer to an adapter
 * with the given bus_id.
 */
struct zfcp_adapter *
zfcp_get_adapter_by_busid(char *bus_id)
{
	struct zfcp_adapter *adapter;
	int found = 0;

	list_for_each_entry(adapter, &zfcp_data.adapter_list_head, list) {
		if ((strncmp(bus_id, zfcp_get_busid_by_adapter(adapter),
			     BUS_ID_SIZE) == 0) &&
		    !atomic_test_mask(ZFCP_STATUS_COMMON_REMOVE,
				      &adapter->status)){
			found = 1;
			break;
		}
	}
	return found ? adapter : NULL;
}

/**
 * zfcp_unit_enqueue - enqueue unit to unit list of a port.
 * @port: pointer to port where unit is added
 * @fcp_lun: FCP LUN of unit to be enqueued
 * Return: pointer to enqueued unit on success, NULL on error
 * Locks: config_sema must be held to serialize changes to the unit list
 *
 * Sets up some unit internal structures and creates sysfs entry.
 */
struct zfcp_unit *
zfcp_unit_enqueue(struct zfcp_port *port, fcp_lun_t fcp_lun)
{
	struct zfcp_unit *unit;

	/*
	 * check that there is no unit with this FCP_LUN already in list
	 * and enqueue it.
	 * Note: Unlike for the adapter and the port, this is an error
	 */
	read_lock_irq(&zfcp_data.config_lock);
	unit = zfcp_get_unit_by_lun(port, fcp_lun);
	read_unlock_irq(&zfcp_data.config_lock);
	if (unit)
		return NULL;

	unit = kzalloc(sizeof (struct zfcp_unit), GFP_KERNEL);
	if (!unit)
		return NULL;

	/* initialise reference count stuff */
	atomic_set(&unit->refcount, 0);
	init_waitqueue_head(&unit->remove_wq);

	unit->port = port;
	unit->fcp_lun = fcp_lun;

	/* setup for sysfs registration */
	snprintf(unit->sysfs_device.bus_id, BUS_ID_SIZE, "0x%016llx", fcp_lun);
	unit->sysfs_device.parent = &port->sysfs_device;
	unit->sysfs_device.release = zfcp_sysfs_unit_release;
	dev_set_drvdata(&unit->sysfs_device, unit);

	/* mark unit unusable as long as sysfs registration is not complete */
	atomic_set_mask(ZFCP_STATUS_COMMON_REMOVE, &unit->status);

	spin_lock_init(&unit->latencies.lock);
	unit->latencies.write.channel.min = 0xFFFFFFFF;
	unit->latencies.write.fabric.min = 0xFFFFFFFF;
	unit->latencies.read.channel.min = 0xFFFFFFFF;
	unit->latencies.read.fabric.min = 0xFFFFFFFF;
	unit->latencies.cmd.channel.min = 0xFFFFFFFF;
	unit->latencies.cmd.fabric.min = 0xFFFFFFFF;

	if (device_register(&unit->sysfs_device)) {
		kfree(unit);
		return NULL;
	}

	if (zfcp_sysfs_unit_create_files(&unit->sysfs_device)) {
		device_unregister(&unit->sysfs_device);
		return NULL;
	}

	zfcp_unit_get(unit);
	unit->scsi_lun = scsilun_to_int((struct scsi_lun *)&unit->fcp_lun);

	write_lock_irq(&zfcp_data.config_lock);
	list_add_tail(&unit->list, &port->unit_list_head);
	atomic_clear_mask(ZFCP_STATUS_COMMON_REMOVE, &unit->status);
	atomic_set_mask(ZFCP_STATUS_COMMON_RUNNING, &unit->status);
	write_unlock_irq(&zfcp_data.config_lock);

	port->units++;
	zfcp_port_get(port);

	return unit;
}

void
zfcp_unit_dequeue(struct zfcp_unit *unit)
{
	zfcp_unit_wait(unit);
	write_lock_irq(&zfcp_data.config_lock);
	list_del(&unit->list);
	write_unlock_irq(&zfcp_data.config_lock);
	unit->port->units--;
	zfcp_port_put(unit->port);
	zfcp_sysfs_unit_remove_files(&unit->sysfs_device);
	device_unregister(&unit->sysfs_device);
}

/*
 * Allocates a combined QTCB/fsf_req buffer for erp actions and fcp/SCSI
 * commands.
 * It also genrates fcp-nameserver request/response buffer and unsolicited
 * status read fsf_req buffers.
 *
 * locks:       must only be called with zfcp_data.config_sema taken
 */
static int
zfcp_allocate_low_mem_buffers(struct zfcp_adapter *adapter)
{
	adapter->pool.fsf_req_erp =
		mempool_create_slab_pool(ZFCP_POOL_FSF_REQ_ERP_NR,
					 zfcp_data.fsf_req_qtcb_cache);
	if (!adapter->pool.fsf_req_erp)
		return -ENOMEM;

	adapter->pool.fsf_req_scsi =
		mempool_create_slab_pool(ZFCP_POOL_FSF_REQ_SCSI_NR,
					 zfcp_data.fsf_req_qtcb_cache);
	if (!adapter->pool.fsf_req_scsi)
		return -ENOMEM;

	adapter->pool.fsf_req_abort =
		mempool_create_slab_pool(ZFCP_POOL_FSF_REQ_ABORT_NR,
					 zfcp_data.fsf_req_qtcb_cache);
	if (!adapter->pool.fsf_req_abort)
		return -ENOMEM;

	adapter->pool.fsf_req_status_read =
		mempool_create_kmalloc_pool(ZFCP_POOL_STATUS_READ_NR,
					    sizeof(struct zfcp_fsf_req));
	if (!adapter->pool.fsf_req_status_read)
		return -ENOMEM;

	adapter->pool.data_status_read =
		mempool_create_slab_pool(ZFCP_POOL_STATUS_READ_NR,
					 zfcp_data.sr_buffer_cache);
	if (!adapter->pool.data_status_read)
		return -ENOMEM;

	adapter->pool.data_gid_pn =
		mempool_create_slab_pool(ZFCP_POOL_DATA_GID_PN_NR,
					 zfcp_data.gid_pn_cache);
	if (!adapter->pool.data_gid_pn)
		return -ENOMEM;

	return 0;
}

/**
 * zfcp_free_low_mem_buffers - free memory pools of an adapter
 * @adapter: pointer to zfcp_adapter for which memory pools should be freed
 * locking:  zfcp_data.config_sema must be held
 */
static void
zfcp_free_low_mem_buffers(struct zfcp_adapter *adapter)
{
	if (adapter->pool.fsf_req_erp)
		mempool_destroy(adapter->pool.fsf_req_erp);
	if (adapter->pool.fsf_req_scsi)
		mempool_destroy(adapter->pool.fsf_req_scsi);
	if (adapter->pool.fsf_req_abort)
		mempool_destroy(adapter->pool.fsf_req_abort);
	if (adapter->pool.fsf_req_status_read)
		mempool_destroy(adapter->pool.fsf_req_status_read);
	if (adapter->pool.data_status_read)
		mempool_destroy(adapter->pool.data_status_read);
	if (adapter->pool.data_gid_pn)
		mempool_destroy(adapter->pool.data_gid_pn);
}

static void zfcp_dummy_release(struct device *dev)
{
	return;
}

int zfcp_status_read_refill(struct zfcp_adapter *adapter)
{
	while (atomic_read(&adapter->stat_miss) > 0)
		if (zfcp_fsf_status_read(adapter, ZFCP_WAIT_FOR_SBAL))
			break;
	else
		atomic_dec(&adapter->stat_miss);

	if (ZFCP_STATUS_READS_RECOM <= atomic_read(&adapter->stat_miss)) {
		zfcp_erp_adapter_reopen(adapter, 0, 103, NULL);
		return 1;
	}
	return 0;
}

static void _zfcp_status_read_scheduler(struct work_struct *work)
{
	zfcp_status_read_refill(container_of(work, struct zfcp_adapter,
					     stat_work));
}

/*
 * Enqueues an adapter at the end of the adapter list in the driver data.
 * All adapter internal structures are set up.
 * Proc-fs entries are also created.
 *
 * returns:	0             if a new adapter was successfully enqueued
 *              ZFCP_KNOWN    if an adapter with this devno was already present
 *		-ENOMEM       if alloc failed
 * locks:	config_sema must be held to serialise changes to the adapter list
 */
struct zfcp_adapter *
zfcp_adapter_enqueue(struct ccw_device *ccw_device)
{
	int retval = 0;
	struct zfcp_adapter *adapter;

	/*
	 * Note: It is safe to release the list_lock, as any list changes
	 * are protected by the config_sema, which must be held to get here
	 */

	/* try to allocate new adapter data structure (zeroed) */
	adapter = kzalloc(sizeof (struct zfcp_adapter), GFP_KERNEL);
	if (!adapter) {
		ZFCP_LOG_INFO("error: allocation of base adapter "
			      "structure failed\n");
		goto out;
	}

	ccw_device->handler = NULL;

	/* save ccw_device pointer */
	adapter->ccw_device = ccw_device;

	retval = zfcp_qdio_allocate_queues(adapter);
	if (retval)
		goto queues_alloc_failed;

	retval = zfcp_qdio_allocate(adapter);
	if (retval)
		goto qdio_allocate_failed;

	retval = zfcp_allocate_low_mem_buffers(adapter);
	if (retval) {
		ZFCP_LOG_INFO("error: pool allocation failed\n");
		goto failed_low_mem_buffers;
	}

	/* initialise reference count stuff */
	atomic_set(&adapter->refcount, 0);
	init_waitqueue_head(&adapter->remove_wq);

	/* initialise list of ports */
	INIT_LIST_HEAD(&adapter->port_list_head);

	/* initialise list of ports to be removed */
	INIT_LIST_HEAD(&adapter->port_remove_lh);

	/* initialize list of fsf requests */
	spin_lock_init(&adapter->req_list_lock);
	retval = zfcp_reqlist_alloc(adapter);
	if (retval) {
		ZFCP_LOG_INFO("request list initialization failed\n");
		goto failed_low_mem_buffers;
	}

	/* initialize debug locks */

	spin_lock_init(&adapter->hba_dbf_lock);
	spin_lock_init(&adapter->san_dbf_lock);
	spin_lock_init(&adapter->scsi_dbf_lock);
	spin_lock_init(&adapter->rec_dbf_lock);

	retval = zfcp_adapter_debug_register(adapter);
	if (retval)
		goto debug_register_failed;

	/* initialize error recovery stuff */

	rwlock_init(&adapter->erp_lock);
	sema_init(&adapter->erp_ready_sem, 0);
	INIT_LIST_HEAD(&adapter->erp_ready_head);
	INIT_LIST_HEAD(&adapter->erp_running_head);

	/* initialize abort lock */
	rwlock_init(&adapter->abort_lock);

	/* initialise some erp stuff */
	init_waitqueue_head(&adapter->erp_thread_wqh);
	init_waitqueue_head(&adapter->erp_done_wqh);

	/* initialize lock of associated request queue */
	rwlock_init(&adapter->request_queue.queue_lock);
	INIT_WORK(&adapter->stat_work, _zfcp_status_read_scheduler);

	/* mark adapter unusable as long as sysfs registration is not complete */
	atomic_set_mask(ZFCP_STATUS_COMMON_REMOVE, &adapter->status);

	dev_set_drvdata(&ccw_device->dev, adapter);

	if (zfcp_sysfs_adapter_create_files(&ccw_device->dev))
		goto sysfs_failed;

	adapter->generic_services.parent = &adapter->ccw_device->dev;
	adapter->generic_services.release = zfcp_dummy_release;
	snprintf(adapter->generic_services.bus_id, BUS_ID_SIZE,
		 "generic_services");

	if (device_register(&adapter->generic_services))
		goto generic_services_failed;

	/* put allocated adapter at list tail */
	write_lock_irq(&zfcp_data.config_lock);
	atomic_clear_mask(ZFCP_STATUS_COMMON_REMOVE, &adapter->status);
	list_add_tail(&adapter->list, &zfcp_data.adapter_list_head);
	write_unlock_irq(&zfcp_data.config_lock);

	zfcp_data.adapters++;

	goto out;

 generic_services_failed:
	zfcp_sysfs_adapter_remove_files(&adapter->ccw_device->dev);
 sysfs_failed:
	zfcp_adapter_debug_unregister(adapter);
 debug_register_failed:
	dev_set_drvdata(&ccw_device->dev, NULL);
	zfcp_reqlist_free(adapter);
 failed_low_mem_buffers:
	zfcp_free_low_mem_buffers(adapter);
	if (qdio_free(ccw_device) != 0)
		ZFCP_LOG_NORMAL("bug: qdio_free for adapter %s failed\n",
				zfcp_get_busid_by_adapter(adapter));
 qdio_allocate_failed:
	zfcp_qdio_free_queues(adapter);
 queues_alloc_failed:
	kfree(adapter);
	adapter = NULL;
 out:
	return adapter;
}

/*
 * returns:	0 - struct zfcp_adapter  data structure successfully removed
 *		!0 - struct zfcp_adapter  data structure could not be removed
 *			(e.g. still used)
 * locks:	adapter list write lock is assumed to be held by caller
 */
void
zfcp_adapter_dequeue(struct zfcp_adapter *adapter)
{
	int retval = 0;
	unsigned long flags;

	cancel_work_sync(&adapter->stat_work);
	zfcp_adapter_scsi_unregister(adapter);
	device_unregister(&adapter->generic_services);
	zfcp_sysfs_adapter_remove_files(&adapter->ccw_device->dev);
	dev_set_drvdata(&adapter->ccw_device->dev, NULL);
	/* sanity check: no pending FSF requests */
	spin_lock_irqsave(&adapter->req_list_lock, flags);
	retval = zfcp_reqlist_isempty(adapter);
	spin_unlock_irqrestore(&adapter->req_list_lock, flags);
	if (!retval) {
		ZFCP_LOG_NORMAL("bug: adapter %s (%p) still in use, "
				"%i requests outstanding\n",
				zfcp_get_busid_by_adapter(adapter), adapter,
				atomic_read(&adapter->reqs_active));
		retval = -EBUSY;
		goto out;
	}

	zfcp_adapter_debug_unregister(adapter);

	/* remove specified adapter data structure from list */
	write_lock_irq(&zfcp_data.config_lock);
	list_del(&adapter->list);
	write_unlock_irq(&zfcp_data.config_lock);

	/* decrease number of adapters in list */
	zfcp_data.adapters--;

	ZFCP_LOG_TRACE("adapter %s (%p) removed from list, "
		       "%i adapters still in list\n",
		       zfcp_get_busid_by_adapter(adapter),
		       adapter, zfcp_data.adapters);

	retval = qdio_free(adapter->ccw_device);
	if (retval)
		ZFCP_LOG_NORMAL("bug: qdio_free for adapter %s failed\n",
				zfcp_get_busid_by_adapter(adapter));

	zfcp_free_low_mem_buffers(adapter);
	/* free memory of adapter data structure and queues */
	zfcp_qdio_free_queues(adapter);
	zfcp_reqlist_free(adapter);
	kfree(adapter->fc_stats);
	kfree(adapter->stats_reset_data);
	ZFCP_LOG_TRACE("freeing adapter structure\n");
	kfree(adapter);
 out:
	return;
}

/**
 * zfcp_port_enqueue - enqueue port to port list of adapter
 * @adapter: adapter where remote port is added
 * @wwpn: WWPN of the remote port to be enqueued
 * @status: initial status for the port
 * @d_id: destination id of the remote port to be enqueued
 * Return: pointer to enqueued port on success, NULL on error
 * Locks: config_sema must be held to serialize changes to the port list
 *
 * All port internal structures are set up and the sysfs entry is generated.
 * d_id is used to enqueue ports with a well known address like the Directory
 * Service for nameserver lookup.
 */
struct zfcp_port *
zfcp_port_enqueue(struct zfcp_adapter *adapter, wwn_t wwpn, u32 status,
		  u32 d_id)
{
	struct zfcp_port *port;
	int check_wwpn;

	check_wwpn = !(status & ZFCP_STATUS_PORT_NO_WWPN);
	/*
	 * check that there is no port with this WWPN already in list
	 */
	if (check_wwpn) {
		read_lock_irq(&zfcp_data.config_lock);
		port = zfcp_get_port_by_wwpn(adapter, wwpn);
		read_unlock_irq(&zfcp_data.config_lock);
		if (port)
			return NULL;
	}

	port = kzalloc(sizeof (struct zfcp_port), GFP_KERNEL);
	if (!port)
		return NULL;

	/* initialise reference count stuff */
	atomic_set(&port->refcount, 0);
	init_waitqueue_head(&port->remove_wq);

	INIT_LIST_HEAD(&port->unit_list_head);
	INIT_LIST_HEAD(&port->unit_remove_lh);

	port->adapter = adapter;

	if (check_wwpn)
		port->wwpn = wwpn;

	atomic_set_mask(status, &port->status);

	/* setup for sysfs registration */
	if (status & ZFCP_STATUS_PORT_WKA) {
		switch (d_id) {
		case ZFCP_DID_DIRECTORY_SERVICE:
			snprintf(port->sysfs_device.bus_id, BUS_ID_SIZE,
				 "directory");
			break;
		case ZFCP_DID_MANAGEMENT_SERVICE:
			snprintf(port->sysfs_device.bus_id, BUS_ID_SIZE,
				 "management");
			break;
		case ZFCP_DID_KEY_DISTRIBUTION_SERVICE:
			snprintf(port->sysfs_device.bus_id, BUS_ID_SIZE,
				 "key_distribution");
			break;
		case ZFCP_DID_ALIAS_SERVICE:
			snprintf(port->sysfs_device.bus_id, BUS_ID_SIZE,
				 "alias");
			break;
		case ZFCP_DID_TIME_SERVICE:
			snprintf(port->sysfs_device.bus_id, BUS_ID_SIZE,
				 "time");
			break;
		default:
			kfree(port);
			return NULL;
		}
		port->d_id = d_id;
		port->sysfs_device.parent = &adapter->generic_services;
	} else {
		snprintf(port->sysfs_device.bus_id,
			 BUS_ID_SIZE, "0x%016llx", wwpn);
		port->sysfs_device.parent = &adapter->ccw_device->dev;
	}
	port->sysfs_device.release = zfcp_sysfs_port_release;
	dev_set_drvdata(&port->sysfs_device, port);

	/* mark port unusable as long as sysfs registration is not complete */
	atomic_set_mask(ZFCP_STATUS_COMMON_REMOVE, &port->status);

	if (device_register(&port->sysfs_device)) {
		kfree(port);
		return NULL;
	}

	if (zfcp_sysfs_port_create_files(&port->sysfs_device, status)) {
		device_unregister(&port->sysfs_device);
		return NULL;
	}

	zfcp_port_get(port);

	write_lock_irq(&zfcp_data.config_lock);
	list_add_tail(&port->list, &adapter->port_list_head);
	atomic_clear_mask(ZFCP_STATUS_COMMON_REMOVE, &port->status);
	atomic_set_mask(ZFCP_STATUS_COMMON_RUNNING, &port->status);
	if (d_id == ZFCP_DID_DIRECTORY_SERVICE)
		if (!adapter->nameserver_port)
			adapter->nameserver_port = port;
	adapter->ports++;
	write_unlock_irq(&zfcp_data.config_lock);

	zfcp_adapter_get(adapter);

	return port;
}

void
zfcp_port_dequeue(struct zfcp_port *port)
{
	zfcp_port_wait(port);
	write_lock_irq(&zfcp_data.config_lock);
	list_del(&port->list);
	port->adapter->ports--;
	write_unlock_irq(&zfcp_data.config_lock);
	if (port->rport)
		fc_remote_port_delete(port->rport);
	port->rport = NULL;
	zfcp_adapter_put(port->adapter);
	zfcp_sysfs_port_remove_files(&port->sysfs_device,
				     atomic_read(&port->status));
	device_unregister(&port->sysfs_device);
}

/* Enqueues a nameserver port */
int
zfcp_nameserver_enqueue(struct zfcp_adapter *adapter)
{
	struct zfcp_port *port;

	port = zfcp_port_enqueue(adapter, 0, ZFCP_STATUS_PORT_WKA,
				 ZFCP_DID_DIRECTORY_SERVICE);
	if (!port) {
		ZFCP_LOG_INFO("error: enqueue of nameserver port for "
			      "adapter %s failed\n",
			      zfcp_get_busid_by_adapter(adapter));
		return -ENXIO;
	}
	zfcp_port_put(port);

	return 0;
}

#undef ZFCP_LOG_AREA
