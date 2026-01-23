/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (c) 2025 Axelera AI. All rights reserved. */

#ifndef TRITON_PCIE_HDMA_H
#define TRITON_PCIE_HDMA_H

#define __packed __attribute__((__packed__))

#ifndef BIT
#define BIT(nr) (1 << (nr))
#endif

#define HDMA_V0_LOCAL_ABORT_INT_EN  BIT(6)
#define HDMA_V0_REMOTE_ABORT_INT_EN BIT(5)
#define HDMA_V0_LOCAL_STOP_INT_EN   BIT(4)
#define HDMA_V0_REMOTE_STOP_INT_EN  BIT(3)
#define HDMA_V0_ABORT_INT_MASK	    BIT(2)
#define HDMA_V0_STOP_INT_MASK	    BIT(0)
#define HDMA_V0_LINKLIST_EN	    BIT(0)
#define HDMA_V0_CONSUMER_CYCLE_STAT BIT(1)
#define HDMA_V0_CONSUMER_CYCLE_BIT  BIT(0)
#define HDMA_V0_DOORBELL_START	    BIT(0)
#define HDMA_V0_CH_STATUS_MASK	    GENMASK(1, 0)

#define HDMA_DESC_BASE 0x0000000008000000
#define HDMA_DESC_SIZE 0x0000000002000000
#define HDMA_LINKED_LIST_DESC_OFF \
	(HDMA_DESC_SIZE - sizeof(struct dw_hdma_ll_buf))
#define HDMA_LINKED_LIST_DESC_BASE (HDMA_DESC_BASE + HDMA_LINKED_LIST_DESC_OFF)

enum dw_hdma_dir { DW_HDMA_DIR_READ, DW_HDMA_DIR_WRITE };

// HDMA Watermark Register fields/bits
enum {
	WATERMARK_RWIE = BIT(0),
	WATERMARK_LWIE = BIT(1),
};

// HDMA Status Register values
enum {
	STATUS_REG_RUNNING = 0x1, // Channel is active and transferring data
	STATUS_REG_ABORTED =
		0x2, // An error condition is detected, and the HDMA has stopped this channel
	STATUS_REG_STOPPED =
		0x3 // The HDMA has transferred all data for this channel
};

// HDMA Interrupt Status Register fields/bits
enum {
	INT_STATUS_STOP = BIT(0),
	INT_STATUS_WATERMARK = BIT(1),
	INT_STATUS_ABORT = BIT(2),
	INT_STATUS_ERROR = BIT(3)
};

// HDMA Interrupt Setup Register fields/bits
enum {
	INT_SETUP_STOP_MASK = BIT(0),
	INT_SETUP_WATERMARK_MASK = BIT(1),
	INT_SETUP_ABORT_MASK = BIT(2),
	INT_SETUP_RSIE = BIT(3),
	INT_SETUP_LSIE = BIT(4),
	INT_SETUP_RAIE = BIT(5),
	INT_SETUP_LAIE = BIT(6)
};

// HDMA Control1 Register fields/bits
enum {
	CONTROL1_LLEN = BIT(0), // Linked List Enable
};

// In LL-mode: control d-word of data element
enum {
	DW_HDMA_V0_CB = BIT(0),
	DW_HDMA_V0_LLP = BIT(2),
	DW_HDMA_V0_LWIE = BIT(3),
	DW_HDMA_V0_RIE = BIT(4)
};

