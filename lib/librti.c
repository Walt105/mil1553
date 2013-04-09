#include <mil1553.h>
#include <librti.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <stdio.h>
#include <errno.h>
#include <sys/ioctl.h>

#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/sem.h>

#define DEBUG

/* ===================================== */

#define TYPEDESC_LEN 30
struct rti_type_s {

	char typedesc[TYPEDESC_LEN];
	unsigned short sigl;
	unsigned short sigh;
};

#define RTI_TYPES 9

struct rti_type_s rti_types[RTI_TYPES] = {

	{ "CMIG Interface G64 3h",     0xFFFF, 0xFFFF },
	{ "CMIH Interface G64 6h",     0xFFFF, 0xFFFF },
	{ "CMIM Interface MICENE",     0xFFFF, 0xFFFF },
	{ "CMIG PS G64 Interface",     0xFFFD, 0xFFFD },
	{ "CMMX MPX Timing",           0xFFA0, 0xFFA7 },
	{ "CMMT MPX Timing",           0xFFA8, 0xFFAF },
	{ "CMCG Command Response G64", 0xFFD0, 0xFFDF },
	{ "CMIO RelayBox REBOOT",      0xFFCF, 0xFFCF },
	{ "CMIO RelayBox MILO16",      0xFFC0, 0xFFCE }
};


char *rtilib_sig_to_str(unsigned short sig) {

	int i;
	struct rti_type_s *rti_type;

	for (i=0; i<RTI_TYPES; i++) {
		rti_type = &rti_types[i];
		if ((sig >= rti_type->sigl) && (sig <= rti_type->sigh))
			return rti_type->typedesc;
	}
	return "RTI? Unknown RTI signature";
}

/* ===================================== */

char *csr_names[16] = { "TB:",  "RB:",  "INV:", "RL:",
			"INE:", "INT:", "RTP:", "RRP:",
			"BC:",  "BCR:", "LRR:", "NEM:",
			"BRD:", "LOC:", "TES:", "OPE:" };

char *rtilib_csr_to_str(unsigned short csr) {

	int i;
	static char res[128];

	memset(res, 0, 128);

	for (i=0; i<16; i++) {
		if ((1<<i) & csr)
		   strcat(res,csr_names[i]);
	}
	return res;
}

/* ===================================== */

char *str_names[STR_RTI_SHIFT] = { "TF:",  "DBC:", "SF:",  "BUY:",
				   "BRO:", "TB:",  "RB:",  "TIM:",
				   "SR:",  "INS:", "ME:" };

char *rtilib_str_to_str(unsigned short str) {

	unsigned int i, rti;
	static char res[128];

	memset(res, 0, 128);

	rti = (str & STR_RTI_MASK) >> STR_RTI_SHIFT;
	sprintf(res,"RTI:%02d ",rti);

	for (i=0; i<STR_RTI_SHIFT; i++) {
		if ((1<<i) & str)
		   strcat(res,str_names[i]);
	}
	return res;
}

/* ===================================== */

static void dump_buf(unsigned short *buf, int wc)
{
	int i;

	printf("wc = %d\n", wc);
	for (i = 0; i < wc; i++) {
		if (i % 2 == 0)
			printf("%04x: ", i);
		printf("%04x ", buf[i]);
		if (i % 2 == 1)
			printf("\n");
	}
	printf("\n");
}

#define MIL1553_DEV_PATH "/dev/mil1553"
#define MAX_BCS 32
static key_t semkey;
static int semid;
static struct sembuf op_lock[] = {
	{ 0, 0, SEM_UNDO, },
	{ 0, 1, SEM_UNDO, },
};
static struct sembuf op_unlock[] = {
	{ 0, -1, SEM_UNDO | IPC_NOWAIT, },
};

int rtilib_init(void)
{
	int i, cc;

	cc = open(MIL1553_DEV_PATH, O_RDWR, 0);
	if (cc < 0)
		return cc;

	semkey = ftok(MIL1553_DEV_PATH, 1553);
	if (semkey == -1) {
		perror(__func__);
		return -1;
	}
	semid = semget(semkey, MAX_BCS, IPC_CREAT | 0666);
	if (semid == -1) {
		perror(__func__);
		return -1;
	}

	return cc;
}

void rtilib_lock_bc(int bc)
{
	/* FIXME: semop could fail */
	op_lock[0].sem_num = bc;
	op_lock[1].sem_num = bc;
	semop(semid, &op_lock[0], 2);
}

