/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (c) 2025 Axelera AI. All rights reserved.  */
#ifndef AXL_AIPU_PCIE_EDMA_H
#define AXL_AIPU_PCIE_EDMA_H

#ifndef BIT
#define BIT(nr) (1 << (nr))
#endif // AXL_AIPU_PCIE_EDMA_H

/* This is kept for backward compatibility with firmware versions up to 1.5.0 */
#define EDMA_L2_BASE		      0x0000000008000000
#define EDMA_L2_SIZE		      0x0000000002000000
#define EDMA_L2_LINKED_LIST_DESC_SIZE (sizeof(struct dw_edma_ll_buf))
#define EDMA_L2_LINKED_LIST_DESC_OFF \
	(EDMA_L2_SIZE - EDMA_L2_LINKED_LIST_DESC_SIZE)
#define EDMA_L2_LINKED_LIST_DESC_BASE \
	(EDMA_L2_BASE + EDMA_L2_LINKED_LIST_DESC_OFF)

enum dw_edma_control {
	DW_EDMA_V0_CB = BIT(0),
	DW_EDMA_V0_TCB = BIT(1),
	DW_EDMA_V0_LLP = BIT(2),
	DW_EDMA_V0_LIE = BIT(3),
	DW_EDMA_V0_RIE = BIT(4),
	DW_EDMA_V0_CS = BIT(5) | BIT(6),
	DW_EDMA_V0_CCS = BIT(8),
	DW_EDMA_V0_LLE = BIT(9),
};
enum {
	DW_EDMA_V0_CS_RUN = 0x1,
	DW_EDMA_V0_CS_HALT = 0x2,
	DW_EDMA_V0_CS_STOP = 0x3,
};
#define DW_EDMA_CTRL_CS(reg) ((reg & DW_EDMA_V0_CS) >> 5)
enum dw_edma_dir { DW_EDMA_DIR_READ, DW_EDMA_DIR_WRITE };

