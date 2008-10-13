/*-
 * Copyright (C) 2008 MARVELL INTERNATIONAL LTD.
 * All rights reserved.
 *
 * Developed by Semihalf.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of MARVELL nor the names of contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <sys/systm.h>
#include <sys/bus.h>

#include <machine/bus.h>

#include <arm/mv/mvreg.h>
#include <arm/mv/mvvar.h>

static int win_eth_can_remap(int i);

static int decode_win_cpu_valid(void);
static int decode_win_usb_valid(void);
static int decode_win_eth_valid(void);
static int decode_win_pcie_valid(void);

static void decode_win_cpu_setup(void);
static void decode_win_usb_setup(uint32_t ctrl);
static void decode_win_eth_setup(uint32_t base);
static void decode_win_pcie_setup(uint32_t base);

static uint32_t dev, rev;

uint32_t
read_cpu_ctrl(uint32_t reg)
{

	return (bus_space_read_4(obio_tag, MV_CPU_CONTROL_BASE, reg));
}

void
write_cpu_ctrl(uint32_t reg, uint32_t val)
{

	bus_space_write_4(obio_tag, MV_CPU_CONTROL_BASE, reg, val);
}

void
cpu_reset(void)
{

	write_cpu_ctrl(RSTOUTn_MASK, SOFT_RST_OUT_EN);
	write_cpu_ctrl(SYSTEM_SOFT_RESET, SYS_SOFT_RST);
	while (1);
}

uint32_t
cpu_extra_feat(void)
{
	uint32_t ef = 0;

	soc_id(&dev, &rev);
	if (dev == MV_DEV_88F6281 || dev == MV_DEV_MV78100)
		__asm __volatile("mrc p15, 1, %0, c15, c1, 0" : "=r" (ef));
	else if (dev == MV_DEV_88F5182 || dev == MV_DEV_88F5281)
		__asm __volatile("mrc p15, 0, %0, c14, c0, 0" : "=r" (ef));
	else if (bootverbose)
		printf("This ARM Core does not support any extra features\n");

	return (ef);
}

uint32_t
soc_power_ctrl_get(uint32_t mask)
{

	if (mask != CPU_PM_CTRL_NONE)
		mask &= read_cpu_ctrl(CPU_PM_CTRL);

	return (mask);
}

uint32_t
get_tclk(void)
{

#if defined(SOC_MV_DISCOVERY)
	return (TCLK_200MHZ);
#else
	return (TCLK_166MHZ);
#endif
}

void
soc_id(uint32_t *dev, uint32_t *rev)
{

	/*
	 * Notice: system identifiers are available in the registers range of
	 * PCIE controller, so using this function is only allowed (and
	 * possible) after the internal registers range has been mapped in via
	 * pmap_devmap_bootstrap().
	 */
	*dev = bus_space_read_4(obio_tag, MV_PCIE_BASE, 0) >> 16;
	*rev = bus_space_read_4(obio_tag, MV_PCIE_BASE, 8) & 0xff;
}

void
soc_identify(void)
{
	uint32_t d, r;
	const char *dev;
	const char *rev;

	soc_id(&d, &r);

	printf("SOC: ");
	if (bootverbose)
		printf("(0x%4x:0x%02x) ", d, r);

	rev = "";
	switch (d) {
	case MV_DEV_88F5181:
		dev = "Marvell 88F5181";
		if (r == 3)
			rev = "B1";
		break;
	case MV_DEV_88F5182:
		dev = "Marvell 88F5182";
		if (r == 2)
			rev = "A2";
		break;
	case MV_DEV_88F5281:
		dev = "Marvell 88F5281";
		if (r == 4)
			rev = "D0";
		else if (r == 5)
			rev = "D1";
		else if (r == 6)
			rev = "D2";
		break;
	case MV_DEV_88F6281:
		dev = "Marvell 88F6281";
		break;
	case MV_DEV_MV78100:
		dev = "Marvell MV78100";
		break;
	default:
		dev = "UNKNOWN";
		break;
	}

	printf("%s", dev);
	if (*rev != '\0')
		printf(" rev %s", rev);
	printf(", TClock %dMHz\n", get_tclk() / 1000 / 1000);

	/* TODO add info on currently set endianess */
}

