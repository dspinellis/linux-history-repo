/* ------------------------------------------------------------
 * rpa_vscsi.c
 * (C) Copyright IBM Corporation 1994, 2003
 * Authors: Colin DeVilbiss (devilbis@us.ibm.com)
 *          Santiago Leon (santil@us.ibm.com)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307
 * USA
 *
 * ------------------------------------------------------------
 * RPA-specific functions of the SCSI host adapter for Virtual I/O devices
 *
 * This driver allows the Linux SCSI peripheral drivers to directly
 * access devices in the hosting partition, either on an iSeries
 * hypervisor system or a converged hypervisor system.
 */

#include <asm/vio.h>
#include <asm/iommu.h>
#include <asm/hvcall.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include "ibmvscsi.h"

/* ------------------------------------------------------------
 * Routines for managing the command/response queue
 */
/**
 * ibmvscsi_handle_event: - Interrupt handler for crq events
 * @irq:	number of irq to handle, not used
 * @dev_instance: ibmvscsi_host_data of host that received interrupt
 * @regs:	pt_regs with registers
 *
 * Disables interrupts and schedules srp_task
 * Always returns IRQ_HANDLED
 */
static irqreturn_t ibmvscsi_handle_event(int irq,
					 void *dev_instance,
					 struct pt_regs *regs)
{
	struct ibmvscsi_host_data *hostdata =
	    (struct ibmvscsi_host_data *)dev_instance;
	vio_disable_interrupts(to_vio_dev(hostdata->dev));
	tasklet_schedule(&hostdata->srp_task);
	return IRQ_HANDLED;
}

/**
 * release_crq_queue: - Deallocates data and unregisters CRQ
 * @queue:	crq_queue to initialize and register
 * @host_data:	ibmvscsi_host_data of host
 *
 * Frees irq, deallocates a page for messages, unmaps dma, and unregisters
 * the crq with the hypervisor.
 */
void ibmvscsi_release_crq_queue(struct crq_queue *queue,
				struct ibmvscsi_host_data *hostdata,
				int max_requests)
{
	long rc;
	struct vio_dev *vdev = to_vio_dev(hostdata->dev);
	free_irq(vdev->irq, (void *)hostdata);
	tasklet_kill(&hostdata->srp_task);
	do {
		rc = plpar_hcall_norets(H_FREE_CRQ, vdev->unit_address);
	} while ((rc == H_Busy) || (H_isLongBusy(rc)));
	dma_unmap_single(hostdata->dev,
			 queue->msg_token,
			 queue->size * sizeof(*queue->msgs), DMA_BIDIRECTIONAL);
	free_page((unsigned long)queue->msgs);
}

/**
 * crq_queue_next_crq: - Returns the next entry in message queue
 * @queue:	crq_queue to use
 *
 * Returns pointer to next entry in queue, or NULL if there are no new 
 * entried in the CRQ.
 */
static struct viosrp_crq *crq_queue_next_crq(struct crq_queue *queue)
{
	struct viosrp_crq *crq;
	unsigned long flags;

	spin_lock_irqsave(&queue->lock, flags);
	crq = &queue->msgs[queue->cur];
	if (crq->valid & 0x80) {
		if (++queue->cur == queue->size)
			queue->cur = 0;
	} else
		crq = NULL;
	spin_unlock_irqrestore(&queue->lock, flags);

	return crq;
}

/**
 * ibmvscsi_send_crq: - Send a CRQ
 * @hostdata:	the adapter
 * @word1:	the first 64 bits of the data
 * @word2:	the second 64 bits of the data
 */
int ibmvscsi_send_crq(struct ibmvscsi_host_data *hostdata, u64 word1, u64 word2)
{
	struct vio_dev *vdev = to_vio_dev(hostdata->dev);

	return plpar_hcall_norets(H_SEND_CRQ, vdev->unit_address, word1, word2);
}

/**
 * ibmvscsi_task: - Process srps asynchronously
 * @data:	ibmvscsi_host_data of host
 */
static void ibmvscsi_task(void *data)
{
	struct ibmvscsi_host_data *hostdata = (struct ibmvscsi_host_data *)data;
	struct vio_dev *vdev = to_vio_dev(hostdata->dev);
	struct viosrp_crq *crq;
	int done = 0;

	while (!done) {
		/* Pull all the valid messages off the CRQ */
		while ((crq = crq_queue_next_crq(&hostdata->queue)) != NULL) {
			ibmvscsi_handle_crq(crq, hostdata);
			crq->valid = 0x00;
		}

		vio_enable_interrupts(vdev);
		if ((crq = crq_queue_next_crq(&hostdata->queue)) != NULL) {
			vio_disable_interrupts(vdev);
			ibmvscsi_handle_crq(crq, hostdata);
			crq->valid = 0x00;
		} else {
			done = 1;
		}
	}
}

