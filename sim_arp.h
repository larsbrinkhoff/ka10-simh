/* sim_arp.h: ARP
  ------------------------------------------------------------------------------

   Copyright (c) 2018, Lars Brinkhoff

  ------------------------------------------------------------------------------
*/

#ifndef SIM_ARP_H
#define SIM_ARP_H

#include "sim_defs.h"

#ifdef  __cplusplus
extern "C" {
#endif

t_stat arp_reset (void);

/* Set Ethernet interface MAC address and IP address. */
t_stat arp_set_address (uint8 *mac, uint8 *ip);

/* Resolve IP address with ARP. */
t_stat arp_resolve_ip (uint8 *address);

/* Send IP packet to Ethernet, resolving the address with ARP if necessary. */
t_stat arp_send_ip (uint8 *packet, int n);

#ifdef  __cplusplus
}
#endif

#endif /* SIM_ARP_H */

