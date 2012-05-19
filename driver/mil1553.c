/**
 * Julian Lewis March 28 2012 BE/CO/HT
 * Julian.Lewis@cern.ch
 *
 * This is a total rewrite of the CBMIA PCI driver to control MIL 1553
 * MIL 1553 bus controler CBMIA module
 *
 * This code relies on a new firmware version number 0x206 and later
 * In this version proper access to the TXREG uses a busy done bit.
 * Software polling has been implemented, hardware polling is removed.
 * The bus speed is fixed at 1Mbit.
 * Hardware test points and diagnostic/debug registers are added.
 *
 * Mil1553 driver
 * This driver is a complete rewrite of a previous version from Yuri
 * Version 1 was started by: BE/CO/HT Julian Lewis Tue 15th Feb 2011
 *
 * TODO: The tx-queues are not needed due to the rti and quick data protocols
 *       being implemented in user space. The queue mechanism adds complexity
 *       that it turns out is not needed. The queuing mechanisms therefore
 *       need revising. For the time being they are harmless and work fine so
 *       appart from more difficult code there are no other problems.
 */

#include <linux/spinlock.h>
#include <linux/fs.h>
#include <linux/types.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <asm/io.h>
#include <linux/uaccess.h>
#include <linux/interrupt.h>
#include <linux/wait.h>
#include <linux/mutex.h>

#include "mil1553.h"
#include "mil1553P.h"

#ifndef MIL1553_DRIVER_VERSION
#define MIL1553_DRIVER_VERSION	"noversion"
#endif
char *mil1553_driver_version = MIL1553_DRIVER_VERSION;

static int   mil1553_major      = 0;
static char *mil1553_major_name = "mil1553";

MODULE_AUTHOR("Julian Lewis BE/CO/HT CERN");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("MIL1553 Driver");
MODULE_SUPPORTED_DEVICE("CBMIA PCI module");

/**
 * Module parameters bcs=7,4,3,5 pci_bus=1,1,2,3 pci_slot=4,5,4,5
 */

static int bcs[MAX_DEVS];
static int pci_buses[MAX_DEVS];
static int pci_slots[MAX_DEVS];

static int bc_num;
static int pci_bus_num;
static int pci_slot_num;

module_param_array(bcs,       int, &bc_num,       0);
module_param_array(pci_buses, int, &pci_bus_num,  0);
module_param_array(pci_slots, int, &pci_slot_num, 0);

MODULE_PARM_DESC(bcs,       "bus controller number 1..8");
MODULE_PARM_DESC(pci_buses, "pci bus number");
MODULE_PARM_DESC(pci_slots, "pci slot number");

static int dump_packet;			/* to dump corrupted packets */
module_param(dump_packet, int, 0);

#ifndef COMPILE_TIME
#define COMPILE_TIME 0
#endif

/**
 * Drivers static working area
 */

struct working_area_s wa;

/**
 * =========================================================
 * Debug ioctl calls
 */

#define NAME(x) [mil1553##x] = #x
static char *ioctl_names[] = {

	NAME(GET_DEBUG_LEVEL),
	NAME(SET_DEBUG_LEVEL),

	NAME(GET_TIMEOUT_MSEC),
	NAME(SET_TIMEOUT_MSEC),

	NAME(GET_DRV_VERSION),

	NAME(GET_STATUS),
	NAME(GET_TEMPERATURE),

	NAME(GET_BCS_COUNT),
	NAME(GET_BC_INFO),

	NAME(RAW_READ),
	NAME(RAW_WRITE),

	NAME(GET_UP_RTIS),
	NAME(SEND),
	NAME(RECV),

	NAME(LOCK_BC),
	NAME(UNLOCK_BC),

	NAME(QUEUE_SIZE),
	NAME(RESET),

	NAME(SET_POLLING),
	NAME(GET_POLLING),

	NAME(SET_TP),
	NAME(GET_TP),
};

/**
 * =========================================================
 * @brief Get device corresponding to a given BC
 * @param Given BC number of device
 * @return Pointer to device if exists, else NULL
 */

struct mil1553_device_s *get_dev(int bc)
{
	int i;
	struct mil1553_device_s *mdev;

	if (bc > 0) {
		for (i=0; i<wa.bcs; i++) {
			mdev = &(wa.mil1553_dev[i]);
			if (mdev->bc == bc)
				return mdev;
		}
	}
	return NULL;
}

/**
 * =========================================================
 * @brief Validate insmod args, can be empty
 * @return 1=OK 0=ERROR
 */

static int check_args(void)
{
       int i;

       if ((bc_num < 0) || (bc_num > MAX_DEVS)) {
	       printk("mill1553:bad BC count:%d, not installing\n",bc_num);
	       return 0;
       }
       if ((bc_num != pci_slot_num) || (bc_num != pci_bus_num)) {
	       printk("mill1553:bad parameter count\n");
	       return 0;
       }
       for (i=0; i<bc_num; i++) {
	       if ((bcs[i] <= 0) || (bcs[i] >= MAX_DEVS)) {
		       printk("mill1553:bad BC num:%d\n", bcs[i]);
		       return 0;
	       }
       }
       return 1;
}

/**
 * =========================================================
 * @brief       Hunt for a BC number
 * @param bus   The PCI bus
 * @param slot  The PCI slot number
 * @return      The bc number or zero if not found
 */

int hunt_bc(int bus, int slot)
{
	int i;
	for (i=0; i<bc_num; i++)
		if ((pci_slots[i] == slot)
		&&  (pci_buses[i] == bus))
			return bcs[i];
	return 0;
}

/**
 * =========================================================
 * @brief Print debug information
 * @param debug_level  debug level 0..7
 * @param ionr         command number decoded
 * @param iosz         size of arg in bytes
 * @param iodr         io direction
 * @param kmem         points to kernel memory where arg copied to/from
 * @param flag         Before or after logic flag
 *
 * For people developing mil1553 code this debug routine is useful, especially
 * if they are attempting to use raw IO. There are 7 levels of debug implemented
 * throughout the driver code that controls just how much information gets printed
 * out. This will help in maintanence, debugging modifications etc.
 */

#define MAX_PCOUNT 16
#define BEFORE 1
#define AFTER 2

static void debug_ioctl(int   debug_level,
			int   ionr,
			int   iosz,
			int   iodr,
			void *kmem,
			int   flag)
{
	int cindx, *values, pcount, i;

	if (flag == BEFORE) {

		if ((ionr <= mil1553FIRST) || (ionr >= mil1553LAST)) {
			printk("mil1553:Illegal ioctl:ionr:%d iosz:%d iodr:%d\n",ionr,iosz,iodr);
			return;
		}

		if (!debug_level)
			return;

		cindx = ionr - mil1553FIRST -1;

		printk("\n=> mil1553:ioctl:ionr:%d[%s] iosz:%d iodr:%d[",ionr,ioctl_names[cindx],iosz,iodr);
		if (iodr & _IOC_WRITE)
			printk("W");
		if (iodr & _IOC_READ)
			printk("R");
		printk("]\n");
	}

	if (debug_level > 1) {
		values = kmem;
		pcount = iosz / sizeof(int);
		if (pcount > MAX_PCOUNT)
			pcount = MAX_PCOUNT;

		if ((flag == BEFORE) && (iodr & _IOC_WRITE)) {
			printk("IO Buffer BEFORE logic:");
			for (i=0; i<pcount; i++) {
				if (!(i % 4))
					printk("\nkmem:%02d:",i);
				printk("0x%08X ",values[i]);
			}
		}
		if ((flag == AFTER) && (iodr & _IOC_READ)) {
			printk("IO Buffer AFTER logic:");
			for (i=0; i<pcount; i++) {
				if (!(i % 4))
					printk("\nkmem:%02d:",i);
				printk("0x%08X ",values[i]);
			}
		}
		printk("\n");
	}
}