int
soc_decode_win(void)
{

	/* Retrieve our ID: some windows facilities vary between SoC models */
	soc_id(&dev, &rev);

	if (decode_win_cpu_valid() != 1 || decode_win_usb_valid() != 1 ||
	    decode_win_eth_valid() != 1 || decode_win_idma_valid() != 1 ||
	    decode_win_pcie_valid() != 1)
		return(-1);

	decode_win_cpu_setup();
	decode_win_usb_setup(MV_USB0_BASE);
	decode_win_eth_setup(MV_ETH0_BASE);
	if (dev == MV_DEV_MV78100)
		decode_win_eth_setup(MV_ETH1_BASE);
	decode_win_idma_setup();
	decode_win_pcie_setup(MV_PCIE_BASE);

	/* TODO set up decode wins for SATA */

	return (0);
}

/**************************************************************************
 * Decode windows registers accessors
 **************************************************************************/
WIN_REG_IDX_RD(win_cpu, cr, MV_WIN_CPU_CTRL, MV_MBUS_BRIDGE_BASE)
WIN_REG_IDX_RD(win_cpu, br, MV_WIN_CPU_BASE, MV_MBUS_BRIDGE_BASE)
WIN_REG_IDX_RD(win_cpu, remap_l, MV_WIN_CPU_REMAP_LO, MV_MBUS_BRIDGE_BASE)
WIN_REG_IDX_RD(win_cpu, remap_h, MV_WIN_CPU_REMAP_HI, MV_MBUS_BRIDGE_BASE)
WIN_REG_IDX_WR(win_cpu, cr, MV_WIN_CPU_CTRL, MV_MBUS_BRIDGE_BASE)
WIN_REG_IDX_WR(win_cpu, br, MV_WIN_CPU_BASE, MV_MBUS_BRIDGE_BASE)
WIN_REG_IDX_WR(win_cpu, remap_l, MV_WIN_CPU_REMAP_LO, MV_MBUS_BRIDGE_BASE)
WIN_REG_IDX_WR(win_cpu, remap_h, MV_WIN_CPU_REMAP_HI, MV_MBUS_BRIDGE_BASE)

WIN_REG_IDX_RD(ddr, br, MV_WIN_DDR_BASE, MV_DDR_CADR_BASE)
WIN_REG_IDX_RD(ddr, sz, MV_WIN_DDR_SIZE, MV_DDR_CADR_BASE)

WIN_REG_IDX_RD(win_usb, cr, MV_WIN_USB_CTRL, MV_USB_AWR_BASE)
WIN_REG_IDX_RD(win_usb, br, MV_WIN_USB_BASE, MV_USB_AWR_BASE)
WIN_REG_IDX_WR(win_usb, cr, MV_WIN_USB_CTRL, MV_USB_AWR_BASE)
WIN_REG_IDX_WR(win_usb, br, MV_WIN_USB_BASE, MV_USB_AWR_BASE)

WIN_REG_BASE_IDX_RD(win_eth, br, MV_WIN_ETH_BASE)
WIN_REG_BASE_IDX_RD(win_eth, sz, MV_WIN_ETH_SIZE)
WIN_REG_BASE_IDX_RD(win_eth, har, MV_WIN_ETH_REMAP)
WIN_REG_BASE_IDX_WR(win_eth, br, MV_WIN_ETH_BASE)
WIN_REG_BASE_IDX_WR(win_eth, sz, MV_WIN_ETH_SIZE)
WIN_REG_BASE_IDX_WR(win_eth, har, MV_WIN_ETH_REMAP)
WIN_REG_BASE_RD(win_eth, bare, 0x290)
WIN_REG_BASE_RD(win_eth, epap, 0x294)
WIN_REG_BASE_WR(win_eth, bare, 0x290)
WIN_REG_BASE_WR(win_eth, epap, 0x294)

