
/*==========================================================================*/
static char rcsid[] = "$Id: powrt.c,v 2.41 2010/03/29 08:24:48 nmn Exp $";
/*==========================================================================*/
/*
 * Real-Time task to handle double ppm for POW EM,
 * based on PSB version 17/12/93 not yet containing double ppm.
 * Possibility of two slaves per master added 1/6/94 by W. Heinze.
 * Modified to handle POW-V instead of POW conditionally by N. de Metz-Noblat.
 * 19-FEB-99 (Heinze): Generalize double PPM (machine condition included)
 *                     Read hardware min/ max before initialization
 *                     For ADE telegram no reference to next user lines
 *
 *  Double PPM equipment if:
 *  - PPMV <> 0
 *  - ELMSTR <> 0 for slave equipment: element number of Master Equipment
 *  - PLSDBL = Double PPM condition: tells when the slave is selected.
 *  - There can be up to 2 slaves per master (i. e. 3 values can be selected)
 *
 * 03-AUG-99 (Heinze): Multiple power converters added
 * 18-APR-01 (Heinze): Hardware min/max values only replaced if valid initial acquisition
 * 30-JAN-02 (Heinze): Take away references to LPI
 * 01-MAR-05 (Heinze): Add multiple acquisitions per cycle
 * 01-FEB-08 (Sicard): extend doublePPM to more than 2 slaves, remove Camac specifics
 * 16-Feb-09 NMN+CHS:  fix for SPS CNGS: properly check against Tgm and PPM_NR
 */
/*==========================================================================*/

#ifndef LYNX
#define LYNX
#endif

/* Conditionnal compilation for POW-V included */

/* include files searched in /usr/local/include or /usr/include */
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>              /* for fprintf(stderr,...) */
#include <string.h>
#include <sys/types.h>
#include <sys/time.h>
#include <time.h>
#include <errno.h>
#include <math.h>

#include <tgm/con.h>
#include <tgm/tgm.h>            /* telegram access routines */
#include <err/err.h>
#include <gm/gm_def.h>          /* general EM definitions */
#include <gm/eqp_def.h>         /* names of modules and columns */
#include <gm/camac_def.h>
#include <gm/access.h>
#include <dscrt/dscrtlib.h>
#include <drvrutil/camaclib.h>

#include "pow_messages.h"       /* ctrl. and acqn. message formats */

#ifdef V_VERSION

#include <drvrutil/mil1553quicklib.h> /* MIL-1553 quick_data library routines */
extern void eqm_ptr_iv();	/* non documented subroutine to find pointer to data table */	

#endif


/*--------------------------------------------------------------------------*/
/* CONSTANTS:                                                               */
/*--------------------------------------------------------------------------*/
#ifndef V_VERSION    /* CAMAC variant */

/* masks corresponding to data structure of SDVME */
#define F23_MASK       (23<<F_IBIT)
#define A12_MASK       (A8_MASK + A4_MASK)
#define F0A1            A1_MASK
#define F16A2          (F16_MASK | A2_MASK)
#define F23A12         (F23_MASK | A12_MASK)
#define F25A0           F25_MASK
#define FA_MASK        (F31_MASK | A15_MASK)

#else    /* VME variant */

/* define error reporting error codes. */
#define  RT_OK            0      /* Welldone no error */
#define  NO_CON           1      /* Not connected */
#define  COM_ERR         -1      /* Communication error */
#define  CCV_ERR        0x1      /* Error in reading CCV values */
#define  AQN_ERR        0x2      /* Error in writing acquisition values */

#endif

static int max_pls_line = 24;
static int tgm_first_line = 1;
static int tgm_last_line = 24;

#define  MX_SLV       9       /* max nbr of double ppm slaves per master*/

/*--------------------------------------------------------------------------*/
/* define Macros for more comfortable use of Data Table access routines     */
/*--------------------------------------------------------------------------*/
#define DTRS(val, col) {\
    data_desc.data = val;\
    data_desc.type = EQM_TYP_INT;\
    data_desc.size = mx;\
    for(i = 0; i <= mx; i++) co[i] = 0;\
    eqm_dtr_iv(blsx, col, &data_desc, 1, el, mx+1, 0, co, mx+1);\
}    

#define DTARS(val, col) {\
    data_desc.data = val;\
    data_desc.type = EQM_TYP_INT;\
    data_desc.size = act->nb;\
    for(k = 0; k <= act->nb; k++) act->co[k] = 0;\
    eqm_dtr_iv(blsx, col, &data_desc, 1, act->el, act->nb+1, 0, act->co, act->nb+1);\
}    

#define RDT(val, sz, colref, pls) {\
    data_desc.data = val;\
    data_desc.type = EQM_TYP_INT;\
    data_desc.size = sz * cact->nb;\
    for(k = 0; k <= cact->nb; k++) cact->co[k] = 0;\
    eqm_rtr_iv(blsx, &colref, &data_desc, sz, cact->el, cact->nb+1, pls, cact->co, cact->nb+1);\
}

#define WDT(val, sz, colref, pls) {\
    int k; data_desc.data = val;\
    data_desc.type = EQM_TYP_INT;\
    data_desc.size = sz * cact->nb;\
    for(k = 0; k <= cact->nb; k++) cact->co[k] = 0;\
    eqm_rtw_iv(blsx, &colref, &data_desc, sz, cact->el, cact->nb+1, pls, cact->co, cact->nb+1);\
}

#define WDTD(val, sz, colref, pls) {\
    int k; data_desc.data = val;\
    data_desc.type = EQM_TYP_DOUBLE;\
    data_desc.size = sz * cact->nb;\
    for(k = 0; k <= cact->nb; k++) cact->co[k] = 0;\
    eqm_rtw_iv(blsx, &colref, &data_desc, sz, cact->el, cact->nb+1, pls, cact->co, cact->nb+1);\
}

#define RDTD(val, sz, colref, pls) {\
    int k; data_desc.data = val;\
    data_desc.type = EQM_TYP_DOUBLE;\
    data_desc.size = sz * cact->nb;\
    for(k = 0; k <= cact->nb; k++) cact->co[k] = 0;\
    eqm_rtr_iv(blsx, &colref, &data_desc, sz, cact->el, cact->nb+1, pls, cact->co, cact->nb+1);\
}

#define MyCalloc(a) CheckedAlloc(a, sizeof(int))
#define MyCallocd(a) CheckedAllocd(a, sizeof(double))
#define UPW(a) ((a >> 16) & 0x0ffff)
#define LOW(a) (a & 0x0ffff)

/*--------------------------------------------------------------------------*/
/* Reentrancy structure for one kind of PPM. Power supplies belong to the   */
/* same kind of PPM if they have the same acqn. and control interrupt,      */
/* the same PPMV value and the same kind of PPM, i. e. simple or double     */
/*--------------------------------------------------------------------------*/
typedef struct action {

    struct action  *next; /* pointer to the next record(object) */

    int     id;         /* Action ID number */
    int     ci;         /* Control Interrupt */
    int     ai;         /* Acquisition Interrupt */
	
    int     grp;        /* PPMV value */
    int     trm;        /* Treatment code */
    int     nb;         /* Number of equipments found per kind of PPM */
    int     first;      /* Flag, if 1, getting configuration after interrupt */
    int    *el;         /* List of elements (internal nr.) per kind of PPM */
    int    *eqn;        /* List of elements (equipment nr.) per kind of PPM */
    int    *co;         /* Completion codes      */
    int    *er;         /* Error codes from 1553 */
    int    *tc;         /* Data column for current (present) telegram */
    int    *tn;         /* Data column for next telegram */
    int    *t0;         /* Data column 0 for acquisition */
    int    *ad;         /* ADDRESS1 */
    double *stmp;       /* cycle stamp */

#ifndef V_VERSION       /* CAMAC variant */

    int    *v2;         /* contains CNAF values for CAMAC access */
    int    *v3;         /* current value */
    int    *v4;         /* other values for CAMAC access */
    int    *az;         /* CAMAC addresses for actions per QUAD */
    int     nbz;        /* number of elements (QUADs) in az array */

#else    /* VME variant */

    struct  quick_data_buffer  *ctl; /* linked list of control buffers */
    struct  quick_data_buffer  *acq; /* linked list of acqn. buffers */

    int    *va;         /* Acquisition values */
    int    *v2;         /* Control values non-PPM */
    int    *v3;         /* Control Values PPM */
    int    *hwmn;       /* hw min values */
    int    *hwmx;       /* hw max values */
    int    *erres;      /* errors corresponding to hw min/max values */
    int    lst_ctr_da[MAX_PPM+1];  /* Last control date = f(user) */
    
    /* now follow the data for multiple acquisition power converters */
    int    its;  	/* interrupt marking the begin of multiple acquisitions, resetting the data table */
    int    *namm;	/* number of current measurements, i.e. the ones actually done, stored in NAMM */
    double *meanv;	/* mean value of acquisitions, stored in MES */
    double *stdev;	/* standard deviation of acquisitions, stored in MES2 */
    double *dmn;	/* minimum value of acquisitions, stored in DMN */
    double *dmx;	/* maximum value of acquisitions, sored in DMX */
    double *acqv;	/* acquisition value */
    double *sum;	/* running sum of acqv */
    double *sumsq;	/* running sum of squares of acqv */
    double *aq_ac;	/* array containing the multiple acquisitions */
    
#endif

    int     fdppm;      /* flag Double PPM associated to master equ. */
    int    *ms;         /* if double PPM: master equ. number */
    int    *nr_slav;    /* if double PPM: nb slaves per master */

    int    *slve;       /* if double PPM: slave equ. numbers array*/
    int    *plsc;       /* if double PPM: current (present) group array*/
    int    *plsn;       /* if double PPM: next group  array*/
    int    *tgm_ma;     /* if double PPM: machine number array*/
    int    *plsbp;      /* if double PPM: array of condition[s] to select slave */
    int    *grpty;      /* if double PPM: array of type of group: bit-pattern / exclusive*/

} Action; /* one action table per kind of ppm */