/**
 * =========================================================
 * @brief Calculate the number of items on a queue of size qsz
 * @param rp Read pointer
 * @param wp Write pointer
 * @param maximum queue size
 * @return number of items remaining on queue
 */

static uint32_t get_queue_size(uint32_t rp, uint32_t wp, uint32_t qsz)
{
	if (wp >= rp)
		return (wp - rp);
	return (qsz - (rp - wp) + 1);
}

/**
 * =========================================================
 * @brief Get the next write pointer
 * @param rp Read pointer
 * @param wp Address of write pointer
 * @param maximum queue size
 * @return 1=All OK, 0=Queue is FULL
 */

static int get_next_wp(uint32_t rp, uint32_t *wp, uint32_t qsz)
{
       if (get_queue_size(rp,*wp,qsz) < qsz) {
	       if (++*wp >= qsz)
		       *wp = 0;
	       return 1;
       }
       return 0;
}

/**
 * =========================================================
 * @brief Get the next read pointer
 * @param rp Address of Read pointer
 * @param Write pointer
 * @param maximum queue size
 * @return 1=All OK, 0=Queue is EMPTY
 */

static int get_next_rp(uint32_t *rp, uint32_t wp, uint32_t qsz)
{
       if (get_queue_size(*rp,wp,qsz) > 0) {
	       if (++*rp >= qsz)
		       *rp = 0;
	       return 1;
       }
       return 0;
}

/**
 * =========================================================
 * @brief Reset a TX queue
 * @param mdev pointer to cbmia device context
 *
 * When an RTI times out, the tx_queue may still have items
 * on it. The client has already seen a timeout so the remaining
 * items on the queue need to be cleaned up.
 */

static void reset_tx_queue(struct mil1553_device_s *mdev)
{
	struct tx_queue_s *tx_queue;
	unsigned long flags;

	tx_queue = mdev->tx_queue;

	spin_lock_irqsave(&tx_queue->lock,flags);
	tx_queue->rp = 0;
	tx_queue->wp = 0;
	mdev->busy_done = BC_DONE;                      /** Transaction done */
	spin_unlock_irqrestore(&tx_queue->lock,flags);
}

/**
 * =========================================================
 * @brief           Read U32 integers from mapped address space
 * @param mdev      Mill1553 device
 * @param riob      IO buffer descriptor
 * @param buf       Buffer to hold data read
 * @return          Number of bytes read
 */

static int _raw_read(struct mil1553_device_s *mdev,
		     struct mil1553_riob_s *riob,
		     void *buf)
{

	int i;
	uint32_t *uip, *hip;

	uip = buf;
	if ((!mdev) || (!mdev->memory_map))
		return 0;

	hip = (uint32_t *) mdev->memory_map + riob->reg_num;

	for (i=0; i<riob->regs; i++) {
		uip[i] = ioread32be(&hip[i]);
	}

	/*
	 * Remember that i will be greater than length
	 * when the loop terminates.
	 */

	return i*sizeof(int);
}

/**
 * @brief Just calls _raw_read with spin lock protection
 */

static int raw_read(struct mil1553_device_s *mdev,
		    struct mil1553_riob_s *riob,
		    void *buf)
{
	int res;
	spin_lock(&mdev->lock);
	res = _raw_read(mdev, riob, buf);
	spin_unlock(&mdev->lock);
	return res;
}

/**
 * =========================================================
 * @brief           Write U32 integers to mapped address space
 * @param mdev      Mill1553 device
 * @param riob      IO buffer descriptor
 * @param buf       Buffer to holding data to write
 * @return          Number of bytes written
 */

static int _raw_write(struct mil1553_device_s *mdev,
		      struct mil1553_riob_s *riob,
		      void *buf)
{

	int i;
	uint32_t *uip, *hip;

	uip = buf;
	if ((!mdev) || (!mdev->memory_map))
		return 0;

	hip = (uint32_t *) mdev->memory_map + riob->reg_num;

	for (i=0; i<riob->regs; i++) {
		iowrite32be(uip[i],&hip[i]);
	}

	/*
	 * Remember that i will be greater than length
	 * when the loop terminates.
	 */

	return i*sizeof(int);
}

/**
 * @brief Just calls _raw_write with spin lock protection
 */

static int raw_write(struct mil1553_device_s *mdev,
		     struct mil1553_riob_s *riob,
		     void *buf)
{
	int res;
	spin_lock(&mdev->lock);
	res = _raw_write(mdev, riob, buf);
	spin_unlock(&mdev->lock);
	return res;
}

/**
 * =========================================================
 * Attempt to read the RTI signature from all RTIs 1..30, in
 * the case the RTI responds the resulting interrupt sets
 * the corresponding bit in an new RTI present mask.
 */

#if 0
static int do_start_tx_(struct mil1553_device_s *mdev, uint32_t txreg)
{
	struct memory_map_s *memory_map = mdev->memory_map;
	int i, irqs, timeleft;

	if (mutex_lock_interruptible(&mdev->tx_attempt) != 0) {
		printk(KERN_ERR "mil1553: TX aborted by signal\n");
		return -EINTR;
	}
	irqs = mdev->icnt;
	for (i = 0; i < TX_TRIES; i++) {
		if ((ioread32be(&memory_map->hstat) & HSTAT_BUSY_BIT) == 0) {
			iowrite32be(txreg, &memory_map->txreg);
			mdev->tx_count++;
			break;
		}
		printk(KERN_ERR "mil1553: HSTAT_BUSY_BIT != 0 in do_start_tx; "
				"tx_count %d, ms %u on pid %d\n", mdev->tx_count,
					jiffies_to_msecs(jiffies), current->pid);
		udelay(TX_WAIT_US);
	}
	timeleft = wait_event_interruptible_timeout(mdev->int_complete,
			mdev->icnt != irqs, usecs_to_jiffies(CBMIA_INT_TIMEOUT));
	if (timeleft < 0) {
		reset_tx_queue(mdev);
		printk(KERN_ERR "mil1553: wait interrupt timeout or signal"
				"at bc:tx_count %d:%d!\n", mdev->bc, mdev->tx_count);
	}
	mutex_unlock(&mdev->tx_attempt);
	return timeleft;
}
#endif

#define BETWEEN_TRIES_MS 1
#define TX_TRIES 100
#define TX_WAIT_US 10
#define CBMIA_INT_TIMEOUT (msecs_to_jiffies(6))
#define INT_MISSING_TIMEOUT (msecs_to_jiffies(20000))

static int do_start_tx(struct mil1553_device_s *mdev, uint32_t txreg)
{
	struct memory_map_s *memory_map = mdev->memory_map;
	int i, icnt, timeleft;

	do {
		timeleft = wait_event_interruptible_timeout(mdev->int_complete,
			atomic_read(&mdev->busy) == 0, INT_MISSING_TIMEOUT);
		if (timeleft == 0) {
			printk(KERN_ERR "mil1553: missing int in bc %d\n", mdev->bc);
			atomic_set(&mdev->busy, 0);
		}
		if (signal_pending(current))
			return -ERESTARTSYS;
	} while (atomic_xchg(&mdev->busy, 1));

	icnt = mdev->icnt;
	for (i = 0; i < TX_TRIES; i++) {
		if ((ioread32be(&memory_map->hstat) & HSTAT_BUSY_BIT) == 0) {
			mdev->jif0 = jiffies;
			iowrite32be(txreg, &memory_map->txreg);
			mdev->tx_count++;
			break;
		}
		printk(KERN_ERR "mil1553: HSTAT_BUSY_BIT != 0 in do_start_tx; "
				"tx_count %d, ms %u on pid %d\n", mdev->tx_count,
					jiffies_to_msecs(jiffies), current->pid);
		udelay(TX_WAIT_US);
	}
	udelay(3*TX_WAIT_US);
	timeleft = wait_event_interruptible_timeout(mdev->int_complete,
					icnt < mdev->icnt, CBMIA_INT_TIMEOUT);
	if (timeleft <= 0)
		printk(KERN_ERR "mil1553: interrupt pending"
				" after %d msecs in bc %d, timeleft = %d\n",
				jiffies_to_msecs(CBMIA_INT_TIMEOUT),
				mdev->bc, timeleft);
	return 0;
}