WIN_REG_BASE_IDX_RD(win_pcie, cr, MV_WIN_PCIE_CTRL);
WIN_REG_BASE_IDX_RD(win_pcie, br, MV_WIN_PCIE_BASE);
WIN_REG_BASE_IDX_RD(win_pcie, remap, MV_WIN_PCIE_REMAP);
WIN_REG_BASE_IDX_WR(win_pcie, cr, MV_WIN_PCIE_CTRL);
WIN_REG_BASE_IDX_WR(win_pcie, br, MV_WIN_PCIE_BASE);
WIN_REG_BASE_IDX_WR(win_pcie, remap, MV_WIN_PCIE_REMAP);
WIN_REG_BASE_IDX_WR(pcie, bar, MV_PCIE_BAR);

WIN_REG_IDX_RD(win_idma, br, MV_WIN_IDMA_BASE, MV_IDMA_BASE)
WIN_REG_IDX_RD(win_idma, sz, MV_WIN_IDMA_SIZE, MV_IDMA_BASE)
WIN_REG_IDX_RD(win_idma, har, MV_WIN_IDMA_REMAP, MV_IDMA_BASE)
WIN_REG_IDX_RD(win_idma, cap, MV_WIN_IDMA_CAP, MV_IDMA_BASE)
WIN_REG_IDX_WR(win_idma, br, MV_WIN_IDMA_BASE, MV_IDMA_BASE)
WIN_REG_IDX_WR(win_idma, sz, MV_WIN_IDMA_SIZE, MV_IDMA_BASE)
WIN_REG_IDX_WR(win_idma, har, MV_WIN_IDMA_REMAP, MV_IDMA_BASE)
WIN_REG_IDX_WR(win_idma, cap, MV_WIN_IDMA_CAP, MV_IDMA_BASE)
WIN_REG_RD(win_idma, bare, 0xa80, MV_IDMA_BASE)
WIN_REG_WR(win_idma, bare, 0xa80, MV_IDMA_BASE)

/**************************************************************************
 * Decode windows helper routines
 **************************************************************************/
void
soc_dump_decode_win(void)
{
	int i;

	soc_id(&dev, &rev);

	for (i = 0; i < MV_WIN_CPU_MAX; i++) {
		printf("CPU window#%d: c 0x%08x, b 0x%08x", i,
		    win_cpu_cr_read(i),
		    win_cpu_br_read(i));

		if (win_cpu_can_remap(i))
			printf(", rl 0x%08x, rh 0x%08x",
			    win_cpu_remap_l_read(i),
			    win_cpu_remap_h_read(i));

		printf("\n");
	}
	printf("Internal regs base: 0x%08x\n",
	    bus_space_read_4(obio_tag, MV_INTREGS_BASE, 0));

	for (i = 0; i < MV_WIN_DDR_MAX; i++)
		printf("DDR CS#%d: b 0x%08x, s 0x%08x\n", i,
		    ddr_br_read(i), ddr_sz_read(i));
	
	for (i = 0; i < MV_WIN_USB_MAX; i++)
		printf("USB window#%d: c 0x%08x, b 0x%08x\n", i,
		    win_usb_cr_read(i), win_usb_br_read(i));

	for (i = 0; i < MV_WIN_ETH_MAX; i++) {
		printf("ETH window#%d: b 0x%08x, s 0x%08x", i,
		    win_eth_br_read(MV_ETH0_BASE, i),
		    win_eth_sz_read(MV_ETH0_BASE, i));

		if (win_eth_can_remap(i))
			printf(", ha 0x%08x",
			    win_eth_har_read(MV_ETH0_BASE, i));

		printf("\n");
	}
	printf("ETH windows: bare 0x%08x, epap 0x%08x\n",
	    win_eth_bare_read(MV_ETH0_BASE),
	    win_eth_epap_read(MV_ETH0_BASE));

	decode_win_idma_dump();
	printf("\n");
}

/**************************************************************************
 * CPU windows routines
 **************************************************************************/
int
win_cpu_can_remap(int i)
{

	/* Depending on the SoC certain windows have remap capability */
	if ((dev == MV_DEV_88F5182 && i < 2) ||
	    (dev == MV_DEV_88F5281 && i < 4) ||
	    (dev == MV_DEV_88F6281 && i < 4) ||
	    (dev == MV_DEV_MV78100 && i < 8))
		return (1);

	return (0);
}

