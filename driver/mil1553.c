/*
 * =========================================================
 * Mil1553 driver
 * This driver is a complete rewrite of a previous version from Yuri
 * BE/CO/HT Julian Lewis Tue 15th Feb 2011
 */

#include <linux/kernel.h>
#include <linux/fcntl.h>
#include <linux/errno.h>
#include <linux/spinlock.h>
#include <linux/fs.h>
#include <linux/types.h>
#include <linux/module.h>
#include <linux/version.h>
#include <linux/pci.h>
#include <linux/uaccess.h>
#include <linux/types.h>
#include <linux/interrupt.h>
#include <linux/irqreturn.h>
#include <linux/wait.h>

#include <mil1553.h>
#include <mil1553P.h>

static int   mil1553_major      = 0;
static char *mil1553_major_name = "mil1553";

MODULE_AUTHOR("Julian Lewis BE/CO/HT CERN");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("MIL1553 Driver");
MODULE_SUPPORTED_DEVICE("CBMIA PCI module");

#ifndef COMPILE_TIME
#define COMPILE_TIME 0
#endif

/*
 * Driver routines
 */

int  mil1553_open(struct inode *inode, struct file *filp);
int  mil1553_close(struct inode *inode, struct file *filp);

int  mil1553_ioctl(struct inode *inode,
		   struct file *filp,
		   unsigned int cmd,
		   unsigned long arg);

void mil1553_uninstall(void);
int  mil1553_install(void);

long mil1553_ioctl_ulck(struct file *filp,
			unsigned int cmd,
			unsigned long arg);

int  mil1553_ioctl_lck(struct inode *inode,
		       struct file *filp,
		       unsigned int cmd,
		       unsigned long arg);

struct file_operations mil1553_fops = {
	.owner          = THIS_MODULE,
	.ioctl          = mil1553_ioctl_lck,
	.unlocked_ioctl = mil1553_ioctl_ulck,
	.open           = mil1553_open,
	.release        = mil1553_close,
};

/**
 * Drivers static working area
 */

struct working_area_s wa;

/**
 * =========================================================
 * Debug ioctl calls
 */

char *ioctl_names[mil1553IOCTL_FUNCTIONS] = {
	"GET_DEBUG_LEVEL",
	"SET_DEBUG_LEVEL",

	"GET_TIMEOUT_MSEC",
	"SET_TIMEOUT_MSEC",

	"GET_DRV_VERSION",

	"GET_STATUS",
	"SET_BUS_SPEED",

	"GET_BCS_COUNT",
	"GET_BC_INFO",

	"RAW_READ",
	"RAW_WRITE",

	"GET_UP_RTIS",
	"SEND",
	"RECV"
};

/**
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

		printk("mil1553:ioctl:ionr:%d[%s] iosz:%d iodr:%d[",ionr,ioctl_names[cindx],iosz,iodr);
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
		      void *buf) {

	int i;
	uint32_t *uip, *hip;

	uip = buf;
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
 * @brief      Get the up_rtis for a given BC
 * @param mdev Mill1553 device
 * @return     Mask of up RTIs
 *
 * This routine should never be run while packets are being processed
 * I.E The queue tx_size should be empty, otherwise it disrupts the bus
 */

#define TRIES 500
#define BETWEEN_TRIES_US 200
#define POLLING_OFF (CMD_POLL_OFF << CMD_POLL_OFF_SHIFT)