/*--------------------------------------------------------------------------*/
/* Global variables:                                                        */
/*--------------------------------------------------------------------------*/
char           *program;          /* program name */
int             blsx = (-1);	  /* EM Block Serial Number */
Action         *act0 = NULL;      /* Actions list start, initiated with 0 */
Action         *act;		  /* Actions list current pointer */
TgmMachine      plstb;            /* telegram to use for normal PPM */
Cardinal        user_grp_nr;      /* telegram group number */
Cardinal        next_user_grp_nr; /* telegram "next" group number */
int             pres_grp_val;     /* telegram USER group value */
int             next_grp_val;     /* telegram next USER group value */
int             no_timing = 0;    /* true if PPMVI, PPMAQI = 0 for POW-V */
data_array      data_desc;        /* data descriptor for rtgrim_acc */
col_descr       col_ccva;         /* column descriptor of CCVA */
col_descr       col_aq;           /* column descriptor of AQ */
col_descr       col_bffr;         /* column descriptor of BFFR */
col_descr       col_err1;         /* column descriptor of ERR1 */
col_descr       col_err2;         /* column descriptor of ERR2 */
col_descr       col_stmp;         /* column descriptor of AQSTMP */

#ifdef V_VERSION    /* VME variant */

col_descr       col_ccac;         /* column descriptor of CCAC */
col_descr       col_fupa;         /* column descriptor of FUPA */
col_descr       col_hwmn;         /* column descriptor of HWMN */
col_descr       col_hwmx;         /* column descriptor of HWMX */
col_descr       col_erres;        /* column descriptor of ERRES */
col_descr       col_aq_ac;        /* column descriptor of AQ_AC */
col_descr       col_namm;         /* column descriptor of NAMM */
col_descr       col_mes;          /* column descriptor of MES */
col_descr       col_mes2;         /* column descriptor of MES2 */
col_descr       col_dmn;          /* column descriptor of DMN */
col_descr       col_dmx;          /* column descriptor of DMX */

#endif

/*--------------------------------------------------------------------------*/
/* Variables defined in argument list:                                      */
/*--------------------------------------------------------------------------*/
static int        verbose_flg   = FALSE; /* verbose start flag */
static int        exit_flg      = FALSE; /* exit after configuration check flag */
static int        delay_flg     = FALSE; /* number of ms to wait before ACQ */
static int        pdelay_flg    = FALSE; /* print total traitement time  */
static int        trace_acq_flg = FALSE; /* trace acquisition flag */
static int        trace_ctl_flg = FALSE; /* trace control flag */
static int        jitter_flg    = FALSE; /* check for jitter > jitter_val */
static int        jitter_val    = 0;     /* jitter value */
static int        trace_gbelm   = 0;     /* global elem nb to be traced */

static int        trace_elm     = 0;     /* local elem nb corresponding to glob. elem. */


/*
 * Different actions corresponding to different values of the PPMV bit
 * (normally, only PPMV = 1 is used):
 *      PPMV    Acquisition  Control    Explanation
 *        0       Present       Next    Non-PPM
 *        1       Present       Next    PPM standard
 *        2       Present       None
 *        3       Next          Next    Special for Forewarnings
 *        4       None          Present
 *        5       None          Next
 *        7       Present       Present

 * The ppm arrays contain the parameters 1 = next, 0 = present, -1 = previous
   for the acquisition and control routines:
 */
int ppm_acq[] = { EQP_RTPRESENT,    /* 0 */
                  EQP_RTPRESENT,    /* 1 */
                  EQP_RTPRESENT,    /* 2 */
                  EQP_RTNEXT,       /* 3 */ 
                  EQP_RTPREVIOUS,   /* 4 */ 
                  EQP_RTPREVIOUS,   /* 5 */ 
                  EQP_RTPRESENT,    /* 6 */ 
                  EQP_RTPRESENT };  /* 7 */  

int ppm_ctl[] = { EQP_RTNEXT,       /* 0 */
                  EQP_RTNEXT,       /* 1 */
                  EQP_RTPREVIOUS,   /* 2 */
                  EQP_RTNEXT,       /* 3 */
                  EQP_RTPRESENT,    /* 4 */
                  EQP_RTNEXT,       /* 5 */
                  EQP_RTNEXT,       /* 6 */    
                  EQP_RTPRESENT };  /* 7 */    



/*====================================================*/
/* calloc(), checking for errors...                   */
/*====================================================*/
static int *CheckedAlloc(int nb, int sz) 
{
	int *p;

	p = (int *) calloc((unsigned)nb, (unsigned)sz);
	if (p == NULL) {
		fprintf(stderr, "\n%s: CheckedAlloc: not enough memory. Aborting!\n", program);
		exit(-1);
	}
	return (p);
}

static double *CheckedAllocd(int nb, int sz)
{
	double *p;

	p = (double *) calloc((unsigned)nb, (unsigned)sz);
	if (p == NULL) {
		fprintf(stderr, "\n%s: CheckedAllocd: not enough memory. Aborting!\n", program);
		exit(-1);
	}
	return (p);
}
/* end of CheckedAlloc() */


/*====================================================*/
/* Return current time in ms for measurements         */
/*====================================================*/
static int GetCurrentTimeMsec()
{
    struct timeval tt;

    gettimeofday(&tt, NULL);
    return(tt.tv_sec * 1000 + (tt.tv_usec/1000));

} /* end of GetCurrentTimeMsec() */


/*====================================================*/
/* Initialise PLS lines                               */
/*====================================================*/
static void InitPLS()
{    
    int i;
    
    /* Attach to the telegram routines */
    if (TgmAttach(plstb, TgmTELEGRAM) == TgmFAILURE) {
        fprintf(stderr, "%s: InitPLS: can't attach to Tgm. Aborting!\n", program);
        exit(-1);
    }

    /* Get USER group number */
    user_grp_nr = TgmGetGroupNumber(plstb, "USER");    
    if (user_grp_nr == TgmFAILURE) {
        fprintf (stderr, "%s: InitPLS: can't get Tgm group number for USER. Aborting!\n", program);
        exit(-1);
    }

    tgm_first_line = TgmFirstLine(plstb,"USER");
    tgm_last_line  = TgmLastLine(plstb,"USER");
    
    if (plstb == TgmADE || plstb == TgmSCT) { /* ADE, SCT: no next user, next same value as present */
        next_user_grp_nr = user_grp_nr;
	for (i = 0; i < 8; i++) { /* forbid NEXT lines on ADE and SCT */
            if (ppm_ctl[i] == EQP_RTNEXT) ppm_ctl[i] = EQP_RTPRESENT;
            if (ppm_acq[i] == EQP_RTNEXT) ppm_acq[i] = EQP_RTPRESENT;
        }
    }
    else { /* for other machines: get next group number */
        next_user_grp_nr = TgmGetNextGroupNumber(plstb, user_grp_nr);
        if (next_user_grp_nr == TgmFAILURE) {
            fprintf (stderr, "%s: InitPLS: can't get next Tgm group number for USER. Aborting!\n", program);
            exit(-1);
        }
    }

} /* end of InitPLS() */


/*====================================================*/
/* Get CURRENT USER group value                       */
/*====================================================*/
static int GetCurrentUserGroup()
{       
    if (TgmGetGroupValue(plstb, TgmCURRENT, 0, user_grp_nr, &pres_grp_val) == TgmFAILURE)
        pres_grp_val = -1;

    /* Check group value got */
    if ( (pres_grp_val < tgm_first_line) || (pres_grp_val > tgm_last_line) ) {
        fprintf(stderr, "%s: GetCurrentUserGroup: telegram uncoherent...\n", program);
        return(-1); /* error... */
    }
    return(0);        

} /* end of GetCurrentUserGroup() */


/*====================================================*/
/* Get NEXT USER group value                          */
/*====================================================*/
static int GetNextUserGroup()
{    
    
    if (plstb == TgmADE || plstb == TgmSCT) {  /* ADE specific: non-PPM machine! */
        next_grp_val = pres_grp_val;  
        return(0);
    }        
    else  { /* for other machines */
        if (TgmGetGroupValue(plstb, TgmCURRENT, 0, next_user_grp_nr, &next_grp_val) == TgmFAILURE)
            next_grp_val = -1;
        
        /* Check group value got */
	if ( (next_grp_val < tgm_first_line) || (next_grp_val > tgm_last_line) ) {
            fprintf(stderr, "%s: GetNextUserGroup: telegram uncoherent...\n", program);
            return(-1); /* error... */
        }            
        return(0);
    }

} /* end of GetNextUserGroup() */


/*=====================================================*/
/* Find group type and bit pattern flag for double ppm */
/*=====================================================*/
static int FindBitPatternFlag(int machine, int group)
{
    int bp_flag = FALSE;
    static TgmGroupDescriptor tgdesc;

    TgmGetGroupDescriptor(machine, group, &tgdesc);

    switch (tgdesc.Type) {
	case TgmBIT_PATTERN:   bp_flag = TRUE; break;

	case TgmEXCLUSIVE:
	case TgmNUMERIC_VALUE: bp_flag = FALSE; break;

	default: bp_flag = FALSE;
	       fprintf(stderr, "%s: FindBitPatternFlag: can't find group type for double PPM\n", program);
    }
    return (bp_flag);

} /* end of FindBitPatternFlag() */

