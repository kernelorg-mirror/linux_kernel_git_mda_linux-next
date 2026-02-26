.. SPDX-License-Identifier: GPL-2.0

============================
icssg-prueth devlink support
============================

This document describes the devlink features implemented by the ``icssg-prueth``
device driver for the Texas Instruments ICSSG (Industrial Communication
SubSystem Gigabit) PRU Ethernet subsystem.

Parameters
==========

The ``icssg-prueth`` driver implements the following generic parameters.

.. list-table:: Generic parameters implemented
   :widths: 5 5 5 85

   * - Name
     - Type
     - Mode
     - Description
   * - ``ctf_queues``
     - u32
     - runtime
     - Controls Cut-Through Forwarding (CTF) configuration for transmit queues.
       CTF allows packet forwarding to begin before the entire frame is received,
       significantly reducing latency for time-sensitive networking applications.

       **Bit Layout:**

       The parameter uses a 32-bit value where the lower 16 bits control the
       two ICSSG ports. Each port has 8 transmit queues numbered 0-7 (per-port
       numbering):

       - Bits 0-7: Port 0 (emac0) queues 0-7
       - Bits 8-15: Port 1 (emac1) queues 0-7
       - Bits 16-31: Reserved

       Each bit enables or disables CTF for the corresponding queue. For example,
       bit 0 controls queue 0 of Port 0, and bit 8 controls queue 0 of Port 1.

       **Requirements:**

       CTF requires specific hardware and driver configuration:

       - Both Ethernet ports must be configured in the device tree
       - Driver must be in switch mode or HSR offload mode

       **Mode validation:**

       - In switch mode: Both ports must be added to a bridge
       - In HSR offload mode: HSR interface must be configured
       - Attempting to configure CTF in unsupported modes (e.g., dual-EMAC
         mode with independent ports) will fail with error:
         "CTF only supported in switch or HSR offload mode"
       - If only one port is configured in device tree, CTF configuration
         will fail with error: "CTF requires both ports configured"

       The driver validates mode requirements for both get and set operations.
       Configuration changes are rejected immediately if requirements are not
       met, ensuring users receive clear feedback about unsupported
       configurations.

       **Examples:**

       Enable CTF on all queues for both ports (0xffff = 65535):

       .. code-block:: bash

          devlink dev param set platform/icssg1-eth name ctf_queues \
            value 65535 cmode runtime

       Enable CTF only on port 0, all queues (0x00ff = 255):

       .. code-block:: bash

          devlink dev param set platform/icssg1-eth name ctf_queues \
            value 255 cmode runtime

       Enable CTF only on queue 0 of port 0 and queue 0 of port 1 (0x0101 = 257):

       .. code-block:: bash

          devlink dev param set platform/icssg1-eth name ctf_queues \
            value 257 cmode runtime

       Disable CTF on all queues:

       .. code-block:: bash

          devlink dev param set platform/icssg1-eth name ctf_queues \
            value 0 cmode runtime

       Check current configuration:

       .. code-block:: bash

          devlink dev param show platform/icssg1-eth name ctf_queues

       **Checking driver mode:**

       To verify the driver is in a compatible mode before configuring CTF:

       For switch mode, check if both ports are in a bridge:

       .. code-block:: bash

          bridge link show

       Both icssg1-eth ports should be listed as bridge members.

       For HSR offload mode, check for HSR interface:

       .. code-block:: bash

          ip link show type hsr

       An HSR interface using the icssg1-eth ports should be present.

       **Notes:**

       - Configuration changes take effect immediately when the interface is up
       - If the interface is down, the configuration is applied during the next
         interface initialization
       - CTF is typically used in industrial automation and time-sensitive
         networking (TSN) scenarios where low latency is critical
       - Bits 16-31 are reserved and ignored; setting them has no effect but
         should be avoided for forward compatibility