static uint32_t _get_up_rtis(struct mil1553_device_s *mdev)
{
	struct memory_map_s *memory_map;
	uint32_t cmd;
	uint32_t up_rtis = 0; /* online RTI's */
	int i;

	/* Do nothing if queue is not empty */

	if (get_queue_size(mdev->tx_queue->rp,mdev->tx_queue->wp,QSZ) > 0)
		return mdev->up_rtis;

	/* Interrupts off */

	memory_map = mdev->memory_map;
	iowrite32be(0,&memory_map->inten);

	/* Remember cmd register and switch on hardware RTI polling */

	cmd = ioread32be(&memory_map->cmd);
	iowrite32be(0,&memory_map->cmd);

/**
 * Note about this next loop:
 * The hardware polling is too fast for the RTI cards, so the returned bit
 * pattern of live RTIs may be incomplete. By reading many times the idea is
 * that sooner or later every live RTI will have had is bit read at least once.
 * This frig masks an underlying hardware error.
 */

	for (i=0; i <TRIES; i++) {
		up_rtis |= ioread32be(&memory_map->up_rtis);
		udelay(BETWEEN_TRIES_US);
	}

	/* Switch off hardware RTI polling and restore cmd register */

	cmd |= POLLING_OFF;
	iowrite32be(cmd,&memory_map->cmd);

	/* Re enable interrupts */

	iowrite32be(INTEN_INF,&memory_map->inten);

	mdev->up_rtis = up_rtis;
	return up_rtis;
}

static uint32_t get_up_rtis(struct mil1553_device_s *mdev)
{
	uint32_t res;
	spin_lock(&mdev->lock);
	res = _get_up_rtis(mdev);
	spin_unlock(&mdev->lock);
	return res;
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
	if (!up_rtis)
		up_rtis = get_up_rtis(mdev);
	mask = 1 << rtin;
	if (mask & up_rtis)
		return 1;
	return 0;
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
		mdev->busy_done = BC_DONE;                     /** All done */
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

	wc = (tx_item->txreg & TXREG_WC_MASK) >> TXREG_WC_SHIFT;

	lregp = (uint32_t *) memory_map->txbuf;
	for (i=0; i<(wc + 1)/2; i++) {
		lreg  =  tx_item->txbuf[i*2 + 1] << 16;
		lreg |= (tx_item->txbuf[i*2 + 0] & 0xFFFF);
		iowrite32be(lreg,&lregp[i]);
	}

	/* Issue the start command, we get an interrupt when done */

	iowrite32be(tx_item->txreg,&memory_map->txreg);           /* Start */
}

/**
 * Called only from IOCTL ie from user space
 */

static void start_tx(int debug_level,
		     struct mil1553_device_s *mdev)
{
	spin_lock(&mdev->lock);
	if (mdev->busy_done == BC_DONE)       /** If busy do nothing */
		_start_tx(debug_level,mdev);
	spin_unlock(&mdev->lock);
}

/**
 * =========================================================
 * @brief Find the start item for a BC
 * @param item_count is the number of items in the array
 * @param item_array is an array to execute on RTIs for multiple bus controllers
 * @return index of first item in array for the given BC or -1 if not found
 *
 * Contiguous items for each BC are marked with start..all..end and
 * form a transaction.
 *
 * Users can be woken up in three ways..
 * (1) For the Start item in the transaction.
 * (2) For All items in the transaction.
 * (3) For the Last item in the transaction.
 *
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
 * @return index of first item in array for the given BC or -1 if not found
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
 * ready and the ISR will itself will pull the next item off the queue and start it.
 */

static int send_items(struct client_s *client,
		      unsigned int item_count,
		      struct mil1553_tx_item_s *item_array)
{

	unsigned long flags;
	int i, j, wc, bc, rtin=1, res, strs[MAX_DEVS], ends[MAX_DEVS];
	struct tx_item_s tx_item;
	uint32_t *rp, *wp;
	struct mil1553_device_s *mdev = NULL;
	struct tx_queue_s *tx_queue;

	if (client->debug_level > 2)
		printk("mil1553:send_items:item_count:%d\n",item_count);


	if (item_count <=0)
		return 0;

	/* Check the RTIs are up */

	for (i=0; i<item_count; i++) {
		bc = item_array[i].bc;
		if (bc >= wa.bcs) {
			if (client->debug_level > 2)
				printk("mil1553:send_items:No such BC:Error:Bc:%02d\n",rtin);
			return -ENODEV;
		}

		rtin = item_array[i].rti_number;
		mdev = &(wa.mil1553_dev[bc]);
		if (check_rti_up(mdev,rtin) == 0) {
			if (client->debug_level > 2)
				printk("mil1553:send_items:CheckRtis:Error:Bc:%02d Rti:%02d Down\n",bc,rtin);
			return -ENODEV;
		}
	}

