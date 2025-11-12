# Linux RNDIS Gadget Compatibility Notes

This note explains how to enable CherryUSB’s host stack to bind to RNDIS
interfaces exposed by Linux gadgets (class `0x02`, subclass `0x02`, protocol
`0xFF`), such as the ME15.

## Why an extra switch?

Upstream CherryUSB only matches the Microsoft wireless-controller signature
(`0xE0/0x01/0x03`). When a gadget advertises the legacy CDC-ACM signature the
host stack prints:

```
[E/usbh_core] do not support Class:0x02,Subclass:0x02,Protocl:0xFF
```

and never loads the RNDIS driver.

## Build-time define

CherryUSB now exposes `CONFIG_USBHOST_RNDIS_LINUX_GADGET`. When this macro is
enabled (set in your project’s `usb_config.h` or via Kconfig), the class matcher
in `class/wireless/usbh_rndis.c` switches to the CDC-ACM signature required by
Linux gadgets. With the macro unset, the original wireless signature remains the
default.

## Quick enablement checklist

1. In your CherryUSB configuration, uncomment or define
   `CONFIG_USBHOST_RNDIS_LINUX_GADGET`.
2. Rebuild the host firmware.
3. On enumeration you should now see:
   
   ```
   [I/usbh_core] Loading rndis class driver
   [I/usbh_rndis] rndis init success
   ```

Once the class binds, the existing ESP-IDF glue (`platform/idf/usbh_net.c`) will
instantiate netif `u2`, allowing the ME15 to communicate with the ESP32-S3 over
USB.
