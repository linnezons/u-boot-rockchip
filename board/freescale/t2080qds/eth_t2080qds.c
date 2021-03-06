/*
 * Copyright 2013 Freescale Semiconductor, Inc.
 *
 * Shengzhou Liu <Shengzhou.Liu@freescale.com>
 *
 * SPDX-License-Identifier:     GPL-2.0+
 */

#include <common.h>
#include <command.h>
#include <netdev.h>
#include <asm/mmu.h>
#include <asm/processor.h>
#include <asm/immap_85xx.h>
#include <asm/fsl_law.h>
#include <asm/fsl_serdes.h>
#include <asm/fsl_portals.h>
#include <asm/fsl_liodn.h>
#include <malloc.h>
#include <fm_eth.h>
#include <fsl_mdio.h>
#include <miiphy.h>
#include <phy.h>
#include <asm/fsl_dtsec.h>
#include <asm/fsl_serdes.h>
#include "../common/qixis.h"
#include "../common/fman.h"
#include "t2080qds_qixis.h"

#define EMI_NONE	0xFFFFFFFF
#define EMI1_RGMII1	0
#define EMI1_RGMII2     1
#define EMI1_SLOT1	2
#define EMI1_SLOT2	6
#define EMI1_SLOT3	3
#define EMI1_SLOT4	4
#define EMI1_SLOT5	5
#define EMI2		7

static int mdio_mux[NUM_FM_PORTS];

static const char * const mdio_names[] = {
	"T2080QDS_MDIO_RGMII1",
	"T2080QDS_MDIO_RGMII2",
	"T2080QDS_MDIO_SLOT1",
	"T2080QDS_MDIO_SLOT3",
	"T2080QDS_MDIO_SLOT4",
	"T2080QDS_MDIO_SLOT5",
	"T2080QDS_MDIO_SLOT2",
	"T2080QDS_MDIO_10GC",
};

/* Map SerDes1 8 lanes to default slot, will be initialized dynamically */
static u8 lane_to_slot[] = {3, 3, 3, 3, 1, 1, 1, 1};

static const char *T2080qds_mdio_name_for_muxval(u8 muxval)
{
	return mdio_names[muxval];
}

struct mii_dev *mii_dev_for_muxval(u8 muxval)
{
	struct mii_dev *bus;
	const char *name = T2080qds_mdio_name_for_muxval(muxval);

	if (!name) {
		printf("No bus for muxval %x\n", muxval);
		return NULL;
	}

	bus = miiphy_get_dev_by_name(name);

	if (!bus) {
		printf("No bus by name %s\n", name);
		return NULL;
	}

	return bus;
}

struct T2080qds_mdio {
	u8 muxval;
	struct mii_dev *realbus;
};

static void T2080qds_mux_mdio(u8 muxval)
{
	u8 brdcfg4;
	if (muxval < 7) {
		brdcfg4 = QIXIS_READ(brdcfg[4]);
		brdcfg4 &= ~BRDCFG4_EMISEL_MASK;
		brdcfg4 |= (muxval << BRDCFG4_EMISEL_SHIFT);
		QIXIS_WRITE(brdcfg[4], brdcfg4);
	}
}

static int T2080qds_mdio_read(struct mii_dev *bus, int addr, int devad,
				int regnum)
{
	struct T2080qds_mdio *priv = bus->priv;

	T2080qds_mux_mdio(priv->muxval);

	return priv->realbus->read(priv->realbus, addr, devad, regnum);
}

static int T2080qds_mdio_write(struct mii_dev *bus, int addr, int devad,
				int regnum, u16 value)
{
	struct T2080qds_mdio *priv = bus->priv;

	T2080qds_mux_mdio(priv->muxval);

	return priv->realbus->write(priv->realbus, addr, devad, regnum, value);
}

static int T2080qds_mdio_reset(struct mii_dev *bus)
{
	struct T2080qds_mdio *priv = bus->priv;

	return priv->realbus->reset(priv->realbus);
}