/* XXX This should check for overlapping remap fields too.. */
int
decode_win_overlap(int win, int win_no, const struct decode_win *wintab)
{
	const struct decode_win *tab;
	int i;

	tab = wintab;

	for (i = 0; i < win_no; i++, tab++) {
		if (i == win)
			/* Skip self */
			continue;

		if ((tab->base + tab->size - 1) < (wintab + win)->base)
			continue;

		else if (((wintab + win)->base + (wintab + win)->size - 1) <
		    tab->base)
			continue;
		else
			return (i);
	}

	return (-1);
}

static int
decode_win_cpu_valid(void)
{
	int i, j, rv;
	uint32_t b, e, s;

	if (cpu_wins_no > MV_WIN_CPU_MAX) {
		printf("CPU windows: too many entries: %d\n", cpu_wins_no);
		return (-1);
	}

	rv = 1;
	for (i = 0; i < cpu_wins_no; i++) {

		if (cpu_wins[i].target == 0) {
			printf("CPU window#%d: DDR target window is not "
			    "supposed to be reprogrammed!\n", i);
			rv = 0;
		}

		if (cpu_wins[i].remap >= 0 && win_cpu_can_remap(i) != 1) {
			printf("CPU window#%d: not capable of remapping, but "
			    "val 0x%08x defined\n", i, cpu_wins[i].remap);
			rv = 0;
		}

		s = cpu_wins[i].size;
		b = cpu_wins[i].base;
		e = b + s - 1;
		if (s > (0xFFFFFFFF - b + 1)) {
			/*
			 * XXX this boundary check should account for 64bit
			 * and remapping..
			 */
			printf("CPU window#%d: no space for size 0x%08x at "
			    "0x%08x\n", i, s, b);
			rv = 0;
			continue;
		}

		j = decode_win_overlap(i, cpu_wins_no, &cpu_wins[0]);
		if (j >= 0) {
			printf("CPU window#%d: (0x%08x - 0x%08x) overlaps "
			    "with #%d (0x%08x - 0x%08x)\n", i, b, e, j,
			    cpu_wins[j].base,
			    cpu_wins[j].base + cpu_wins[j].size - 1);
			rv = 0;
		}
	}

	return (rv);
}

static void
decode_win_cpu_setup(void)
{
	uint32_t br, cr;
	int i;

	/* Disable all CPU windows */
	for (i = 0; i < MV_WIN_CPU_MAX; i++) {
		win_cpu_cr_write(i, 0);
		win_cpu_br_write(i, 0);
		if (win_cpu_can_remap(i)) {
			win_cpu_remap_l_write(i, 0);
			win_cpu_remap_h_write(i, 0);
		}
	}

	for (i = 0; i < cpu_wins_no; i++)
		if (cpu_wins[i].target > 0) {

			br = cpu_wins[i].base & 0xffff0000;
			win_cpu_br_write(i, br);

			if (win_cpu_can_remap(i)) {
				if (cpu_wins[i].remap >= 0) {
					win_cpu_remap_l_write(i,
					    cpu_wins[i].remap & 0xffff0000);
					win_cpu_remap_h_write(i, 0);
				} else {
					/*
					 * Remap function is not used for
					 * a given window (capable of
					 * remapping) - set remap field with the
					 * same value as base.
					 */
					win_cpu_remap_l_write(i,
					     cpu_wins[i].base & 0xffff0000);
					win_cpu_remap_h_write(i, 0);
				}
			}

			cr = ((cpu_wins[i].size - 1) & 0xffff0000) |
			    (cpu_wins[i].attr << 8) |
			    (cpu_wins[i].target << 4) | 1;

			win_cpu_cr_write(i, cr);
		}
}

/*
 * Check if we're able to cover all active DDR banks.
 */
static int
decode_win_can_cover_ddr(int max)
{
	int i, c;

	c = 0;
	for (i = 0; i < MV_WIN_DDR_MAX; i++)
		if (ddr_is_active(i))
			c++;

	if (c > max) {
		printf("Unable to cover all active DDR banks: "
		    "%d, available windows: %d\n", c, max);
		return (0);
	}

	return (1);
}

/**************************************************************************
 * DDR windows routines
 **************************************************************************/
int
ddr_is_active(int i)
{

	if (ddr_sz_read(i) & 0x1)
		return (1);

	return (0);
}

uint32_t
ddr_base(int i)
{

	return (ddr_br_read(i) & 0xff000000);
}