/*====================================================*/
/* Build linked list of action structures             */
/*====================================================*/
static void BuildActions()
{
    int         i, j, k, cc, plsmsk, group;
    int         cfg_err;
    int         single;
    int         mx;         /* max number of equipments */
    int         n;          /* length of element array */
    int         mstr;       /* internal equipment number of master p. s. */
    int         el_mstr;    /* internal index number of master p. s. */
    int         flag;       /* 1 for master, 0 for slave equipment */
        
    /* Temporary arrays */
    int         *el;        /* internal element numbers */
    int         *co;        /* completion code */
    int		*its;	    /* interrupt starting the cycle for multiple acquisitions */
    int         *ci;        /* control interrupt */
    int         *ai;        /* acquisition interrupt */
    int         *grp;       /* ppmv value */
    int         *trm;       /* treatment code */
    int         *cnnt;      /* connect (CNNT) column */
    int         *plsdbl;    /* master element number */
    int         *elmstr;    /* double ppm as read for slaves */
    int         *nr_sla;    /* number of slaves per master */
    int         *slaves;    /* internal element numbers of slaves */
    int         *plsdblsv;  /* double ppm condition for slaves put next to master */
   
#ifndef V_VERSION /* CAMAC */
    int         tmp;

#else /* VME */
    struct  quick_data_buffer  *pc;  /* control buffers */
    struct  quick_data_buffer  *pa;  /* acquisition buffers */
#endif

    cfg_err = FALSE;

    /* Find block serial number for the POW, resp. POW-V equipment */

#ifndef V_VERSION /* CAMAC */
    blsx = blsnam("POW");

#else /* VME */
    blsx = blsnam("POW-V");

#endif

    if (blsx < 0) {
        fprintf(stderr, "%s: BuildActions: can't find BLSNR for EM POW or POW-V\n", program);
        exit(1);
    }

    /* Get the max PPM capability from datatable */
    max_pls_line = rdbls_gtfield (blsx, PPM_NR);



    /* <<< DEBUG info >>> Translate the traced element number */
    if (trace_acq_flg || trace_ctl_flg) {
        trace_elm = get_locelno(blsx, trace_gbelm); /* get corresponding local elem. number */
        if (trace_elm <= 0) { /* wrong input parameter has been entered */
            fprintf(stderr, "%s: BuildActions: invalid POW equipment number %d to trace. Aborting!\n", program, trace_gbelm);
            exit(1);
        }
    }

    /* Get the column descriptors */
    iv_pos (blsx, EQP_CCVA, &col_ccva);
    iv_pos (blsx, EQP_AQ,   &col_aq);
    iv_pos (blsx, EQP_BFFR, &col_bffr);
    iv_pos (blsx, EQP_ERR1, &col_err1);
    iv_pos (blsx, EQP_ERR2, &col_err2);
    iv_pos (blsx, EQP_AQSTMP, &col_stmp);

#ifdef V_VERSION /* VME */
    iv_pos (blsx, EQP_CCAC, &col_ccac);
    iv_pos (blsx, EQP_FUPA, &col_fupa);
    iv_pos (blsx, EQP_HWMN, &col_hwmn);
    iv_pos (blsx, EQP_HWMX, &col_hwmx);
    iv_pos (blsx, EQP_ERRES, &col_erres);
    iv_pos (blsx, EQP_AQ_AC, &col_aq_ac);
    iv_pos (blsx, EQP_NAMM, &col_namm);
    iv_pos (blsx, EQP_MES, &col_mes);
    iv_pos (blsx, EQP_MES2, &col_mes2);
    iv_pos (blsx, EQP_DMN, &col_dmn);
    iv_pos (blsx, EQP_DMX, &col_dmx);
	
#endif

    /* Find the number of equipments of class POW (POW-V) */
    mx = rdbls_gtfield(blsx, EL_COUNT);

    /* 
     * Allocate temporary data for sorting the elements:
     * =================================================
     * all columns except el, er and co start with the first element;
     * el[0] contains number of elements and er[0], co[0] contain global coco.
     * The same is true for el and er, co in action list 
     */
    n = mx + 1;
    el      = MyCalloc(n);
    co      = MyCalloc(n);
    its	    = MyCalloc(n);
    ci      = MyCalloc(n);
    ai      = MyCalloc(n);
    grp     = MyCalloc(n);
    trm     = MyCalloc(n);
    cnnt    = MyCalloc(n);
    elmstr  = MyCalloc(n);
    plsdbl  = MyCalloc(n);
    nr_sla  = MyCalloc(n);
    slaves   = MyCalloc(n*MX_SLV);
    plsdblsv = MyCalloc(n*MX_SLV);
    
    /* Build local list of all elements; el[0] = number of elements */
    el[0] = mx; for (i = 1; i <= mx; i++) el[i] = i;

    /* 
     * Read data table columns (instance variables) and put them into the temporary storage:
     * ================================================================ 
     * IT_START = interrupt marking the cycle begin for multiple acquisitions 
     * PPMCVI = control interrupt number
     * PPMAQI = acquisition interrupt number
     * PPMV   = PPMV value encoded in GRP
     * CNNT   = POW connected if CNNT=1
     * ELMSTR = master equ. nr. of slaves
     * PLSDBL = PLS condition for double ppm, joined to slave power supplies
     */   
   
#ifdef V_VERSION /* VME */

    DTRS(its, EQP_IT_START);
    if (co[0] !=  EQP_NOERR) {
        fprintf(stderr, "%s: BuildActions: can't read IT_START column in DT, coco = %d. Aborting!\n", program, co[0]);
        exit(1);
    } 
#endif
     
    DTRS(ci, EQP_PPMCVI);
    if (co[0] !=  EQP_NOERR) {
        fprintf(stderr, "%s: BuildActions: can't read PPMCVI column in DT, coco = %d. Aborting!\n", program, co[0]);
        exit(1);
    }

    DTRS(ai, EQP_PPMAQI);
    if (co[0] !=  EQP_NOERR) {
        fprintf(stderr, "%s: BuildActions: can't read PPMAQI column in DT, coco = %d. Aborting!\n", program, co[0]);
        exit(1);
    }

    DTRS(cnnt, EQP_CNNT);
    if (co[0] !=  EQP_NOERR) {
        fprintf(stderr, "%s: BuildActions: can't read CNNT column in DT, coco = %d. Aborting!\n", program, co[0]);
        exit(1);
    }

    DTRS(grp, EQP_GRP);
    if (co[0] !=  EQP_NOERR) {
        fprintf(stderr, "%s: BuildActions: can't read GRP column in DT, coco = %d. Aborting!\n", program, co[0]);
        exit(1);
    }

    DTRS(trm, EQP_TRM);
    if (co[0] !=  EQP_NOERR) {
        fprintf(stderr, "%s: BuildActions: can't read TRM column in DT, coco = %d. Aborting!\n", program, co[0]);
        exit(1);
    }

    DTRS(elmstr, EQP_ELMSTR);
    if (co[0] !=  EQP_NOERR) {
        fprintf(stderr, "%s: BuildActions: can't read ELMSTR column in DT, coco = %d. Aborting!\n", program, co[0]);
        exit(1);
    }

    DTRS(plsdbl, EQP_PLSDBL);
    if (co[0] !=  EQP_NOERR) {
        fprintf(stderr, "%s: BuildActions: can't read PLSDBL column in DT, coco = %d. Aborting!\n", program, co[0]);
        exit(1);
    }
  
    /* Decode PPMV value which is encoded in grp[i] */
    for (i = 0; i < mx; i++) {
      grp[i] &= PPMV_MASK; grp[i] >>= PPMVSET;
      trm[i] = ((trm[i] >> 8) & 0x8) >> 3;
      nr_sla[i] = 0;
    }

    /* 
     * Establish the columns nr_sla (nr. of slaves), slaves (internal
     * equipment numbers), plsdblsv (condition to select slaves).
     * They have entries for all master double PPM power supplies.
     * Note: el_mstr is not the internal equ. nr. but the index of column arr.  
     */
    for (i = 0; i < mx; i++) { /* scan all elements */
        /* this is a connected slave power supply? */
        if ( (elmstr[i] > 0) && (plsdbl[i] != -1) && (cnnt[i] != 0) ) {
            el_mstr = get_locelno(blsx, elmstr[i]) - 1; /* index in array */
	    j = nr_sla[el_mstr];
	    if (j >= MX_SLV) {
		fprintf(stderr, "%s: BuildActions Master nr.%d has too many slaves\n",
		program, elmstr[i]);
	    }
	    else {
		slaves[el_mstr*MX_SLV+j] = el[i+1];
		plsdblsv[el_mstr*MX_SLV+j] = plsdbl[i];
		nr_sla[el_mstr] += 1;
            }
        }
    } /* end of scan */

    /* 
     * Sort all elements by the following criteria:
     * ============================================
     * Search elements for common its/acq/ctrl/ppmv/dblppm. One action is formed
     * per kind of ppm. The first action formed receives the NULL pointer
     * and becomes so the last action in the linked list of all actions.
     * Skip not connected and slave equipments (having elmstr <> 0 and plsdbl != -1) 
     */
    for (n = 0, i = 0; i < mx; i++) { /* scan all elements ... */

#ifndef V_VERSION
	/* CAMAC: ppm and non ppm if msb of aqntrm = 1 */
        if ( ((grp[i] != 0) || (trm[i] == 1)) && (cnnt[i] != 0) && (elmstr[i] == 0) )

#else    /* VME: also non ppm equipment! */
        if ( (cnnt[i] != 0) && ((elmstr[i] == 0) || (plsdbl[i] == -1)) )

#endif
        {
	    flag = (nr_sla[i] != 0);     /* this are master power supplies */
            act = act0;
            while (act) {
                if ( (act->ci == ci[i])     /* same ctrl. interrupt */
                  &&(act->ai == ai[i])     /* same acqn. interrupt */
#ifdef V_VERSION /* VME */
		  &&(act->its == its[i])   /* same its interrupt */
#endif
                  &&(act->grp == grp[i])   /* same PPM group */
                  &&(act->fdppm == flag) ) 
                    break; /* skip, this record already exists */

                act = act->next; /* advance to the next record */
            }

            if (act == 0) {
                /* Allocate memory for the new Action object */
                act = (Action *) CheckedAlloc(1, sizeof(Action));
                act->id = n++;            /* Action ID */
                act->ci = ci[i];          /* store ctrl. interrupt */
                act->ai = ai[i];          /* store acqn. interrupt */
#ifdef V_VERSION /* VME */
		act->its = its[i];        /* store its interrupt */
#endif
                act->grp = grp[i];        /* store PPM group */
                act->trm = trm[i];        /* store treatment code */
                act->fdppm = flag;        /* double ppm master or not double ppm */
                act->next = act0;
                act0 = act;
            }
        }

    } /* end of scan all elements ... */
    
    /* The last formed action is now first in list */
    for (act = act0; act; act = act->next) { /* scan Action objects */

        /* Count number of equipment per ppm kind */
        for (n = 0, i = 0; i < mx; i++)  {
	    flag = (nr_sla[i] != 0);
            if (  (cnnt[i] != 0)                        /* ONLY connected elements */ 
              && (elmstr[i] == 0 || plsdbl[i] == -1) 
              && (act->ci == ci[i])                    /* same ctrl. interrupt */
              && (act->ai == ai[i])                    /* same acq. interrupt */
#ifdef V_VERSION /* VME */
	      && (act->its == its[i])                  /* same its interrupt */
#endif
              && (act->grp == grp[i])                  /* same PPM group */
              && (act->fdppm == flag) )
                n++;
        }

        act->nb = n;  /* actual number of elements passed selection criteria */  

        act->el = MyCalloc(n+1);
        act->eqn = MyCalloc(n+1);
        act->el[0] = n;
        act->eqn[0] = n;         /* 'eqn' contains equipment numbers */ 

        /* Build local list of sorted elements: one per object */
        for (n = 0, i = 0; i < mx; i++) { /* scan all elements */
	    flag = (nr_sla[i] != 0);
            if (  (cnnt[i] != 0)                          /* ONLY connected elements */ 
              && ((elmstr[i] == 0) || (plsdbl[i] == -1)) 
              && (act->ci == ci[i])                      /* same ctrl. interrupt */ 
              && (act->ai == ai[i])                      /* same acq. interrupt */ 
#ifdef V_VERSION /* VME */
	      && (act->its == its[i])                    /* same its interrupt */
#endif 
              && (act->grp == grp[i])                    /* same PPM group */ 
	      && (act->fdppm == flag) ) {                /*same doublePPM condition*/

                act->el[n+1] = el[i+1];  /* add element to list of equipments (internal nbrs.) */
                act->eqn[n+1] = get_globelno(blsx, el[i+1]); /* list of glob. equipment nbrs. */
                n++; /* be ready for next element */
            }
        }    
    
    } /* end of scan Action objects */

    /* Check that Action objects have been created */
    if (act0 == 0) { /* starting pointer points to NULL */
        fprintf(stderr, "%s: BuildActions: no PPM to handle. Aborting!\n", program);
        exit(1);
    }

    single = ((act0->next == 0) && (act0->ci == 0) && (act0->ai == 0));
    if (single) { /* no ppm and one single action */

#ifndef V_VERSION   /* CAMAC */
	fprintf(stderr, "%s: BuildActions: there is no external interrupt. Abort RT-program!\n", program);
        exit(1);
#else               /* VME */
	fprintf(stderr, "%s: BuildActions: no timing, acquisition and control all 1.2 seconds!\n", program);
	no_timing = 1;
#endif

    }
    
    /* Free temporary arrays */
    free(el);
    free(ci);
    free(ai);   
#ifdef V_VERSION /* VME */  
    free(its);
#endif
    free(grp);
    free(trm);
    free(co);
    free(elmstr);
    free(plsdbl);


    /* Extract RO data from the Data Table */
    for (act = act0; act; act = act->next) { /* scan all actions in list */

        n = act->nb + 1;
        act->tc = MyCalloc(n);
        act->tn = MyCalloc(n);
        act->t0 = MyCalloc(n);
        act->ad = MyCalloc(n);
        act->co = MyCalloc(n);
        act->er = MyCalloc(n);
	act->stmp = MyCallocd(n);

        /* Reserve storage for the double ppm parameters */
        if (act->fdppm) {
            act->ms      = MyCalloc(n);   /* Space to hold master number */
	    act->nr_slav = MyCalloc(n);   /* nb of slaves */
	    act->slve    = MyCalloc(n*MX_SLV);   /* slave eqnumbers */
	    act->plsc   = MyCalloc(n*MX_SLV);   /* current pls group */
	    act->plsn   = MyCalloc(n*MX_SLV);   /* next pls group */
	    act->tgm_ma = MyCalloc(n*MX_SLV);   /* TgmMachine number */
	    act->plsbp  = MyCalloc(n*MX_SLV);   /* bit pattern condition to select slave */
	    act->grpty  = MyCalloc(n*MX_SLV);   /* group type bit-pat or excl*/

	/*fill double ppm data for each master*/
	    for (i = 0; i < (n-1); i++) {  /*loop on master*/
                mstr = act->el[i+1];      /* internal eqn of master */
                el_mstr = mstr - 1;       /* internal index of master */
                act->ms[i] = mstr;        /* save local eqn of master */
		act->nr_slav[i] = nr_sla[el_mstr]; /* nb of slaves */
		for (j=0; j < nr_sla[el_mstr]; j++) {
		    plsmsk = plsdblsv[el_mstr*MX_SLV+j];
		    k = i*MX_SLV+j;  /*slave info storage index*/
		    act->slve[k] = slaves[el_mstr*MX_SLV+j];
		    act->tgm_ma[k] = (plsmsk>>24); /* machine number */
		    act->plsc[k] = ((plsmsk>>16) & 0x0ff); /*group-no*/
		    act->plsn[k] = act->plsc[k];  /* for ADE and SCT: no Next group */
		    if (act->tgm_ma[k] != TgmADE && act->tgm_ma[k] != TgmSCT)
			act->plsn[k] = TgmGetNextGroupNumber(act->tgm_ma[k], act->plsc[k]);
		    act->plsbp[k] = (plsmsk & 0x0ffff); /* bit pattern condition */
		    group = ppm_ctl[act->grp] ? act->plsn[k] : act->plsc[k];
		    act->grpty[k] = FindBitPatternFlag(act->tgm_ma[k], group);
                }
            }
        }

        /* Get group numbers for PRESENT telegram lines */
        cc = init_grp(blsx, act->el, EQP_RTPRESENT, &act->tc[0]);
        if (cc != EQP_NOERR) {
	    fprintf(stderr, "%s: BuildActions can't init group for PRESENT lines, coco = %d. Aborting!\n", program, cc);
            exit(1);
        }

	if (plstb == TgmADE  || plstb == TgmSCT)  /* ADE and CTF specific */
            for (i = 0; i < act->nb; i++) act->tn[i] = act->tc[i];        

        else { /* all others: get group numbers for NEXT telegram lines */
            cc = init_grp(blsx, act->el, EQP_RTNEXT, &act->tn[0]);
            if (cc != EQP_NOERR) {
		fprintf(stderr, "%s: BuildActions can't init group for NEXT lines, coco = %d. Aborting!\n", program, cc);
                exit(1);
            }
        }

        /* Check group numbers got above */
        for (i = 0; i < act->nb; i++) { /* scan all elements */

            if (  ((act->tc[i] != 0) && (act->tc[i] != user_grp_nr))
              || ((act->tn[i] != 0) && (act->tn[i] != next_user_grp_nr)) ) { /* config. error! */

                if (!cfg_err) { /* printout once the next head message */ 
                    fprintf(stderr, "%s: BuildActions: all ppm group values must be USER!!!\n", program);
                    cfg_err = TRUE; /* fix config. error condition */
                }
                                                
                /* trace all relevant parameters */
                fprintf(stderr, "%s: element=%d, grp=%d, next_grp=%d (user_grp=%d, next_user=%d)\n", program,
                        act->eqn[i+1], act->tc[i], act->tn[i], user_grp_nr, next_user_grp_nr);
            }
        }

        DTARS (act->ad, EQP_ADDRESS1);  /* read basic addresses */
        if (act->co[0] !=  EQP_NOERR) {
	    fprintf(stderr, "%s: BuildActions: can't read EQP_ADDRESS1 column, coco = %d. Aborting!\n", program, act->co[0]);
            exit(1);
        }

#ifndef V_VERSION    /* CAMAC */
        act->v2 = MyCalloc(n);
        act->v3 = MyCalloc(n);
        act->v4 = MyCalloc(n);
        act->az = MyCalloc(n);

        /* build the table for global CAMAC actions, i.e. for all QUADs */
        act->nbz = 0;
        for (i = 0; i < act->nb; i++) {
            tmp = act->ad[i] & (~FA_MASK);
            for (j = 0; j < act->nbz; j++) if (tmp == act->az[j]) break;
            if (j == act->nbz) {
                act->az[act->nbz] = tmp;
                act->nbz++;
            }
            act->first = 0; /* no acquisition of configuration */
        }

#else    /* VME */
        /* Allocate memory for ctrl. and acqn. messages ... */
        act->ctl = (struct quick_data_buffer *) CheckedAlloc(n+1, sizeof(struct quick_data_buffer));
        act->acq = (struct quick_data_buffer *) CheckedAlloc(n+1, sizeof(struct quick_data_buffer));
        act->va    = CheckedAlloc(n, 44);  /* n*44 bytes!!! */
        act->v2    = CheckedAlloc(n, 24);
        act->v3    = CheckedAlloc(n, 20);
        act->hwmn  = CheckedAlloc(n, sizeof(int));
        act->hwmx  = CheckedAlloc(n, sizeof(int));
        act->erres = CheckedAlloc(n, sizeof(int));
	
	if (act->its > 0) {
	  act->namm  = CheckedAlloc(n, sizeof(int));
	  act->meanv = CheckedAllocd(n, sizeof(double));
	  act->stdev = CheckedAllocd(n, sizeof(double));
	  act->dmn   = CheckedAllocd(n, sizeof(double));
	  act->dmx   = CheckedAllocd(n, sizeof(double));
	  act->acqv  = CheckedAllocd(n, sizeof(double));
	  act->sum   = CheckedAllocd(n, sizeof(double));
	  act->sumsq = CheckedAllocd(n, sizeof(double));
	  act->aq_ac  = CheckedAllocd(n, 16 * sizeof(double)); 	
	}  

        /* Initialise control blocks for M1553 I/O */
        pc = act->ctl;    /* pointer to control message */
        pa = act->acq;    /* pointer to acquisition message */
        for (i = 0; i < act->nb; i++, pc++, pa++) {
            pa->bc = pc->bc = UPW(act->ad[i]); /* set BC number */
            pa->rt = pc->rt = LOW(act->ad[i]); /* set RT number */ 
            pc->stamp = act->el[i+1];          /* STAMP == element number */
            if (i != (act->nb -1)) {
                pc->next = &pc[1];             /* point to next message in list */
                pa->next = &pa[1];
            }
        }
	
        act->first = 1;         /* flag for requesting configuration */
#endif

    } /* end of scan all Actions in the list */

    /* Free unnecessary more memory */
    free(nr_sla);
    free(slaves);
    free(plsdblsv);

    if (cfg_err) { /* if config. error has been fixed */
	fprintf(stderr, "%s: BuildActions can't be started due to config error. Aborting!\n", program);
        exit(1);
    }

} /* end of BuildActions() */