struct dw_hdma_v0_ch_regs {
	u32 ch_en; /* 0x0000 */
	u32 doorbell; /* 0x0004 */
	u32 prefetch; /* 0x0008 */
	u32 handshake; /* 0x000c */
	union {
		u64 reg; /* 0x0010..0x0014 */
		struct {
			u32 lsb; /* 0x0010 */
			u32 msb; /* 0x0014 */
		};
	} llp;
	u32 cycle_sync; /* 0x0018 */
	u32 transfer_size; /* 0x001c */
	union {
		u64 reg; /* 0x0020..0x0024 */
		struct {
			u32 lsb; /* 0x0020 */
			u32 msb; /* 0x0024 */
		};
	} sar;
	union {
		u64 reg; /* 0x0028..0x002c */
		struct {
			u32 lsb; /* 0x0028 */
			u32 msb; /* 0x002c */
		};
	} dar;
	u32 watermark_en; /* 0x0030 */
	u32 control1; /* 0x0034 */
	u32 func_num; /* 0x0038 */
	u32 qos; /* 0x003c */
	u32 padding_1[16]; /* 0x0040..0x007c */
	u32 ch_stat; /* 0x0080 */
	u32 int_stat; /* 0x0084 */
	u32 int_setup; /* 0x0088 */
	u32 int_clear; /* 0x008c */
	union {
		u64 reg; /* 0x0090..0x0094 */
		struct {
			u32 lsb; /* 0x0090 */
			u32 msb; /* 0x0094 */
		};
	} msi_stop;
	union {
		u64 reg; /* 0x0098..0x009c */
		struct {
			u32 lsb; /* 0x0098 */
			u32 msb; /* 0x009c */
		};
	} msi_watermark;
	union {
		u64 reg; /* 0x00a0..0x00a4 */
		struct {
			u32 lsb; /* 0x00a0 */
			u32 msb; /* 0x00a4 */
		};
	} msi_abort;
	u32 msi_msgdata; /* 0x00a8 */
	u32 padding_2[21]; /* 0x00ac..0x00fc */
} __packed;

struct dw_hdma_v0_ch {
	struct dw_hdma_v0_ch_regs wr; /* 0x0000 */
	struct dw_hdma_v0_ch_regs rd; /* 0x0100 */
} __packed;

struct dw_hdma_v0_regs {
	struct dw_hdma_v0_ch ch[HDMA_V0_MAX_NR_CH]; /* 0x0000..0x0fa8 */
} __packed;

#define DESC_MEM_SIZE (32 * 1024)
#define DW_HDMA_LL_NUM \
	DESC_MEM_SIZE / (2 * HDMA_V0_MAX_NR_CH * sizeof(struct dw_hdma_v0_lli))
#define DW_HDMA_LL_MAX_NUM (DW_HDMA_LL_NUM - 3)

struct dw_hdma_v0_lli {
	u32 control;
	u32 transfer_size;
	union {
		u64 reg;
		struct {
			u32 lsb;
			u32 msb;
		};
	} sar;
	union {
		u64 reg;
		struct {
			u32 lsb;
			u32 msb;
		};
	} dar;
} __packed;

struct dw_hdma_v0_llp {
	u32 control;
	u32 reserved;
	union {
		u64 reg;
		struct {
			u32 lsb;
			u32 msb;
		};
	} llp;
} __packed;

struct dw_hdma_lld_ch {
	struct dw_hdma_v0_lli rd[DW_HDMA_LL_NUM];
	struct dw_hdma_v0_lli wr[DW_HDMA_LL_NUM];
} __packed;

struct dw_hdma_ll_buf {
	struct dw_hdma_lld_ch ch[HDMA_V0_MAX_NR_CH];
} __packed;

enum {
	HDMA_EN_OFF_WRCH_0 = 0x000,
	HDMA_DOORBELL_OFF_WRCH_0 = 0x004,
	HDMA_ELEM_PF_OFF_WRCH_0 = 0x008,
	HDMA_HANDSHAKE_OFF_WRCH_0 = 0x00C,
	HDMA_LLP_LOW_OFF_WRCH_0 = 0x010,
	HDMA_LLP_HIGH_OFF_WRCH_0 = 0x014,
	HDMA_CYCLE_OFF_WRCH_0 = 0x018,
	HDMA_XFERSIZE_OFF_WRCH_0 = 0x01C,
	HDMA_SAR_LOW_OFF_WRCH_0 = 0x020,
	HDMA_SAR_HIGH_OFF_WRCH_0 = 0x024,
	HDMA_DAR_LOW_OFF_WRCH_0 = 0x028,
	HDMA_DAR_HIGH_OFF_WRCH_0 = 0x02C,
	HDMA_WATERMARK_EN_OFF_WRCH_0 = 0x030,
	HDMA_CONTROL1_OFF_WRCH_0 = 0x034,
	HDMA_FUNC_NUM_OFF_WRCH_0 = 0x038,
	HDMA_QOS_OFF_WRCH_0 = 0x03C,
	HDMA_STATUS_OFF_WRCH_0 = 0x080,
	HDMA_INT_STATUS_OFF_WRCH_0 = 0x084,
	HDMA_INT_SETUP_OFF_WRCH_0 = 0x088,
	HDMA_INT_CLEAR_OFF_WRCH_0 = 0x08C,
	HDMA_MSI_STOP_LOW_OFF_WRCH_0 = 0x090,
	HDMA_MSI_STOP_HIGH_OFF_WRCH_0 = 0x094,
	HDMA_MSI_WATERMARK_LOW_OFF_WRCH_0 = 0x098,
	HDMA_MSI_WATERMARK_HIGH_OFF_WRCH_0 = 0x09C,
	HDMA_MSI_ABORT_LOW_OFF_WRCH_0 = 0x0A0,
	HDMA_MSI_ABORT_HIGH_OFF_WRCH_0 = 0x0A4,
	HDMA_MSI_MSGD_OFF_WRCH_0 = 0x0A8,