uint32_t
ddr_size(int i)
{

	return ((ddr_sz_read(i) | 0x00ffffff) + 1);
}

uint32_t
ddr_attr(int i)
{

	return (i == 0 ? 0xe :
	    (i == 1 ? 0xd :
	    (i == 2 ? 0xb :
	    (i == 3 ? 0x7 : 0xff))));
}

uint32_t
ddr_target(int i)
{

	/* Mbus unit ID is 0x0 for DDR SDRAM controller */
	return (0);
}

/**************************************************************************
 * USB windows routines
 **************************************************************************/
static int
decode_win_usb_valid(void)
{

	return (decode_win_can_cover_ddr(MV_WIN_USB_MAX));
}

/*
 * Set USB decode windows.
 */
static void
decode_win_usb_setup(uint32_t ctrl)
{
	uint32_t br, cr;
	int i, j;

	/* Disable and clear all USB windows */
	for (i = 0; i < MV_WIN_USB_MAX; i++) {
		win_usb_cr_write(i, 0);
		win_usb_br_write(i, 0);
	}

	/* Only access to active DRAM banks is required */
	for (i = 0; i < MV_WIN_DDR_MAX; i++)
		if (ddr_is_active(i)) {
			br = ddr_base(i);
			/*
			 * XXX for 6281 we should handle Mbus write burst limit
			 * field in the ctrl reg
			 */
			cr = (((ddr_size(i) - 1) & 0xffff0000) |
			    (ddr_attr(i) << 8) | (ddr_target(i) << 4) | 1); 

			/* Set the first free USB window */
			for (j = 0; j < MV_WIN_USB_MAX; j++) {
				if (win_usb_cr_read(j) & 0x1)
					continue;

				win_usb_br_write(j, br);
				win_usb_cr_write(j, cr);
				break;
			}
		}
}

/**************************************************************************
 * ETH windows routines
 **************************************************************************/

static int
win_eth_can_remap(int i)
{

	/* ETH encode windows 0-3 have remap capability */
	if (i < 4)
		return (1);
	
	return (0);
}

static int
eth_bare_read(uint32_t base, int i)
{
	uint32_t v;

	v = win_eth_bare_read(base);
	v &= (1 << i);

	return (v >> i);
}

static void
eth_bare_write(uint32_t base, int i, int val)
{
	uint32_t v;

	v = win_eth_bare_read(base);
	v &= ~(1 << i);
	v |= (val << i);
	win_eth_bare_write(base, v);
}

static void
eth_epap_write(uint32_t base, int i, int val)
{
	uint32_t v;

	v = win_eth_epap_read(base);
	v &= ~(0x3 << (i * 2));
	v |= (val << (i * 2));
	win_eth_epap_write(base, v);
}

static void
decode_win_eth_setup(uint32_t base)
{
	uint32_t br, sz;
	int i, j;

	/* Disable, clear and revoke protection for all ETH windows */
	for (i = 0; i < MV_WIN_ETH_MAX; i++) {

		eth_bare_write(base, i, 1);
		eth_epap_write(base, i, 0);
		win_eth_br_write(base, i, 0);
		win_eth_sz_write(base, i, 0);
		if (win_eth_can_remap(i))
			win_eth_har_write(base, i, 0);
	}

	/* Only access to active DRAM banks is required */
	for (i = 0; i < MV_WIN_DDR_MAX; i++)
		if (ddr_is_active(i)) {

			br = ddr_base(i) | (ddr_attr(i) << 8) | ddr_target(i); 
			sz = ((ddr_size(i) - 1) & 0xffff0000);

			/* Set the first free ETH window */
			for (j = 0; j < MV_WIN_ETH_MAX; j++) {
				if (eth_bare_read(base, j) == 0)
					continue;

				win_eth_br_write(base, j, br);
				win_eth_sz_write(base, j, sz);

				/* XXX remapping ETH windows not supported */

				/* Set protection RW */
				eth_epap_write(base, j, 0x3);

				/* Enable window */
				eth_bare_write(base, j, 0);
				break;
			}
		}
}

static int
decode_win_eth_valid(void)
{

	return (decode_win_can_cover_ddr(MV_WIN_ETH_MAX));
}