static void ping_rtis(struct mil1553_device_s *mdev)
{
	int rti;
	uint32_t txreg;
	struct memory_map_s *memory_map;

	memory_map = mdev->memory_map;
	if (mdev->busy_done == BC_DONE) {       /** Make sure no transaction in progress */
		for (rti=1; rti<=30; rti++) {   /** Next RTI to poll */
			txreg = ((1  << TXREG_WC_SHIFT)   & TXREG_WC_MASK)
			      | ((30 << TXREG_SUBA_SHIFT) & TXREG_SUBA_MASK)
			      | ((1  << TXREG_TR_SHIFT)   & TXREG_TR_MASK)
			      | ((rti<< TXREG_RTI_SHIFT)  & TXREG_RTI_MASK);
			do_start_tx(mdev, txreg);
			msleep(BETWEEN_TRIES_MS);               /** Wait between pollings */
		}
	}
}

/**
 * =========================================================
 * @brief Check RTI is up
 * @param mdev Mill1553 device
 * @param rtin RTI number
 * @return 0 RTI-Down 1 RTI-Up
 */

static int check_rti_up(struct mil1553_device_s *mdev, unsigned int rtin)
{
	uint32_t up_rtis, mask;

	up_rtis = mdev->up_rtis;
	mask = 1 << rtin;
	if (mask & up_rtis)
		return 1;
	return 0;
}

/**
 * =========================================================
 * @brief Get the word count from a txreg
 * @param txreg
 * @return the word count
 *
 * After much trial and error the behaviour of wc
 * values on the cbmia seems to be as follows...
 * On reading data the status is always prefixed and
 * this plays no part in the word count interpretation.
 * A value of zero represents a word count of 32 in
 * the appropriate modes.
 */

unsigned int get_wc(unsigned int txreg)
{
	unsigned int wc;

	wc = (txreg & TXREG_WC_MASK) >> TXREG_WC_SHIFT;
	if (wc == 0)
		wc = 32;
	return wc;
}

/**
 * =========================================================
 * @brief Start the RTI with the current tx queue entry.
 * @param mdev is the bus controller descriptor to start
 *
 * This routine gets the current item on the device queue
 * if any, sends it to the RTI and starts the command.
 * This gets called from the ISR and from the IOCTL code.
 */

static void _start_tx(int debug_level,
		      struct mil1553_device_s *mdev)
{
	struct memory_map_s *memory_map;
	uint32_t *rp, *wp, wc;
	struct tx_queue_s *tx_queue;
	struct tx_item_s *tx_item;
	unsigned long flags;
	int i;
	uint32_t lreg, *lregp;

	if (debug_level > 4)
		printk("mil1553:start_tx:Bc:%02d UpRtis:0x%08X\n",mdev->bc,mdev->up_rtis);

	memory_map = mdev->memory_map;

	/* Get the current transmission item off the queue */

	tx_queue = mdev->tx_queue;

	spin_lock_irqsave(&tx_queue->lock,flags);
	rp = &tx_queue->rp;
	wp = &tx_queue->wp;
	if (get_queue_size(*rp,*wp,QSZ) == 0) {
		mdev->busy_done = BC_DONE;                     /** Transaction done */
		spin_unlock_irqrestore(&tx_queue->lock,flags);
		if (debug_level > 5)
			printk("mil1553:start_tx:Queue empty\n");
		return;
	}
	tx_item = &(tx_queue->tx_item[*rp]);
	spin_unlock_irqrestore(&tx_queue->lock,flags);

	/* Copy the item wc long to the tx buffer */
	/* Remember txbuf is accessed as u32 but wc is the u16 count */
	/* Word order is little endian, but hey byte order is big endian !! */

	wc = get_wc(tx_item->txreg);
	lregp = (uint32_t *) memory_map->txbuf;
	for (i=0; i<(wc + 1)/2; i++) {
		lreg  =  tx_item->txbuf[i*2 + 1] << 16;
		lreg |= (tx_item->txbuf[i*2 + 0] & 0xFFFF);
		iowrite32be(lreg,&lregp[i]);
	}

	/* Issue the start command, we get an interrupt when done. */

	do_start_tx(mdev, tx_item->txreg);
}

/**
 * @brief Called only from IOCTL ie from user space
 * It just calls _start_tx with a spin_lock reservation
 *
 * As the TX queue never has more than one item on it, then
 * _start_tx never gets called from the ISR. This is part of
 * the code that may be suppressed in the TODO list. For the
 * present you know that mdev->lock will always get taken and that
 * mdev->busy_done is always DONE. This is because every send
 * or receive transaction puts only on item on the TX queue.
 */

static void start_tx(int debug_level,
		     struct mil1553_device_s *mdev)
{
	mutex_lock_interruptible(&mdev->mutex);
	if (mdev->busy_done == BC_DONE)      /** If transaction in progress no need */
		_start_tx(debug_level,mdev); /** to start, leave that to the ISR    */
	else
		printk(KERN_ERR "jdgc: leaving transaction to ISR\n");
	mutex_unlock(&mdev->mutex);
}

/**
 * =========================================================
 * @brief Find the start item for a BC
 * @param item_count is the number of items in the array
 * @param item_array is an array to execute on RTIs for multiple bus controllers
 * @return index of first item in array for the given BC or -1 if not found
 *
 * Contiguous items for each BC are marked with start..all..end and
 * form a transaction. When a transaction is in progress mdev->busy_done is
 * set BUSY. However as the user code always uses an item count of one, then
 * the transaction terminates imediatley so mdev->busy_done will always be DONE.
 *
 * Users can be woken up in three ways..
 * (1) For the Start item in the transaction.
 * (2) For All items in the transaction.
 * (3) For the Last item in the transaction.
 */

int find_start(unsigned int bc,
	       unsigned int item_count,
	       struct mil1553_tx_item_s *item_array)
{
	int i;

	for (i=0; i<item_count; i++)
		if (bc == item_array[i].bc)
			return i;
	return -1;
}

/**
 * @brief Find the end item for a BC
 * @param item_count is the number of items in the array
 * @param item_array is an array to execute on RTIs for multiple bus controllers
 * @return index of last item in array for the given BC or -1 if not found
 */

int find_end(unsigned int bc,
	     unsigned int item_count,
	     struct mil1553_tx_item_s *item_array)
{
	int i;

	for (i=item_count-1; i>=0; i--)
		if (bc == item_array[i].bc)
			return i;
	return -1;
}

/**
 * =========================================================
 * @brief Send items to RTIs
 * @param client putting items on the queues that will recieve replies from the ISR
 * @param item_count is the number of items in the array
 * @param item_array is the array of items to be placed on BC queues
 * @return number of items added to queue if OK or negative error
 *
 * Check all the RTIs in the array of items are up.
 * Find the start and end items for each bus controller (identify transactions).
 * Copy from the item array to the tx_item being built.
 * Initialize extra fields in the tx_item being built.
 * Put the item on the tx_queue for the each bus controller.
 * Increment the write pointer on the tx_queue for the given bus controller.
 * Start the hardware transfer by writing to the txreg.
 *
 * Once the first item in the BC queue is started, the RTI will interrupt when
 * ready and the ISR will itself pull the next item off the queue and start it.
 */