static int T2080qds_mdio_init(char *realbusname, u8 muxval)
{
	struct T2080qds_mdio *pmdio;
	struct mii_dev *bus = mdio_alloc();

	if (!bus) {
		printf("Failed to allocate T2080QDS MDIO bus\n");
		return -1;
	}

	pmdio = malloc(sizeof(*pmdio));
	if (!pmdio) {
		printf("Failed to allocate T2080QDS private data\n");
		free(bus);
		return -1;
	}

	bus->read = T2080qds_mdio_read;
	bus->write = T2080qds_mdio_write;
	bus->reset = T2080qds_mdio_reset;
	sprintf(bus->name, T2080qds_mdio_name_for_muxval(muxval));

	pmdio->realbus = miiphy_get_dev_by_name(realbusname);

	if (!pmdio->realbus) {
		printf("No bus with name %s\n", realbusname);
		free(bus);
		free(pmdio);
		return -1;
	}

	pmdio->muxval = muxval;
	bus->priv = pmdio;

	return mdio_register(bus);
}

void board_ft_fman_fixup_port(void *fdt, char *compat, phys_addr_t addr,
				enum fm_port port, int offset)
{
	int phy;
	char alias[20];
	struct fixed_link f_link;
	ccsr_gur_t *gur = (void *)(CONFIG_SYS_MPC85xx_GUTS_ADDR);
	u32 srds_s1 = in_be32(&gur->rcwsr[4]) &
				FSL_CORENET2_RCWSR4_SRDS1_PRTCL;

	srds_s1 >>= FSL_CORENET2_RCWSR4_SRDS1_PRTCL_SHIFT;

	if (fm_info_get_enet_if(port) == PHY_INTERFACE_MODE_SGMII) {
		phy = fm_info_get_phy_address(port);
		switch (port) {
		case FM1_DTSEC1:
		case FM1_DTSEC2:
		case FM1_DTSEC9:
		case FM1_DTSEC10:
			sprintf(alias, "phy_sgmii_s3_%x", phy);
			fdt_set_phy_handle(fdt, compat, addr, alias);
			fdt_status_okay_by_alias(fdt, "emi1_slot3");
			break;
		case FM1_DTSEC5:
		case FM1_DTSEC6:
			if (mdio_mux[port] == EMI1_SLOT1) {
				sprintf(alias, "phy_sgmii_s1_%x", phy);
				fdt_set_phy_handle(fdt, compat, addr, alias);
				fdt_status_okay_by_alias(fdt, "emi1_slot1");
			} else if (mdio_mux[port] == EMI1_SLOT2) {
				sprintf(alias, "phy_sgmii_s2_%x", phy);
				fdt_set_phy_handle(fdt, compat, addr, alias);
				fdt_status_okay_by_alias(fdt, "emi1_slot2");
			}
			break;
		default:
			break;
		}

	} else if (fm_info_get_enet_if(port) == PHY_INTERFACE_MODE_XGMII) {
		switch (srds_s1) {
		case 0x66: /* XFI interface */
		case 0x6b:
		case 0x6c:
		case 0x6d:
		case 0x71:
			f_link.phy_id = port;
			f_link.duplex = 1;
			f_link.link_speed = 10000;
			f_link.pause = 0;
			f_link.asym_pause = 0;
			/* no PHY for XFI */
			fdt_delprop(fdt, offset, "phy-handle");
			fdt_setprop(fdt, offset, "fixed-link", &f_link,
				    sizeof(f_link));
			break;
		default:
			break;
		}
	}
}

void fdt_fixup_board_enet(void *fdt)
{
	return;
}

/*
 * This function reads RCW to check if Serdes1{E,F,G,H} is configured
 * as slot 1/2/3 and update the lane_to_slot[] array accordingly
 */
static void initialize_lane_to_slot(void)
{
	ccsr_gur_t *gur = (void *)(CONFIG_SYS_MPC85xx_GUTS_ADDR);
	u32 srds_s1 = in_be32(&gur->rcwsr[4]) &
				FSL_CORENET2_RCWSR4_SRDS1_PRTCL;

	srds_s1 >>= FSL_CORENET2_RCWSR4_SRDS1_PRTCL_SHIFT;

	switch (srds_s1) {
	case 0x51:
	case 0x5f:
	case 0x65:
	case 0x6b:
	case 0x71:
		lane_to_slot[5] = 2;
		lane_to_slot[6] = 2;
		lane_to_slot[7] = 2;
		break;
	case 0xa6:
	case 0x8e:
	case 0x8f:
	case 0x82:
	case 0x83:
	case 0xd3:
	case 0xd9:
	case 0xcb:
		lane_to_slot[6] = 2;
		lane_to_slot[7] = 2;
		break;
	case 0xda:
		lane_to_slot[4] = 3;
		lane_to_slot[5] = 3;
		lane_to_slot[6] = 3;
		lane_to_slot[7] = 3;
		break;
	default:
		break;
	}
}