/**
 * initialize_crq_queue: - Initializes and registers CRQ with hypervisor
 * @queue:	crq_queue to initialize and register
 * @hostdata:	ibmvscsi_host_data of host
 *
 * Allocates a page for messages, maps it for dma, and registers
 * the crq with the hypervisor.
 * Returns zero on success.
 */
int ibmvscsi_init_crq_queue(struct crq_queue *queue,
			    struct ibmvscsi_host_data *hostdata,
			    int max_requests)
{
	int rc;
	struct vio_dev *vdev = to_vio_dev(hostdata->dev);

	queue->msgs = (struct viosrp_crq *)get_zeroed_page(GFP_KERNEL);

	if (!queue->msgs)
		goto malloc_failed;
	queue->size = PAGE_SIZE / sizeof(*queue->msgs);

	queue->msg_token = dma_map_single(hostdata->dev, queue->msgs,
					  queue->size * sizeof(*queue->msgs),
					  DMA_BIDIRECTIONAL);

	if (dma_mapping_error(queue->msg_token))
		goto map_failed;

	rc = plpar_hcall_norets(H_REG_CRQ,
				vdev->unit_address,
				queue->msg_token, PAGE_SIZE);
	if (rc == 2) {
		/* Adapter is good, but other end is not ready */
		printk(KERN_WARNING "ibmvscsi: Partner adapter not ready\n");
	} else if (rc != 0) {
		printk(KERN_WARNING "ibmvscsi: Error %d opening adapter\n", rc);
		goto reg_crq_failed;
	}

	if (request_irq(vdev->irq,
			ibmvscsi_handle_event,
			0, "ibmvscsi", (void *)hostdata) != 0) {
		printk(KERN_ERR "ibmvscsi: couldn't register irq 0x%x\n",
		       vdev->irq);
		goto req_irq_failed;
	}

	rc = vio_enable_interrupts(vdev);
	if (rc != 0) {
		printk(KERN_ERR "ibmvscsi:  Error %d enabling interrupts!!!\n",
		       rc);
		goto req_irq_failed;
	}

	queue->cur = 0;
	spin_lock_init(&queue->lock);

	tasklet_init(&hostdata->srp_task, (void *)ibmvscsi_task,
		     (unsigned long)hostdata);

	return 0;

      req_irq_failed:
	do {
		rc = plpar_hcall_norets(H_FREE_CRQ, vdev->unit_address);
	} while ((rc == H_Busy) || (H_isLongBusy(rc)));
      reg_crq_failed:
	dma_unmap_single(hostdata->dev,
			 queue->msg_token,
			 queue->size * sizeof(*queue->msgs), DMA_BIDIRECTIONAL);
      map_failed:
	free_page((unsigned long)queue->msgs);
      malloc_failed:
	return -1;
}

/**
 * reset_crq_queue: - resets a crq after a failure
 * @queue:	crq_queue to initialize and register
 * @hostdata:	ibmvscsi_host_data of host
 *
 */
void ibmvscsi_reset_crq_queue(struct crq_queue *queue,
			      struct ibmvscsi_host_data *hostdata)
{
	int rc;
	struct vio_dev *vdev = to_vio_dev(hostdata->dev);

	/* Close the CRQ */
	do {
		rc = plpar_hcall_norets(H_FREE_CRQ, vdev->unit_address);
	} while ((rc == H_Busy) || (H_isLongBusy(rc)));

	/* Clean out the queue */
	memset(queue->msgs, 0x00, PAGE_SIZE);
	queue->cur = 0;

	/* And re-open it again */
	rc = plpar_hcall_norets(H_REG_CRQ,
				vdev->unit_address,
				queue->msg_token, PAGE_SIZE);
	if (rc == 2) {
		/* Adapter is good, but other end is not ready */
		printk(KERN_WARNING "ibmvscsi: Partner adapter not ready\n");
	} else if (rc != 0) {
		printk(KERN_WARNING
		       "ibmvscsi: couldn't register crq--rc 0x%x\n", rc);
	}
}