	HDMA_EN_OFF_RDCH_0 = 0x100,
	HDMA_DOORBELL_OFF_RDCH_0 = 0x104,
	HDMA_ELEM_PF_OFF_RDCH_0 = 0x108,
	HDMA_HANDSHAKE_OFF_RDCH_0 = 0x10C,
	HDMA_LLP_LOW_OFF_RDCH_0 = 0x110,
	HDMA_LLP_HIGH_OFF_RDCH_0 = 0x114,
	HDMA_CYCLE_OFF_RDCH_0 = 0x118,
	HDMA_XFERSIZE_OFF_RDCH_0 = 0x11C,
	HDMA_SAR_LOW_OFF_RDCH_0 = 0x120,
	HDMA_SAR_HIGH_OFF_RDCH_0 = 0x124,
	HDMA_DAR_LOW_OFF_RDCH_0 = 0x128,
	HDMA_DAR_HIGH_OFF_RDCH_0 = 0x12C,
	HDMA_WATERMARK_EN_OFF_RDCH_0 = 0x130,
	HDMA_CONTROL1_OFF_RDCH_0 = 0x134,
	HDMA_FUNC_NUM_OFF_RDCH_0 = 0x138,
	HDMA_QOS_OFF_RDCH_0 = 0x13C,
	HDMA_STATUS_OFF_RDCH_0 = 0x180,
	HDMA_INT_STATUS_OFF_RDCH_0 = 0x184,
	HDMA_INT_SETUP_OFF_RDCH_0 = 0x188,
	HDMA_INT_CLEAR_OFF_RDCH_0 = 0x18C,
	HDMA_MSI_STOP_LOW_OFF_RDCH_0 = 0x190,
	HDMA_MSI_STOP_HIGH_OFF_RDCH_0 = 0x194,
	HDMA_MSI_WATERMARK_LOW_OFF_RDCH_0 = 0x198,
	HDMA_MSI_WATERMARK_HIGH_OFF_RDCH_0 = 0x19C,
	HDMA_MSI_ABORT_LOW_OFF_RDCH_0 = 0x1A0,
	HDMA_MSI_ABORT_HIGH_OFF_RDCH_0 = 0x1A4,
	HDMA_MSI_MSGD_OFF_RDCH_0 = 0x1A8,

	HDMA_EN_OFF_WRCH_1 = 0x200,
	HDMA_DOORBELL_OFF_WRCH_1 = 0x204,
	HDMA_ELEM_PF_OFF_WRCH_1 = 0x208,
	HDMA_HANDSHAKE_OFF_WRCH_1 = 0x20C,
	HDMA_LLP_LOW_OFF_WRCH_1 = 0x210,
	HDMA_LLP_HIGH_OFF_WRCH_1 = 0x214,
	HDMA_CYCLE_OFF_WRCH_1 = 0x218,
	HDMA_XFERSIZE_OFF_WRCH_1 = 0x21C,
	HDMA_SAR_LOW_OFF_WRCH_1 = 0x220,
	HDMA_SAR_HIGH_OFF_WRCH_1 = 0x224,
	HDMA_DAR_LOW_OFF_WRCH_1 = 0x228,
	HDMA_DAR_HIGH_OFF_WRCH_1 = 0x22C,
	HDMA_WATERMARK_EN_OFF_WRCH_1 = 0x230,
	HDMA_CONTROL1_OFF_WRCH_1 = 0x234,
	HDMA_FUNC_NUM_OFF_WRCH_1 = 0x238,
	HDMA_QOS_OFF_WRCH_1 = 0x23C,
	HDMA_STATUS_OFF_WRCH_1 = 0x280,
	HDMA_INT_STATUS_OFF_WRCH_1 = 0x284,
	HDMA_INT_SETUP_OFF_WRCH_1 = 0x288,
	HDMA_INT_CLEAR_OFF_WRCH_1 = 0x28C,
	HDMA_MSI_STOP_LOW_OFF_WRCH_1 = 0x290,
	HDMA_MSI_STOP_HIGH_OFF_WRCH_1 = 0x294,
	HDMA_MSI_WATERMARK_LOW_OFF_WRCH_1 = 0x298,
	HDMA_MSI_WATERMARK_HIGH_OFF_WRCH_1 = 0x29C,
	HDMA_MSI_ABORT_LOW_OFF_WRCH_1 = 0x2A0,
	HDMA_MSI_ABORT_HIGH_OFF_WRCH_1 = 0x2A4,
	HDMA_MSI_MSGD_OFF_WRCH_1 = 0x2A8,