enum {
	DMA_CTRL_DATA_ARB_PRIOR_OFF = 0x0,
	DMA_CTRL_OFF = 0x8,
	DMA_WRITE_ENGINE_EN_OFF = 0xc,
	DMA_WRITE_DOORBELL_OFF = 0x10,
	DMA_WRITE_CHANNEL_ARB_WEIGHT_LOW_OFF = 0x18,
	DMA_WRITE_CHANNEL_ARB_WEIGHT_HIGH_OFF = 0x1c,
	DMA_READ_ENGINE_EN_OFF = 0x2c,
	DMA_READ_DOORBELL_OFF = 0x30,
	DMA_READ_CHANNEL_ARB_WEIGHT_LOW_OFF = 0x38,
	DMA_READ_CHANNEL_ARB_WEIGHT_HIGH_OFF = 0x3c,
	DMA_WRITE_INT_STATUS_OFF = 0x4c,
	DMA_WRITE_INT_MASK_OFF = 0x54,
	DMA_WRITE_INT_CLEAR_OFF = 0x58,
	DMA_WRITE_ERR_STATUS_OFF = 0x5c,
	DMA_WRITE_DONE_IMWR_LOW_OFF = 0x60,
	DMA_WRITE_DONE_IMWR_HIGH_OFF = 0x64,
	DMA_WRITE_ABORT_IMWR_LOW_OFF = 0x68,
	DMA_WRITE_ABORT_IMWR_HIGH_OFF = 0x6c,
	DMA_WRITE_CH01_IMWR_DATA_OFF = 0x70,
	DMA_WRITE_CH23_IMWR_DATA_OFF = 0x74,
	DMA_WRITE_CH45_IMWR_DATA_OFF = 0x78,
	DMA_WRITE_CH67_IMWR_DATA_OFF = 0x7c,
	DMA_WRITE_LINKED_LIST_ERR_EN_OFF = 0x90,
	DMA_READ_INT_STATUS_OFF = 0xa0,
	DMA_READ_INT_MASK_OFF = 0xa8,
	DMA_READ_INT_CLEAR_OFF = 0xac,
	DMA_READ_ERR_STATUS_LOW_OFF = 0xb4,
	DMA_READ_ERR_STATUS_HIGH_OFF = 0xb8,
	DMA_READ_LINKED_LIST_ERR_EN_OFF = 0xc4,
	DMA_READ_DONE_IMWR_LOW_OFF = 0xcc,
	DMA_READ_DONE_IMWR_HIGH_OFF = 0xd0,
	DMA_READ_ABORT_IMWR_LOW_OFF = 0xd4,
	DMA_READ_ABORT_IMWR_HIGH_OFF = 0xd8,
	DMA_READ_CH01_IMWR_DATA_OFF = 0xdc,
	DMA_READ_CH23_IMWR_DATA_OFF = 0xe0,
	DMA_READ_CH45_IMWR_DATA_OFF = 0xe4,
	DMA_READ_CH67_IMWR_DATA_OFF = 0xe8,
	DMA_WRITE_CH0_PWR_EN_OFF = 0x128,
	DMA_WRITE_CH1_PWR_EN_OFF = 0x12c,
	DMA_WRITE_CH2_PWR_EN_OFF = 0x130,
	DMA_WRITE_CH3_PWR_EN_OFF = 0x134,
	DMA_READ_CH0_PWR_EN_OFF = 0x168,
	DMA_READ_CH1_PWR_EN_OFF = 0x16c,
	DMA_READ_CH2_PWR_EN_OFF = 0x170,
	DMA_READ_CH3_PWR_EN_OFF = 0x174,
	DMA_CH_CONTROL1_OFF_WRCH_0 = 0x200,
	DMA_TRANSFER_SIZE_OFF_WRCH_0 = 0x208,
	DMA_SAR_LOW_OFF_WRCH_0 = 0x20c,
	DMA_SAR_HIGH_OFF_WRCH_0 = 0x210,
	DMA_DAR_LOW_OFF_WRCH_0 = 0x214,
	DMA_DAR_HIGH_OFF_WRCH_0 = 0x218,
	DMA_LLP_LOW_OFF_WRCH_0 = 0x21c,
	DMA_LLP_HIGH_OFF_WRCH_0 = 0x220,
	DMA_CH_CONTROL1_OFF_RDCH_0 = 0x300,
	DMA_TRANSFER_SIZE_OFF_RDCH_0 = 0x308,
	DMA_SAR_LOW_OFF_RDCH_0 = 0x30c,
	DMA_SAR_HIGH_OFF_RDCH_0 = 0x310,
	DMA_DAR_LOW_OFF_RDCH_0 = 0x314,
	DMA_DAR_HIGH_OFF_RDCH_0 = 0x318,
	DMA_LLP_LOW_OFF_RDCH_0 = 0x31c,
	DMA_LLP_HIGH_OFF_RDCH_0 = 0x320,
	DMA_CH_CONTROL1_OFF_WRCH_1 = 0x400,
	DMA_TRANSFER_SIZE_OFF_WRCH_1 = 0x408,
	DMA_SAR_LOW_OFF_WRCH_1 = 0x40c,
	DMA_SAR_HIGH_OFF_WRCH_1 = 0x410,
	DMA_DAR_LOW_OFF_WRCH_1 = 0x414,
	DMA_DAR_HIGH_OFF_WRCH_1 = 0x418,
	DMA_LLP_LOW_OFF_WRCH_1 = 0x41c,
	DMA_LLP_HIGH_OFF_WRCH_1 = 0x420,
	DMA_CH_CONTROL1_OFF_RDCH_1 = 0x500,
	DMA_TRANSFER_SIZE_OFF_RDCH_1 = 0x508,
	DMA_SAR_LOW_OFF_RDCH_1 = 0x50c,
	DMA_SAR_HIGH_OFF_RDCH_1 = 0x510,
	DMA_DAR_LOW_OFF_RDCH_1 = 0x514,
	DMA_DAR_HIGH_OFF_RDCH_1 = 0x518,
	DMA_LLP_LOW_OFF_RDCH_1 = 0x51c,
	DMA_LLP_HIGH_OFF_RDCH_1 = 0x520,
	DMA_CH_CONTROL1_OFF_WRCH_2 = 0x600,
	DMA_TRANSFER_SIZE_OFF_WRCH_2 = 0x608,
	DMA_SAR_LOW_OFF_WRCH_2 = 0x60c,
	DMA_SAR_HIGH_OFF_WRCH_2 = 0x610,
	DMA_DAR_LOW_OFF_WRCH_2 = 0x614,
	DMA_DAR_HIGH_OFF_WRCH_2 = 0x618,
	DMA_LLP_LOW_OFF_WRCH_2 = 0x61c,
	DMA_LLP_HIGH_OFF_WRCH_2 = 0x620,
	DMA_CH_CONTROL1_OFF_RDCH_2 = 0x700,
	DMA_TRANSFER_SIZE_OFF_RDCH_2 = 0x708,
	DMA_SAR_LOW_OFF_RDCH_2 = 0x70c,
	DMA_SAR_HIGH_OFF_RDCH_2 = 0x710,
	DMA_DAR_LOW_OFF_RDCH_2 = 0x714,
	DMA_DAR_HIGH_OFF_RDCH_2 = 0x718,
	DMA_LLP_LOW_OFF_RDCH_2 = 0x71c,
	DMA_LLP_HIGH_OFF_RDCH_2 = 0x720,
	DMA_CH_CONTROL1_OFF_WRCH_3 = 0x800,
	DMA_TRANSFER_SIZE_OFF_WRCH_3 = 0x808,
	DMA_SAR_LOW_OFF_WRCH_3 = 0x80c,
	DMA_SAR_HIGH_OFF_WRCH_3 = 0x810,
	DMA_DAR_LOW_OFF_WRCH_3 = 0x814,
	DMA_DAR_HIGH_OFF_WRCH_3 = 0x818,
	DMA_LLP_LOW_OFF_WRCH_3 = 0x81c,
	DMA_LLP_HIGH_OFF_WRCH_3 = 0x820,
	DMA_CH_CONTROL1_OFF_RDCH_3 = 0x900,
	DMA_TRANSFER_SIZE_OFF_RDCH_3 = 0x908,
	DMA_SAR_LOW_OFF_RDCH_3 = 0x90c,
	DMA_SAR_HIGH_OFF_RDCH_3 = 0x910,
	DMA_DAR_LOW_OFF_RDCH_3 = 0x914,
	DMA_DAR_HIGH_OFF_RDCH_3 = 0x918,
	DMA_LLP_LOW_OFF_RDCH_3 = 0x91c,
	DMA_LLP_HIGH_OFF_RDCH_3 = 0x920
};

