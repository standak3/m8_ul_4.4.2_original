#ifndef __RIVAFB_H
#define __RIVAFB_H

#include <linux/fb.h>
#include <video/vga.h>
#include <linux/i2c.h>
#include <linux/i2c-algo-bit.h>

#include "riva_hw.h"

#define NUM_SEQ_REGS		0x05
#define NUM_CRT_REGS		0x41
#define NUM_GRC_REGS		0x09
#define NUM_ATC_REGS		0x15

#define DDC_SCL_READ_MASK       (1 << 2)
#define DDC_SCL_WRITE_MASK      (1 << 5)
#define DDC_SDA_READ_MASK       (1 << 3)
#define DDC_SDA_WRITE_MASK      (1 << 4)

struct riva_regs {
	u8 attr[NUM_ATC_REGS];
	u8 crtc[NUM_CRT_REGS];
	u8 gra[NUM_GRC_REGS];
	u8 seq[NUM_SEQ_REGS];
	u8 misc_output;
	RIVA_HW_STATE ext;
};

struct riva_par;

struct riva_i2c_chan {
	struct riva_par *par;
	unsigned long   ddc_base;
	struct i2c_adapter adapter;
	struct i2c_algo_bit_data algo;
};

struct riva_par {
	RIVA_HW_INST riva;	
	u32 pseudo_palette[16]; 
	u32 palette[16];        
	u8 __iomem *ctrl_base;	
	unsigned dclk_max;	

	struct riva_regs initial_state;	
	struct riva_regs current_state;
#ifdef CONFIG_X86
	struct vgastate state;
#endif
	struct mutex open_lock;
	unsigned int ref_count;
	unsigned char *EDID;
	unsigned int Chipset;
	int forceCRTC;
	Bool SecondCRTC;
	int FlatPanel;
	struct pci_dev *pdev;
	int cursor_reset;
#ifdef CONFIG_MTRR
	struct { int vram; int vram_valid; } mtrr;
#endif
	struct riva_i2c_chan chan[3];
};

void riva_common_setup(struct riva_par *);
unsigned long riva_get_memlen(struct riva_par *);
unsigned long riva_get_maxdclk(struct riva_par *);
void riva_delete_i2c_busses(struct riva_par *par);
void riva_create_i2c_busses(struct riva_par *par);
int riva_probe_i2c_connector(struct riva_par *par, int conn, u8 **out_edid);

#endif 