#ifdef V_VERSION
/*====================================================*/
/*  Convert MIL-1553 error to EM error codes          */
/*====================================================*/
static int c1553toem(int err)
{
    int e = 0;
    
    switch (err) {
       case RT_OK:            e = RT_OK; break;
       case BC_not_connected: e = EQP_BCNOTCON; break;
       case RT_not_connected: e = EQP_RTNOTCON; break;
       case TB_not_set:       e = EQP_TBNOTSET; break;
       case RB_set:           e = EQP_RBSET; break;
       case Bad_buffer:       e = EQP_BADBUF; break;
       case M1553_error:      e = EQP_M1553ERR; break;
       case ETIMEDOUT:        e = EQP_TIMOUT; break;
       case ECONNREFUSED:     e = EQP_INTERFERR; break;
       default:               e = EQP_SYS5ERR;
    }
    return (e);

} /* end of c1553toem() */ 

#endif


/*====================================================*/
/* Evaluate double ppm condition to select slave      */
/*====================================================*/
static int IsSlave(int gv, int bp_flag, int pls_bp)
{
	int ma, mb, slave_flg = FALSE;

	if (bp_flag) slave_flg = ((gv & pls_bp) != 0);  /*bit pattern*/
	else {
	    ma = pls_bp & 0x0ff;
	    mb = ((pls_bp >> 8) & 0x0ff);
	    if (ma != 0) slave_flg = (gv == ma);
	    if (mb != 0) slave_flg = (slave_flg || (gv == mb));
	}
	return(slave_flg);

} /* end of IsSlave() */


