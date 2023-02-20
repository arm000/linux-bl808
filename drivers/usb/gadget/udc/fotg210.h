// SPDX-License-Identifier: GPL-2.0+
/*
 * Faraday FOTG210 USB UDC driver
 *
 * Copyright (C) 2013-2019 Faraday Technology Corporation
 * Author: Faraday CTD/SD Dept.
 */

#include <linux/kthread.h>

/* HW extend the FIFO number from F0~F3 to F0~F15
 * HW version >= v1.30.0 */
//#define EXTEND_FIFO		1

#define FOTG210_MAX_NUM_EP	(4 + 1) /* ep1~4 + ep0 */
#ifdef EXTEND_FIFO
#define FOTG210_MAX_FIFO_NUM	16 /* fifo0...fifo15 */
#else
#define FOTG210_MAX_FIFO_NUM	4 /* fifo0...fifo3 */
#endif

#define FIFO0	0
#define FIFO1	1
#define FIFO2	2
#define FIFO3	3
#define CXFIFO	(FOTG210_MAX_FIFO_NUM)
#ifdef EXTEND_FIFO
#define FIFO4	4
#define FIFO5	5
#define FIFO6	6
#define FIFO7	7
#define FIFO8	8
#define FIFO9	9
#define FIFO10	10
#define FIFO11	11
#define FIFO12	12
#define FIFO13	13
#define FIFO14	14
#define FIFO15	15
#endif

#ifdef CONFIG_USB_FOTG210_OTG
/* Host controller register */
#define FOTG210_USBCMD		0x10
#define USBCMD_RUN		(1 << 0)

/* OTG Control Status Register (0x80) */
#define FOTG210_OTGCSR		0x80
#define OTGCSR_ID		(1 << 21)
#define OTGCSR_CROLE		(1 << 20)
#define OTGCSR_VBUS_VLD		(1 << 19)
#define OTGCSR_B_HNP_EN		(1 << 1)
#define OTGCSR_B_BUS_REQ	(1 << 0)
#endif

/* Global Mask of HC/OTG/DEV interrupt Register (0xC4) */
#define FOTG210_GMIR		0xC4
#define GMIR_INT_POLARITY	(1 << 3) /* Active High */
#define GMIR_MHC_INT		(1 << 2)
#define GMIR_MOTG_INT		(1 << 1)
#define GMIR_MDEV_INT		(1 << 0)

/*  Device Main Control Register (0x100) */
#define FOTG210_DMCR		0x100
#define DMCR_HS_EN		(1 << 6)
#define DMCR_CHIP_EN		(1 << 5)
#define DMCR_SFRST		(1 << 4)
#define DMCR_GOSUSP		(1 << 3)
#define DMCR_GLINT_EN		(1 << 2)
#define DMCR_HALF_SPEED		(1 << 1) /* set 1 when SCLK < 30MHz */
#define DMCR_CAP_RMWAKUP	(1 << 0)

/* Device Address Register (0x104) */
#define FOTG210_DAR		0x104
#define DAR_AFT_CONF		(1 << 7)

/* Device Test Register (0x108) */
#define FOTG210_DTR		0x108
#define DTR_TST_CLRFF		(1 << 0)

/* PHY Test Mode Selector register (0x114) */
#define FOTG210_PHYTMSR		0x114
#define PHYTMSR_TST_PKT		(1 << 4)
#define PHYTMSR_TST_SE0NAK	(1 << 3)
#define PHYTMSR_TST_KSTA	(1 << 2)
#define PHYTMSR_TST_JSTA	(1 << 1)
#define PHYTMSR_UNPLUG		(1 << 0)

/* Cx configuration and FIFO Empty Status register (0x120) */
#define FOTG210_DCFESR		0x120
#define DCFESR_FIFO_EMPTY(fifo)	(1 << 8 << (fifo))
#define DCFESR_CX_EMP		(1 << 5)
#define DCFESR_CX_CLR		(1 << 3)
#define DCFESR_CX_STL		(1 << 2)
#define DCFESR_TST_PKDONE	(1 << 1)
#define DCFESR_CX_DONE		(1 << 0)