static int send_items(struct client_s *client,
		      unsigned int item_count,
		      struct mil1553_tx_item_s *item_array)
{

	unsigned long flags;
	int i, j, wc, bc, rtin=1, res, strs[MAX_DEVS+1], ends[MAX_DEVS+1];
	struct tx_item_s tx_item;
	uint32_t *rp, *wp;
	struct mil1553_device_s *mdev = NULL;
	struct tx_queue_s *tx_queue;

	if (client->debug_level > 2)
		printk("mil1553:send_items:item_count:%d\n",item_count);


	if (item_count <= 0)
		return 0;

	/* Check the RTIs are up */

	for (i=0; i<item_count; i++) {
		bc = item_array[i].bc;
		mdev = get_dev(bc);
		if (!mdev) {
			if (client->debug_level > 2)
				printk("mil1553:send_items:No such BC:Error:Bc:%02d\n",rtin);
			return -EFAULT;
		}

		rtin = item_array[i].rti_number;
		if (check_rti_up(mdev,rtin) == 0) {
			if (client->debug_level > 2)
				printk("mil1553:send_items:CheckRtis:Warning:Bc:%02d Rti:%02d Down, changing to Up\n",bc,rtin);
		}
	}

	/* For each installed BC get the start and end item numbers */
	/* that will deliniate a transaction. */

	for (i=0; i<wa.bcs; i++) {
		bc = wa.mil1553_dev[i].bc;
		if (bc > 0) {
			strs[bc] = find_start(bc,item_count,item_array);
			ends[bc] = find_end(bc,item_count,item_array);
		}
	}

	res = 0;
	for (i=0; i<item_count; i++) {

		/* Build a tx_item for this array entry */

		memset(&tx_item,0,sizeof(struct tx_item_s));
		tx_item.client = client;
		bc = item_array[i].bc;
		tx_item.bc = bc;
		client->bc = bc;
		rtin = item_array[i].rti_number;
		tx_item.rti_number = rtin;
		tx_item.txreg = item_array[i].txreg;
		tx_item.no_reply = item_array[i].no_reply;

		wc = get_wc(tx_item.txreg);
		for (j=0; j<wc; j++) {
			tx_item.txbuf[j] = item_array[i].txbuf[j];
			if (client->debug_level > 7) {
				if (!(j % 4)) printk("\nSend:%02d ",j);
				printk("0x%04hX ",tx_item.txbuf[j]);
			}
		}

		/* Users can be interrupted on ALL, START and END items in the transaction */

		tx_item.pk_type = TX_ALL;
		if (strs[bc] == i)
			tx_item.pk_type |= TX_START;
		if (ends[bc] == i)
			tx_item.pk_type |= TX_END;

		/* Get the target tx_queue for the bus controller */

		mdev = get_dev(bc);
		if (!mdev)
			return -EFAULT;

		tx_queue = mdev->tx_queue;

		/* Put item on the tx_queue of the bus controller */

		spin_lock_irqsave(&tx_queue->lock,flags);
		rp = &tx_queue->rp;
		wp = &tx_queue->wp;
		if (get_queue_size(*rp,*wp,QSZ) < QSZ) {
			memcpy(&tx_queue->tx_item[*wp],
			       &tx_item,sizeof(struct tx_item_s));
			get_next_wp(*rp,wp,QSZ);
			res++;
		}
		spin_unlock_irqrestore(&tx_queue->lock,flags);

		if (client->debug_level > 3)
			printk("mil1553:send_items:Bc:%02d Queue:Rp:%02d Wp:%02d\n",
			       bc, *rp, *wp);

	}

	/* Start all bus controllers with their transactions to be done */

	for (bc=1; bc<=MAX_DEVS; bc++) {
		if (strs[bc] >= 0) {
			mdev = get_dev(bc);
			if (mdev) {
				start_tx(client->debug_level,mdev);
				if (client->debug_level > 3)
					printk("mil1553:send_items:Bc:%d start_tx\n",bc);
			}
		}
	}

	return res;
}

/**
 * =========================================================
 * @brief Read entry from clients queue and give it to him
 * @param client that wants to read its queue
 * @param mrecv is what the ISR recieved from the RTI and put on the queue
 * @return 0 is OK else a negative error
 *
 * If there is something on the queue it is returned imediatley, if the queue
 * is empty the call waits for a wake up call from the ISR or times out.
 */

struct acq_msg {

	/* req_msg */

	short     family;
	char      type;
	char      sub_family;
	short     member;
	short     service;
	short	  machine_number;
	short	  pls_line;
	short	  unix_seconds;
	short	  unix_us;
	short     specialist;

	/* specific part */

	unsigned char phys_status; /*  0 - PHY_STUS_NOT_DEFINED
				       1 - PHY_STUS_OPERATIONAL
				       2 - PHY_STUS_PARTIALLY_OPERATIONAL
				       3 - PHY_STUS_NOT_OPERATIONAL
				       4 - PHY_STUS_NEEDS_COMMISIONING */

	unsigned char static_status; /* enum TConverterState from types.h */
	unsigned char ext_aspect;    /* enum Texternal_aspects from types.h */
	unsigned char status_qualif; /* bitfield.
					b0 - interlock fault
					b1 - unresettable fault
					b2 - resettable fault
					b3 - busy
					b4 - warning
					b5 - CCV out of limits in G64
					(CCV == current control value)
					b6 - forewarning pulse missing
					b7 - veto security */
	short         busytime;
	uint32_t         aqn;
	uint32_t         aqn1;
	uint32_t         aqn2;
	uint32_t         aqn3;
};

void dump(uint32_t *buf, int wc)
{
	int i;

	printk("VetoSecurity packt: wc = %d\n", wc);
	for (i = 0; i < (wc + 2) / 2; i++) {
		printk(": %04x %04x\n", buf[i*2 + 0], buf[i*2 + 1]);
	}
	printk("\n");
}