/**************************************************************************
 * PCIE windows routines
 **************************************************************************/

static void
decode_win_pcie_setup(uint32_t base)
{
	uint32_t size = 0;
	uint32_t cr, br;
	int i, j;

	for (i = 0; i < MV_PCIE_BAR_MAX; i++)
		pcie_bar_write(base, i, 0);

	for (i = 0; i < MV_WIN_PCIE_MAX; i++) {
		win_pcie_cr_write(base, i, 0);
		win_pcie_br_write(base, i, 0);
		win_pcie_remap_write(base, i, 0);
	}

	for (i = 0; i < MV_WIN_DDR_MAX; i++) {
		if (ddr_is_active(i)) {
			/* Map DDR to BAR 1 */
			cr = (ddr_size(i) - 1) & 0xffff0000;
			size += ddr_size(i) & 0xffff0000;
			cr |= (ddr_attr(i) << 8) | (ddr_target(i) << 4) | 1;
			br = ddr_base(i);

			/* Use the first available PCIE window */
			for (j = 0; j < MV_WIN_PCIE_MAX; j++) {
				if (win_pcie_cr_read(base, j) != 0)
					continue;

				win_pcie_br_write(base, j, br);
				win_pcie_cr_write(base, j, cr);
				break;
			}
		}
	}

	/*
	 * Upper 16 bits in BAR register is interpreted as BAR size
	 * (in 64 kB units) plus 64kB, so substract 0x10000
	 * form value passed to register to get correct value.
	 */
	size -= 0x10000;
	pcie_bar_write(base, 0, size | 1);
}

static int
decode_win_pcie_valid(void)
{

	return (decode_win_can_cover_ddr(MV_WIN_PCIE_MAX));
}

/**************************************************************************
 * IDMA windows routines
 **************************************************************************/
#if defined(SOC_MV_ORION) || defined(SOC_MV_DISCOVERY)
static int
idma_bare_read(int i)
{
	uint32_t v;

	v = win_idma_bare_read();
	v &= (1 << i);

	return (v >> i);
}

static void
idma_bare_write(int i, int val)
{
	uint32_t v;

	v = win_idma_bare_read();
	v &= ~(1 << i);
	v |= (val << i);
	win_idma_bare_write(v);
}

/*
 * Sets channel protection 'val' for window 'w' on channel 'c'
 */
static void
idma_cap_write(int c, int w, int val)
{
	uint32_t v;

	v = win_idma_cap_read(c);
	v &= ~(0x3 << (w * 2));
	v |= (val << (w * 2));
	win_idma_cap_write(c, v);
}

/*
 * Set protection 'val' on all channels for window 'w'
 */
static void
idma_set_prot(int w, int val)
{
	int c;

	for (c = 0; c < MV_IDMA_CHAN_MAX; c++)
		idma_cap_write(c, w, val);
}

static int
win_idma_can_remap(int i)
{

	/* IDMA decode windows 0-3 have remap capability */
	if (i < 4)
		return (1);
	
	return (0);
}