#define LINKED_LIST_RAM_OFF  0x4000
#define LINKED_LIST_RAM_SIZE 100 * 1024

// DMA Channel Context Registers for Each Channel
#define __packed __attribute__((__packed__))
struct dw_edma_v0_ch_regs {
	u32 ch_control1; /* 0x0000 */
	u32 ch_control2; /* 0x0004 */
	u32 transfer_size; /* 0x0008 */
	union {
		u64 reg; /* 0x000c..0x0010 */
		struct {
			u32 lsb; /* 0x000c */
			u32 msb; /* 0x0010 */
		};
	} sar;
	union {
		u64 reg; /* 0x0014..0x0018 */
		struct {
			u32 lsb; /* 0x0014 */
			u32 msb; /* 0x0018 */
		};
	} dar;
	union {
		u64 reg; /* 0x001c..0x0020 */
		struct {
			u32 lsb; /* 0x001c */
			u32 msb; /* 0x0020 */
		};
	} llp;
} __packed;
struct dw_edma_v0_ch {
	struct dw_edma_v0_ch_regs wr; /* 0x0200 */
	u32 padding_1[55]; /* 0x0224..0x02fc */
	struct dw_edma_v0_ch_regs rd; /* 0x0300 */
	u32 padding_2[55]; /* 0x0324..0x03fc */
} __packed;

