# Linux CDC-NCM Gadget Notes

CherryUSB’s CDC-NCM host driver has been tuned to match the control
sequence that worked in the AndroidNetworkBridge project. The stack keeps
permission checks to a minimum, asserts the control lines, and then moves
straight into data mode using the descriptor-provided defaults.

## Build-time requirements

- Make sure the host build enables `CONFIG_USBHOST_PLATFORM_CDC_NCM`.
- The default CDC-NCM RX/TX buffers are 16 KB. Override
  `CONFIG_USBHOST_CDC_NCM_ETH_MAX_RX_SIZE` / `_TX_SIZE` if you need a different
  value.
- No additional application code is required to start DHCP—the IDF glue brings
  the interface up as `u1` and immediately launches the DHCP client. If you
  prefer static addressing, stop DHCP and assign an IP before starting any
  traffic (see `main/main.c` in the ESP-IDF example).

## What the stack negotiates automatically

On connect CherryUSB now performs the minimal CDC Ethernet handshake that
proved reliable with the ME15 gadget:

1. `SET_CONTROL_LINE_STATE` (DTR/RTS asserted) so the gadget leaves its reset
   state.
2. `SET_ETHERNET_PACKET_FILTER` enabling directed, multicast and broadcast
   frames.

The NTB format, maximum NTB size, datagram count and Ethernet segment size are
sourced from the gadget’s descriptors and left unchanged—many Linux gadgets
stall the optional `SET_NTB_*`/`SET_MAX_DATAGRAM_SIZE` requests, so the driver
no longer depends on them.

## Expected logs

With the gadget connected you should see output similar to:

```
[I/usbh_core] Loading cdc_ncm class driver
[I/usbh_cdc_ncm] CDC NCM MAC address 42:d6:8d:8b:00:aa
[I/usbh_cdc_ncm] CDC NCM Max Segment Size:1514
CDC NCM ntb parameters:
  ...
[I/usbh_cdc_ncm] CDC NCM configured using descriptor defaults: NTB input 16384 bytes, max datagram 1514
[I/usbh_net_netif_glue] NETIF u1 Got IP Address
```

Once `u1` receives its lease (or your static address is applied), the interface
is fully operational. Use standard LWIP APIs (or the helper hooks in
`platform/idf/usbh_net.c`) to monitor link status or add additional
diagnostics.