/* Device IDLE Counter Register (0x124) */
#define FOTG210_DICR		0x124

/* Device Mask of Interrupt Group Register (0x130) */
#define FOTG210_DMIGR		0x130
#define DMIGR_MINT_G4		(1 << 4)
#define DMIGR_MINT_G3		(1 << 3)
#define DMIGR_MINT_G2		(1 << 2)
#define DMIGR_MINT_G1		(1 << 1)
#define DMIGR_MINT_G0		(1 << 0)

/* Device Mask of Interrupt Source Group 0 (0x134) */
#define FOTG210_DMISGR0		0x134
#define DMISGR0_MCX_COMEND_INT	(1 << 3)
#define DMISGR0_MCX_OUT_INT	(1 << 2)
#define DMISGR0_MCX_IN_INT	(1 << 1)
#define DMISGR0_MCX_SETUP_INT	(1 << 0)

/* Device Mask of Interrupt Source Group 1 Register (0x138)*/
#define FOTG210_DMISGR1		0x138
#define DMISGR1_MFN_IN_INT(fifo)	(1 << (16 + (fifo)))
#define DMISGR1_MFN_OUTSPK_INT(fifo)	(0x3 << (fifo) * 2)

/* Device Mask of Interrupt Source Group 2 Register (0x13C) */
#define FOTG210_DMISGR2		0x13C
#define DMISGR2_MDEV_WAKEUP_VBUS	(1 << 10)
#define DMISGR2_MDEV_IDLE	(1 << 9)
#define DMISGR2_MDMA_ERROR	(1 << 8)
#define DMISGR2_MDMA_CMPLT	(1 << 7)
#ifdef EXTEND_FIFO
#define DMISGR2_MFN_OUTSPK_INT(fifo)	(0x3 << 16 << (((fifo) - 8) * 2))
#endif

/* Device Interrupt group Register (0x140) */
#define FOTG210_DIGR		0x140
#define DIGR_INT_G4		(1 << 4)
#define DIGR_INT_G3		(1 << 3)
#define DIGR_INT_G2		(1 << 2)
#define DIGR_INT_G1		(1 << 1)
#define DIGR_INT_G0		(1 << 0)

/* Device Interrupt Source Group 0 Register (0x144) */
#define FOTG210_DISGR0		0x144
#define DISGR0_CX_COMABT_INT	(1 << 5)
#define DISGR0_CX_COMFAIL_INT	(1 << 4)
#define DISGR0_CX_COMEND_INT	(1 << 3)
#define DISGR0_CX_OUT_INT	(1 << 2)
#define DISGR0_CX_IN_INT	(1 << 1)
#define DISGR0_CX_SETUP_INT	(1 << 0)

/* Device Interrupt Source Group 1 Register (0x148) */
#define FOTG210_DISGR1		0x148
#define DISGR1_FN_IN_INT(fifo)	(1 << 16 << (fifo))
#define DISGR1_FN_OUT_INT(fifo)	(1 << ((fifo) * 2))
#define DISGR1_FN_SPK_INT(fifo)	(1 << 1 << ((fifo) * 2))

/* Device Interrupt Source Group 2 Register (0x14C) */
#define FOTG210_DISGR2		0x14C
#define DISGR2_DMA_ERROR	(1 << 8)
#define DISGR2_DMA_CMPLT	(1 << 7)
#define DISGR2_RX0BYTE_INT	(1 << 6)
#define DISGR2_TX0BYTE_INT	(1 << 5)
#define DISGR2_ISO_SEQ_ABORT_INT	(1 << 4)
#define DISGR2_ISO_SEQ_ERR_INT	(1 << 3)
#define DISGR2_RESM_INT		(1 << 2)
#define DISGR2_SUSP_INT		(1 << 1)
#define DISGR2_USBRST_INT	(1 << 0)
#ifdef EXTEND_FIFO
#define DISGR2_FN_OUT_INT(fifo)	(1 << 16 << (((fifo) - 8) * 2))
#define DISGR2_FN_SPK_INT(fifo)	(1 << 17 << (((fifo) - 8) * 2))
#endif