struct dw_edma_v0_legacy {
	u32 viewport_sel; /* 0x00f8 */
	struct dw_edma_v0_ch_regs ch; /* 0x0100..0x0120 */
} __packed;

struct dw_edma_v0_unroll {
	u32 padding_1; /* 0x00f8 */
	u32 wr_engine_chgroup; /* 0x0100 */
	u32 rd_engine_chgroup; /* 0x0104 */
	union {
		u64 reg; /* 0x0108..0x010c */
		struct {
			u32 lsb; /* 0x0108 */
			u32 msb; /* 0x010c */
		};
	} wr_engine_hshake_cnt;
	u32 padding_2[2]; /* 0x0110..0x0114 */
	union {
		u64 reg; /* 0x0120..0x0124 */
		struct {
			u32 lsb; /* 0x0120 */
			u32 msb; /* 0x0124 */
		};
	} rd_engine_hshake_cnt;
	u32 padding_3[2]; /* 0x0120..0x0124 */
	u32 wr_ch0_pwr_en; /* 0x0128 */
	u32 wr_ch1_pwr_en; /* 0x012c */
	u32 wr_ch2_pwr_en; /* 0x0130 */
	u32 wr_ch3_pwr_en; /* 0x0134 */
	u32 wr_ch4_pwr_en; /* 0x0138 */
	u32 wr_ch5_pwr_en; /* 0x013c */
	u32 wr_ch6_pwr_en; /* 0x0140 */
	u32 wr_ch7_pwr_en; /* 0x0144 */
	u32 padding_4[8]; /* 0x0148..0x0164 */
	u32 rd_ch0_pwr_en; /* 0x0168 */
	u32 rd_ch1_pwr_en; /* 0x016c */
	u32 rd_ch2_pwr_en; /* 0x0170 */
	u32 rd_ch3_pwr_en; /* 0x0174 */
	u32 rd_ch4_pwr_en; /* 0x0178 */
	u32 rd_ch5_pwr_en; /* 0x018c */
	u32 rd_ch6_pwr_en; /* 0x0180 */
	u32 rd_ch7_pwr_en; /* 0x0184 */
	u32 padding_5[30]; /* 0x0188..0x01fc */
	struct dw_edma_v0_ch ch[EDMA_V0_MAX_NR_CH]; /* 0x0200..0x1120 */
} __packed;
struct dw_edma_v0_regs {
	/* eDMA global registers */
	u32 ctrl_data_arb_prior; /* 0x0000 */
	u32 padding_1; /* 0x0004 */
	u32 ctrl; /* 0x0008 */
	u32 wr_engine_en; /* 0x000c */
	u32 wr_doorbell; /* 0x0010 */
	u32 padding_2; /* 0x0014 */
	union {
		u64 reg; /* 0x0018..0x001c */
		struct {
			u32 lsb; /* 0x0018 */
			u32 msb; /* 0x001c */
		};
	} wr_ch_arb_weight;
	u32 padding_3[3]; /* 0x0020..0x0028 */
	u32 rd_engine_en; /* 0x002c */
	u32 rd_doorbell; /* 0x0030 */
	u32 padding_4; /* 0x0034 */
	union {
		u64 reg; /* 0x0038..0x003c */
		struct {
			u32 lsb; /* 0x0038 */
			u32 msb; /* 0x003c */
		};
	} rd_ch_arb_weight;
	u32 padding_5[3]; /* 0x0040..0x0048 */
	/* eDMA interrupts registers */
	u32 wr_int_status; /* 0x004c */
	u32 padding_6; /* 0x0050 */
	u32 wr_int_mask; /* 0x0054 */
	u32 wr_int_clear; /* 0x0058 */
	u32 wr_err_status; /* 0x005c */
	union {
		u64 reg; /* 0x0060..0x0064 */
		struct {
			u32 lsb; /* 0x0060 */
			u32 msb; /* 0x0064 */
		};
	} wr_done_imwr;
	union {
		u64 reg; /* 0x0068..0x006c */
		struct {
			u32 lsb; /* 0x0068 */
			u32 msb; /* 0x006c */
		};
	} wr_abort_imwr;
	u32 wr_ch01_imwr_data; /* 0x0070 */
	u32 wr_ch23_imwr_data; /* 0x0074 */
	u32 wr_ch45_imwr_data; /* 0x0078 */
	u32 wr_ch67_imwr_data; /* 0x007c */
	u32 padding_7[4]; /* 0x0080..0x008c */
	u32 wr_linked_list_err_en; /* 0x0090 */
	u32 padding_8[3]; /* 0x0094..0x009c */
	u32 rd_int_status; /* 0x00a0 */
	u32 padding_9; /* 0x00a4 */
	u32 rd_int_mask; /* 0x00a8 */
	u32 rd_int_clear; /* 0x00ac */
	u32 padding_10; /* 0x00b0 */
	union {
		u64 reg; /* 0x00b4..0x00b8 */
		struct {
			u32 lsb; /* 0x00b4 */
			u32 msb; /* 0x00b8 */
		};
	} rd_err_status;
	u32 padding_11[2]; /* 0x00bc..0x00c0 */
	u32 rd_linked_list_err_en; /* 0x00c4 */
	u32 padding_12; /* 0x00c8 */
	union {
		u64 reg; /* 0x00cc..0x00d0 */
		struct {
			u32 lsb; /* 0x00cc */
			u32 msb; /* 0x00d0 */
		};
	} rd_done_imwr;
	union {
		u64 reg; /* 0x00d4..0x00d8 */
		struct {
			u32 lsb; /* 0x00d4 */
			u32 msb; /* 0x00d8 */
		};
	} rd_abort_imwr;
	u32 rd_ch01_imwr_data; /* 0x00dc */
	u32 rd_ch23_imwr_data; /* 0x00e0 */
	u32 rd_ch45_imwr_data; /* 0x00e4 */
	u32 rd_ch67_imwr_data; /* 0x00e8 */
	u32 padding_13[4]; /* 0x00ec..0x00f8 */
	/* eDMA channel context grouping */
	union dw_edma_v0_type {
		struct dw_edma_v0_legacy legacy; /* 0x00f8..0x0120 */
		struct dw_edma_v0_unroll unroll; /* 0x00f8..0x1120 */
	} type;
} __packed;