void mil1553_print_acq_msg(struct acq_msg *acq_p) {

	int sec, usec;

	sec  = acq_p->unix_seconds;
	usec = acq_p->unix_us;

	printk(KERN_ERR"==> AcquisitionMessage: ");
	printk(KERN_ERR"Sec:0x%08X Usec:0x%08X\n",sec,usec);
	printk(KERN_ERR"   PhysicalStatus :%02d",acq_p->phys_status);
	if (acq_p->phys_status == 0) printk(KERN_ERR" ==> NotDefined");
	if (acq_p->phys_status == 1) printk(KERN_ERR" ==> Operational");
	if (acq_p->phys_status == 2) printk(KERN_ERR" ==> PartialOperation");
	if (acq_p->phys_status == 3) printk(KERN_ERR" ==> NotInOperation");
	if (acq_p->phys_status == 4) printk(KERN_ERR" ==> NeedsComissioning");
	printk(KERN_ERR"\n");
	printk(KERN_ERR"   ConvertorStatus:%02d",acq_p->static_status);
	if (acq_p->static_status == 0 ) printk(KERN_ERR" ==> Unconfigured");
	if (acq_p->static_status == 1 ) printk(KERN_ERR" ==> Off");
	if (acq_p->static_status == 2 ) printk(KERN_ERR" ==> Standby");
	if (acq_p->static_status == 3 ) printk(KERN_ERR" ==> On");
	if (acq_p->static_status == 4 ) printk(KERN_ERR" ==> AtPowerOn");
	if (acq_p->static_status == 5 ) printk(KERN_ERR" ==> BeforeStandby");
	if (acq_p->static_status == 6 ) printk(KERN_ERR" ==> BeforeOff");
	if (acq_p->static_status == 7 ) printk(KERN_ERR" ==> OffToOnRetarded");
	if (acq_p->static_status == 8 ) printk(KERN_ERR" ==> OffToStandbyRetarded");
	if (acq_p->static_status == 9 ) printk(KERN_ERR" ==> WaitingCurrentZero_1");
	if (acq_p->static_status == 10) printk(KERN_ERR" ==> WaitingCurrentZero_2");
	printk(KERN_ERR"\n");
	printk(KERN_ERR"   ExternalAspects:%02d",acq_p->ext_aspect);
	if (acq_p->ext_aspect == 0) printk(KERN_ERR" ==> NotDefined");
	if (acq_p->ext_aspect == 1) printk(KERN_ERR" ==> NotConnected");
	if (acq_p->ext_aspect == 2) printk(KERN_ERR" ==> Local");
	if (acq_p->ext_aspect == 3) printk(KERN_ERR" ==> Remote");
	if (acq_p->ext_aspect == 4) printk(KERN_ERR" ==> VetoSecurity");
	if (acq_p->ext_aspect == 5) printk(KERN_ERR" ==> BeamToDump");
	printk(KERN_ERR"\n");
	printk(KERN_ERR"   StatusQualifier:0x%02hx ==> ",acq_p->status_qualif);
	if (acq_p->status_qualif & 0x01) printk(KERN_ERR"InterlockFault:");
	if (acq_p->status_qualif & 0x02) printk(KERN_ERR"UnresetableFault:");
	if (acq_p->status_qualif & 0x04) printk(KERN_ERR"ResetableFault:");
	if (acq_p->status_qualif & 0x08) printk(KERN_ERR"Busy:");
	if (acq_p->status_qualif & 0x10) printk(KERN_ERR"Warning:");
	if (acq_p->status_qualif & 0x20) printk(KERN_ERR"CCVOutOfRange:");
	if (acq_p->status_qualif & 0x40) printk(KERN_ERR"ForewarningPulseMissing:");
	if (acq_p->status_qualif & 0x80) printk(KERN_ERR"VetoSecurity:");
	printk(KERN_ERR"\n");
	printk(KERN_ERR"   BusyTime       :%02d\n",acq_p->busytime);
	printk(KERN_ERR"   aqn|[1-3]      :%x %x %x %x\n",acq_p->aqn,acq_p->aqn1,acq_p->aqn2,acq_p->aqn3);
	printk(KERN_ERR"<==\n");
}

static void packet_dump(uint32_t *rxbuf, int wc)
{
	struct acq_msg *msg = (struct acq_msg *)rxbuf;
	uint32_t copy[64];

	memcpy(copy, rxbuf, wc);

	if (msg->ext_aspect != 4)
		return;
	dump(copy, wc);
	msg = (struct acq_msg *)copy;
	mil1553_print_acq_msg(msg);
}

int read_queue(struct client_s *client, struct mil1553_recv_s *mrecv)
{
	unsigned long flags;
	int i, cc, wc, qs;
	struct rx_queue_s *rx_queue;
	struct rti_interrupt_s *rti_interrupt;
	uint32_t *wp, *rp, icnt;

	client->pk_type = mrecv->pk_type;
	client->timeout = mrecv->timeout;

	do {
		rx_queue = &client->rx_queue;
		spin_lock_irqsave(&rx_queue->lock,flags);
		wp = &rx_queue->wp;
		rp = &rx_queue->rp;
		rti_interrupt = &rx_queue->rti_interrupt[*rp];

		qs = get_queue_size(*rp,*wp,QSZ);
		if (qs > 0) {

			wc = rti_interrupt->wc; /* This must be in [1..32] */

			mrecv->interrupt.bc         = rti_interrupt->bc;
			mrecv->interrupt.rti_number = rti_interrupt->rti_number;
			mrecv->interrupt.wc         = wc +1; /* Status reg */
			mrecv->icnt                 = client->icnt;
			mrecv->pk_type              = client->pk_type;

			for (i=0; i<wc +1; i++) /* Prepended status */
				mrecv->interrupt.rxbuf[i] = rti_interrupt->rxbuf[i];

			if (dump_packet && wc > 20) {
				packet_dump(rti_interrupt->rxbuf, wc+1);
			}
			get_next_rp(rp,*wp,QSZ);
		}
		spin_unlock_irqrestore(&rx_queue->lock,flags);
		if (qs)
			return 0;

		// printk(KERN_ERR "mil1553: jdgc: this should not happen\n");
		// printk(KERN_ERR "mil1553: jdgc: qs == 0\n");
		icnt = client->icnt;
		cc = wait_event_interruptible_timeout(client->wait_queue,
						     icnt != client->icnt,
						     client->timeout);
		if (cc == 0)
			return -ETIME;

		if (cc < 0)
			return cc;

	} while (1);

	return 0; /* This never happens */
}

/**
 * =========================================================
 * Interrupt service routine.
 * Interrupts come from RTIs.
 * The ISR reads data from the RTI,
 * puts the result on the initiating clients queue,
 * wakes him up, and starts the next transaction.
 */


static irqreturn_t mil1553_isr(int irq, void *arg)
{
	struct mil1553_device_s *mdev = arg;
	struct memory_map_s *memory_map;
	uint32_t isrc, *rp, *wp, bc;
	struct tx_queue_s *tx_queue;
	struct tx_item_s *tx_item;
	struct rx_queue_s *rx_queue;
	struct rti_interrupt_s *rti_interrupt;
	struct client_s *client;
	unsigned long flags;
	uint32_t lreg, *lregp;
	int i, rtin, pk_ok;
	int timeout;
	uint32_t delta;

	memory_map = mdev->memory_map;
	isrc = ioread32be(&memory_map->isrc);   /** Read and clear the interrupt */
	if ((isrc & ISRC) == 0)
		return IRQ_NONE;

	mdev->icnt++;
	delta = jiffies - mdev->jif0;
	if (delta > mdev->jifd)
		mdev->jifd = jiffies_to_usecs(delta);
	wa.icnt++;
	if (!atomic_xchg(&mdev->busy, 0)) {
		printk("mil1553 horror\n");
	}
	wake_up_interruptible(&mdev->int_complete);

	rtin = (isrc & ISRC_RTI_MASK) >> ISRC_RTI_SHIFT; /** Zero on timeout */
	if (isrc & ISRC_TIME_OUT) {
		timeout = 1;
	} else {
		mdev->up_rtis |= 1 << rtin;
		timeout = 0;
	}
	bc = mdev->bc;

	pk_ok = 0;
	if ((ISRC_GOOD_BITS & isrc) && ((ISRC_BAD_BITS & isrc) == 0))
		pk_ok = 1;

	/* Read the current item from the tx_queue to find the client. */
	/* This was initiated by the client that will receive the result */

	tx_queue = mdev->tx_queue;
	spin_lock_irqsave(&tx_queue->lock,flags);
	rp = &tx_queue->rp;
	wp = &tx_queue->wp;
	if (get_queue_size(*rp,*wp,QSZ) == 0) {
		/* Queue empty or crap in packet, keep isrc for debug */
		wa.isrdebug = isrc;
		mdev->busy_done = BC_DONE;
		spin_unlock_irqrestore(&tx_queue->lock,flags);
		return IRQ_HANDLED;
	}
	tx_item = &(tx_queue->tx_item[*rp]); /* Get item at read pointer */
	get_next_rp(rp,*wp,QSZ);             /* and set rp to the next item */
	if (!pk_ok || timeout) {
		mdev->up_rtis &= ~(1 << tx_item->rti_number);
		wa.isrdebug = isrc;
		mdev->busy_done = BC_DONE;
		spin_unlock_irqrestore(&tx_queue->lock,flags);
		return IRQ_HANDLED;
	}
	spin_unlock_irqrestore(&tx_queue->lock,flags);

	if (!tx_item->no_reply) {            /* Client wants reply ? */

		/* Write RTI data to the initiating clients queue */

		client = tx_item->client;
		rx_queue = &(client->rx_queue);
		spin_lock_irqsave(&rx_queue->lock,flags);
		rp = &rx_queue->rp;
		wp = &rx_queue->wp;
		client->icnt++;

		if (get_queue_size(*rp,*wp,QSZ) < QSZ) {
			rti_interrupt = &(client->rx_queue.rti_interrupt[*wp]);
			rti_interrupt->rti_number = rtin;
			rti_interrupt->wc = (isrc & ISRC_WC_MASK) >> ISRC_WC_SHIFT;
			rti_interrupt->bc = bc;

			/* Remember rxbuf is accessed as u32 but wc is the u16 count */
			/* Word order is little endian */
			lregp = (uint32_t *) memory_map->rxbuf;
			for (i=0; i<(rti_interrupt->wc + 2)/2 ; i++) {  /* Prepended status */
			       lreg  = ioread32be(&lregp[i]);
			       rti_interrupt->rxbuf[i*2 + 1] = lreg >> 16;
			       rti_interrupt->rxbuf[i*2 + 0] = lreg & 0xFFFF;
			}
		}

		get_next_wp(*rp,wp,QSZ);
		spin_unlock_irqrestore(&rx_queue->lock,flags);

		if (tx_item->pk_type & client->pk_type)	/* Wake only demanded types */
			wake_up(&client->wait_queue);
	}

	/** Remember that some items can have both START and END set */ 
	if (tx_item->pk_type & TX_START)         /** Start packet stream */
		mdev->busy_done = BC_BUSY;       /** Set Transaction start */

	if (tx_item->pk_type & TX_END)           /** End packet stream */
		mdev->busy_done = BC_DONE;       /** Transaction done */

	return IRQ_HANDLED;
}