void
decode_win_idma_setup(void)
{
	uint32_t br, sz;
	int i, j;

	/*
	 * Disable and clear all IDMA windows, revoke protection for all channels
	 */
	for (i = 0; i < MV_WIN_IDMA_MAX; i++) {

		idma_bare_write(i, 1);
		win_idma_br_write(i, 0);
		win_idma_sz_write(i, 0);
		if (win_idma_can_remap(i) == 1)
			win_idma_har_write(i, 0);
	}
	for (i = 0; i < MV_IDMA_CHAN_MAX; i++)
		win_idma_cap_write(i, 0);

	/*
	 * Set up access to all active DRAM banks
	 */
	for (i = 0; i < MV_WIN_DDR_MAX; i++)
		if (ddr_is_active(i)) {
			br = ddr_base(i) | (ddr_attr(i) << 8) | ddr_target(i); 
			sz = ((ddr_size(i) - 1) & 0xffff0000);

			/* Place DDR entries in non-remapped windows */
			for (j = 0; j < MV_WIN_IDMA_MAX; j++)
				if (win_idma_can_remap(j) != 1 &&
				    idma_bare_read(j) == 1) {

					/* Configure window */
					win_idma_br_write(j, br);
					win_idma_sz_write(j, sz);

					/* Set protection RW on all channels */
					idma_set_prot(j, 0x3);

					/* Enable window */
					idma_bare_write(j, 0);
					break;
				}
		}

	/*
	 * Remaining targets -- from statically defined table
	 */
	for (i = 0; i < idma_wins_no; i++)
		if (idma_wins[i].target > 0) {
			br = (idma_wins[i].base & 0xffff0000) |
			    (idma_wins[i].attr << 8) | idma_wins[i].target; 
			sz = ((idma_wins[i].size - 1) & 0xffff0000);

			/* Set the first free IDMA window */
			for (j = 0; j < MV_WIN_IDMA_MAX; j++) {
				if (idma_bare_read(j) == 0)
					continue;

				/* Configure window */
				win_idma_br_write(j, br);
				win_idma_sz_write(j, sz);
				if (win_idma_can_remap(j) && idma_wins[j].remap >= 0)
					win_idma_har_write(j, idma_wins[j].remap);

				/* Set protection RW on all channels */
				idma_set_prot(j, 0x3);

				/* Enable window */
				idma_bare_write(j, 0);
				break;
			}
		}
}

int
decode_win_idma_valid(void)
{
	const struct decode_win *wintab;
	int c, i, j, rv;
	uint32_t b, e, s;

	if (idma_wins_no > MV_WIN_IDMA_MAX) {
		printf("IDMA windows: too many entries: %d\n", idma_wins_no);
		return (-1);
	}
	for (i = 0, c = 0; i < MV_WIN_DDR_MAX; i++)
		if (ddr_is_active(i))
			c++;

	if (idma_wins_no > (MV_WIN_IDMA_MAX - c)) {
		printf("IDMA windows: too many entries: %d, available: %d\n",
		    idma_wins_no, MV_WIN_IDMA_MAX - c);
		return (-1);
	}

	wintab = idma_wins;
	rv = 1;
	for (i = 0; i < idma_wins_no; i++, wintab++) {

		if (wintab->target == 0) {
			printf("IDMA window#%d: DDR target window is not supposed "
			    "to be reprogrammed!\n", i);
			rv = 0;
		}

		if (wintab->remap >= 0 && win_cpu_can_remap(i) != 1) {
			printf("IDMA window#%d: not capable of remapping, but "
			    "val 0x%08x defined\n", i, wintab->remap);
			rv = 0;
		}

		s = wintab->size;
		b = wintab->base;
		e = b + s - 1;
		if (s > (0xFFFFFFFF - b + 1)) {
			/* XXX this boundary check should accont for 64bit and
			 * remapping.. */
			printf("IDMA window#%d: no space for size 0x%08x at "
			    "0x%08x\n", i, s, b);
			rv = 0;
			continue;
		}

		j = decode_win_overlap(i, idma_wins_no, &idma_wins[0]);
		if (j >= 0) {
			printf("IDMA window#%d: (0x%08x - 0x%08x) overlaps with "
			    "#%d (0x%08x - 0x%08x)\n", i, b, e, j,
			    idma_wins[j].base,
			    idma_wins[j].base + idma_wins[j].size - 1);
			rv = 0;
		}
	}

	return (rv);
}

void
decode_win_idma_dump(void)
{
	int i;

	for (i = 0; i < MV_WIN_IDMA_MAX; i++) {
		printf("IDMA window#%d: b 0x%08x, s 0x%08x", i,
		    win_idma_br_read(i), win_idma_sz_read(i));
		
		if (win_idma_can_remap(i))
			printf(", ha 0x%08x", win_idma_har_read(i));

		printf("\n");
	}
	for (i = 0; i < MV_IDMA_CHAN_MAX; i++)
		printf("IDMA channel#%d: ap 0x%08x\n", i,
		    win_idma_cap_read(i));
	printf("IDMA windows: bare 0x%08x\n", win_idma_bare_read());
}
#else

/* Provide dummy functions to satisfy the build for SoCs not equipped with IDMA */
int
decode_win_idma_valid(void)
{

	return (1);
}

void
decode_win_idma_setup(void)
{
}

void
decode_win_idma_dump(void)
{
}
#endif