// This means we use 4K for each channels so we use 32Kbytes for all descriptors
#define DESC_MEM_SIZE (32 * 1024)
#define DW_EDMA_LL_NUM \
	DESC_MEM_SIZE / (2 * EDMA_V0_MAX_NR_CH * sizeof(struct dw_edma_v0_lli))
#define DW_EDMA_LL_MAX_NUM (DW_EDMA_LL_NUM - 3)
struct dw_edma_v0_lli {
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

struct dw_edma_lld_ch {
	struct dw_edma_v0_lli rd[DW_EDMA_LL_NUM];
	struct dw_edma_v0_lli wr[DW_EDMA_LL_NUM];
} __packed;

struct dw_edma_ll_buf {
	struct dw_edma_lld_ch ch[EDMA_V0_MAX_NR_CH];
} __packed;

#define _LSB(x) (uint32_t)(x)
#define _MSB(x) (uint32_t)((x) >> 32)

#define SET_RW_32_CH(edma, dir, name, ch, value) \
	writel(value, &(__edma_ch(edma, dir, ch)->name))

#define GET_RW_32_CH(edma, dir, name, ch) \
	readl(&(__edma_ch(edma, dir, ch)->name))

static inline uint64_t __get_ll_base(volatile struct dw_edma_ll_buf *hwlldch,
				     enum dw_edma_dir dir, int ch)
{
	if (dir == DW_EDMA_DIR_WRITE)
		return (uint64_t)(&hwlldch->ch[ch].wr[0].control);