/* Device Receive Zero-Length Data Packet Register (0x150)*/
#define FOTG210_RX0BYTE		0x150
#define RX0BYTE_EP8		(1 << 7)
#define RX0BYTE_EP7		(1 << 6)
#define RX0BYTE_EP6		(1 << 5)
#define RX0BYTE_EP5		(1 << 4)
#define RX0BYTE_EP4		(1 << 3)
#define RX0BYTE_EP3		(1 << 2)
#define RX0BYTE_EP2		(1 << 1)
#define RX0BYTE_EP1		(1 << 0)

/* Device Transfer Zero-Length Data Packet Register (0x154)*/
#define FOTG210_TX0BYTE		0x154
#define TX0BYTE_EP8		(1 << 7)
#define TX0BYTE_EP7		(1 << 6)
#define TX0BYTE_EP6		(1 << 5)
#define TX0BYTE_EP5		(1 << 4)
#define TX0BYTE_EP4		(1 << 3)
#define TX0BYTE_EP3		(1 << 2)
#define TX0BYTE_EP2		(1 << 1)
#define TX0BYTE_EP1		(1 << 0)

/* Device IN Endpoint x MaxPacketSize Register (0x160+4*(x-1), x=1~8) */
#define FOTG210_INEPMPSR(ep)	(0x160 + 4 * ((ep) - 1))
#define INOUTEPMPSR_MPS(mps)	((mps) & 0x7FF)
#define INOUTEPMPSR_STL_EP	(1 << 11)
#define INOUTEPMPSR_RESET_TSEQ	(1 << 12)
#define INEPMPSR_TX_NUM_HBW(num)	((num) << 13)
#define INEPMPSR_TX0BYTE	(1 << 15)

/* Device OUT Endpoint x MaxPacketSize Register (0x180+4*(x-1), x=1~8) */
#define FOTG210_OUTEPMPSR(ep)	(0x180 + 4 * ((ep) - 1))

/* Device Endpoint 1~4 Map Register (0x1A0) */
#define FOTG210_EPMAP		0x1A0
#define EPMAP_FIFONO(fifo, ep, dir)		\
	(((fifo) << ((ep) - 1) * 8) << ((dir) ? 0 : 4))
#ifndef EXTEND_FIFO
#define EPMAP_FIFONOMSK(ep, dir)	\
	((3 << ((ep) - 1) * 8) << ((dir) ? 0 : 4))
#else
#define EPMAP_FIFONOMSK(ep, dir)	\
	((0xF << ((ep) - 1) * 8) << ((dir) ? 0 : 4))
#endif

/* Device Endpoint 5~8 Map Register (0x1A4) */
#define FOTG210_EPMAP2		0x1A4

/* Device FIFO 0~3 Map Register (0x1A8) */
#define FOTG210_FIFOMAP		0x1A8
#define FIFOMAP_DIROUT(fifo)	(0x0 << 4 << ((fifo) * 8))
#define FIFOMAP_DIRIN(fifo)	(0x1 << 4 << ((fifo) * 8))
#define FIFOMAP_BIDIR(fifo)	(0x2 << 4 << ((fifo) * 8))
#define FIFOMAP_NA(fifo)	(0x3 << 4 << ((fifo) * 8))
#define FIFOMAP_EPNO(fifo, ep)	((ep) << ((fifo) * 8))
#define FIFOMAP_EPNOMSK(fifo)	(0xF << ((fifo) * 8))
#ifdef EXTEND_FIFO
/* Device FIFO 4~7 Map Register (0x1D8) */
#define FOTG210_FIFOMAP2		0x1D8
/* Device FIFO 8~11 Map Register (0x1E0) */
#define FOTG210_FIFOMAP3		0x1E0
/* Device FIFO 12~15 Map Register (0x1E8) */
#define FOTG210_FIFOMAP4		0x1E8
#endif