void rtilib_unlock_bc(int bc)
{
	/* FIXME: semop could fail */
	op_unlock[0].sem_num = bc;
	semop(semid, &op_unlock[0], 1);
}

int rtilib_send_receive(int fn,
			int bc,
			int rti,
			int wc,
			int sa,
			int tr,
			int nreply,
			unsigned short *rxbuf,
			unsigned short *txbuf) {

	struct mil1553_send_recv_s sr, *srp = &sr;
	int i, cc;

#if 0
	printf("\njdgc: calling send_receive "
		"%d:%d wc:%d sa:%d tr:%d %s -------------\n",
		bc, rti, wc, sa, tr,
		nreply? "noreply" : "reply");
#endif

	srp->bc = bc;
	srp->rti = rti;
	srp->wc = wc;
	srp->tr= tr;
	srp->sa= sa;
	srp->wants_reply = !nreply;

	if (txbuf) {
		for (i = 0; i < wc; i++) {
			if (i >= TX_BUF_SIZE)
				break;
			srp->txbuf[i] = txbuf[i];
		}
	}

#if 0
	printf("jdgc: sending txbuf -----------\n");
	dump_buf(txbuf, wc);
#endif
	cc = ioctl(fn, MIL1553_SEND_RECEIVE, srp);
	if (cc)
		return cc;

	if (!srp->wants_reply)
		return cc;

	if (rxbuf) {
		for (i = 0; i < srp->received_wc + 2; i++) {
			if (i > RX_BUF_SIZE+1)
				break;
			rxbuf[i] = srp->rxbuf[i];
		}
	}
#if 0
	printf("jdgc: got rxbuf -----------\n");
	dump_buf(rxbuf, wc+1);
#endif
	return 0;
}

/* ===================================== */

int rtilib_read_csr(int fn, int bc, int rti, unsigned short *csr, unsigned short *str) {

	unsigned short rxbuf[RX_BUF_SIZE];
	unsigned short txbuf[TX_BUF_SIZE];
	int cc, wc, sa, tr;

	memset(rxbuf, 0, sizeof(unsigned short) * RX_BUF_SIZE);
	memset(txbuf, 0, sizeof(unsigned short) * TX_BUF_SIZE);

	wc = 1; sa = SA_CSR; tr = TR_READ;
	cc = rtilib_send_receive(fn,bc,rti,wc,sa,tr,REPLY,rxbuf,txbuf);
	*str = rxbuf[0];
	*csr = rxbuf[1];
	return cc;
}

/* ===================================== */

int rtilib_clear_csr(int fn, int bc, int rti, unsigned short csr) {

	unsigned short rxbuf[RX_BUF_SIZE];
	unsigned short txbuf[TX_BUF_SIZE];
	int cc, wc, sa, tr;

	memset(rxbuf, 0, sizeof(unsigned short) * RX_BUF_SIZE);
	memset(txbuf, 0, sizeof(unsigned short) * TX_BUF_SIZE);

	wc = 1; sa = SA_CLEAR_CSR; tr = TR_WRITE;
	txbuf[0] = csr;
	cc = rtilib_send_receive(fn,bc,rti,wc,sa,tr,REPLY,rxbuf,txbuf);
	return cc;
}

/* ===================================== */

int rtilib_set_csr(int fn, int bc, int rti, unsigned short csr) {

	unsigned short rxbuf[RX_BUF_SIZE];
	unsigned short txbuf[TX_BUF_SIZE];
	int cc, wc, sa, tr;

	memset(rxbuf, 0, sizeof(unsigned short) * RX_BUF_SIZE);
	memset(txbuf, 0, sizeof(unsigned short) * TX_BUF_SIZE);

	wc = 1; sa = SA_SET_CSR; tr = TR_WRITE;
	txbuf[0] = csr;
	cc = rtilib_send_receive(fn,bc,rti,wc,sa,tr,REPLY,rxbuf,txbuf);
	return cc;
}

/* ===================================== */

/**
 * This is just for debugging the RTI
 */

int rtilib_read_rxbuf(int fn, int bc, int rti, int wc, unsigned short *rxbuf) {

	unsigned short txbuf[TX_BUF_SIZE];
	int cc, sa, tr;

	memset(txbuf, 0, sizeof(unsigned short) * TX_BUF_SIZE);

	rtilib_clear_csr(fn,bc,rti,CSR_RRP);

	sa = SA_RXBUF; tr = TR_READ;
	cc = rtilib_send_receive(fn,bc,rti,wc,sa,tr,REPLY,rxbuf,txbuf);
	return cc;
}

/* ===================================== */