	return (uint64_t)(&hwlldch->ch[ch].rd[0].control);
}
static inline volatile struct dw_edma_v0_ch_regs __iomem *
__edma_ch(volatile struct dw_edma_v0_regs *edma, enum dw_edma_dir dir, int ch)
{
	if (dir == DW_EDMA_DIR_WRITE)
		return &edma->type.unroll.ch[ch].wr;

	return &edma->type.unroll.ch[ch].rd;
}

#define SET_32(edma, name, value) writel(value, &(edma->name))
#define SET_RW_32(edma, dir, name, value)               \
	do {                                            \
		if (dir == DW_EDMA_DIR_WRITE)           \
			SET_32(edma, wr_##name, value); \
		else                                    \
			SET_32(edma, rd_##name, value); \
	} while (0)

#define GET_32(edma, name) readl(&(edma->name))
#define GET_RW_32(edma, dir, name)                                \
	(((dir) == DW_EDMA_DIR_WRITE) ? GET_32(edma, wr_##name) : \
					GET_32(edma, rd_##name))

#define GET_64(edma, name) readq(&(edma->name))

#define SET_64(edma, name, value) writeq(value, &(edma->name))

#define GET_32_CH(edma, name, channel) \
	readl(&(edma->type.unroll.ch[channel].name))

#define SET_32_CH(edma, name, channel, value) \
	writel(value, &(edma->type.unroll.ch[channel].name))

#define GET_64_CH(edma, name, channel) \
	readq(&(edma->type.unroll.ch[channel].name))

#define SET_64_CH(edma, name, channel, value) \
	writeq(value, &(edma->type.unroll.ch[channel].name))

#define LL_SET_RW_32_CH(ll, dir, channel, index, name, value)           \
	do {                                                            \
		if (dir == DW_EDMA_DIR_WRITE)                           \
			writel(value, &ll->ch[channel].wr[index].name); \
		else                                                    \
			writel(value, &ll->ch[channel].rd[index].name); \
	} while (0)

#define LL_SET_RW_64_CH(ll, dir, channel, index, name, value)           \
	do {                                                            \
		if (dir == DW_EDMA_DIR_WRITE)                           \
			writeq(value, &ll->ch[channel].wr[index].name); \
		else                                                    \
			writeq(value, &ll->ch[channel].rd[index].name); \
	} while (0)

#define LL_SET_32_WRCH(ll, channel, index, name, value) \
	writel(value, &(ll->ch[channel].wr[index].name))

#define LL_SET_64_WRCH(ll, channel, index, name, value) \
	writeq(value, &(ll->ch[channel].wr[index].name))

#define LL_GET_RW_32_CH(ll, dir, channel, index, name)    \
	(((dir) == DW_EDMA_DIR_WRITE) ?                   \
		 readl(&ll->ch[channel].wr[index].name) : \
		 readl(&ll->ch[channel].rd[index].name))

#define LL_GET_RW_64_CH(ll, dir, channel, index, name)    \
	(((dir) == DW_EDMA_DIR_WRITE) ?                   \
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

/* Host memory descriptor access macros (direct memory access, not MMIO) */
#define LL_SET_RW_32_HOST(ll, dir, channel, index, name, value)   \
	do {                                                      \
		if (dir == DW_EDMA_DIR_WRITE)                     \
			ll->ch[channel].wr[index].name = (value); \
		else                                              \
			ll->ch[channel].rd[index].name = (value); \
	} while (0)

#define LL_SET_RW_64_HOST(ll, dir, channel, index, name, value)       \
	do {                                                          \
		if (dir == DW_EDMA_DIR_WRITE)                         \
			ll->ch[channel].wr[index].name.reg = (value); \
		else                                                  \
			ll->ch[channel].rd[index].name.reg = (value); \
	} while (0)

#define LL_GET_RW_32_HOST(ll, dir, channel, index, name)                 \
	(((dir) == DW_EDMA_DIR_WRITE) ? ll->ch[channel].wr[index].name : \
					ll->ch[channel].rd[index].name)

#define LL_GET_RW_64_HOST(ll, dir, channel, index, name)                     \
	(((dir) == DW_EDMA_DIR_WRITE) ? ll->ch[channel].wr[index].name.reg : \
					ll->ch[channel].rd[index].name.reg)

#endif // AXL_AIPU_PCIE_EDMA_H