/*====================================================*/
/* Double PPM: select local equipment of slave or     */
/*             master for this cycle                  */
/*====================================================*/
static void SelectEquipmentNumber(Action *cact)
{
    int i,j, k, gv, group, tgm;
    
    tgm = ppm_ctl[cact->grp]; /*Present/Next indicator*/
    
    for (i = 0; i < cact->nb; i++) { /* scan all elements of Action object */

        cact->el[i+1] = cact->ms[i]; /* set master by default */
        
	for (j=0; j<cact->nr_slav[i]; j++) { /*loop on all slaves*/
	    k = i*MX_SLV+j;
	    group = tgm ? cact->plsn[k] : cact->plsc[k];
	    TgmGetGroupValue(cact->tgm_ma[k], TgmCURRENT, 0, group, &gv);

	    /* Check ppm condition for slave j */
	    if (IsSlave(gv, cact->grpty[k], cact->plsbp[k])) {
		cact->el[i+1] = cact->slve[k];    /* set slave */
		break;   /* Exit on 1rst slave active found  */
	    }
	}
    }

} /* end of SelectEquipmentNumber() */


#ifdef V_VERSION    /* VME */
/*====================================================*/
/* Request of acquisition of hardware MIN/MAX values  */
/*====================================================*/
static void ReqAcquisitionMinMax(Action *cact)
{
    int i, k;
    int coco, err;

    struct timeval da;
    struct quick_data_buffer *pc;  /* control buffers */


    if (cact->fdppm) return;       /* no request for double ppm power conv. */

    err = FALSE;                   /* error flag for printing erroneous power converters */

    RDT(cact->v2, 6, col_ccac, 0); /* non ppm data used for config. request */
    if (cact->co[0] !=  EQP_NOERR) 
	fprintf(stderr, "%s: ReqAcquisitionMinMax: can't read CCAC DTcolumn, er= %d!\n", program, cact->co[0]);

    /* Initialize configuration request message for MIL-1553 */
    pc = cact->ctl;  /* set pointer to ctrl. message */    
    (void) memset(pc, 0, cact->nb * sizeof(struct quick_data_buffer)); /* clear ctrl. buffers */

    /* Create linked list of ctrl. messages */
    gettimeofday(&da, NULL); /* get current time (TOD) */
    for (i = 0; i < cact->nb; i++, pc++) { /* scan all elements for given Action object */

        /* Initialise control block for MIL-1553 I/O */
        pc->bc = UPW(cact->ad[i]);     /* set BC number */
        pc->rt = LOW(cact->ad[i]);     /* set RT number */
        pc->stamp = cact->el[i+1];     /* set STAMP field == element number */
        pc->next = ((i < (cact->nb-1)) ? &pc[1] : NULL); /* ptr. to next control block */ 

        /* Prepare request message */
        pc->error = 0;
	memcpy(&pc->pkt[0], &cact->v2[i*6], 22);
	memcpy(&pc->pkt[12], &da, 8);   /* set TOD */
        pc->pkt[6] = 0;
        pc->pkt[7] = 5;                /* service request for configuration */
        pc->pktcnt = 22;               /* length of configuration request message */
    }

    /* Send request messages */
    if (send_quick_data(cact->ctl))     /* if MIL-1553 error returned */
        fprintf(stderr, "%s: ReqAcquisitionMinMax: QCKDATERR for requesting hw min/max acquisition\n", program);

    else { /* check error field for each ctrl. message */
        for(i = 0; i < cact->nb; i++) { /* scan all ctrl. buffers */
            coco = c1553toem(cact->ctl[i].error);
            if (coco != 0) { 
                err = TRUE;
                fprintf(stderr, "eq_nr.(coco) = %d(%d), ", cact->eqn[i+1], coco);
            }
        }
    }
    if (err) fprintf(stderr, "\n (erroneous power converters at requesting hw min/max)\n");

} /* end of ReqAcquisitionMinMax() */


/*====================================================*/
/* Acquisition of hardware MIN/MAX values             */
/*====================================================*/
static void DoAcquisitionMinMax(Action *cact)
{
    int i, k;
    int sz;
    int coco, err;
    struct  quick_data_buffer  *pa;  /* acqn. buffers */

    if (cact->fdppm) return; /* no acquisition for double ppm power conv. */

    err = FALSE; /* error flag for printing erroneous power converters */

    /* Clear errors from MIL-1553 */
    sz = (cact->nb + 1) * sizeof(int);
    (void) memset(&cact->erres[0], 0, sz);

    /* Read MIN/MAX values arrays from data table */
    RDT(cact->hwmx, 1, col_hwmx, 0);
    RDT(cact->hwmn, 1, col_hwmn, 0);

    /* Clear acqn. buffers */ 
    pa = cact->acq; /* set pointer to acqn. message */
    (void) memset(pa, 0, cact->nb * sizeof(struct quick_data_buffer));    

    /* Initialize acquisition message for MIL-1553 */
    for (i = 0; i < cact->nb; i++, pa++) {
        pa->bc = UPW(cact->ad[i]);    /* BC number */
        pa->rt = LOW(cact->ad[i]);    /* RT number */
        pa->next = ((i < (cact->nb-1)) ? &pa[1] : NULL); /* pointer to next acq. message */
    }

    /* Get acquisition messages from all existing G64s. If hwmx contains a nonzero value,
       it is considered to be valid (by a previous powrt run or a programmer's action), erres = 0.
       The erres variable giving the coco for an EM reading is treated correspondingly */
       
    if (get_quick_data(cact->acq)) { /* if MIL-1553 error returned */
	for (i = 0; i < cact->nb; i++) if (cact->hwmx[i] == 0) cact->erres[i] = EQP_QCKDATERR;
        fprintf(stderr, "%s: DoAcquisitionMinMax: QCKDATERR for acquiring hw min/max\n", program);
    }
    else { /* check error field for each acqn. message */
        for (i = 0; i < cact->nb; i++) { /* scan all acqn. buffers */
            coco = c1553toem(cact->acq[i].error);
	    if ( (coco == 0) && (cact->acq[i].pkt[7] == 5) ) { /* no error, hwmx/mn overwritten */
		memcpy(&cact->hwmx[i], &cact->acq[i].pkt[32], sizeof(int));
		memcpy(&cact->hwmn[i], &cact->acq[i].pkt[36], sizeof(int));
            }
            else {
                err = TRUE;
                fprintf(stderr, "nr.(coco)=%d(%d), ", cact->eqn[i+1], coco);
		if (cact->hwmx[i] == 0) cact->erres[i] = coco;
                if (coco == 0) {
                    fprintf(stderr, "service provided = %d, ", cact->acq[i].pkt[7]);
		    if (cact->hwmx[i] == 0) cact->erres[i] = EQP_SERVICERR;
                }
            }
	}
    }
    
    if (err) fprintf(stderr, "\n (erroneous power converters at acquiring hw min/max)\n");

    /* Copy min, max and error of configuration message to HWMN, HWMX, ERRES columns */
    WDT(cact->hwmn, 1, col_hwmn, 0);
    if (cact->co[0] !=  EQP_NOERR) 
	fprintf(stderr, "%s: DoAcquisitionMinMax: can't write HWMN DTcolumn, er= %d!\n", program, cact->co[0]);
    
    WDT(cact->hwmx, 1, col_hwmx, 0);
    if (cact->co[0] !=  EQP_NOERR) 
	fprintf(stderr, "%s: DoAcquisitionMinMax: can't write HWMX DTcolumn, er= %d!\n", program, cact->co[0]);

    WDT(cact->erres, 1, col_erres, 0);
    if (cact->co[0] !=  EQP_NOERR) 
	fprintf(stderr, "%s: DoAcquisitionMinMax: can't write ERRES DTcolumn, er= %d!\n", program, cact->co[0]);
	
    if (cact->its > 0) {
       for (i = 0; i < cact->nb; i++) {
          cact->namm[i] = 0; 
	  cact->meanv[i] = 0;  /* create a zero columns */
       }         
       for (i = 0; i <= max_pls_line; i++) {
          WDT(cact->namm, 1, col_namm, -i);
          WDTD(cact->meanv, 1, col_mes, -i);
	  WDTD(cact->meanv, 1, col_dmn, -i);
	  WDTD(cact->meanv, 1, col_dmx, -i);
	  WDTD(cact->meanv, 1, col_mes2, -i);	  
          if (cact->co[0] !=  EQP_NOERR) 
             fprintf(stderr, "%s: Resetting of NAMM column in DT, coco = %d!\n", program, cact->co[0]);
       }
    }   	     	

} /* end of DoAcquisitionMinMax() */

#endif