int board_eth_init(bd_t *bis)
{
#if defined(CONFIG_FMAN_ENET)
	int i, idx, lane, slot, interface;
	struct memac_mdio_info dtsec_mdio_info;
	struct memac_mdio_info tgec_mdio_info;
	ccsr_gur_t *gur = (void *)(CONFIG_SYS_MPC85xx_GUTS_ADDR);
	u32 rcwsr13 = in_be32(&gur->rcwsr[13]);
	u32 srds_s1;

	srds_s1 = in_be32(&gur->rcwsr[4]) &
					FSL_CORENET2_RCWSR4_SRDS1_PRTCL;
	srds_s1 >>= FSL_CORENET2_RCWSR4_SRDS1_PRTCL_SHIFT;

	initialize_lane_to_slot();

	/* Initialize the mdio_mux array so we can recognize empty elements */
	for (i = 0; i < NUM_FM_PORTS; i++)
		mdio_mux[i] = EMI_NONE;

	dtsec_mdio_info.regs =
		(struct memac_mdio_controller *)CONFIG_SYS_FM1_DTSEC_MDIO_ADDR;

	dtsec_mdio_info.name = DEFAULT_FM_MDIO_NAME;

	/* Register the 1G MDIO bus */
	fm_memac_mdio_init(bis, &dtsec_mdio_info);

	tgec_mdio_info.regs =
		(struct memac_mdio_controller *)CONFIG_SYS_FM1_TGEC_MDIO_ADDR;
	tgec_mdio_info.name = DEFAULT_FM_TGEC_MDIO_NAME;

	/* Register the 10G MDIO bus */
	fm_memac_mdio_init(bis, &tgec_mdio_info);

	/* Register the muxing front-ends to the MDIO buses */
	T2080qds_mdio_init(DEFAULT_FM_MDIO_NAME, EMI1_RGMII1);
	T2080qds_mdio_init(DEFAULT_FM_MDIO_NAME, EMI1_RGMII2);
	T2080qds_mdio_init(DEFAULT_FM_MDIO_NAME, EMI1_SLOT1);
	T2080qds_mdio_init(DEFAULT_FM_MDIO_NAME, EMI1_SLOT2);
	T2080qds_mdio_init(DEFAULT_FM_MDIO_NAME, EMI1_SLOT3);
	T2080qds_mdio_init(DEFAULT_FM_MDIO_NAME, EMI1_SLOT4);
	T2080qds_mdio_init(DEFAULT_FM_MDIO_NAME, EMI1_SLOT5);
	T2080qds_mdio_init(DEFAULT_FM_TGEC_MDIO_NAME, EMI2);

	/* Set the two on-board RGMII PHY address */
	fm_info_set_phy_address(FM1_DTSEC3, RGMII_PHY1_ADDR);
	if ((rcwsr13 & FSL_CORENET_RCWSR13_EC2) ==
			FSL_CORENET_RCWSR13_EC2_DTSEC4_RGMII)
		fm_info_set_phy_address(FM1_DTSEC4, RGMII_PHY2_ADDR);
	else
		fm_info_set_phy_address(FM1_DTSEC10, RGMII_PHY2_ADDR);

	switch (srds_s1) {
	case 0x1c:
	case 0x95:
	case 0xa2:
	case 0x94:
		/* SGMII in Slot3 */
		fm_info_set_phy_address(FM1_DTSEC9, SGMII_CARD_PORT1_PHY_ADDR);
		fm_info_set_phy_address(FM1_DTSEC10, SGMII_CARD_PORT2_PHY_ADDR);
		fm_info_set_phy_address(FM1_DTSEC1, SGMII_CARD_PORT3_PHY_ADDR);
		fm_info_set_phy_address(FM1_DTSEC2, SGMII_CARD_PORT4_PHY_ADDR);
		/* SGMII in Slot2 */
		fm_info_set_phy_address(FM1_DTSEC5, SGMII_CARD_PORT3_PHY_ADDR);
		fm_info_set_phy_address(FM1_DTSEC6, SGMII_CARD_PORT4_PHY_ADDR);
		break;
	case 0x51:
	case 0x5f:
	case 0x65:
		/* XAUI/HiGig in Slot3 */
		fm_info_set_phy_address(FM1_10GEC1, FM1_10GEC1_PHY_ADDR);
		/* SGMII in Slot2 */
		fm_info_set_phy_address(FM1_DTSEC5, SGMII_CARD_PORT3_PHY_ADDR);
		fm_info_set_phy_address(FM1_DTSEC6, SGMII_CARD_PORT4_PHY_ADDR);
		break;
	case 0x66:
		/*
		 * XFI does not need a PHY to work, but to avoid U-boot use
		 * default PHY address which is zero to a MAC when it found
		 * a MAC has no PHY address, we give a PHY address to XFI
		 * MAC, and should not use a real XAUI PHY address, since
		 * MDIO can access it successfully, and then MDIO thinks
		 * the XAUI card is used for the XFI MAC, which will cause
		 * error.
		 */
		fm_info_set_phy_address(FM1_10GEC1, 4);
		fm_info_set_phy_address(FM1_10GEC2, 5);
		fm_info_set_phy_address(FM1_10GEC3, 6);
		fm_info_set_phy_address(FM1_10GEC4, 7);
		break;
	case 0x6b:
		fm_info_set_phy_address(FM1_10GEC1, 4);
		fm_info_set_phy_address(FM1_10GEC2, 5);
		fm_info_set_phy_address(FM1_10GEC3, 6);
		fm_info_set_phy_address(FM1_10GEC4, 7);
		/* SGMII in Slot2 */
		fm_info_set_phy_address(FM1_DTSEC5, SGMII_CARD_PORT3_PHY_ADDR);
		fm_info_set_phy_address(FM1_DTSEC6, SGMII_CARD_PORT2_PHY_ADDR);
		break;
	case 0x6c:
	case 0x6d:
		fm_info_set_phy_address(FM1_10GEC1, 4);
		fm_info_set_phy_address(FM1_10GEC2, 5);
		/* SGMII in Slot3 */
		fm_info_set_phy_address(FM1_DTSEC1, SGMII_CARD_PORT3_PHY_ADDR);
		fm_info_set_phy_address(FM1_DTSEC2, SGMII_CARD_PORT4_PHY_ADDR);
		break;
	case 0x71:
		/* SGMII in Slot3 */
		fm_info_set_phy_address(FM1_DTSEC1, SGMII_CARD_PORT3_PHY_ADDR);
		fm_info_set_phy_address(FM1_DTSEC2, SGMII_CARD_PORT4_PHY_ADDR);
		/* SGMII in Slot2 */
		fm_info_set_phy_address(FM1_DTSEC5, SGMII_CARD_PORT3_PHY_ADDR);
		fm_info_set_phy_address(FM1_DTSEC6, SGMII_CARD_PORT2_PHY_ADDR);
		break;
	case 0xa6:
	case 0x8e:
	case 0x8f:
	case 0x82:
	case 0x83:
		/* SGMII in Slot3 */
		fm_info_set_phy_address(FM1_DTSEC9, SGMII_CARD_PORT1_PHY_ADDR);
		fm_info_set_phy_address(FM1_DTSEC10, SGMII_CARD_PORT2_PHY_ADDR);
		fm_info_set_phy_address(FM1_DTSEC1, SGMII_CARD_PORT3_PHY_ADDR);
		fm_info_set_phy_address(FM1_DTSEC2, SGMII_CARD_PORT4_PHY_ADDR);
		/* SGMII in Slot2 */
		fm_info_set_phy_address(FM1_DTSEC5, SGMII_CARD_PORT3_PHY_ADDR);
		fm_info_set_phy_address(FM1_DTSEC6, SGMII_CARD_PORT2_PHY_ADDR);
		break;
	case 0xa4:
	case 0x96:
	case 0x8a:
		/* SGMII in Slot3 */
		fm_info_set_phy_address(FM1_DTSEC9, SGMII_CARD_PORT1_PHY_ADDR);
		fm_info_set_phy_address(FM1_DTSEC10, SGMII_CARD_PORT2_PHY_ADDR);
		fm_info_set_phy_address(FM1_DTSEC1, SGMII_CARD_PORT3_PHY_ADDR);
		fm_info_set_phy_address(FM1_DTSEC2, SGMII_CARD_PORT4_PHY_ADDR);
		break;
	case 0xd9:
	case 0xd3:
	case 0xcb:
		/* SGMII in Slot3 */
		fm_info_set_phy_address(FM1_DTSEC10, SGMII_CARD_PORT2_PHY_ADDR);
		fm_info_set_phy_address(FM1_DTSEC1, SGMII_CARD_PORT3_PHY_ADDR);
		fm_info_set_phy_address(FM1_DTSEC2, SGMII_CARD_PORT4_PHY_ADDR);
		/* SGMII in Slot2 */
		fm_info_set_phy_address(FM1_DTSEC5, SGMII_CARD_PORT3_PHY_ADDR);
		fm_info_set_phy_address(FM1_DTSEC6, SGMII_CARD_PORT2_PHY_ADDR);
		break;
	default:
		break;
	}

	for (i = FM1_DTSEC1; i < FM1_DTSEC1 + CONFIG_SYS_NUM_FM1_DTSEC; i++) {
		idx = i - FM1_DTSEC1;
		interface = fm_info_get_enet_if(i);
		switch (interface) {
		case PHY_INTERFACE_MODE_SGMII:
			lane = serdes_get_first_lane(FSL_SRDS_1,
					SGMII_FM1_DTSEC1 + idx);
			if (lane < 0)
				break;
			slot = lane_to_slot[lane];
			debug("FM1@DTSEC%u expects SGMII in slot %u\n",
			      idx + 1, slot);
			if (QIXIS_READ(present2) & (1 << (slot - 1)))
				fm_disable_port(i);

			switch (slot) {
			case 1:
				mdio_mux[i] = EMI1_SLOT1;
				fm_info_set_mdio(i, mii_dev_for_muxval(
						 mdio_mux[i]));
				break;
			case 2:
				mdio_mux[i] = EMI1_SLOT2;
				fm_info_set_mdio(i, mii_dev_for_muxval(
						 mdio_mux[i]));
				break;
			case 3:
				mdio_mux[i] = EMI1_SLOT3;
				fm_info_set_mdio(i, mii_dev_for_muxval(
						mdio_mux[i]));
				break;
			}
			break;
		case PHY_INTERFACE_MODE_RGMII:
			if (i == FM1_DTSEC3)
				mdio_mux[i] = EMI1_RGMII1;
			else if (i == FM1_DTSEC4 || FM1_DTSEC10)
				mdio_mux[i] = EMI1_RGMII2;
			fm_info_set_mdio(i, mii_dev_for_muxval(mdio_mux[i]));
			break;
		default:
			break;
		}
	}

	for (i = FM1_10GEC1; i < FM1_10GEC1 + CONFIG_SYS_NUM_FM1_10GEC; i++) {
		idx = i - FM1_10GEC1;
		switch (fm_info_get_enet_if(i)) {
		case PHY_INTERFACE_MODE_XGMII:
			if (srds_s1 == 0x51) {
				lane = serdes_get_first_lane(FSL_SRDS_1,
						XAUI_FM1_MAC9 + idx);
			} else if ((srds_s1 == 0x5f) || (srds_s1 == 0x65)) {
				lane = serdes_get_first_lane(FSL_SRDS_1,
						HIGIG_FM1_MAC9 + idx);
			} else {
				if (i == FM1_10GEC1 || i == FM1_10GEC2)
					lane = serdes_get_first_lane(FSL_SRDS_1,
						XFI_FM1_MAC9 + idx);
				else
					lane = serdes_get_first_lane(FSL_SRDS_1,
						XFI_FM1_MAC1 + idx);
			}

			if (lane < 0)
				break;
			mdio_mux[i] = EMI2;
			fm_info_set_mdio(i, mii_dev_for_muxval(mdio_mux[i]));

			if ((srds_s1 == 0x66) || (srds_s1 == 0x6b) ||
			    (srds_s1 == 0x6c) || (srds_s1 == 0x6d) ||
			    (srds_s1 == 0x71)) {
				/* As XFI is in cage intead of a slot, so
				 * ensure doesn't disable the corresponding port
				 */
				break;
			}

			slot = lane_to_slot[lane];
			if (QIXIS_READ(present2) & (1 << (slot - 1)))
				fm_disable_port(i);
			break;
		default:
			break;
		}
	}

	cpu_eth_init(bis);
#endif /* CONFIG_FMAN_ENET */

	return pci_eth_init(bis);
}