/**
 * =========================================================
 * @brief           Add the next mil1553 pci device
 * @param  pcur     Previous added device or NULL
 * @param  mdev     Device context
 * @return          Pointer to pci_device structure or NULL
 */

#define BAR2 2
#define PCICR 1
#define MEM_SPACE_ACCESS 2

struct pci_dev *add_next_dev(struct pci_dev *pcur,
			     struct mil1553_device_s *mdev)
{
	struct pci_dev *pprev = pcur;
	int cc, len;
	char bar_name[32];

	pcur = pci_get_device(VID_CERN, DID_MIL1553, pprev);
	if (!pcur)
		return NULL;

	mdev->pci_bus_num = pcur->bus->number;
	mdev->pci_slt_num = PCI_SLOT(pcur->devfn);
	cc = pci_enable_device(pcur);
	printk("mil1553:VID:0x%X DID:0x%X BUS:%d SLOT:%d",
	       VID_CERN,
	       DID_MIL1553,
	       mdev->pci_bus_num,
	       mdev->pci_slt_num);
	if (cc) {
		printk(" pci_enable:ERROR:%d",cc);
		return NULL;
	} else
		printk(" Enabled:OK\n");

	/*
	 * Map BAR2 the CBMIA FPGA (Its BIG endian !!)
	 */

	sprintf(bar_name,"mil1553.bc.%d.bar.%d",mdev->bc,BAR2);
	len = pci_resource_len(pcur, BAR2);
	cc = pci_request_region(pcur, BAR2, bar_name);
	if (cc) {
		pci_disable_device(pcur);
		printk("mil1553:pci_request_region:len:0x%x:%s:ERROR:%d\n",len,bar_name,cc);
		return NULL;

	}
	mdev->memory_map = (struct memory_map_s *) pci_iomap(pcur,BAR2,len);

	/*
	 * Configure interrupt handler
	 */

	cc = request_irq(pcur->irq, mil1553_isr, IRQF_SHARED, "MIL1553", mdev);
	if (cc) {
		pci_disable_device(pcur);
		printk("mil1553:request_irq:ERROR%d\n",cc);
		return NULL;
	}

	printk("mil1553:Device Bus:%d Slot:%d INSTALLED:OK\n",
	       mdev->pci_bus_num,
	       mdev->pci_slt_num);
	return pcur;
}

/**
 * =========================================================
 * @brief           Release a PCI device
 * @param dev       PCI device handle
 */

void release_device(struct mil1553_device_s *mdev)
{

	if (mdev->pdev) {
		pci_iounmap(mdev->pdev, (void *) mdev->memory_map);
		pci_release_region(mdev->pdev, BAR2);
		pci_disable_device(mdev->pdev);
		pci_dev_put(mdev->pdev);
		free_irq(mdev->pdev->irq,mdev);
		printk("mil1553:BC:%d RELEASED DEVICE:OK\n",mdev->bc);
	}
}

/**
 * Initialize device hardware:
 * Clear interrupt.
 * Enable interrupt.
 * Read 64-bit serial number.
 * Set transaction done
 */

static void init_device(struct mil1553_device_s *mdev)
{

	struct memory_map_s *memory_map = mdev->memory_map;

	ioread32be(&memory_map->isrc);
	iowrite32be(INTEN,&memory_map->inten);

	mdev->snum_h = ioread32be(&memory_map->snum_h);
	mdev->snum_l = ioread32be(&memory_map->snum_l);

	mdev->busy_done = BC_DONE; /** End transaction */
}

/**
 * =========================================================
 * @brief           Get an unused BC number
 * @return          A lun number or zero if none available
 */

static int used_bcs = 1;

int get_unused_bc(void)
{

	int i, bc, bit;

	for (i=0; i<bc_num; i++) {
		bc = bcs[i];
		bit = 1 << bc;
		used_bcs |= bit;
	}

	for (i=1; i<MAX_DEVS; i++) {
		bit = 1 << i;
		if (used_bcs & bit)
			continue;
		used_bcs |= bit;
		return i;
	}
	return 0;
}

/**
 * =========================================================
 * Open
 * Allocate a client context and initialize it
 * Place pointer to client in the file private data pointer
 */

int mil1553_open(struct inode *inode, struct file *filp)
{

	struct client_s *client;

	client = kmalloc(sizeof(struct client_s),GFP_KERNEL);
	if (client == NULL)
		return -ENOMEM;

	memset(client,0,sizeof(struct client_s));

	init_waitqueue_head(&client->wait_queue);
	client->timeout = msecs_to_jiffies(RTI_TIMEOUT);
	spin_lock_init(&client->rx_queue.lock);

	filp->private_data = client;
	return 0;
}

/**
 * =========================================================
 * Close
 */

int mil1553_close(struct inode *inode, struct file *filp)
{

	struct client_s         *client;
	struct mil1553_device_s *mdev;
	int                      bc;

	client = (struct client_s *) filp->private_data;
	if (client) {
		bc = client->bc_locked;
		if (bc) {
			mdev = get_dev(bc);
			if (mdev)
				mutex_unlock(&mdev->bc_lock);
		}
		kfree(client);
		filp->private_data = NULL;
	}
	return 0;
}

/**
 * =========================================================
 * Ioctl
 */