	/* For each installed BC get the start and end item numbers */
	/* that will deliniate a transaction. */

	for (i=0; i<wa.bcs; i++) {
		strs[i] = find_start(i,item_count,item_array);
		ends[i] = find_end(i,item_count,item_array);
	}

	res = 0;
	for (i=0; i<item_count; i++) {

		/* Build a tx_item for this array entry */

		memset(&tx_item,0,sizeof(struct tx_item_s));
		tx_item.client = client;
		bc = item_array[i].bc;
		tx_item.bc = bc;
		rtin = item_array[i].rti_number;
		tx_item.rti_number = rtin;
		tx_item.txreg = item_array[i].txreg;
		tx_item.no_reply = item_array[i].no_reply;

		wc = (tx_item.txreg & TXREG_WC_MASK) >> TXREG_WC_SHIFT;
		for (j=0; j<wc; j++)
			tx_item.txbuf[j] = item_array[i].txbuf[j];

		/* Users can be interrupted on ALL, START and END items in the transaction */

		tx_item.pk_type = TX_ALL;
		if (strs[bc] == i)
			tx_item.pk_type |= TX_START;
		if (ends[bc] == i)
			tx_item.pk_type |= TX_END;

		/* Get the target tx_queue for the bus controller */

		mdev = &(wa.mil1553_dev[bc]);
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

	for (bc=0; bc<wa.bcs; bc++) {
		if (strs[bc] >= 0) {
			start_tx(client->debug_level,mdev);
			if (client->debug_level > 3)
				printk("mil1553:send_items:Bc:%d start_tx\n",bc);
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

int read_queue(struct client_s *client, struct mil1553_recv_s *mrecv)
{
	unsigned long flags;
	int i, cc, res, wc;
	struct rx_queue_s *rx_queue;
	struct rti_interrupt_s *rti_interrupt;
	uint32_t *wp, *rp, icnt;

	client->pk_type = mrecv->pk_type;
	client->timeout = mrecv->timeout;

	do {
		res = 0;
		rx_queue = &client->rx_queue;
		spin_lock_irqsave(&rx_queue->lock,flags);
		wp = &rx_queue->wp;
		rp = &rx_queue->rp;
		if (get_queue_size(*rp,*wp,QSZ) > 0) {

			rti_interrupt = &rx_queue->rti_interrupt[*rp];

			mrecv->interrupt.bc         = rti_interrupt->bc;
			mrecv->interrupt.rti_number = rti_interrupt->rti_number;
			mrecv->interrupt.wc         = rti_interrupt->wc;
			mrecv->icnt                 = client->icnt;
			mrecv->pk_type              = client->pk_type;

			wc = rti_interrupt->wc +1;
			if (wc > RX_BUF_SIZE) wc = RX_BUF_SIZE;

			for (i=0; i<wc; i++)
				mrecv->interrupt.rxbuf[i] = rti_interrupt->rxbuf[i];

			if (client->debug_level > 4) {
				printk("mil1553:read_queue:rp:%02d wp:%02d wc:%02d icnt:%02d bc:%02d rti:%02d\n",
				       *rp,*wp,
				       rti_interrupt->wc,
				       client->icnt,
				       rti_interrupt->bc,
				       rti_interrupt->rti_number);

				printk("mil1553:read_queue:rxbuf: 4 words ...\n");
				for (i=0; i<4; i++)
					printk("0x%04X ",rti_interrupt->rxbuf[i]);
				printk("\n");
			}

			get_next_rp(rp,*wp,QSZ);
			res = 1;
		}
		spin_unlock_irqrestore(&rx_queue->lock,flags);
		if (res)
			return 0;

		icnt = client->icnt;
		cc = wait_event_interruptible_timeout(client->wait_queue,
						     icnt != client->icnt,
						     client->timeout);
		if (cc == 0)
			return -ETIME;
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
	int i;

	wa.isrdebug = 0;
	wa.icnt++;

	memory_map = mdev->memory_map;
	isrc = ioread32be(&memory_map->isrc);   /** Read and clear the interrupt */
	if ((isrc & ISRC_IRQ) == 0)
		return IRQ_NONE;

	wa.isrdebug |= 0x1;

	bc = mdev->bc;

	/* Read the current item from the tx_queue to find the client. */
	/* This was initiated by the client that will receive the result */

	tx_queue = mdev->tx_queue;
	spin_lock_irqsave(&tx_queue->lock,flags);
	rp = &tx_queue->rp;
	wp = &tx_queue->wp;
	if (get_queue_size(*rp,*wp,QSZ) == 0) {
		mdev->busy_done = BC_DONE;
		spin_unlock_irqrestore(&tx_queue->lock,flags);

		wa.isrdebug |= 0x2;

		return IRQ_HANDLED;
	}
	tx_item = &(tx_queue->tx_item[*rp]); /* Get item at read pointer */
	get_next_rp(rp,*wp,QSZ);             /* and set rp to the next item */
	spin_unlock_irqrestore(&tx_queue->lock,flags);

	if (!tx_item->no_reply) {            /* Client wants reply ? */

		/* Write RTI data to the initiating clients queue */

		client = tx_item->client;
		rx_queue = &(client->rx_queue);
		spin_lock_irqsave(&rx_queue->lock,flags);
		rp = &rx_queue->rp;
		wp = &rx_queue->wp;
		client->icnt++;

		wa.isrdebug |= 0x4;

		if (get_queue_size(*rp,*wp,QSZ) < QSZ) {
			rti_interrupt = &(client->rx_queue.rti_interrupt[*wp]);
			rti_interrupt->rti_number =
				(isrc & ISRC_RTI_MASK) >> ISRC_RTI_SHIFT;
			rti_interrupt->wc = (isrc & ISRC_WC_MASK) >> ISRC_WC_SHIFT;
			rti_interrupt->bc = bc;

			/* Remember rxbuf is accessed as u32 but wc is the u16 count */
			/* Word order is little endian */

			lregp = (uint32_t *) memory_map->rxbuf;
			for (i=0; i<(rti_interrupt->wc + 1)/2 ; i++) {
			       lreg  = ioread32be(&lregp[i]);
			       rti_interrupt->rxbuf[i*2 + 1] = lreg >> 16;
			       rti_interrupt->rxbuf[i*2 + 0] = lreg & 0xFFFF;
			}

			wa.isrdebug |= 0x8;
		}
		get_next_wp(*rp,wp,QSZ);
		spin_unlock_irqrestore(&rx_queue->lock,flags);

		if (tx_item->pk_type & client->pk_type) { /* Wake only demanded types */
			wake_up(&client->wait_queue);

			wa.isrdebug |= 0x10;
		}
	}

	/** Remember that some items can have both START and END set */

	if (tx_item->pk_type & TX_START) {       /** Start packet stream */
		mdev->busy_done = BC_BUSY;       /** Device is busy */

		wa.isrdebug |= 0x20;
	}

	if (tx_item->pk_type & TX_END) {         /** End packet stream */
		mdev->busy_done = BC_DONE;       /** Device is done */

		wa.isrdebug |= 0x40;
	}

	_start_tx(0,mdev);                       /** Start next item */
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

	printk("mil1553:BC:%d INSTALLED:OK\n",mdev->bc);
	return pcur;
}

/**
 * =========================================================
 * @brief           Release a PCI device
 * @param dev       PCI device handle
 */

void release_device(struct mil1553_device_s *mdev) {

	pci_iounmap(mdev->pdev, (void *) mdev->memory_map);
	pci_release_region(mdev->pdev, BAR2);
	pci_disable_device(mdev->pdev);
	pci_dev_put(mdev->pdev);
	free_irq(mdev->pdev->irq,mdev);
	printk("mil1553:BC:%d RELEASED DEVICE:OK\n",mdev->bc);
}

/**
 * Initialize device hardware:
 * Clear interrupt.
 * Enable interrupt.
 * Select initial speed and polling off.
 * Read 64-bit serial number.
 * Get the up RTIs.
 */

#define INITIAL_SPEED (CMD_SPEED_1M << CMD_SPEED_SHIFT)

static void init_device(struct mil1553_device_s *mdev) {

	struct memory_map_s *memory_map = mdev->memory_map;
	uint32_t cmd;

	ioread32be(&memory_map->isrc);
	iowrite32be(INTEN_INF,&memory_map->inten);

	cmd = POLLING_OFF | INITIAL_SPEED;
	iowrite32be(cmd,&memory_map->cmd);

	mdev->snum_h = ioread32be(&memory_map->snum_h);
	mdev->snum_l = ioread32be(&memory_map->snum_l);

	get_up_rtis(mdev);
}

/**
 * =========================================================
 * Installer, hunt down modules and install them
 */

int mil1553_install(void)
{
	int cc, bc;
	struct pci_dev *pdev = NULL;
	struct mil1553_device_s *mdev;

	cc = register_chrdev(mil1553_major, mil1553_major_name, &mil1553_fops);
	if (cc < 0)
		return cc;
	if (mil1553_major == 0)
		mil1553_major = cc; /* dynamic */

	memset(&wa, 0, sizeof(struct working_area_s));

	for (bc=0; bc<MAX_DEVS; bc++) {
		mdev = &wa.mil1553_dev[bc];
		mdev->bc = bc;
		spin_lock_init(&mdev->lock);
		mdev->tx_queue = &wa.tx_queue[bc];
		spin_lock_init(&mdev->tx_queue->lock);

		mdev->pdev = add_next_dev(pdev,mdev);
		if (!mdev->pdev)
			break;
		init_device(mdev);
		printk("BC:%d SerialNumber:0x%08X%08X\n",
			bc,mdev->snum_h,mdev->snum_l);
		pdev = mdev->pdev;
		wa.bcs++;
	}
	printk("mil1553:Installed:%d Bus controllers\n",wa.bcs);
	return 0;
}

/**
 * =========================================================
 * Open
 * Allocate a client context and initialize it
 * Place pointer to client in the file private data pointer
 */

int mil1553_open(struct inode *inode, struct file *filp) {

	struct client_s *client;

	client = kmalloc(sizeof(struct client_s),GFP_KERNEL);
	if (client == NULL)
		return -ENOMEM;

	memset(client,0,sizeof(struct client_s));

	init_waitqueue_head(&client->wait_queue);
	client->timeout = msecs_to_jiffies(TIMEOUT);
	spin_lock_init(&client->rx_queue.lock);

	filp->private_data = client;
	return 0;
}

/**
 * =========================================================
 * Close
 */

int mil1553_close(struct inode *inode, struct file *filp) {

	struct client_s *client;

	client = (struct client_s *) filp->private_data;
	if (client)
		kfree(client);
	filp->private_data = NULL;
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

	int cc = 0;
	unsigned int cnt, blen, bc;

	uint32_t reg;

	unsigned long *ularg;

	struct mil1553_riob_s       *riob;
	struct mil1553_device_s     *mdev;
	struct mil1553_send_s       *msend;
	struct mil1553_recv_s       *mrecv;
	struct mil1553_bus_speed_s  *bus_speed;
	struct mil1553_dev_info_s   *dev_info;

	struct client_s *client = (struct client_s *) filp->private_data;

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
			if (bc >= wa.bcs) {
				cc = -ENODEV;
				goto error_exit;
			}
			mdev = &wa.mil1553_dev[bc];
			memory_map = mdev->memory_map;
			reg = ioread32be(&memory_map->hstat);
			*ularg = (reg & HSTAT_STAT_MASK) >> HSTAT_STAT_SHIFT;
		break;

		case mil1553SET_BUS_SPEED:     /** Set the bus speed */

			bus_speed = mem;
			bc = bus_speed->bc;
			if (bc >= wa.bcs) {
				cc = -ENODEV;
				goto error_exit;
			}
			mdev = &wa.mil1553_dev[bc];
			memory_map = mdev->memory_map;
			reg = ((bus_speed->speed << CMD_SPEED_SHIFT) & CMD_SPEED_MASK) | CMD_POLL_OFF;
			iowrite32be(reg,&memory_map->cmd);
		break;

		case mil1553GET_BCS_COUNT:     /** Get the Bus Controllers count */

			*ularg = wa.bcs;
		break;

		case mil1553GET_BC_INFO:       /** Get information aboult a Bus Controller */

			dev_info = mem;
			bc = dev_info->bc;
			if (bc >= wa.bcs) {
				cc = -ENODEV;
				goto error_exit;
			}
			mdev = &wa.mil1553_dev[bc];
			memory_map = mdev->memory_map;
			dev_info->pci_bus_num = mdev->pci_bus_num;
			dev_info->pci_slt_num = mdev->pci_slt_num;
			dev_info->snum_h = mdev->snum_h;
			dev_info->snum_l = mdev->snum_l;

			reg = ioread32be(&memory_map->hstat);
			dev_info->hardware_ver_num = (reg & HSTAT_VER_MASK) >> HSTAT_VER_SHIFT;

			reg = ioread32be(&memory_map->cmd);
			dev_info->speed = (reg & CMD_SPEED_MASK) >> CMD_SPEED_SHIFT;
			dev_info->icnt = wa.icnt;
			dev_info->isrdebug = wa.isrdebug;
		break;

		case mil1553RAW_READ:          /** Raw read PCI registers */

			riob = mem;
			if ((riob->regs > MAX_REGS) || (riob->bc >= wa.bcs)) {
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
			if (riob->bc < MAX_DEVS) {
				cnt = raw_read(&wa.mil1553_dev[riob->bc],
					       riob, buf);
				if (cnt)
					cc = copy_to_user(riob->buffer,
							  buf, blen);
			}
			kfree(buf);
			if (!cnt || cc)
				goto error_exit;
		break;

		case mil1553RAW_WRITE:         /** Raw write PCI registers */

			riob = mem;
			if ((riob->regs > MAX_REGS) || (riob->bc >= wa.bcs)) {
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
			if (riob->bc < MAX_DEVS) {
				cnt = raw_write(&wa.mil1553_dev[riob->bc],
						riob, buf);
			}
			kfree(buf);
			if (!cnt || cc)
				goto error_exit;
		break;

		case mil1553GET_UP_RTIS:
			bc = *ularg;
			if (bc >= MAX_DEVS) {
				cc = -ENODEV;
				goto error_exit;
			}
			mdev = &wa.mil1553_dev[bc];
			*ularg = get_up_rtis(mdev);
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
			if (cc) {
				kfree(buf);
				goto error_exit;
			}
			cc = send_items(client,
					msend->item_count,
					msend->tx_item_array);
			kfree(buf);
			if (cc <= 0)
				goto error_exit;
		break;

		case mil1553RECV:
			mrecv = mem;
			cc = read_queue(client,mrecv);
			if (cc)
				goto error_exit;
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

long mil1553_ioctl_ulck(struct file *filp, unsigned int cmd, unsigned long arg) {
	int res;

	res = mil1553_ioctl(filp->f_dentry->d_inode, filp, cmd, arg);
	return res;
}

/**
 * =========================================================
 */

int mil1553_ioctl_lck(struct inode *inode, struct file *filp,
		      unsigned int cmd, unsigned long arg) {
	int res;

	res = mil1553_ioctl(inode, filp, cmd, arg);
	return res;
}

/**
 * =========================================================
 * Uninstall the driver
 */

void mil1553_uninstall(void) {
	int bc;
	struct mil1553_device_s *mdev;

	for (bc=0; bc<wa.bcs; bc++) {
		mdev = &wa.mil1553_dev[bc];
		release_device(mdev);
	}
	unregister_chrdev(mil1553_major,mil1553_major_name);
	printk("mil1553:Driver uninstalled\n");
}

/**
 * =========================================================
 */

module_init(mil1553_install);
module_exit(mil1553_uninstall);