/*====================================================*/
/* Control Interrupt handling, called once per action */
/*====================================================*/
static void DoControl(Action *cact)
{
    int    i, k, sz;
    int    tgm;    /* flag for telegram: current = 0, next = 1 */
    int    gval;   /* group value corresponding to next or present telegram */
    int    tr_nb;  /* element(equipment) number for debug tracing */

#ifdef V_VERSION
    int                els[2];         /* element array for single element */
    int                cos[2];         /* completion array for single element */
    float             *ccvv_ptr;
    short             *pls_ptr;
    unsigned int       fupa;           /* actuation except reset */
    nonppm_ctrl_msg   *dtrs;           /* dtrs points to message structure */
    int                lst_ctr_da = 0;
    int                dif_ctr_da;
    int                indx_ctr_da;
    struct timeval     da;
    struct quick_data_buffer *pc;      /* control buffers */
#endif


    tgm = ppm_ctl[cact->grp];
    if (tgm < 0) return;  /* no actions */

    /* Clear errors from CAMAC and MIL-1553 */
    sz = (cact->nb+1) * sizeof(int);
    (void) memset(&cact->er[0], 0, sz);

    /* Find group value */
    gval = (tgm) ? next_grp_val : pres_grp_val;

#ifdef V_VERSION
    indx_ctr_da = gval;
#endif

    if (cact->grp == 0)  gval = 0;        /* for non ppm action */

#ifndef V_VERSION /* CAMAC */
    tr_nb = -1;
    if (cact->grp) { /* control only for ppm CAMAC power conv. */

        /* Double PPM: recompute local eqn to use for this cycle */
	if (cact->fdppm) SelectEquipmentNumber(cact);

	RDT (cact->v3, 1, col_ccva, -gval); /* normal case */

        if (trace_ctl_flg && (cact->co[0] != EQP_NOERR)) 
            fprintf(stderr, "%s: DoControl: can't read CCVA column in DT, coco = %d!\n", program, cact->co[0]);

        /* F16A2  = write quad register B */
	for (i = 0; i < cact->nb; i++) {
	     cact->v2[i] = cact->ad[i] + F16A2;
            /* <<< DEBUG info >>> select element for tracing */
            if (trace_ctl_flg && (cact->el[i+1] == trace_elm)) tr_nb = i;
	}
        pmcami(cact->nb, cact->v3, (long *)cact->v2, 5, &cact->er[1], &cact->er[0]);

	/* <<< DEBUG info >>> */
	if (trace_ctl_flg && (tr_nb >= 0)) /* trace infos for 'tr_nb'-element */
	    fprintf(stderr, "%s: DoControl: gb_elm=%d, pls=%3d, ccv=%d, err=%d\n",
		    program, trace_gbelm, gval, cact->v3[tr_nb], cact->er[tr_nb+1]);
    }
#else    /* VME */  

    /* Read non-PPM actuation always from master CCAC */
    if (cact->fdppm) for (i = 0; i < cact->nb; i++) cact->el[i+1] = cact->ms[i];

    RDT (cact->v2, 6, col_ccac, 0); /* get non ppm data */
    if (trace_ctl_flg && (cact->co[0] != EQP_NOERR)) 
        fprintf(stderr, "%s: DoControl: can't read CCAC column in DT, coco = %d!\n", program, cact->co[0]);

    /* But read CCVA from slave if needed */
    if (cact->fdppm) SelectEquipmentNumber(cact);

    RDT (cact->v3, 5, col_ccva, -gval); /* normal case */
     
    if (trace_ctl_flg && (cact->co[0] != EQP_NOERR)) 
        fprintf(stderr, "%s: DoControl: can't read CCVA column in DT, coco = %d!\n", program, cact->co[0]);

    /* Clear ctrl. message buffers */
    pc = cact->ctl;  /* set pointer to ctrl. block */
    (void) memset(pc, 0, cact->nb * sizeof(struct quick_data_buffer));

    /* Create linked list of control messages */
    gettimeofday(&da, NULL);  /* get TOD (Unix format) */
    for (i = 0; i < cact->nb; i++, pc++) { /* scan all elements ... */

        /* Initialise control block for MIL-1553 I/O */
        pc->bc = UPW(cact->ad[i]);    /* set BC number */
        pc->rt = LOW(cact->ad[i]);    /* set RT number */
        pc->stamp = cact->el[i+1];    /* set STAMP == el. number */
        pc->next = ((i < (cact->nb -1)) ? &pc[1] : NULL); /* pointer to next ctrl. block */
        
        /* Prepare control message */
	memcpy(&pc->pkt[0], &cact->v2[i*6], 24);  /* non-ppm ctrl. values (actuations) */
	memcpy(&pc->pkt[24], &cact->v3[i*5], 20); /* ppm ctrl. values */
	memcpy(&pc->pkt[12], &da, 8);             /* set TOD field */
        pc->pkt[8] = 0;
	pc->pkt[9] = plstb;                      /* machine type: CPS, PSB, CTF */
        pc->pkt[10] = 0;
        pc->pkt[11] = (tgm) ? next_grp_val : pres_grp_val; /* cycle */
        pc->error = 0;
        pc->pktcnt = 44;                         /* size of control message */ 
    }

    tr_nb = -1;

    /* Send messages over MIL-1553: cact->nb messages */
    if (send_quick_data(cact->ctl) != 0) { /* MIL-1553 error (encoded in errno) */
        for (i = 0; i <= cact->nb; i++) cact->er[i] = EQP_QCKDATERR; /* log errors */
        /* <<< DEBUG info >>> */
        if (trace_ctl_flg) fprintf(stderr, "%s: DoControl: send_quick_data ioctl error = %d\n", program, errno);
    }

    else { 
        for (i = 0; i < cact->nb; i++) { /* check error field for each message */
            cact->er[i+1] = c1553toem(cact->ctl[i].error);
            /* <<< DEBUG info >>> select element for tracing */
            if (trace_ctl_flg && (cact->el[i+1] == trace_elm)) tr_nb = i;
        }
    }

    /* <<< DEBUG info >>> */
    if (trace_ctl_flg && (tr_nb >= 0)) { /* trace infos for 'tr_nb'-element */
        ccvv_ptr = (float *)&cact->ctl[tr_nb].pkt[24];
        pls_ptr  = (short *)&cact->ctl[tr_nb].pkt[10];
        fprintf(stderr, "%s: DoControl: gb_elm=%d, pls=%3d, ccv=%f, CCSACT=%d, change bit=%d, err=%d\n", 
                program, trace_gbelm, *pls_ptr, *ccvv_ptr, cact->ctl[tr_nb].pkt[23],
                cact->ctl[tr_nb].pkt[22], cact->er[tr_nb+1]);
    }

    /* <<< DEBUG info >>> */
    if (jitter_flg) {
        lst_ctr_da = GetCurrentTimeMsec();
        if (cact->lst_ctr_da[indx_ctr_da] != 0) {
            dif_ctr_da = lst_ctr_da - cact->lst_ctr_da[indx_ctr_da] ;
            while (dif_ctr_da > 900) dif_ctr_da -= 1200;
            if ( (dif_ctr_da < -jitter_val) || (dif_ctr_da > jitter_val) )
		fprintf(stderr, "%s: jitter=%4d ms for ci=%d, ai=%d, grp=%d, user=%d\n", program,
                        dif_ctr_da, cact->ci, cact->ai, cact->grp, indx_ctr_da);
        }
        cact->lst_ctr_da[indx_ctr_da] = lst_ctr_da;
    }    

    /* For the reset case reconstitute the previous actuation into CCSACT */
    data_desc.type = EQM_TYP_INT;
    els[0] = 1; /* one element at the time */
    for (i = 0; i < cact->nb; i++) { /* scan all elements */
        dtrs = (nonppm_ctrl_msg *) & (cact->v2[i*6]);
        if (dtrs->ccsact == 4) { /* for reset reconstitute old ccac columns with fupa */
            data_desc.data = &fupa; /* data structure to read fupa */
            data_desc.size = 1;
            els[1] = cact->el[i+1];
            cos[0] = cos[1] = 0; /* clear cocos */
            eqm_rtr_iv(blsx, &col_fupa, &data_desc, 1, els, 2, 0, cos, 2);
            if (trace_ctl_flg && (cos[0] != EQP_NOERR)) 
                fprintf(stderr, "%s: DoControl: can't read FUPA column in DT, coco = %d!\n", program, cos[0]);
            dtrs->ccsact = fupa;
            data_desc.data = &(cact->v2[i*6]); /* data structure to write previous data into ccac */
            data_desc.size = 6;
            cos[0] = cos[1] = 0;
            eqm_rtw_iv(blsx, &col_ccac, &data_desc, 6, els, 2, 0, cos, 2);
            if (trace_ctl_flg && (cos[0] != EQP_NOERR)) 
                fprintf(stderr, "%s: DoControl: can't write CCAC column in DT, coco = %d!\n", program, cos[0]);
        }
    }

#endif

    /* Write error column into non-PPM ERR1 column (for possible use in EM) */

    WDT(&cact->er[1], 1, col_err1, 0);
    if (trace_ctl_flg && (cact->co[0] != EQP_NOERR))
	fprintf(stderr, "%s: DoControl: can't write ERR1 column in DT, coco = %d!\n", program, cact->co[0]);
    
} /* end of DoControl() */ 


#ifndef V_VERSION /* CAMAC control */
/*====================================================*/
/* Additional CAMAC control:                          */
/* =========================                          */
/* Control interrupt handling, final sending to QUADs */
/* For CAMAC controlled power converters, this func-  */
/* tion is called after DoControl(). This serves to   */
/* send the data to the Singles.                      */
/*====================================================*/

static void DoAddCamacControl(Action *cact)
{
    int    i;
    int    tgm; /* flag for telegram: current = 0, next = 1 */
        
    tgm = ppm_ctl[cact->grp];     
    if (tgm < 0) return; /* illegal PPM value */

    if (cact->grp) { /* control only for ppm CAMAC power conv. */

        /* F23A12 = reset possible LAM sources, only once per QUAD! */
        for (i = cact->nbz-1; i >= 0; i-- ) {
            cact->v2[i] = cact->az[i] | F23A12;
            cact->v4[i] = 0x0ff;
        }
        /* Send LAM reset cmd. to CAMAC */ 
        pmcami(cact->nbz, cact->v4, (long *)cact->v2, 5, &cact->er[1], &cact->er[0]);

        /* F25A0 = send CCVs, only once per QUAD */
        for (i = cact->nbz-1; i >= 0; i-- ) cact->v2[i] = cact->az[i] | F25A0;
        /* Send control values to CAMAC */
        pmcami(cact->nbz, cact->v4, (long *)cact->v2, 5, &cact->er[1], &cact->er[0]);
	if (trace_ctl_flg && (cact->er[0] != 0))
	    fprintf(stderr, "%s: DoAddCamacControl: error = %d in sending F25.A0\n",
					 program, cact->er[0]);
    }

} /* end of DoAddCamacControl() */
#endif