/**
 * The receive buffer RXBUF is what the RTI receives from the BC.
 */

int rtilib_write_rxbuf(int fn, int bc, int rti, int wc, unsigned short *txbuf) {

	unsigned short rxbuf[RX_BUF_SIZE];
	int cc, sa, tr;

	memset(rxbuf, 0, sizeof(unsigned short) * RX_BUF_SIZE);

	cc = rtilib_set_csr(fn,bc,rti,CSR_RRP);
	if (cc)
		return cc;

	sa = SA_RXBUF; tr = TR_WRITE;
	cc = rtilib_send_receive(fn,bc,rti,wc,sa,tr,REPLY,rxbuf,txbuf);
	return cc;
}

/* ===================================== */

/**
 * The receive buffer RXBUF is what the RTI receives from the BC.
 * Writing to the RTI RXBUF means SEND data to the RTI equipment from DSC.
 * Check the RB (RXBUF BUSY) bit in the STR (STATUS), error exit if busy.
 * Reset the RXBUF read/write pointer.
 * Write the data to the RXBUF.
 * Set the RB bit (data ready), enable interrupt to end user equipment and do interrupt.
 *
 * Note the RB bit is used to synchronise access by the BC and the end user equipment.
 * The BC sets it to one after writing data, this tells the
 * RTI data is available, the RTI then reads the data and sets the RB bit
 * after the data has been written.
 */

int __rtilib_send_eqp(int fn, int bc, int rti, int wc, unsigned short *txbuf) {

	unsigned short str;
	int cc;

	cc = rtilib_read_str(fn,bc,rti,&str);
	if (cc)
		return cc;

	if (STR_RB & str)
		return -EBUSY;  /* So the equipment hasn't finished the last transaction */

	cc = rtilib_write_rxbuf(fn,bc,rti,wc,txbuf);
	if (cc)
		return cc;

	return rtilib_set_csr(fn,bc,rti,CSR_RB | CSR_INT | CSR_INE);
}

int rtilib_send_eqp(int fn, int bc, int rti, int wc, unsigned short *txbuf)
{
	int cc;

	rtilib_lock_bc(bc);
	cc = __rtilib_send_eqp(fn, bc, rti, wc, txbuf);
	rtilib_unlock_bc(bc);

	return cc;
}

/* ===================================== */

/**
 * The transmit buffer TXBUF is what the RTI sends to the BC.
 */

int rtilib_read_txbuf(int fn, int bc, int rti, int wc, unsigned short *rxbuf) {

	unsigned short txbuf[TX_BUF_SIZE];
	int cc, sa, tr;

	memset(txbuf, 0, sizeof(unsigned short) * TX_BUF_SIZE);

	cc = rtilib_set_csr(fn,bc,rti,CSR_RTP);
	if (cc)
		return cc;

	sa = SA_TXBUF; tr = TR_READ;
	return rtilib_send_receive(fn,bc,rti,wc,sa,tr,REPLY,rxbuf,txbuf);
}

/* ===================================== */

/**
 * The transmit buffer TXBUF is what the RTI sends to the BC.
 * Reading from the RTI TX buf means get data from the RTI equipment for the DSC.
 * Poll the TB (TXBUF READY) bit in the STR (STATUS), exit on timeout.
 * Reset the TXBUF read/write pointer.
 * Read the data from the TXBUF.
 * Clear the TB bit (data read), clear interrupt to end user equipment.
 *
 * Note the TB bit is used to synchronise access by the BC and the end user equipment.
 * The RTI sets it to one after writing data, this tells the BC
 * data is available, the BC then reads the data and clears th TB bit
 * after data has been read.
 */

#define WAIT_POLLS 3
#define WAIT_TB_us 1000

int __rtilib_recv_eqp(int fn, int bc, int rti, int wc, unsigned short *rxbuf) {

	unsigned short str, tb;
	int i, cc;

	/* Poll the TB bit WAIT_POLL times waiting WAIT_TB_us between tests */

	tb = 0;
	for (i=0; i<WAIT_POLLS; i++) {
		cc = rtilib_read_str(fn,bc,rti,&str);
		if (cc)
			return cc;
		tb = str & STR_TB;
		if (tb) break;
		usleep(WAIT_TB_us); /* According to the man page this is thread safe */
	}

	if (!tb)
		return -ETIMEDOUT;

	cc = rtilib_read_txbuf(fn,bc,rti,wc,rxbuf);
	if (cc)
		return cc;

	return rtilib_clear_csr(fn,bc,rti,CSR_TB | CSR_INT);
}