int mil1553_ioctl(struct inode *inode, struct file *filp,
		  unsigned int cmd, unsigned long arg)
{

	void *mem, *buf; /* Io memory */
	int iodr;        /* Io Direction */
	int iosz;        /* Io Size in bytes */
	int ionr;        /* Io Number */

	struct memory_map_s *memory_map;

	int bc, cc = 0;
	uint32_t *wp, *rp;
	unsigned int cnt, blen;

	uint32_t reg, tp;

	unsigned long *ularg, flags;

	struct mil1553_riob_s       *riob;
	struct mil1553_device_s     *mdev;
	struct mil1553_send_s       *msend;
	struct mil1553_recv_s       *mrecv;
	struct mil1553_dev_info_s   *dev_info;

	struct rx_queue_s *rx_queue;
	struct client_s   *client = (struct client_s *) filp->private_data;

	ionr = _IOC_NR(cmd);
	iodr = _IOC_DIR(cmd);
	iosz = _IOC_SIZE(cmd);

	if ((ionr >= mil1553LAST) || (ionr <= mil1553FIRST))
		return -ENOTTY;

	if ((mem = kmalloc(iosz,GFP_KERNEL)) == NULL)
		return -ENOMEM;

	if (iodr & _IOC_WRITE) {
		cc = copy_from_user(mem, (char *) arg, iosz);
		if (cc)
			goto error_exit;
	}

	debug_ioctl(client->debug_level,ionr,iosz,iodr,mem,BEFORE);

	ularg = mem;

	switch (ionr) {

		case mil1553SET_POLLING:

			wa.nopol = *ularg;
		break;

		case mil1553GET_POLLING:

			*ularg = wa.nopol;
		break;

		case mil1553GET_DEBUG_LEVEL:   /** Get the debug level 0..7 */

			*ularg = client->debug_level;
		break;

		case mil1553SET_DEBUG_LEVEL:   /** Set the debug level 0..7 */

			client->debug_level = *ularg;
		break;

		case mil1553GET_TIMEOUT_MSEC:  /** Get the client timeout in milliseconds */

			*ularg = jiffies_to_msecs(client->timeout);
		break;

		case mil1553SET_TIMEOUT_MSEC:  /** Set the client timeout in milliseconds */

			client->timeout = msecs_to_jiffies(*ularg);
		break;

		case mil1553GET_DRV_VERSION:   /** Get the UTC driver compilation data */

			*ularg = COMPILE_TIME;
		break;

		case mil1553GET_STATUS:        /** Reads the status register */

			bc = *ularg;
			mdev = get_dev(bc);
			if (!mdev) {
				cc = -EFAULT;
				goto error_exit;
			}
			memory_map = mdev->memory_map;
			reg = ioread32be(&memory_map->hstat);
			*ularg = (reg & HSTAT_STAT_MASK) >> HSTAT_STAT_SHIFT;
		break;

		case mil1553RESET:             /** Reads the status register */

			bc = *ularg;
			mdev = get_dev(bc);
			if (!mdev) {
				cc = -EFAULT;
				goto error_exit;
			}
			memory_map = mdev->memory_map;
			iowrite32be(CMD_RESET,&memory_map->cmd);
			init_device(mdev);
			reset_tx_queue(mdev);
			mdev->up_rtis = 0;
			wa.isrdebug = 0;
		break;

		case mil1553GET_TEMPERATURE:

			bc = *ularg;
			mdev = get_dev(bc);
			if (!mdev) {
				cc = -EFAULT;
				goto error_exit;
			}
			memory_map = mdev->memory_map;
			*ularg = ioread32be(&memory_map->temp);
		break;

		case mil1553SET_TP:
			bc = *ularg & MAX_DEVS_MASK;
			tp = *ularg & CMD_TPS_MASK;
			mdev = get_dev(bc);
			if (!mdev) {
				cc = -EFAULT;
				goto error_exit;
			}
			memory_map = mdev->memory_map;
			iowrite32be(tp,&memory_map->cmd);
		break;

		case mil1553GET_TP:
			bc = *ularg & MAX_DEVS_MASK;
			mdev = get_dev(bc);
			if (!mdev) {
				cc = -EFAULT;
				goto error_exit;
			}
			memory_map = mdev->memory_map;
			*ularg = ioread32be(&memory_map->cmd) & (CMD_TPS_MASK | bc);
		break;

		case mil1553GET_BCS_COUNT:     /** Get the Bus Controllers count */

			*ularg = wa.bcs;
		break;

		case mil1553GET_BC_INFO:       /** Get information aboult a Bus Controller */

			dev_info = mem;
			bc = dev_info->bc;
			mdev = get_dev(bc);
			if (!mdev) {
				cc = -EFAULT;
				goto error_exit;
			}
			memory_map = mdev->memory_map;
			dev_info->pci_bus_num = mdev->pci_bus_num;
			dev_info->pci_slt_num = mdev->pci_slt_num;
			dev_info->snum_h = mdev->snum_h;
			dev_info->snum_l = mdev->snum_l;

			reg = ioread32be(&memory_map->hstat);
			dev_info->hardware_ver_num = (reg & HSTAT_VER_MASK) >> HSTAT_VER_SHIFT;

			dev_info->tx_frames         = ioread32be(&memory_map->tx_frames);
			dev_info->rx_frames         = ioread32be(&memory_map->rx_frames);
			dev_info->rx_errors         = ioread32be(&memory_map->rx_errors);
			dev_info->timeouts          = ioread32be(&memory_map->timeouts);
			dev_info->parity_errors     = ioread32be(&memory_map->parity_errors);
			dev_info->manchester_errors = ioread32be(&memory_map->manchester_errors);
			dev_info->wc_errors         = ioread32be(&memory_map->wc_errors);
			dev_info->tx_clash_errors   = ioread32be(&memory_map->tx_clash_errors);
			dev_info->nb_wds            = ioread32be(&memory_map->nb_wds);
			dev_info->rti_timeouts      = ioread32be(&memory_map->rti_timeouts);

			dev_info->icnt = mdev->icnt;
			dev_info->tx_count = mdev->tx_count;
			dev_info->isrdebug = wa.isrdebug;
			dev_info->jifd = mdev->jifd;
			dev_info->quick_owned = atomic_read(&mdev->quick_owned);
			dev_info->quick_owner = mdev->quick_owner;
		break;

		case mil1553RAW_READ:          /** Raw read PCI registers */

			riob = mem;
			if (riob->regs > MAX_REGS) {
				cc = -EADDRNOTAVAIL;
				goto error_exit;
			}
			blen = riob->regs*sizeof(int);
			buf = kmalloc(blen,GFP_KERNEL);
			if (!buf) {
				kfree(mem);
				return -ENOMEM;
			}
			cnt = 0;
			bc = riob->bc;
			mdev = get_dev(bc);
			if (!mdev) {
				cc = -EFAULT;
				kfree(buf);
				goto error_exit;
			}
			cnt = raw_read(mdev, riob, buf);
			if (cnt)
				cc = copy_to_user(riob->buffer,
						  buf, blen);
			kfree(buf);
			if (!cnt || cc)
				goto error_exit;
		break;

		case mil1553RAW_WRITE:         /** Raw write PCI registers */

			riob = mem;
			if (riob->regs > MAX_REGS) {
				cc = -EADDRNOTAVAIL;
				goto error_exit;
			}
			blen = riob->regs*sizeof(int);
			buf = kmalloc(blen,GFP_KERNEL);
			if (!buf) {
				kfree(mem);
				return -ENOMEM;
			}
			cc = copy_from_user(buf, riob->buffer, blen);
			cnt = 0;
			bc = riob->bc;
			mdev = get_dev(bc);
			if (!mdev) {
				cc = -EFAULT;
				kfree(buf);
				goto error_exit;
			}
			cnt = raw_write(mdev, riob, buf);
			kfree(buf);
			if (!cnt || cc)
				goto error_exit;
		break;

		case mil1553GET_UP_RTIS:
			bc = *ularg;
			mdev = get_dev(bc);
			if (!mdev) {
				cc = -EFAULT;
				goto error_exit;
			}
			ping_rtis(mdev);
			*ularg = mdev->up_rtis;
		break;

		case mil1553SEND:
			msend = mem;
			blen = msend->item_count*sizeof(struct mil1553_tx_item_s);
			buf = kmalloc(blen,GFP_KERNEL);
			if (!buf) {
				kfree(mem);
				return -ENOMEM;
			}
			cc = copy_from_user(buf, msend->tx_item_array, blen);
			if (msend->item_count != 1) {
				printk(KERN_ERR "jdgc: warning: MIL1553_SEND called with item_count %d != 1\n", msend->item_count);
			}
			if (cc) {
				kfree(buf);
				if (client->debug_level > 4)
					printk("mil1553:SEND:copy_from_user:bytes_not_copied:%d\n",cc);
				goto error_exit;
			}
			cc = send_items(client,
					msend->item_count,
					(struct mil1553_tx_item_s *) buf);
			kfree(buf);
			if (cc <= 0) {
				if (client->debug_level > 4)
					printk("mil1553:SEND:send_items:returned:%d\n",cc);
				goto error_exit;
			}
		break;

		case mil1553QUEUE_SIZE:
			rx_queue = &client->rx_queue;
			wp = &rx_queue->wp;
			rp = &rx_queue->rp;
			spin_lock_irqsave(&rx_queue->lock,flags);
			*ularg = get_queue_size(*rp,*wp,QSZ);
			spin_unlock_irqrestore(&rx_queue->lock,flags);
			if (*ularg > 1)
				printk(KERN_ERR "jdgc: warning: client queue > 1 element\n");
		break;

		case mil1553RECV:
			mrecv = mem;
			cc = read_queue(client,mrecv);
			if (cc) {
				if (client->debug_level > 4)
					printk("mil1553:RECV:read_queue:returned:%d\n",cc);

				/** Clean up after a receive error */

				mdev = get_dev(client->bc);
				if (mdev) {
					init_device(mdev);
					reset_tx_queue(mdev);
				}
				goto error_exit;
			}
		break;

		case mil1553LOCK_BC:
			return 0;
			bc = *ularg;
			mdev = get_dev(bc);
			if (!mdev) {
				cc = -EFAULT;
				goto error_exit;
			}
			if (client->bc_locked != 0) {
				cc = -EDEADLK;
				goto error_exit;
			}
			while (atomic_xchg(&mdev->quick_owned, 1)) {
				cc = wait_event_interruptible_timeout(mdev->quick_wq,
							atomic_read(&mdev->quick_owned) == 0,
							msecs_to_jiffies(5000));
				if (cc == 0)
					printk(KERN_ERR "jdgc: could not get lock of BC %d owned by pid %d\n",
						mdev->bc, mdev->quick_owner);
				if (signal_pending(current))
					return -ERESTARTSYS;
				return -EDEADLK;

			}
			mdev->quick_owner = current->pid;
			client->bc_locked = bc;
			return 0;
		break;

		case mil1553UNLOCK_BC:
			return 0;
			bc = *ularg;
			mdev = get_dev(bc);
			if (!mdev) {
				cc = -EFAULT;
				goto error_exit;
			}
			if (client->bc_locked != bc) {
				cc = -ENOLCK;
				goto error_exit;
			}
			client->bc_locked = 0;
			mdev->quick_owner = 0;
			BUG_ON(atomic_xchg(&mdev->quick_owned, 0) == 0);
			wake_up(&mdev->quick_wq);
			return 0;
		break;

		default:
			goto error_exit;
	}

	debug_ioctl(client->debug_level,ionr,iosz,iodr,mem,AFTER);

	if (iodr & _IOC_READ) {
		cc = copy_to_user((char *) arg, mem, iosz);
		if (cc)
			goto error_exit;
	}

	kfree(mem);
	return 0;

error_exit:
	kfree(mem);

	if ((client) && (client->debug_level > 4))
		printk("mil1553:Ioctl:%d:ErrorExit:%d\n",ionr,cc);

	if (cc < 0)
		return cc;
	return -EACCES;
}