	HDMA_EN_OFF_RDCH_1 = 0x300,
	HDMA_DOORBELL_OFF_RDCH_1 = 0x304,
	HDMA_ELEM_PF_OFF_RDCH_1 = 0x308,
	HDMA_HANDSHAKE_OFF_RDCH_1 = 0x30C,
	HDMA_LLP_LOW_OFF_RDCH_1 = 0x310,
	HDMA_LLP_HIGH_OFF_RDCH_1 = 0x314,
	HDMA_CYCLE_OFF_RDCH_1 = 0x318,
	HDMA_XFERSIZE_OFF_RDCH_1 = 0x31C,
	HDMA_SAR_LOW_OFF_RDCH_1 = 0x320,
	HDMA_SAR_HIGH_OFF_RDCH_1 = 0x324,
	HDMA_DAR_LOW_OFF_RDCH_1 = 0x328,
	HDMA_DAR_HIGH_OFF_RDCH_1 = 0x32C,
	HDMA_WATERMARK_EN_OFF_RDCH_1 = 0x330,
	HDMA_CONTROL1_OFF_RDCH_1 = 0x334,
	HDMA_FUNC_NUM_OFF_RDCH_1 = 0x338,
	HDMA_QOS_OFF_RDCH_1 = 0x33C,
	HDMA_STATUS_OFF_RDCH_1 = 0x380,
	HDMA_INT_STATUS_OFF_RDCH_1 = 0x384,
	HDMA_INT_SETUP_OFF_RDCH_1 = 0x388,
	HDMA_INT_CLEAR_OFF_RDCH_1 = 0x38C,
	HDMA_MSI_STOP_LOW_OFF_RDCH_1 = 0x390,
	HDMA_MSI_STOP_HIGH_OFF_RDCH_1 = 0x394,
	HDMA_MSI_WATERMARK_LOW_OFF_RDCH_1 = 0x398,
	HDMA_MSI_WATERMARK_HIGH_OFF_RDCH_1 = 0x39C,
	HDMA_MSI_ABORT_LOW_OFF_RDCH_1 = 0x3A0,
	HDMA_MSI_ABORT_HIGH_OFF_RDCH_1 = 0x3A4,
	HDMA_MSI_MSGD_OFF_RDCH_1 = 0x3A8,

