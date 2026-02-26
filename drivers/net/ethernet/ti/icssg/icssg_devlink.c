// SPDX-License-Identifier: GPL-2.0
/* Texas Instruments ICSSG Ethernet Driver - Devlink Support
 *
 * Copyright (C) 2024 Texas Instruments Incorporated - https://www.ti.com/
 *
 * Devlink interface for ICSSG driver configuration including:
 * - Cut-Through Forwarding (CTF) per-queue configuration
 *
 * Queue Mask Layout (u32 parameter, ICSSG uses lower 16 bits):
 *   Bits 0-7:  Port 0 (emac0) queues 0-7
 *   Bits 8-15: Port 1 (emac1) queues 8-15
 *   Bits 16-31: Reserved for future use
 *
 * Examples:
 *   0x00ff (255): Port 0 all queues, Port 1 none
 *   0xff00 (65280): Port 0 none, Port 1 all queues
 *   0xffff (65535): Both ports all queues
 *   0x0101 (257): Port 0 queue 0, Port 1 queue 8
 */

#include <net/devlink.h>
#include "icssg_prueth.h"
#include "icssg_switch_map.h"

#define ICSSG_CUT_THRU_BIT		BIT(7)
#define ICSSG_QUEUES_PER_PORT		8
#define ICSSG_QUEUE_MASK		GENMASK(ICSSG_QUEUES_PER_PORT - 1, 0)

/**
 * icssg_config_cut_thru() - Configure CTF for an emac's queues
 * @emac: ICSSG EMAC instance
 *
 * Configures the EXPRESS_PRE_EMPTIVE_Q_MAP register in firmware based on
 * the stored cut_thru_queue_map value. Each emac has its own dram.va memory
 * region containing queue configuration bytes.
 *
 * This function should be called during interface initialization after the
 * firmware is loaded and memory is accessible.
 */
void icssg_config_cut_thru(struct prueth_emac *emac)
{
	void __iomem *config;
	u8 queue_map;
	u8 val;
	int i;

	if (!emac || !emac->dram.va)
		return;

	config = emac->dram.va + ICSSG_CONFIG_OFFSET;
	queue_map = emac->cut_thru_queue_map;

	for (i = 0; i < ICSSG_QUEUES_PER_PORT; i++) {
		val = readb(config + EXPRESS_PRE_EMPTIVE_Q_MAP + i);

		if (queue_map & BIT(i))
			val |= ICSSG_CUT_THRU_BIT;
		else
			val &= ~ICSSG_CUT_THRU_BIT;

		writeb(val, config + EXPRESS_PRE_EMPTIVE_Q_MAP + i);
		netdev_dbg(emac->ndev, "CTF %s for queue %d\n",
			   (queue_map & BIT(i)) ? "enabled" : "disabled", i);
	}
}

/**
 * prueth_dl_cut_thru_check() - Read current CTF configuration from firmware
 * @emac: ICSSG EMAC instance
 *
 * Returns: u8 bitmap with bits 0-7 representing queue CTF status
 */
static u8 prueth_dl_cut_thru_check(struct prueth_emac *emac)
{
	void __iomem *config = emac->dram.va + ICSSG_CONFIG_OFFSET;
	u8 queue_map = 0;
	int i;

	for (i = 0; i < ICSSG_QUEUES_PER_PORT; i++) {
		if (readb(config + EXPRESS_PRE_EMPTIVE_Q_MAP + i) & ICSSG_CUT_THRU_BIT)
			queue_map |= BIT(i);
	}

	return queue_map;
}

/**
 * icssg_dl_ctf_validate() - Validate CTF parameter requirements
 * @prueth: ICSSG driver private data
 * @id: Parameter ID
 * @extack: Netlink extended ACK for error reporting
 *
 * Validates that CTF requirements are met: correct parameter ID, dual-port
 * configuration, and switch or HSR offload mode.
 *
 * Return: 0 if validation passes, negative error code otherwise
 */
static int icssg_dl_ctf_validate(struct prueth *prueth, u32 id,
				 struct netlink_ext_ack *extack)
{
	if (id != DEVLINK_PARAM_GENERIC_ID_CTF_QUEUES)
		return -EOPNOTSUPP;

	/* CTF only supported in dual-port offload mode */
	if (!prueth->emac[PRUETH_MAC0] || !prueth->emac[PRUETH_MAC1]) {
		NL_SET_ERR_MSG_MOD(extack, "CTF requires both ports configured");
		return -EOPNOTSUPP;
	}

	if (!prueth->is_switch_mode && !prueth->is_hsr_offload_mode) {
		NL_SET_ERR_MSG_MOD(extack, "CTF only supported in switch or HSR offload mode");
		return -EOPNOTSUPP;
	}

	return 0;
}

/**
 * icssg_dl_ctf_get() - Get CTF queue configuration
 * @devlink: Devlink instance
 * @id: Parameter ID
 * @ctx: Context for getting parameter value
 * @extack: Netlink extended ACK for error reporting
 *
 * Reads current CTF configuration from firmware for all ports. If the
 * interface is not yet opened (dram.va is NULL), returns the stored
 * configuration value instead.
 *
 * CTF is only supported when both ports are configured and the driver
 * is in offload mode (switch mode or HSR offload mode).
 */
