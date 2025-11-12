# Linux CDC-NCM Gadget Notes

CherryUSB’s CDC-NCM host driver now negotiates the full control handshake
required by the Linux USB gadget stack. This note summarises the steps needed to
link an ME15 (or similar) Linux gadget to the ESP32-S3 CherryUSB host.

## Build-time requirements

- Make sure the host build enables `CONFIG_USBHOST_PLATFORM_CDC_NCM`.
- The default CDC-NCM RX/TX buffers are now 16 KB. Override
  `CONFIG_USBHOST_CDC_NCM_ETH_MAX_RX_SIZE` / `_TX_SIZE` if you need a different
  value.
- No additional application code is required to start DHCP—the IDF glue brings
  the interface up as `u1` and immediately launches the DHCP client.

## What the stack negotiates automatically

On connect CherryUSB now issues the class control requests expected by the
Linux gadget driver:

1. `SET_NTB_FORMAT` – selects NTB16 when supported (NTB32 otherwise).
2. `SET_NTB_INPUT_SIZE` – programs the host’s chosen NTB IN size (capped by the
   configured RX buffer) and datagram count.
3. `SET_MAX_DATAGRAM_SIZE` – aligns the gadget with the host’s Ethernet MTU
   (typically 1514 bytes).
4. `SET_ETHERNET_PACKET_FILTER` – enables directed, multicast and broadcast
   traffic so the gadget can exchange control frames (DHCP/ARP/etc.).

The driver also honours the gadget’s upstream NTB limits when packing outbound
frames.

## Expected logs

With the gadget connected you should see output similar to:

```
[I/usbh_core] Loading cdc_ncm class driver
[I/usbh_cdc_ncm] CDC NCM MAC address 42:d6:8d:8b:00:aa
[I/usbh_cdc_ncm] CDC NCM Max Segment Size:1514
CDC NCM ntb parameters:
  ...
[I/usbh_cdc_ncm] CDC NCM configured: NTB format 16, NTB input 16384 bytes,
                 max datagram 1514, datagrams 1
[I/usbh_net_netif_glue] NETIF u1 Got IP Address
```

Once `u1` receives its lease, the interface is fully operational. Use standard
LWIP APIs (or the helper hooks in `platform/idf/usbh_net.c`) to monitor link
status or add additional diagnostics.