/* Device FIFO 0~3 Confuguration Register (0x1AC) */
#define FOTG210_FIFOCF		0x1AC
#define FIFOCF_TYPE(type, fifo)	((type) << ((fifo) * 8))
#define FIFOCF_BLK_SIN(fifo)	(0x0 << ((fifo) * 8) << 2)
#define FIFOCF_BLK_DUB(fifo)	(0x1 << ((fifo) * 8) << 2)
#define FIFOCF_BLK_TRI(fifo)	(0x2 << ((fifo) * 8) << 2)
#define FIFOCF_BLKSZ_512(fifo)	(0x0 << ((fifo) * 8) << 4)
#define FIFOCF_BLKSZ_1024(fifo)	(0x1 << ((fifo) * 8) << 4)
#define FIFOCF_FIFO_EN(fifo)	(0x1 << ((fifo) * 8) << 5)
#ifdef EXTEND_FIFO
/* Device FIFO 4~7 Confuguration Register (0x1DC) */
#define FOTG210_FIFOCF2		0x1DC
/* Device FIFO 8~11 Confuguration Register (0x1E4) */
#define FOTG210_FIFOCF3		0x1E4
/* Device FIFO 12~15 Confuguration Register (0x1EC) */
#define FOTG210_FIFOCF4		0x1EC
#endif

/* Device FIFO 0~3 Instruction and Byte Count Register (0x1B0+4*n, n=0~3) */
#define FOTG210_FIBCR(fifo)	(0x1B0 + (fifo) * 4)
#define FIBCR_BCFX		0x7FF
#define FIBCR_FFRST		(1 << 12)
#ifdef EXTEND_FIFO
/* Device FIFO 4~15 Instruction and Byte Count Register (0x1F0+4*(n-4), n=4~15) */
#define FOTG210_FIBCR_EX(fifo)	(0x1F0 + ((fifo) -4) * 4)
#endif

/* Device DMA Target FIFO Number Register (0x1C0) */
#define FOTG210_DMATFNR		0x1C0
#define DMATFNR_ACC_CXF		(1 << 4)
#define DMATFNR_ACC_FN(fifo)	(1 << (fifo))
#define DMATFNR_DISDMA		0
#ifdef EXTEND_FIFO
#define DMATFNR_ACC_FN_EX(fifo)	(1 << ((fifo) + 1))
#endif

/* Device DMA Controller Parameter setting 1 Register (0x1C8) */
#define FOTG210_DMACPSR1	0x1C8
#define DMACPSR1_DMA_LEN(len)	(((len) & 0x1FFFF) << 8)
#define DMACPSR1_DMA_ABORT	(1 << 3)
#define DMACPSR1_DMA_TYPE(dir_in)	(((dir_in) ? 1 : 0) << 1)
#define DMACPSR1_DMA_START	(1 << 0)

/* Device DMA Controller Parameter setting 2 Register (0x1CC) */
#define FOTG210_DMACPSR2	0x1CC

/* Device DMA Controller Parameter setting 3 Register (0x1D0) */
#define FOTG210_CXPORT		0x1D0

// --------------- VIRTUAL_DMA (VDMA) -----------------------//
/* Device Virtual DMA CXF Parameter setting 1 Register (0x300) */
#define FOTG210_VDMA_CXFPS1		0x300
/* Device Virtual DMA CXF Parameter setting 2 Register (0x304) */
#define FOTG210_VDMA_CXFPS2		0x304

/* Device Virtual DMA FIFO 0~3 Parameter setting 1 Register (0x308+8*n, n=0~3) */
#define FOTG210_VDMA_FNPS1(fifo)		(0x308 + (fifo) * 8)
#define VDMAFPS1_VDMA_LEN(len)		(((len) & 0x1FFFF) << 8)
#define VDMAFPS1_VDMA_IO		(1 << 2)
#define VDMAFPS1_VDMA_TYPE(dir_in)		(((dir_in) ? 1 : 0) << 1)
#define VDMAFPS1_VDMA_START		(1 << 0)
#define GET_VDMAFPS1_VDMA_LEN(p)	(((p) & (0x1FFFF << 8)) >> 8)
#ifdef EXTEND_FIFO
/* Device Virtual DMA FIFO 4~15 Parameter setting 1 Register (0x350+8*(n-4), n=4~15) */
#define FOTG210_VDMA_FNPS1_EX(fifo)		(0x350 + ((fifo) - 4) * 8)
#endif