/**
 * =========================================================
 */

long mil1553_ioctl_ulck(struct file *filp, unsigned int cmd, unsigned long arg)
{
	int res;

	res = mil1553_ioctl(filp->f_dentry->d_inode, filp, cmd, arg);
	return res;
}

/**
 * =========================================================
 */

int mil1553_ioctl_lck(struct inode *inode, struct file *filp,
		      unsigned int cmd, unsigned long arg)
{
	int res;

	res = mil1553_ioctl(inode, filp, cmd, arg);
	return res;
}

/**
 * =========================================================
 * Uninstall the driver
 */

void mil1553_uninstall(void)
{
	int i;
	struct mil1553_device_s *mdev;

	for (i=0; i<wa.bcs; i++) {
		mdev = &wa.mil1553_dev[i];
		release_device(mdev);
	}
	unregister_chrdev(mil1553_major,mil1553_major_name);
	printk("mil1553:Driver uninstalled\n");
}

/**
 * =========================================================
 */
struct file_operations mil1553_fops = {
	.owner          = THIS_MODULE,
	.ioctl          = mil1553_ioctl_lck,
	.unlocked_ioctl = mil1553_ioctl_ulck,
	.open           = mil1553_open,
	.release        = mil1553_close,
};

/**
 * =========================================================
 * Installer, hunt down modules and install them
 */

int mil1553_install(void)
{
	int cc, i, bc = 0;
	struct pci_dev *pdev = NULL;
	struct mil1553_device_s *mdev;

	memset(&wa, 0, sizeof(struct working_area_s));

	if (check_args()) {

		cc = register_chrdev(mil1553_major, mil1553_major_name, &mil1553_fops);
		if (cc < 0)
			return cc;
		if (mil1553_major == 0)
			mil1553_major = cc; /* dynamic */

		for (i=0; i<MAX_DEVS; i++) {
			mdev = &wa.mil1553_dev[i];
			spin_lock_init(&mdev->lock);
			mdev->tx_queue = &wa.tx_queue[i];
			spin_lock_init(&mdev->tx_queue->lock);
			mutex_init(&mdev->bc_lock);

			mdev->pdev = add_next_dev(pdev,mdev);
			if (!mdev->pdev)
				break;

			bc = hunt_bc(mdev->pci_bus_num,mdev->pci_slt_num);
			printk("mil1553:Hunt:Bus:%d Slot:%d => ",
			       mdev->pci_bus_num,
			       mdev->pci_slt_num);

			if (bc) {
				printk("Found declared BC:%d\n",bc);
			} else {
				bc = get_unused_bc();
				printk("Assigned unused BC:%d\n",bc);
			}

			mdev->bc = bc;
			iowrite32be(CMD_RESET, &mdev->memory_map->cmd);
			init_device(mdev);
			init_waitqueue_head(&mdev->int_complete);
			init_waitqueue_head(&mdev->quick_wq);
			atomic_set(&mdev->busy, 0);
			atomic_set(&mdev->quick_owned, 0);
			mdev->quick_owner = 0;
			mdev->jifd = 0;
			mutex_init(&mdev->tx_attempt);
			mutex_init(&mdev->mutex);
			ping_rtis(mdev);
			printk("BC:%d SerialNumber:0x%08X%08X\n",
				bc,mdev->snum_h,mdev->snum_l);
			pdev = mdev->pdev;
			wa.bcs++;
		}
	}
	printk("mil1553:Installed:%d Bus controllers\n",wa.bcs);
	return 0;
}


module_init(mil1553_install);
module_exit(mil1553_uninstall);