#ifdef V_VERSION /* VME */
/*======================================================*/
/* Reset multiple acquisition parametres at cycle begin */	
/*======================================================*/
static void ResetAquArray(Action *cact)
{
    int     i;
    int     tgm;    	/* flag for telegram: current = 0, next = 1 */
    int     gval;   	/* group value dependent on "present" or "next" group */
    int	    nmeas;	/* number of measures in previous cycle */
    
    tgm = ppm_acq[cact->grp]; 
    if (tgm < 0) return; /* no actions, corresponding to previous cycle */

    /* Find group value corresponding to "present" or "next" */
    gval = (tgm) ? next_grp_val : pres_grp_val;
 	
    /* reset the multiple acquisition parameters */
    
    nmeas = cact->namm[0];
    
    for (i = 0; i < cact->nb; i++) {
	cact->namm[i] = 0;		/* number of measurements */
	cact->sum[i] = 0;		/* sum */
	cact->sumsq[i] = 0;		/* sum of squares */ 
	cact->meanv[i] = 0;		/* running mean value */
	cact->stdev[i] = 0;		/* standard deviation */
	cact->dmn[i] = 0;		/* smallest acquisition */
	cact->dmx[i] = 0;		/* biggest acquisition */
    }	
    
    /* reset the four calculated parameters in the corresponding data tables */
    WDTD(&cact->meanv[0], 1, col_mes, -gval);
    if (trace_acq_flg && (cact->co[0] != EQP_NOERR))
    	fprintf(stderr, "%s: ResetAquArray: can't reset meanv in MES DT, coco = %d!\n", program, cact->co[0]);
    WDTD(&cact->meanv[0], 1, col_mes, 0);	
    WDTD(&cact->stdev[0], 1, col_mes2, -gval);
    WDTD(&cact->stdev[0], 1, col_mes2, 0);
    WDTD(&cact->dmn[0], 1, col_dmn, -gval);
    WDTD(&cact->dmn[0], 1, col_dmn, 0);
    WDTD(&cact->dmx[0], 1, col_dmx, -gval);
    WDTD(&cact->dmx[0], 1, col_dmx, 0);			
    
    /* reset AQ_AC array */
    (void) memset(cact->aq_ac, 0, cact->nb * 16 * sizeof(double));
    WDTD(&cact->aq_ac[0], 16, col_aq_ac, -gval);
    WDTD(&cact->aq_ac[0], 16, col_aq_ac, 0);
     
    /* reset number of acquisitions at the beginning of the cycle */
    WDT(&cact->namm[0], 1, col_namm, -gval);
    WDT(&cact->namm[0], 1, col_namm, 0);
    	
} /* end of ResetAquArray */	  
#endif


/*====================================================*/
/* Acquisition Interrupt handling                     */
/* Multiple acquisitions per cycle for its <> 0       */	
/*====================================================*/
static void DoAcquisition(Action *cact)
{
    int     i;
    int     sz;
    int     tgm;    /* flag for telegram: current = 0, next = 1 */
    int     gval;   /* group value dependent on "present" or "next" group */
    double  stamp;  /* cycle stamp */

#ifdef V_VERSION /* VME */

    int     err;
    int     tr_nb;  /* element(equipment) index for debug tracing */
    float  *acqv_pt;
    short  *pls_pt;
    char   *status_pt;
    struct  quick_data_buffer  *pa; /* acqn. buffers */
    
    int      nm; 	/* actual number of measurements in current cycle */
    double   aqv;	/* acquisition value */

#endif

    tgm = ppm_acq[cact->grp]; 
    if (tgm < 0) return; /* no actions, corresponding to previous cycle */

    /* Clear errors from CAMAC and MIL-1553 */
    sz = (cact->nb+1) * sizeof(int);    
    (void) memset(&cact->er[0], 0, sz);

    /* Find group value corresponding to "present" or "next" */
    gval = (tgm) ? next_grp_val : pres_grp_val;

    /* Double PPM: only master power supplies are acquired */
    if (cact->fdppm) {
        for (i = 0; i < cact->nb; i++) cact->el[i+1] = cact->ms[i];
    }

    /* Get cycle stamp and put them into the data columns gval and 0 */
    stamp = TgmGetLastTelegramTimeStampSeconds(plstb);
    for (i = 0; i < cact->nb; i++) cact->stmp[i] = stamp;
    WDTD (cact->stmp, 1, col_stmp, -gval); /* write cycle stamp into column of current user */
    WDTD (cact->stmp, 1, col_stmp, 0);     /* write cycle stamp into zero column */

#ifndef V_VERSION /* CAMAC */

    /* ADR1+F0A1 = read quad D register */
    for (i = 0; i < cact->nb; i++) cact->v2[i] = cact->ad[i] + F0A1;
    pmcami(cact->nb, cact->v3, (long *)cact->v2, 5, &cact->er[1], &cact->er[0]);

    /* Write acquisition value into user and zero column */
    WDT (cact->v3, 1, col_aq, -gval); /* current ppm user */
    if (trace_acq_flg && (cact->co[0] != EQP_NOERR))
        fprintf(stderr, "%s: DoAcquisition: can't write AQ column(ppm) in DT, coco = %d!\n", program, cact->co[0]);

    WDT (cact->v3, 1, col_aq, 0); /* zero ppm column */
    if (trace_acq_flg && (cact->co[0] != EQP_NOERR))
        fprintf(stderr, "%s: DoAcquisition: can't write AQ column(zero) in DT, coco = %d!\n", program, cact->co[0]);

#else    /* VME */

    pa = cact->acq; /* set pointer to acqn. messages */

    /* Clear acqn. buffers */
    (void) memset(pa, 0, cact->nb * sizeof(struct quick_data_buffer));
    if (cact->its > 0) (void) memset(cact->acqv, 0, cact->nb * sizeof(double));
   
    /* Initialise control blocks for M1553 I/O */
    for (i = 0; i < cact->nb; i++, pa++) {
        pa->bc = UPW(cact->ad[i]);    /* set BC number */
        pa->rt = LOW(cact->ad[i]);    /* set RT number */
        pa->next = ((i < (cact->nb -1)) ? &pa[1] : NULL); /* pointer to next acqn. message */
    }
    
    err = 0;
    tr_nb = -1;
    
    /* Get acqn. messages from all existing G64s */
    if (get_quick_data(cact->acq)) { /* MIL-1553 global error */
        for (i = 0; i <= cact->nb; i++) cact->er[i] = EQP_QCKDATERR; /* log error */
        /* <<< DEBUG info >>> */
        if (trace_acq_flg) fprintf(stderr, "%s: DoAcquisition: get_quick_data ioctl error = %d\n", program, errno);
    }

    else { /* check error field for each acqn. message */
        for (i = 0; i < cact->nb; i++) { 
            /* <<< DEBUG info >>> select index of element to be traced */
            if (trace_acq_flg && (cact->el[i+1] == trace_elm)) tr_nb = i;
            err = cact->er[i+1] = c1553toem(cact->acq[i].error);
	    if (err == 0) {
	      	memcpy(&cact->va[i*11], &cact->acq[i].pkt[0], 11 * sizeof(int));
		acqv_pt = (float*) &cact->va[i*11 + 7];
		if (cact->its > 0) cact->acqv[i] = *acqv_pt; /* write only valid acquisition values into the column set to zero before */
	    }
        }

        /* <<< DEBUG info >>> */
        if (trace_acq_flg && (tr_nb >= 0)) { /* trace selected element */
            acqv_pt = (float *) &cact->va[(tr_nb*11)+7];
            pls_pt  = ((short *) &cact->va[(tr_nb*11)+2])+1;
            status_pt  = ((char *) &cact->va[(tr_nb*11)+5])+3;
            fprintf(stderr, "%s: DoAcquisition: gb_elm=%d, grp_val=%3d, pls=%3d, acq_val=%f, static_status=%d, err=%d\n",
                    program, trace_gbelm, gval, *pls_pt, *acqv_pt, *status_pt, cact->er[tr_nb+1]);
        }

        /* Copy acquisition message to AQ column */
        WDT (cact->va, 11, col_aq, -gval); /* current ppm user */
        if (trace_acq_flg && (cact->co[0] != EQP_NOERR)) 
            fprintf(stderr, "%s: DoAcquisition: can't write AQ column(ppm) in DT, coco = %d!\n", program, cact->co[0]);

        WDT (cact->va, 11, col_aq, 0);     /* zero column */
        if (trace_acq_flg && (cact->co[0] != EQP_NOERR)) 
            fprintf(stderr, "%s: DoAcquisition: can't write AQ column(zero) in DT, coco = %d!\n", program, cact->co[0]);
    }

    /* write the 1553 errors into the BFFR columns (ppm) */
    WDT (&cact->er[1], 1, col_bffr, -gval);
    if (trace_acq_flg && (cact->co[0] != EQP_NOERR)) 
        fprintf(stderr, "%s: DoAcquisition: can't write BFFR column(ppm) in DT, coco = %d!\n", program, cact->co[0]);

    /* write 1553 errors (except TBNOTSET for ppm power supplies) into BFFR col 0 */
    if (cact->grp != 0)  {
       for (i = 0; i < cact->nb; i++) if (cact->er[i+1] == EQP_TBNOTSET) cact->er[i+1] = 0;
    }
    WDT (&cact->er[1], 1, col_bffr, 0);
    if (trace_acq_flg && (cact->co[0] != EQP_NOERR)) 
        fprintf(stderr, "%s: DoAcquisition: can't write BFFR column(zero) in DT, coco = %d!\n", program, cact->co[0]);
	
    /* do the calculations for only up to 16 multiple acquisitions per cycle */
    
    if (cact->its > 0) {
      nm = cact->namm[0] + 1;
      if (nm <= 16) {
	for (i = 0; i < cact->nb; i++) {
	  cact->namm[i] = nm;
    	  aqv = cact->acqv[i];
	  cact->sum[i] += aqv;			/* sum */
	  cact->sumsq[i] += aqv * aqv;		/* sum of squares */ 
	  cact->meanv[i] =(cact->sum[i])/nm;	/* running mean value */
	  if (nm-1 <= 0) {
	    cact->stdev[i] = 0;
	    cact->dmn[i] = aqv;
	    cact->dmx[i] = aqv;
	  }  
	  else {
	    cact->stdev[i] = sqrt((cact->sumsq[i] - cact->sum[i] * cact->sum[i] / nm) / (nm-1));
	    if (aqv < cact->dmn[i]) cact->dmn[i] = aqv;
	    if (aqv > cact->dmx[i]) cact->dmx[i] = aqv;
	  }  
	}
	
	/* update the AQ_AC data table */
	RDTD(&cact->aq_ac[0], 16, col_aq_ac, -gval);
	if (trace_acq_flg && (cact->co[0] != EQP_NOERR))
    	    fprintf(stderr, "%s: DoAcquisition: can't read aq_ac block from AQ_AC DT, coco = %d!\n", program, cact->co[0]);	
	for (i = 0; i < cact->nb; i++) cact->aq_ac[(nm-1)*cact->nb + i] = cact->acqv[i];
	WDTD(&cact->aq_ac[0], 16, col_aq_ac, -gval);
	if (trace_acq_flg && (cact->co[0] != EQP_NOERR))
    	    fprintf(stderr, "%s: DoAcquisition: can't write aq_ac block into AQ_AC DT, coco = %d!\n", program, cact->co[0]);	  
	WDTD(&cact->aq_ac[0], 16, col_aq_ac, 0); 

	/* write the four calculated parameters into the corresponding data tables */
	WDTD(&cact->meanv[0], 1, col_mes, -gval);
	if (trace_acq_flg && (cact->co[0] != EQP_NOERR))
    	    fprintf(stderr, "%s: DoAcquisition: can't write mean value into MES DT, coco = %d!\n", program, cact->co[0]);
	WDTD(&cact->meanv[0], 1, col_mes, 0);
	WDTD(&cact->stdev[0], 1, col_mes2, -gval);
	WDTD(&cact->stdev[0], 1, col_mes2, 0);
	WDTD(&cact->dmn[0], 1, col_dmn, -gval);
	WDTD(&cact->dmn[0], 1, col_dmn, 0);
	WDTD(&cact->dmx[0], 1, col_dmx, -gval);
	WDTD(&cact->dmx[0], 1, col_dmx, 0);
	WDT(&cact->namm[0], 1, col_namm, -gval);
	WDT(&cact->namm[0], 1, col_namm, 0);			
      } 
    }				
#endif
    
    /* Write error column into non-PPM ERR2 column (for possible use in EM) */

    WDT(&cact->er[1], 1, col_err2, 0);
    if (trace_acq_flg && (cact->co[0] != EQP_NOERR))
	fprintf(stderr, "%s: DoAcquisition: can't write ERR2 column in DT, coco = %d!\n", program, cact->co[0]);

} /* end of DoAcquisition() */