	HDMA_EN_OFF_WRCH_2 = 0x400,
	HDMA_DOORBELL_OFF_WRCH_2 = 0x404,
	HDMA_ELEM_PF_OFF_WRCH_2 = 0x408,
	HDMA_HANDSHAKE_OFF_WRCH_2 = 0x40C,
	HDMA_LLP_LOW_OFF_WRCH_2 = 0x410,
	HDMA_LLP_HIGH_OFF_WRCH_2 = 0x414,
	HDMA_CYCLE_OFF_WRCH_2 = 0x418,
	HDMA_XFERSIZE_OFF_WRCH_2 = 0x41C,
	HDMA_SAR_LOW_OFF_WRCH_2 = 0x420,
	HDMA_SAR_HIGH_OFF_WRCH_2 = 0x424,
	HDMA_DAR_LOW_OFF_WRCH_2 = 0x428,
	HDMA_DAR_HIGH_OFF_WRCH_2 = 0x42C,
	HDMA_WATERMARK_EN_OFF_WRCH_2 = 0x430,
	HDMA_CONTROL1_OFF_WRCH_2 = 0x434,
	HDMA_FUNC_NUM_OFF_WRCH_2 = 0x438,
	HDMA_QOS_OFF_WRCH_2 = 0x43C,
	HDMA_STATUS_OFF_WRCH_2 = 0x480,
	HDMA_INT_STATUS_OFF_WRCH_2 = 0x484,
	HDMA_INT_SETUP_OFF_WRCH_2 = 0x488,
	HDMA_INT_CLEAR_OFF_WRCH_2 = 0x48C,
	HDMA_MSI_STOP_LOW_OFF_WRCH_2 = 0x490,
	HDMA_MSI_STOP_HIGH_OFF_WRCH_2 = 0x494,
	HDMA_MSI_WATERMARK_LOW_OFF_WRCH_2 = 0x498,
	HDMA_MSI_WATERMARK_HIGH_OFF_WRCH_2 = 0x49C,
	HDMA_MSI_ABORT_LOW_OFF_WRCH_2 = 0x4A0,
	HDMA_MSI_ABORT_HIGH_OFF_WRCH_2 = 0x4A4,
	HDMA_MSI_MSGD_OFF_WRCH_2 = 0x4A8,

	HDMA_ELEM_PF_OFF_RDCH_2 = 0x508,
	HDMA_EN_OFF_RDCH_2 = 0x500,
	HDMA_DOORBELL_OFF_RDCH_2 = 0x504,
	HDMA_HANDSHAKE_OFF_RDCH_2 = 0x50C,
	HDMA_LLP_LOW_OFF_RDCH_2 = 0x510,
	HDMA_LLP_HIGH_OFF_RDCH_2 = 0x514,
	HDMA_CYCLE_OFF_RDCH_2 = 0x518,
	HDMA_XFERSIZE_OFF_RDCH_2 = 0x51C,
	HDMA_SAR_LOW_OFF_RDCH_2 = 0x520,
	HDMA_SAR_HIGH_OFF_RDCH_2 = 0x524,
	HDMA_DAR_LOW_OFF_RDCH_2 = 0x528,
	HDMA_DAR_HIGH_OFF_RDCH_2 = 0x52C,
	HDMA_WATERMARK_EN_OFF_RDCH_2 = 0x530,
	HDMA_CONTROL1_OFF_RDCH_2 = 0x534,
	HDMA_FUNC_NUM_OFF_RDCH_2 = 0x538,
	HDMA_QOS_OFF_RDCH_2 = 0x53C,
	HDMA_STATUS_OFF_RDCH_2 = 0x580,
	HDMA_INT_STATUS_OFF_RDCH_2 = 0x584,
	HDMA_INT_SETUP_OFF_RDCH_2 = 0x588,
	HDMA_INT_CLEAR_OFF_RDCH_2 = 0x58C,
	HDMA_MSI_STOP_LOW_OFF_RDCH_2 = 0x590,
	HDMA_MSI_STOP_HIGH_OFF_RDCH_2 = 0x594,
	HDMA_MSI_WATERMARK_LOW_OFF_RDCH_2 = 0x598,
	HDMA_MSI_WATERMARK_HIGH_OFF_RDCH_2 = 0x59C,
	HDMA_MSI_ABORT_LOW_OFF_RDCH_2 = 0x5A0,
	HDMA_MSI_ABORT_HIGH_OFF_RDCH_2 = 0x5A4,
	HDMA_MSI_MSGD_OFF_RDCH_2 = 0x5A8,