int rtilib_recv_eqp(int fn, int bc, int rti, int wc, unsigned short *rxbuf)
{
	int cc;

	rtilib_lock_bc(bc);
	cc = __rtilib_recv_eqp(fn, bc, rti, wc, rxbuf);
	rtilib_unlock_bc(bc);

	return cc;
}

/* ===================================== */

/**
 * This is just for debugging the RTI
 */

int rtilib_write_txbuf(int fn, int bc, int rti, int wc, unsigned short *txbuf) {

	unsigned short rxbuf[RX_BUF_SIZE];
	int cc, sa, tr;

	memset(rxbuf, 0, sizeof(unsigned short) * RX_BUF_SIZE);

	rtilib_clear_csr(fn,bc,rti,CSR_RTP);

	sa = SA_TXBUF; tr = TR_WRITE;
	cc = rtilib_send_receive(fn,bc,rti,wc,sa,tr,REPLY,rxbuf,txbuf);
	return cc;
}

/* ===================================== */

int rtilib_read_signature(int fn, int bc, int rti, unsigned short *sig) {

	unsigned short rxbuf[RX_BUF_SIZE];
	unsigned short txbuf[TX_BUF_SIZE];
	int cc, wc, sa, tr;

	memset(rxbuf, 0, sizeof(unsigned short) * RX_BUF_SIZE);
	memset(txbuf, 0, sizeof(unsigned short) * TX_BUF_SIZE);

	wc = 1; sa = SA_SIGNATURE; tr = TR_READ;
	cc = rtilib_send_receive(fn,bc,rti,wc,sa,tr,REPLY,rxbuf,txbuf);
	*sig = rxbuf[1];
	return cc;
}

/* ===================================== */

int rtilib_read_str(int fn, int bc, int rti, unsigned short *str) {

	unsigned short rxbuf[RX_BUF_SIZE];
	unsigned short txbuf[TX_BUF_SIZE];
	int cc, wc, sa, tr;

	memset(rxbuf, 0, sizeof(unsigned short) * RX_BUF_SIZE);
	memset(txbuf, 0, sizeof(unsigned short) * TX_BUF_SIZE);

	wc = MODE_READ_STR; sa = SA_MODE; tr = TR_READ;
	cc = rtilib_send_receive(fn,bc,rti,wc,sa,tr,REPLY,rxbuf,txbuf);
	*str = rxbuf[0];
	return cc;
}

/* ===================================== */

int rtilib_read_last_str(int fn, int bc, int rti, unsigned short *str) {

	unsigned short rxbuf[RX_BUF_SIZE];
	unsigned short txbuf[TX_BUF_SIZE];
	int cc, wc, sa, tr;

	memset(rxbuf, 0, sizeof(unsigned short) * RX_BUF_SIZE);
	memset(txbuf, 0, sizeof(unsigned short) * TX_BUF_SIZE);

	wc = MODE_READ_LAST_STR; sa = SA_MODE; tr = TR_READ;
	cc = rtilib_send_receive(fn,bc,rti,wc,sa,tr,REPLY,rxbuf,txbuf);
	*str = rxbuf[0];
	return cc;
}

/* ===================================== */

int rtilib_master_reset(int fn, int bc, int rti) {

	unsigned short rxbuf[RX_BUF_SIZE];
	unsigned short txbuf[TX_BUF_SIZE];
	int cc, wc, sa, tr;

	memset(rxbuf, 0, sizeof(unsigned short) * RX_BUF_SIZE);
	memset(txbuf, 0, sizeof(unsigned short) * TX_BUF_SIZE);

	wc = MODE_MASTER_RESET; sa = SA_MODE; tr = TR_READ;
	cc = rtilib_send_receive(fn,bc,rti,wc,sa,tr,1,rxbuf,txbuf);
	return cc;
}

/* ===================================== */

int rtilib_read_last_cmd(int fn, int bc, int rti, unsigned short *cmd) {

	unsigned short rxbuf[RX_BUF_SIZE];
	unsigned short txbuf[TX_BUF_SIZE];
	int cc, wc, sa, tr;

	memset(rxbuf, 0, sizeof(unsigned short) * RX_BUF_SIZE);
	memset(txbuf, 0, sizeof(unsigned short) * TX_BUF_SIZE);

	wc = MODE_READ_LAST_CMD; sa = SA_MODE; tr = TR_READ;
	cc = rtilib_send_receive(fn,bc,rti,wc,sa,tr,REPLY,rxbuf,txbuf);
	*cmd = rxbuf[0];
	return cc;
}