/*=============================================*/
/* Trace all Actions                           */
/*=============================================*/
static void TraceActions()
{   
    int         i, j, n = 0;
    static char  *ppm_type[] = {"NO", "PRESENT", "NEXT"};
	

	fprintf(stdout, "\n%s: Printing ACTIONS:\n", program);        
	
	/* Scan & print all actions */
    for (act = act0; act; act = act->next) {
        fprintf(stdout, "\nACTION %d:\n", act->id);
        fprintf(stdout, "  number of equipments = %d\n", act->nb);
        fprintf(stdout, "  list of equipments   = ");
        for(i = 1; i <= act->nb; i++) { 
            fprintf(stdout , "%d ", act->eqn[i]);
            if((i % 8) == 0) fprintf(stdout , "\n                         ");
        }            
        fprintf(stdout, "\n");
	fprintf(stdout, "  control on          %s,", ppm_type[ppm_ctl[act->grp]+1]);
        fprintf(stdout, "  ctrl. interrupt      = %d\n", act->ci);
	fprintf(stdout, "  acquisition on      %s,", ppm_type[ppm_acq[act->grp]+1]);
        fprintf(stdout, "  acq. interrupt       = %d\n", act->ai);
	fprintf(stdout, "  MSB of aqntrm        = %d\n", act->trm);
	if (act->fdppm) {
	    fprintf(stdout, "  Double ppm\n");
	    for (i = 0; i < act->nb; i++) {
		fprintf(stdout, "   nb-slaves:%d =", act->nr_slav[i]);
		for (j=0; j<act->nr_slav[i]; j++)
		    fprintf(stdout,"%d/%x ",get_globelno(blsx,act->slve[i*MX_SLV+j]),act->plsbp[i*MX_SLV+j]);
		    fprintf(stdout, "\n");
		}
	}

        n++; /* count the number of actions */   
    }

#ifndef V_VERSION
	fprintf(stdout, "\n........There are %d registered ACTIONS for POW equipments\n\n", n);
#else
	fprintf(stdout, "\n........There are %d registered ACTIONS for POW-V equipments\n\n", n);
#endif
    
} /* end of TraceActions() */


/*====================================================*/
/* Usage of POWRT (POWVRT) task                       */
/*====================================================*/
static void Usage()
{
    fprintf(stderr, "\nUsage: %s <option>. The options are:\n", program);
    fprintf(stderr, "  -h -help              help (print this message)\n");
    fprintf(stderr, "  -v -verbose           display configuration and continue\n");
    fprintf(stderr, "  -c -config            display configuration and exit\n");
    fprintf(stderr, "  -delay <x>            wait x ms before acquisition\n");
    fprintf(stderr, "  -trace_acq <el.nr.>   trace the aquisition for given power converter\n");
    fprintf(stderr, "  -trace_ctl <el.nr.>   trace the control for given power converter\n");

#ifdef V_VERSION
    fprintf(stderr, "  -jitter <x>           trace all jitter bigger than <x> ms\n");
    fprintf(stderr, "  -time                 print total treatment time in ms\n");
#endif

} /* end of Usage() */


/*====================================================*/
/* Parse command line and get optional parameters     */
/*====================================================*/
static void GetCmdLineOptions(int argc, char **argv)
{
    int i;

    if (argc <= 1 ) return; /* default options */

    for (i = 1; i < argc; i++) { /* scan all arguments ... */
	if ((strcmp(argv[i],"-h") == 0) ||(strcmp(argv[i],"-help") == 0))  { Usage(); exit(-1); }
	else if ((strcmp(argv[i],"-v") == 0) || (strcmp(argv[i],"-verbose") == 0)) verbose_flg = TRUE;
	else if ((strcmp(argv[i],"-c") == 0) || (strcmp(argv[i],"-config") == 0)) exit_flg = verbose_flg = TRUE;
        else if (strcmp(argv[i], "-delay") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "%s: GetCmdLineOptions: missing extra wait delay parameter\n", program);
                Usage(); exit(-1);
            }
            delay_flg= atoi(argv[i]);
        }
        else if (strcmp(argv[i], "-jitter") == 0) { i++; jitter_flg = TRUE; jitter_val = atoi(argv[i]); }
	else if (strcmp(argv[i], "-time") == 0) pdelay_flg = TRUE;
        else if ((strcmp(argv[i], "-trace_acq") == 0) || (strcmp(argv[i], "-trace_ctl") == 0)) {
            if (++i >= argc) {
                fprintf(stderr, "%s: GetCmdLineOptions: missing POW equipment number parameter\n", program);
                Usage(); exit(-1);
            }
            trace_gbelm = atoi(argv[i]); /* get global element to trace */
            if (trace_gbelm <= 0) {
                fprintf(stderr, "%s: GetCmdLineOptions: %s: parameter is not a POW equipment number\n", program, argv[i]);
                exit (-1);
            }
            verbose_flg = TRUE;
            if (strcmp(argv[--i], "-trace_acq") == 0) trace_acq_flg = TRUE;
            else trace_ctl_flg = TRUE;
        }
    }

} /* end of GetCmdLineOptions() */


/*====================================================*/
/* Main program                                       */
/* optional: name of the accelerator (PSB or CPS)     */
/*====================================================*/
int main(int argc, char **argv)
{
    int    irpt;
    int    notwaited = 1;
    int    fd = (-1);      /* file descriptor for connect routine */
    int    date_s = 0;

    /* Get program name for error printouts */
#ifdef __linux__
    extern char *__progname;
    program = __progname;
#else
    program = basename(argv[0]);
#endif

    /* Check command line syntax and get optional arguments */
    GetCmdLineOptions(argc, argv);

    /* Get default Telegram to use */
    plstb = gm_getmachine();

    if (plstb == TgmFCT) {
	fprintf(stderr, "%s: Cannot connect POW to FCT telegram\n", program);
	exit(1);
    }

    /* Ignore spurious signals, detach from terminal, change process-grp */
    dsc_rtsetup (argc, argv);

	/* Register program in this DSC: program name, class, variant */
    dsc_register1(program, EQP_POW, 0);

    /* Initialise PLS lines */
    InitPLS();
    
    /* Build the list of various actions */
    BuildActions();

    /* <<< DEBUG info >>> */
    if (verbose_flg) TraceActions();    
    if (exit_flg) exit(1); /* exit task here if 'config' option has been selected */

    if (no_timing) {    /* no interrupts present, loop all 1.2 sec */
	for (;;) {      /* only for non PPM equipment valid */

	  /* Do acquisition and then control actions */
	  for (act = act0; act; act = act->next) DoAcquisition(act);
	  for (act = act0; act; act = act->next) DoControl(act);

	  usleep(1200000);  /* sleep for 1.2 sec */
	}
    }

    /* Connect to all interrupts presented in action list */
    for (act = act0; act; act = act->next) { /* scan all Action objects */
        fd = dsc_rtconnect(act->ci);  /* always the same file descriptor.. */
        fd = dsc_rtconnect(act->ai);  /* ..for connecting to interrupt ci, ai and its */
#ifdef V_VERSION /* VME */	
	fd = dsc_rtconnect(act->its);
#endif	 
    }


    /* 
     * Infinite Acquisition & Control Loop :
     * =====================================
     */
    for (;;) { /* infinite loop ... */

        if (pdelay_flg) {
            if (date_s != 0) {
                fprintf(stderr, "%d ", GetCurrentTimeMsec() - date_s);
                fflush(stderr);
            }
        }

        /* Wait for next interrupt occurrence */
        irpt = dsc_rtwaitit(fd, program);
        notwaited = 1;

        if (pdelay_flg) date_s = GetCurrentTimeMsec();
        
        /* Get PRESENT and NEXT user group values */
        if(GetCurrentUserGroup() != 0) continue; /* skip this cycle */ 
        if(GetNextUserGroup()    != 0) continue;        

#ifdef V_VERSION /* VME */
	/* Reset multiple acquisition parameters for all actions */
        for (act = act0; act; act = act->next) { /* scan all Actions */
	    if (irpt == act->its) ResetAquArray(act);
	}
#endif	
	/* Do all acquisition actions before control actions */    
	for (act = act0; act; act = act->next) { /* scan all Actions */
            if ( (irpt == act->ai) && (act->first == 0) ) {
                if ((delay_flg) && (notwaited)) { usleep(delay_flg*1000); notwaited = 0; }
                DoAcquisition(act);
            }
        }

	/* Now do all control actions */
        for (act = act0; act; act = act->next) {
            if ( (irpt == act->ci) && (act->first == 0) ) DoControl(act);

#ifdef V_VERSION /* VME */
            if ( (irpt == act->ci) && (act->first != 0) ) { /* execute only 1 time */
                /* Request acquisition of hardware MIN/MAX values for 1553 power converters */
                ReqAcquisitionMinMax(act);
                usleep(1000);
                /* Acquire hardware MIN/MAX values for 1553 power converters */
                DoAcquisitionMinMax(act);
                act->first = 0;  /* clear flag */
            }
#endif
        }

#ifndef V_VERSION /* CAMAC */
	    /* for CAMAC power converters, start execute */
	    for (act = act0; act; act = act->next)
		if (irpt == act->ci) DoAddCamacControl(act);
#endif
    }   /* end of infinite loop ... */

return(0);
}       /* end of main() */