	HDMA_EN_OFF_WRCH_3 = 0x600,
	HDMA_DOORBELL_OFF_WRCH_3 = 0x604,
	HDMA_ELEM_PF_OFF_WRCH_3 = 0x608,
	HDMA_HANDSHAKE_OFF_WRCH_3 = 0x60C,
	HDMA_LLP_LOW_OFF_WRCH_3 = 0x610,
	HDMA_LLP_HIGH_OFF_WRCH_3 = 0x614,
	HDMA_CYCLE_OFF_WRCH_3 = 0x618,
	HDMA_XFERSIZE_OFF_WRCH_3 = 0x61C,
	HDMA_SAR_LOW_OFF_WRCH_3 = 0x620,
	HDMA_SAR_HIGH_OFF_WRCH_3 = 0x624,
	HDMA_DAR_LOW_OFF_WRCH_3 = 0x628,
	HDMA_DAR_HIGH_OFF_WRCH_3 = 0x62C,
	HDMA_WATERMARK_EN_OFF_WRCH_3 = 0x630,
	HDMA_CONTROL1_OFF_WRCH_3 = 0x634,
	HDMA_FUNC_NUM_OFF_WRCH_3 = 0x638,
	HDMA_QOS_OFF_WRCH_3 = 0x63C,
	HDMA_STATUS_OFF_WRCH_3 = 0x680,
	HDMA_INT_STATUS_OFF_WRCH_3 = 0x684,
	HDMA_INT_SETUP_OFF_WRCH_3 = 0x688,
	HDMA_INT_CLEAR_OFF_WRCH_3 = 0x68C,
	HDMA_MSI_STOP_LOW_OFF_WRCH_3 = 0x690,
	HDMA_MSI_STOP_HIGH_OFF_WRCH_3 = 0x694,
	HDMA_MSI_WATERMARK_LOW_OFF_WRCH_3 = 0x698,
	HDMA_MSI_WATERMARK_HIGH_OFF_WRCH_3 = 0x69C,
	HDMA_MSI_ABORT_LOW_OFF_WRCH_3 = 0x6A0,
	HDMA_MSI_ABORT_HIGH_OFF_WRCH_3 = 0x6A4,
	HDMA_MSI_MSGD_OFF_WRCH_3 = 0x6A8,

	HDMA_EN_OFF_RDCH_3 = 0x700,
	HDMA_DOORBELL_OFF_RDCH_3 = 0x704,
	HDMA_ELEM_PF_OFF_RDCH_3 = 0x708,
	HDMA_HANDSHAKE_OFF_RDCH_3 = 0x70C,
	HDMA_LLP_LOW_OFF_RDCH_3 = 0x710,
	HDMA_LLP_HIGH_OFF_RDCH_3 = 0x714,
	HDMA_CYCLE_OFF_RDCH_3 = 0x718,
	HDMA_XFERSIZE_OFF_RDCH_3 = 0x71C,
	HDMA_SAR_LOW_OFF_RDCH_3 = 0x720,
	HDMA_SAR_HIGH_OFF_RDCH_3 = 0x724,
	HDMA_DAR_LOW_OFF_RDCH_3 = 0x728,
	HDMA_DAR_HIGH_OFF_RDCH_3 = 0x72C,
	HDMA_WATERMARK_EN_OFF_RDCH_3 = 0x730,
	HDMA_CONTROL1_OFF_RDCH_3 = 0x734,
	HDMA_FUNC_NUM_OFF_RDCH_3 = 0x738,
	HDMA_QOS_OFF_RDCH_3 = 0x73C,
	HDMA_STATUS_OFF_RDCH_3 = 0x780,
	HDMA_INT_STATUS_OFF_RDCH_3 = 0x784,
	HDMA_INT_SETUP_OFF_RDCH_3 = 0x788,
	HDMA_INT_CLEAR_OFF_RDCH_3 = 0x78C,
	HDMA_MSI_STOP_LOW_OFF_RDCH_3 = 0x790,
	HDMA_MSI_STOP_HIGH_OFF_RDCH_3 = 0x794,
	HDMA_MSI_WATERMARK_LOW_OFF_RDCH_3 = 0x798,
	HDMA_MSI_WATERMARK_HIGH_OFF_RDCH_3 = 0x79C,
	HDMA_MSI_ABORT_LOW_OFF_RDCH_3 = 0x7A0,
	HDMA_MSI_ABORT_HIGH_OFF_RDCH_3 = 0x7A4,
	HDMA_MSI_MSGD_OFF_RDCH_3 = 0x7A8,
};

#define _LSB(x) (uint32_t)(x)
#define _MSB(x) (uint32_t)((x) >> 32)

#define SET_RW_32_CH(hdma, dir, name, ch, value) \
	writel(value, &(__hdma_ch(hdma, dir, ch)->name))

#define GET_RW_32_CH(hdma, dir, name, ch) \
	readl(&(__hdma_ch(hdma, dir, ch)->name))