/* Device Virtual DMA FIFO 0~3 Parameter setting 2 Register (0x30C+8*n, n=0~3) */
#define FOTG210_VDMA_FNPS2(fifo)		(0x30C + (fifo) * 8)
#ifdef EXTEND_FIFO
/* Device Virtual DMA FIFO 4~15 Parameter setting 2 Register (0x354+8*(n-4), n=4~15) */
#define FOTG210_VDMA_FNPS2_EX(fifo)		(0x354 + ((fifo) - 4) * 8)
#endif

/* Device Interrupt Source Group 3 Register (0x328) */
#define FOTG210_DISGR3		0x328
#define DISGR3_VDMA_ERROR_FN(fifo)		(1 << ((fifo) + 17))
#define DISGR3_VDMA_ERROR_CXF		(1 << 16)
#define DISGR3_VDMA_CMPLT_FN(fifo)		(1 << ((fifo) + 1))
#define DISGR3_VDMA_CMPLT_CXF		(1 << 0)

/* Device Mask of Interrupt Source Group 3 Register (0x32C) */
#define FOTG210_DMISGR3		0x32C
#define DMISGR3_MVDMA_ERROR_FN(fifo)		(1 << ((fifo) + 17))
#define DMISGR3_MVDMA_ERROR_CXF		(1 << 16)
#define DMISGR3_MVDMA_CMPLT_FN(fifo)		(1 << ((fifo) + 1))
#define DMISGR3_MVDMA_CMPLT_CXF		(1 << 0)

/* Device Virtual DMA Control Register (0x330) */
#define FOTG210_VDMA_CTRL		0x330
#define VDMA_EN		(1 << 0)

/* Device Interrupt Source Group 4 Register (0x338) */
#define FOTG210_DISGR4		0x338
#define DISGR4_L1_INT		(1 << 0)
#ifdef EXTEND_FIFO
#define DISGR4_VDMA_ERROR_FN(fifo)	(1 << 24 << ((fifo) - 15))
#define DISGR4_VDMA_CMPLT_FN(fifo)	(1 << 16 << ((fifo) - 15))
#endif

/* Device Mask of Interrupt Source Group 4 Register (0x33C) */
#define FOTG210_DMISGR4		0x33C
#define DMISGR4_ML1_INT		(1 << 0)
#ifdef EXTEND_FIFO
#define DMISGR4_MVDMA_ERROR_FN(fifo)	(1 << 24 << ((fifo) - 15))
#define DMISGR4_MVDMA_CMPLT_FN(fifo)	(1 << 16 << ((fifo) - 15))
#endif


struct fotg210_request {
	struct usb_request	req;
	struct list_head	queue;
};

struct fotg210_irq_task {
	dma_addr_t		dma_addr;
	u32			transfer_length;
	struct fotg210_request		*req;
};

struct fotg210_ep {
	struct usb_ep		ep;
	struct fotg210_udc	*fotg210;

	struct list_head	queue;
	unsigned		stall:1;
	unsigned		wedged:1;
	unsigned		use_dma:1;

	unsigned char		epnum;
	unsigned char		type; /* transfer type: bulk/interrupt/iso. */
	unsigned char		dir_in;
	unsigned char		fifonum; /* This ep map to which FIFO number (FIFO 0~3) */
	struct fotg210_irq_task		*irq_current_task;
};

struct fotg210_udc {
	spinlock_t		lock; /* protect the struct */
	void __iomem		*reg;

	struct usb_gadget		gadget;
	struct usb_gadget_driver	*driver;

	struct fotg210_ep	*ep[FOTG210_MAX_NUM_EP];

	struct usb_request	*ep0_req;	/* for internal request */
	__le16			ep0_data;
	u32			ep0_length;	/* for set_feature(TEST_PACKET) */
};

#define gadget_to_fotg210(g)	container_of((g), struct fotg210_udc, gadget)