static int icssg_dl_ctf_get(struct devlink *devlink, u32 id,
			    struct devlink_param_gset_ctx *ctx,
			    struct netlink_ext_ack *extack)
{
	struct prueth_devlink *dl_priv = devlink_priv(devlink);
	struct prueth *prueth = dl_priv->prueth;
	struct prueth_emac *emac;
	u32 tx_queues = 0;
	u8 queue_map;
	int slice;
	int ret;

	ret = icssg_dl_ctf_validate(prueth, id, extack);
	if (ret)
		return ret;

	for (slice = PRUETH_MAC0; slice < PRUETH_NUM_MACS; slice++) {
		emac = prueth->emac[slice];

		/* If interface is up, read from firmware; otherwise use stored value */
		if (emac->dram.va)
			queue_map = prueth_dl_cut_thru_check(emac);
		else
			queue_map = emac->cut_thru_queue_map;

		tx_queues |= (u32)queue_map << (ICSSG_QUEUES_PER_PORT * slice);
	}

	ctx->val.vu32 = tx_queues;

	return 0;
}

/**
 * icssg_dl_ctf_set() - Set CTF queue configuration
 * @devlink: Devlink instance
 * @id: Parameter ID
 * @ctx: Context containing new parameter value
 * @extack: Netlink extended ACK for error reporting
 *
 * Stores CTF configuration and applies it immediately if the interface is up.
 * If the interface is down, the configuration will be applied during the next
 * interface initialization.
 *
 * CTF is only supported when both ports are configured and the driver
 * is in offload mode (switch mode or HSR offload mode).
 */
static int icssg_dl_ctf_set(struct devlink *devlink, u32 id,
			    struct devlink_param_gset_ctx *ctx,
			    struct netlink_ext_ack *extack)
{
	struct prueth_devlink *dl_priv = devlink_priv(devlink);
	struct prueth *prueth = dl_priv->prueth;
	u32 tx_queues = ctx->val.vu32;
	struct prueth_emac *emac;
	int slice;
	int ret;

	ret = icssg_dl_ctf_validate(prueth, id, extack);
	if (ret)
		return ret;

	for (slice = PRUETH_MAC0; slice < PRUETH_NUM_MACS; slice++) {
		emac = prueth->emac[slice];

		/* Store the configuration */
		emac->cut_thru_queue_map =
			(tx_queues >> (ICSSG_QUEUES_PER_PORT * slice)) & ICSSG_QUEUE_MASK;

		/* If interface is up, apply immediately to firmware */
		if (emac->dram.va)
			icssg_config_cut_thru(emac);
	}

	return 0;
}

static const struct devlink_param icssg_devlink_params[] = {
	DEVLINK_PARAM_GENERIC(CTF_QUEUES,
			      BIT(DEVLINK_PARAM_CMODE_RUNTIME),
			      icssg_dl_ctf_get,
			      icssg_dl_ctf_set, NULL),
};

static const struct devlink_ops icssg_devlink_ops = {
};

/**
 * icssg_devlink_register() - Register devlink instance for ICSSG
 * @prueth: ICSSG driver private data
 *
 * Creates and registers a devlink instance with CTF parameter support.
 */
int icssg_devlink_register(struct prueth *prueth)
{
	struct prueth_devlink *dl_priv;
	struct device *dev = prueth->dev;
	int ret;

	prueth->devlink = devlink_alloc(&icssg_devlink_ops,
					sizeof(*dl_priv), dev);
	if (!prueth->devlink)
		return -ENOMEM;

	dl_priv = devlink_priv(prueth->devlink);
	dl_priv->prueth = prueth;

	ret = devlink_params_register(prueth->devlink, icssg_devlink_params,
				      ARRAY_SIZE(icssg_devlink_params));
	if (ret) {
		dev_err(dev, "devlink params register failed: %d\n", ret);
		devlink_free(prueth->devlink);
		return ret;
	}

	devlink_register(prueth->devlink);

	dev_dbg(dev, "devlink registered with CTF support\n");
	dev_dbg(dev, "  CTF queue mask (u32): bits 0-7=port0, 8-15=port1\n");

	return 0;
}
EXPORT_SYMBOL_GPL(icssg_devlink_register);

/**
 * icssg_devlink_unregister() - Unregister devlink instance
 * @prueth: ICSSG driver private data
 */
void icssg_devlink_unregister(struct prueth *prueth)
{
	if (!prueth->devlink)
		return;

	devlink_unregister(prueth->devlink);
	devlink_params_unregister(prueth->devlink, icssg_devlink_params,
				  ARRAY_SIZE(icssg_devlink_params));
	devlink_free(prueth->devlink);
	prueth->devlink = NULL;
}
EXPORT_SYMBOL_GPL(icssg_devlink_unregister);
