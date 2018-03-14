
t_stat arp_reset (void)
{
  return SCPE_OK;
}

/* Set Ethernet interface MAC address and IP address. */
t_stat arp_set_address (uint8 *mac, uint8 *ip)
{
  return SCPE_OK;
}

/* Resolve IP address with ARP. */
t_stat arp_resolve_ip (uint8 *address)
{
  return SCPE_OK;
}

/* Send IP packet to Ethernet, resolving the address with ARP if necessary. */
t_stat arp_send_ip (uint8 *packet, int n)
{
  return SCPE_OK;
}