static inline uint64_t __get_ll_base(volatile struct dw_hdma_ll_buf *hwlldch,
				     enum dw_hdma_dir dir, int ch)
{
	if (dir == DW_HDMA_DIR_WRITE)
		return (uint64_t)(&hwlldch->ch[ch].wr[0].control);

	return (uint64_t)(&hwlldch->ch[ch].rd[0].control);
}
static inline volatile struct dw_hdma_v0_ch_regs __iomem *
__hdma_ch(volatile struct dw_hdma_v0_regs *hdma, enum dw_hdma_dir dir, int ch)
{
	if (dir == DW_HDMA_DIR_WRITE)
		return &hdma->ch[ch].wr;

	return &hdma->ch[ch].rd;
}

#define SET_32(hdma, name, value) writel(value, &(hdma->name))
#define SET_RW_32(hdma, dir, name, value)               \
	do {                                            \
		if (dir == DW_HDMA_DIR_WRITE)           \
			SET_32(hdma, wr_##name, value); \
		else                                    \
			SET_32(hdma, rd_##name, value); \
	} while (0)

#define GET_32(hdma, name) readl(&(hdma->name))
#define GET_RW_32(hdma, dir, name)                                \
	(((dir) == DW_HDMA_DIR_WRITE) ? GET_32(hdma, wr_##name) : \
					GET_32(hdma, rd_##name))

#define GET_64(hdma, name) readq(&(hdma->name))

#define SET_64(hdma, name, value) writeq(value, &(hdma->name))

#define GET_32_CH(hdma, name, channel) readl(&(hdma->ch[channel].name))

#define SET_32_CH(hdma, name, channel, value) \
	writel(value, &(hdma->ch[channel].name))

#define GET_64_CH(hdma, name, channel) readq(&(hdma->ch[channel].name))

#define SET_64_CH(hdma, name, channel, value) \
	writeq(value, &(hdma->ch[channel].name))

#define LL_SET_RW_32_CH(ll, dir, channel, index, name, value)           \
	do {                                                            \
		if (dir == DW_HDMA_DIR_WRITE)                           \
			writel(value, &ll->ch[channel].wr[index].name); \
		else                                                    \
			writel(value, &ll->ch[channel].rd[index].name); \
	} while (0)

#define LL_SET_RW_64_CH(ll, dir, channel, index, name, value)           \
	do {                                                            \
		if (dir == DW_HDMA_DIR_WRITE)                           \
			writeq(value, &ll->ch[channel].wr[index].name); \
		else                                                    \
			writeq(value, &ll->ch[channel].rd[index].name); \
	} while (0)

#define LL_SET_32_WRCH(ll, channel, index, name, value) \
	writel(value, &(ll->ch[channel].wr[index].name))

#define LL_SET_64_WRCH(ll, channel, index, name, value) \
	writeq(value, &(ll->ch[channel].wr[index].name))

#define LL_GET_RW_32_CH(ll, dir, channel, index, name)    \
	(((dir) == DW_HDMA_DIR_WRITE) ?                   \
		 readl(&ll->ch[channel].wr[index].name) : \
		 readl(&ll->ch[channel].rd[index].name))

#define LL_GET_RW_64_CH(ll, dir, channel, index, name)    \
	(((dir) == DW_HDMA_DIR_WRITE) ?                   \
		 readq(&ll->ch[channel].wr[index].name) : \
		 readq(&ll->ch[channel].rd[index].name))

#define LL_GET_32_WRCH(ll, channel, index, name) \
	readl(&(ll->ch[channel].wr[index].name))

#define LL_GET_64_WRCH(ll, channel, index, name) \
	readq(&(ll->ch[channel].wr[index].name))

#define LL_SET_32_RDCH(ll, channel, index, name, value) \
	writel(value, &(ll->ch[channel].rd[index].name))

#define LL_SET_64_RDCH(ll, channel, index, name, value) \
	writeq(value, &(ll->ch[channel].rd[index].name))

#define LL_GET_32_RDCH(ll, channel, index, name) \
	readl(&(ll->ch[channel].rd[index].name))

#define LL_GET_64_RDCH(ll, channel, index, name) \
	readq(&(ll->ch[channel].rd[index].name))

#endif
