/*
 * Copyright (c) 2024, sakumisu
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "usbh_core.h"
#include "usbh_cdc_ncm.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/etharp.h"
#include "lwip/prot/ethernet.h"

#undef USB_DBG_TAG
#define USB_DBG_TAG "usbh_cdc_ncm"
#include "usb_log.h"

#define DEV_FORMAT "/dev/cdc_ncm"

/* general descriptor field offsets */
#define DESC_bLength            0 /** Length offset */
#define DESC_bDescriptorType    1 /** Descriptor type offset */
#define DESC_bDescriptorSubType 2 /** Descriptor subtype offset */

/* interface descriptor field offsets */
#define INTF_DESC_bInterfaceNumber  2 /** Interface number offset */
#define INTF_DESC_bAlternateSetting 3 /** Alternate setting offset */

#define CONFIG_USBHOST_CDC_NCM_ETH_MAX_SEGSZE 1514U

#define CDC_NCM_PACKET_FILTER_DEFAULT 0x000E
#define CDC_NCM_NTB_FORMAT_16        0x0000
#define CDC_NCM_NTB_FORMAT_32        0x0001
#define CDC_NCM_CRC_MODE_CRC16       0x0000
#define CDC_NCM_CRC_MODE_NO_CRC      0x0001

struct cdc_ncm_ntb_input_size_cmd {
    uint32_t dwNtbInMaxSize;
    uint16_t wNtbInMaxDatagrams;
    uint16_t wReserved;
} __PACKED;

struct cdc_ncm_max_datagram_cmd {
    uint16_t wMaxDatagramSize;
    uint16_t wReserved;
} __PACKED;

static USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t g_cdc_ncm_rx_buffer[CONFIG_USBHOST_CDC_NCM_ETH_MAX_RX_SIZE];
static USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t g_cdc_ncm_tx_buffer[CONFIG_USBHOST_CDC_NCM_ETH_MAX_TX_SIZE];
static USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t g_cdc_ncm_inttx_buffer[USB_ALIGN_UP(16, CONFIG_USB_ALIGN_SIZE)];

static USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t g_cdc_ncm_buf[USB_ALIGN_UP(32, CONFIG_USB_ALIGN_SIZE)];

static struct usbh_cdc_ncm g_cdc_ncm_class;
struct netif *ncm_netif = NULL;

static int usbh_cdc_ncm_get_ntb_parameters(struct usbh_cdc_ncm *cdc_ncm_class, struct cdc_ncm_ntb_parameters *param)
{
    struct usb_setup_packet *setup;
    int ret;

    if (!cdc_ncm_class || !cdc_ncm_class->hport) {
        return -USB_ERR_INVAL;
    }
    setup = cdc_ncm_class->hport->setup;

    setup->bmRequestType = USB_REQUEST_DIR_IN | USB_REQUEST_CLASS | USB_REQUEST_RECIPIENT_INTERFACE;
    setup->bRequest = CDC_REQUEST_GET_NTB_PARAMETERS;
    setup->wValue = 0;
    setup->wIndex = cdc_ncm_class->ctrl_intf;
    setup->wLength = 28;

    ret = usbh_control_transfer(cdc_ncm_class->hport, setup, g_cdc_ncm_buf);
    if (ret < 8) {
        return ret;
    }

    memcpy((uint8_t *)param, g_cdc_ncm_buf, MIN(ret - 8, sizeof(struct cdc_ncm_ntb_parameters)));
    return 0;
}

static void print_ntb_parameters(const struct cdc_ncm_ntb_parameters *param)
{
    USB_LOG_RAW("CDC NCM ntb parameters:\r\n");
    USB_LOG_RAW("wLength: 0x%02x             \r\n", param->wLength);
    USB_LOG_RAW("bmNtbFormatsSupported: %s     \r\n", param->bmNtbFormatsSupported ? "NTB16" : "NTB32");

    USB_LOG_RAW("dwNtbInMaxSize: 0x%08x           \r\n", (unsigned int)param->dwNtbInMaxSize);
    USB_LOG_RAW("wNdbInDivisor: 0x%02x \r\n", param->wNdbInDivisor);
    USB_LOG_RAW("wNdbInPayloadRemainder: 0x%02x      \r\n", param->wNdbInPayloadRemainder);
    USB_LOG_RAW("wNdbInAlignment: 0x%02x    \r\n", param->wNdbInAlignment);

    USB_LOG_RAW("dwNtbOutMaxSize: 0x%08x     \r\n", (unsigned int)param->dwNtbOutMaxSize);
    USB_LOG_RAW("wNdbOutDivisor: 0x%02x     \r\n", param->wNdbOutDivisor);
    USB_LOG_RAW("wNdbOutPayloadRemainder: 0x%02x     \r\n", param->wNdbOutPayloadRemainder);
    USB_LOG_RAW("wNdbOutAlignment: 0x%02x     \r\n", param->wNdbOutAlignment);

    USB_LOG_RAW("wNtbOutMaxDatagrams: 0x%02x     \r\n", param->wNtbOutMaxDatagrams);
}


static int usbh_cdc_ncm_set_packet_filter(struct usbh_cdc_ncm *cdc_ncm_class, uint16_t filter)
{
    struct usb_setup_packet *setup;

    if (!cdc_ncm_class || !cdc_ncm_class->hport) {
        return -USB_ERR_INVAL;
    }

    setup = cdc_ncm_class->hport->setup;
    setup->bmRequestType = USB_REQUEST_DIR_OUT | USB_REQUEST_CLASS | USB_REQUEST_RECIPIENT_INTERFACE;
    setup->bRequest = CDC_REQUEST_SET_ETHERNET_PACKET_FILTER;
    setup->wValue = filter;
    setup->wIndex = cdc_ncm_class->ctrl_intf;
    setup->wLength = 0;

    USB_LOG_DBG("SET_ETHERNET_PACKET_FILTER 0x%04x\r\n", filter);

    return usbh_control_transfer(cdc_ncm_class->hport, setup, NULL);
}

static int usbh_cdc_ncm_set_ntb_format(struct usbh_cdc_ncm *cdc_ncm_class, uint16_t format)
{
    struct usb_setup_packet *setup;

    if (!cdc_ncm_class || !cdc_ncm_class->hport) {
        return -USB_ERR_INVAL;
    }

    setup = cdc_ncm_class->hport->setup;
    setup->bmRequestType = USB_REQUEST_DIR_OUT | USB_REQUEST_CLASS | USB_REQUEST_RECIPIENT_INTERFACE;
    setup->bRequest = CDC_REQUEST_SET_NTB_FORMAT;
    setup->wValue = format;
    setup->wIndex = cdc_ncm_class->ctrl_intf;
    setup->wLength = 0;

    USB_LOG_DBG("SET_NTB_FORMAT 0x%04x\r\n", format);
    return usbh_control_transfer(cdc_ncm_class->hport, setup, NULL);
}

static int usbh_cdc_ncm_set_crc_mode(struct usbh_cdc_ncm *cdc_ncm_class, uint16_t mode)
{
    struct usb_setup_packet *setup;

    if (!cdc_ncm_class || !cdc_ncm_class->hport) {
        return -USB_ERR_INVAL;
    }

    setup = cdc_ncm_class->hport->setup;
    setup->bmRequestType = USB_REQUEST_DIR_OUT | USB_REQUEST_CLASS | USB_REQUEST_RECIPIENT_INTERFACE;
    setup->bRequest = CDC_REQUEST_SET_CRC_MODE;
    setup->wValue = mode;
    setup->wIndex = cdc_ncm_class->ctrl_intf;
    setup->wLength = 0;

    USB_LOG_DBG("SET_CRC_MODE 0x%04x\r\n", mode);
    return usbh_control_transfer(cdc_ncm_class->hport, setup, NULL);
}

static int usbh_cdc_ncm_configure(struct usbh_cdc_ncm *cdc_ncm_class)
{
    int ret;
    uint32_t host_ntb_in_size;
    uint16_t host_max_datagram;

    host_ntb_in_size = cdc_ncm_class->ntb_param.dwNtbInMaxSize;
    if (host_ntb_in_size == 0 || host_ntb_in_size > CONFIG_USBHOST_CDC_NCM_ETH_MAX_RX_SIZE) {
        host_ntb_in_size = CONFIG_USBHOST_CDC_NCM_ETH_MAX_RX_SIZE;
    }

    host_max_datagram = cdc_ncm_class->max_segment_size;
    if (host_max_datagram == 0 || host_max_datagram > CONFIG_USBHOST_CDC_NCM_ETH_MAX_SEGSZE) {
        host_max_datagram = CONFIG_USBHOST_CDC_NCM_ETH_MAX_SEGSZE;
    }

    /* Linux only programs CRC/format before enabling traffic. Skip optional
     * setters to mimic the gadget-friendly sequence unless a device explicitly
     * requires them.
     */

    ret = usbh_cdc_ncm_set_crc_mode(cdc_ncm_class, CDC_NCM_CRC_MODE_CRC16);
    if (ret < 0 && ret != -USB_ERR_STALL && ret != -USB_ERR_IO) {
        USB_LOG_WRN("Failed to set CRC mode, ret:%d\r\n", ret);
    }

    ret = usbh_cdc_ncm_set_ntb_format(cdc_ncm_class, CDC_NCM_NTB_FORMAT_16);
    if (ret < 0 && ret != -USB_ERR_STALL && ret != -USB_ERR_IO) {
        USB_LOG_WRN("Failed to set NTB format, ret:%d\r\n", ret);
    }

    /* Linux sets altsetting back to 1 after SET_NTB_FORMAT (with ~21ms delay) */
    if (cdc_ncm_class->hport->config.intf[cdc_ncm_class->data_intf].altsetting_num > 1) {
        uint8_t altsetting = cdc_ncm_class->hport->config.intf[cdc_ncm_class->data_intf].altsetting_num - 1;
        usb_osal_msleep(21); /* Match Linux's delay */
        ret = usbh_set_interface(cdc_ncm_class->hport, cdc_ncm_class->data_intf, altsetting);
        if (ret < 0) {
            USB_LOG_WRN("Failed to restore altsetting %u after SET_NTB_FORMAT, ret=%d\r\n", (unsigned int)altsetting, ret);
        }
    }

    ret = usbh_cdc_ncm_set_packet_filter(cdc_ncm_class, CDC_NCM_PACKET_FILTER_DEFAULT);
    for (int i = 0; ret < 0 && i < 1; i++) {
        usb_osal_msleep(10);
        ret = usbh_cdc_ncm_set_packet_filter(cdc_ncm_class, CDC_NCM_PACKET_FILTER_DEFAULT);
    }
    if (ret < 0) {
        USB_LOG_WRN("Failed to set packet filter, ret:%d\r\n", ret);
    } else {
        for (int i = 0; i < 2; i++) {
            usb_osal_msleep(10);
            usbh_cdc_ncm_set_packet_filter(cdc_ncm_class, CDC_NCM_PACKET_FILTER_DEFAULT);
        }
    }

    cdc_ncm_class->ntb_param.dwNtbInMaxSize = host_ntb_in_size;
    cdc_ncm_class->max_segment_size = host_max_datagram;

    USB_LOG_INFO("CDC NCM configured using descriptor defaults: NTB input %u bytes, max datagram %u\r\n",
                 (unsigned int)host_ntb_in_size,
                 (unsigned int)host_max_datagram);

    cdc_ncm_class->connect_status = true;

    return ret;
}

int usbh_cdc_ncm_get_connect_status(struct usbh_cdc_ncm *cdc_ncm_class)
{
    int ret;

    usbh_int_urb_fill(&cdc_ncm_class->intin_urb, cdc_ncm_class->hport, cdc_ncm_class->intin, g_cdc_ncm_inttx_buffer, 16, USB_OSAL_WAITING_FOREVER, NULL, NULL);
    ret = usbh_submit_urb(&cdc_ncm_class->intin_urb);
    if (ret < 0) {
        return ret;
    }

    if (g_cdc_ncm_inttx_buffer[1] == CDC_ECM_NOTIFY_CODE_NETWORK_CONNECTION) {
        if (g_cdc_ncm_inttx_buffer[2] == CDC_ECM_NET_CONNECTED) {
            cdc_ncm_class->connect_status = true;
        } else {
            cdc_ncm_class->connect_status = false;
        }
    } else if (g_cdc_ncm_inttx_buffer[1] == CDC_ECM_NOTIFY_CODE_CONNECTION_SPEED_CHANGE) {
        memcpy(cdc_ncm_class->speed, &g_cdc_ncm_inttx_buffer[8], 8);
    }
    return 0;
}

static int usbh_cdc_ncm_connect(struct usbh_hubport *hport, uint8_t intf)
{
    struct usb_endpoint_descriptor *ep_desc;
    int ret;
    uint8_t altsetting = 0;
    char mac_buffer[12];
    uint8_t *p;
    uint8_t cur_iface = 0xff;
    uint8_t mac_str_idx = 0xff;

    struct usbh_cdc_ncm *cdc_ncm_class = &g_cdc_ncm_class;

    memset(cdc_ncm_class, 0, sizeof(struct usbh_cdc_ncm));

    cdc_ncm_class->hport = hport;
    cdc_ncm_class->ctrl_intf = intf;
    cdc_ncm_class->data_intf = intf + 1;

    hport->config.intf[intf].priv = cdc_ncm_class;
    hport->config.intf[intf + 1].priv = NULL;

    p = hport->raw_config_desc;
    while (p[DESC_bLength]) {
        switch (p[DESC_bDescriptorType]) {
            case USB_DESCRIPTOR_TYPE_INTERFACE:
                cur_iface = p[INTF_DESC_bInterfaceNumber];
                //cur_alt_setting = p[INTF_DESC_bAlternateSetting];
                break;
            case CDC_CS_INTERFACE:
                if ((cur_iface == cdc_ncm_class->ctrl_intf) && p[DESC_bDescriptorSubType] == CDC_FUNC_DESC_ETHERNET_NETWORKING) {
                    struct cdc_eth_descriptor *desc = (struct cdc_eth_descriptor *)p;
                    mac_str_idx = desc->iMACAddress;
                    cdc_ncm_class->max_segment_size = desc->wMaxSegmentSize;
                    goto get_mac;
                }
                break;

            default:
                break;
        }
        /* skip to next descriptor */
        p += p[DESC_bLength];
    }

get_mac:
    if (mac_str_idx == 0xff) {
        USB_LOG_ERR("Do not find cdc ncm mac string\r\n");
        return -1;
    }

    memset(mac_buffer, 0, 12);
    ret = usbh_get_string_desc(cdc_ncm_class->hport, mac_str_idx, (uint8_t *)mac_buffer, 12);
    if (ret < 0) {
        return ret;
    }

    for (int i = 0, j = 0; i < 12; i += 2, j++) {
        char byte_str[3];
        byte_str[0] = mac_buffer[i];
        byte_str[1] = mac_buffer[i + 1];
        byte_str[2] = '\0';

        uint32_t byte = strtoul(byte_str, NULL, 16);
        cdc_ncm_class->mac[j] = (unsigned char)byte;
    }

    USB_LOG_INFO("CDC NCM MAC address %02x:%02x:%02x:%02x:%02x:%02x\r\n",
                 cdc_ncm_class->mac[0],
                 cdc_ncm_class->mac[1],
                 cdc_ncm_class->mac[2],
                 cdc_ncm_class->mac[3],
                 cdc_ncm_class->mac[4],
                 cdc_ncm_class->mac[5]);

    if (cdc_ncm_class->max_segment_size > CONFIG_USBHOST_CDC_NCM_ETH_MAX_SEGSZE) {
        USB_LOG_ERR("CDC NCM Max Segment Size is overflow, default is %u, but now %u\r\n", CONFIG_USBHOST_CDC_NCM_ETH_MAX_SEGSZE, cdc_ncm_class->max_segment_size);
    } else {
        USB_LOG_INFO("CDC NCM Max Segment Size:%u\r\n", cdc_ncm_class->max_segment_size);
    }

    /* enable int ep */
    ep_desc = &hport->config.intf[intf].altsetting[0].ep[0].ep_desc;
    USBH_EP_INIT(cdc_ncm_class->intin, ep_desc);

    /* Linux does altsetting toggle (1->0->1) before GET_NTB_PARAMETERS,
     * then sets it back to 1 after SET_NTB_FORMAT. Match this sequence.
     */
    if (hport->config.intf[intf + 1].altsetting_num > 1) {
        altsetting = hport->config.intf[intf + 1].altsetting_num - 1;

        /* Initialize endpoints from altsetting 1 */
        for (uint8_t i = 0; i < hport->config.intf[intf + 1].altsetting[altsetting].intf_desc.bNumEndpoints; i++) {
            ep_desc = &hport->config.intf[intf + 1].altsetting[altsetting].ep[i].ep_desc;

            if (ep_desc->bEndpointAddress & 0x80) {
                USBH_EP_INIT(cdc_ncm_class->bulkin, ep_desc);
            } else {
                USBH_EP_INIT(cdc_ncm_class->bulkout, ep_desc);
            }
        }

        /* Linux sequence: SET_INTERFACE(1, alt=1) -> SET_INTERFACE(1, alt=0) -> GET_NTB_PARAMETERS */
        USB_LOG_INFO("Select cdc ncm altsetting: %d\r\n", altsetting);
        ret = usbh_set_interface(cdc_ncm_class->hport, cdc_ncm_class->data_intf, altsetting);
        if (ret < 0) {
            USB_LOG_WRN("Failed to set altsetting %u, ret=%d\r\n", (unsigned int)altsetting, ret);
        }
        ret = usbh_set_interface(cdc_ncm_class->hport, cdc_ncm_class->data_intf, 0);
        if (ret < 0) {
            USB_LOG_WRN("Failed to set altsetting 0, ret=%d\r\n", ret);
        }
    } else {
        for (uint8_t i = 0; i < hport->config.intf[intf + 1].altsetting[0].intf_desc.bNumEndpoints; i++) {
            ep_desc = &hport->config.intf[intf + 1].altsetting[0].ep[i].ep_desc;

            if (ep_desc->bEndpointAddress & 0x80) {
                USBH_EP_INIT(cdc_ncm_class->bulkin, ep_desc);
            } else {
                USBH_EP_INIT(cdc_ncm_class->bulkout, ep_desc);
            }
        }
    }

    /* Get NTB parameters while altsetting is 0 (Linux does this) */
    usbh_cdc_ncm_get_ntb_parameters(cdc_ncm_class, &cdc_ncm_class->ntb_param);
    print_ntb_parameters(&cdc_ncm_class->ntb_param);

    ret = usbh_cdc_ncm_configure(cdc_ncm_class);
    if (ret < 0) {
        return ret;
    }

    strncpy(hport->config.intf[intf].devname, DEV_FORMAT, CONFIG_USBHOST_DEV_NAMELEN);

    USB_LOG_INFO("Register CDC NCM Class:%s\r\n", hport->config.intf[intf].devname);

    usbh_cdc_ncm_run(cdc_ncm_class);
    return 0;
}

static int usbh_cdc_ncm_disconnect(struct usbh_hubport *hport, uint8_t intf)
{
    int ret = 0;

    struct usbh_cdc_ncm *cdc_ncm_class = (struct usbh_cdc_ncm *)hport->config.intf[intf].priv;

    if (cdc_ncm_class) {
        if (cdc_ncm_class->bulkin) {
            usbh_kill_urb(&cdc_ncm_class->bulkin_urb);
        }

        if (cdc_ncm_class->bulkout) {
            usbh_kill_urb(&cdc_ncm_class->bulkout_urb);
        }

        if (cdc_ncm_class->intin) {
            usbh_kill_urb(&cdc_ncm_class->intin_urb);
        }

        if (hport->config.intf[intf].devname[0] != '\0') {
            usb_osal_thread_schedule_other();
            USB_LOG_INFO("Unregister CDC NCM Class:%s\r\n", hport->config.intf[intf].devname);
            usbh_cdc_ncm_stop(cdc_ncm_class);
        }

        memset(cdc_ncm_class, 0, sizeof(struct usbh_cdc_ncm));
    }

    return ret;
}

void usbh_cdc_ncm_rx_thread(CONFIG_USB_OSAL_THREAD_SET_ARGV)
{
    uint32_t g_cdc_ncm_rx_length;
    int ret;
    /* Reduce transfer size to avoid DWC2 FIFO overflow on ESP32-S3.
     * Use endpoint max packet size (64 bytes) to minimize FIFO pressure.
     * The NTB parsing code handles multi-packet transfers correctly.
     */
    uint32_t transfer_size = 64; /* Default to max packet size */
    if (g_cdc_ncm_class.bulkin) {
        transfer_size = USB_GET_MAXPACKETSIZE(g_cdc_ncm_class.bulkin->wMaxPacketSize);
        if (transfer_size == 0) {
            transfer_size = 64; /* Fallback */
        }
    }

    (void)CONFIG_USB_OSAL_THREAD_GET_ARGV;
    USB_LOG_INFO("Create cdc ncm rx thread\r\n");
    // clang-format off
find_class:
    // clang-format on
    if (usbh_find_class_instance("/dev/cdc_ncm") == NULL) {
        goto delete;
    }

    /* Wait for CDC-NCM notification before starting bulk IN transfers.
     * Linux waits for CONNECTION_SPEED_CHANGE/NETWORK_CONNECTION notifications.
     * Poll for up to 2 seconds to receive the notification.
     */
    uint32_t connect_poll_attempts = 0;
    while (g_cdc_ncm_class.connect_status == false) {
        ret = usbh_cdc_ncm_get_connect_status(&g_cdc_ncm_class);
        if (ret < 0) {
            connect_poll_attempts++;
            if (connect_poll_attempts >= 20) {  /* 20 * 100ms = 2 seconds */
                USB_LOG_WRN("No connect notification received after 2s, assuming link up\r\n");
                g_cdc_ncm_class.connect_status = true;
                break;
            }
            usb_osal_msleep(100);
            continue;
        }
        connect_poll_attempts = 0;
    }

    /* Additional delay after receiving notification to let gadget fully settle */
    usb_osal_msleep(200);

    /* Clear endpoint halt before starting bulk IN to ensure clean state.
     * This may help if the endpoint is in a bad state from previous attempts.
     */
    struct usb_setup_packet setup;
    setup.bmRequestType = USB_REQUEST_DIR_OUT | USB_REQUEST_STANDARD | USB_REQUEST_RECIPIENT_ENDPOINT;
    setup.bRequest = USB_REQUEST_CLEAR_FEATURE;
    setup.wValue = USB_FEATURE_ENDPOINT_HALT;
    setup.wIndex = g_cdc_ncm_class.bulkin->bEndpointAddress;
    setup.wLength = 0;
    ret = usbh_control_transfer(g_cdc_ncm_class.hport, &setup, NULL);
    if (ret < 0 && ret != -USB_ERR_STALL && ret != -USB_ERR_IO) {
        USB_LOG_DBG("Failed to clear bulk IN endpoint halt, ret=%d\r\n", ret);
    }

    g_cdc_ncm_rx_length = 0;
    while (1) {
        /* Linux-style: submit URB immediately, process on completion, then submit next immediately.
         * This minimizes gaps where the host isn't ready to receive data.
         */
        usbh_bulk_urb_fill(&g_cdc_ncm_class.bulkin_urb, g_cdc_ncm_class.hport, g_cdc_ncm_class.bulkin, &g_cdc_ncm_rx_buffer[g_cdc_ncm_rx_length], transfer_size, USB_OSAL_WAITING_FOREVER, NULL, NULL);
        ret = usbh_submit_urb(&g_cdc_ncm_class.bulkin_urb);
        if (ret < 0) {
            USB_LOG_DBG("bulk IN submit error ret=%d\r\n", ret);
            if (ret == -USB_ERR_IO || ret == -USB_ERR_STALL || ret == -USB_ERR_BABBLE) {
                USB_LOG_DBG("bulk IN stalled/empty (ret=%d), retrying\r\n", ret);
                /* Clear endpoint halt after babble/stall errors - endpoint may be halted */
                if (ret == -USB_ERR_BABBLE || ret == -USB_ERR_STALL) {
                    struct usb_setup_packet setup;
                    setup.bmRequestType = USB_REQUEST_DIR_OUT | USB_REQUEST_STANDARD | USB_REQUEST_RECIPIENT_ENDPOINT;
                    setup.bRequest = USB_REQUEST_CLEAR_FEATURE;
                    setup.wValue = USB_FEATURE_ENDPOINT_HALT;
                    setup.wIndex = g_cdc_ncm_class.bulkin->bEndpointAddress;
                    setup.wLength = 0;
                    int clear_ret = usbh_control_transfer(g_cdc_ncm_class.hport, &setup, NULL);
                    if (clear_ret < 0 && clear_ret != -USB_ERR_STALL && clear_ret != -USB_ERR_IO) {
                        USB_LOG_DBG("Failed to clear bulk IN endpoint halt after error, ret=%d\r\n", clear_ret);
                    }
                }
                /* Increase delay for babble errors - may indicate FIFO overflow or endpoint state issue */
                usb_osal_msleep(ret == -USB_ERR_BABBLE ? 100 : 20);
                g_cdc_ncm_rx_length = 0;
                continue;
            }
            USB_LOG_WRN("bulk IN submit failed ret=%d, restarting\r\n", ret);
            goto find_class;
        }

        g_cdc_ncm_rx_length += g_cdc_ncm_class.bulkin_urb.actual_length;
        USB_LOG_INFO("NCM bulk IN completed: len=%u\r\n", (unsigned int)g_cdc_ncm_class.bulkin_urb.actual_length);

        /* A transfer is complete because last packet is a short packet.
         * Short packet is not zero, match g_cdc_ncm_rx_length % USB_GET_MAXPACKETSIZE(g_cdc_ncm_class.bulkin->wMaxPacketSize).
         * Short packet is zero, check if g_cdc_ncm_class.bulkin_urb.actual_length < transfer_size, for example transfer is complete with size is 1024 < 2048.
        */
        if ((g_cdc_ncm_rx_length % USB_GET_MAXPACKETSIZE(g_cdc_ncm_class.bulkin->wMaxPacketSize)) ||
            (g_cdc_ncm_class.bulkin_urb.actual_length < transfer_size)) {
            USB_LOG_INFO("NCM RX block length:%d\r\n", g_cdc_ncm_rx_length);
            usb_hexdump(&g_cdc_ncm_rx_buffer[0], MIN(g_cdc_ncm_rx_length, 64));

            struct cdc_ncm_nth16 *nth16 = (struct cdc_ncm_nth16 *)&g_cdc_ncm_rx_buffer[0];
            if ((nth16->dwSignature != CDC_NCM_NTH16_SIGNATURE) ||
                (nth16->wHeaderLength != 12) ||
                (nth16->wBlockLength != g_cdc_ncm_rx_length)) {
                USB_LOG_ERR("invalid rx nth16\r\n");
                g_cdc_ncm_rx_length = 0;
                continue;
            }

            struct cdc_ncm_ndp16 *ndp16 = (struct cdc_ncm_ndp16 *)&g_cdc_ncm_rx_buffer[nth16->wNdpIndex];
            if ((ndp16->dwSignature != CDC_NCM_NDP16_SIGNATURE) &&
                (ndp16->dwSignature != CDC_NCM_NDP16_SIGNATURE_NCM0) &&
                (ndp16->dwSignature != CDC_NCM_NDP16_SIGNATURE_NCM1)) {
                USB_LOG_ERR("invalid rx ndp16\r\n");
                g_cdc_ncm_rx_length = 0;
                continue;
            }

            uint16_t datagram_num = (ndp16->wLength - 8) / 4;

            USB_LOG_INFO("NCM datagram count:%u\r\n", datagram_num);
            for (uint16_t i = 0; i < datagram_num; i++) {
                struct cdc_ncm_ndp16_datagram *ndp16_datagram = (struct cdc_ncm_ndp16_datagram *)&g_cdc_ncm_rx_buffer[nth16->wNdpIndex + 8 + 4 * i];
                if (ndp16_datagram->wDatagramIndex && ndp16_datagram->wDatagramLength) {
                    uint8_t *buf = (uint8_t *)&g_cdc_ncm_rx_buffer[ndp16_datagram->wDatagramIndex];
                    usbh_cdc_ncm_eth_input(buf, ndp16_datagram->wDatagramLength);
                }
            }

            g_cdc_ncm_rx_length = 0;
        } else {
#if CONFIG_USBHOST_CDC_NCM_ETH_MAX_RX_SIZE <= (16 * 1024)
            if (g_cdc_ncm_rx_length == CONFIG_USBHOST_CDC_NCM_ETH_MAX_RX_SIZE) {
#else
            if ((g_cdc_ncm_rx_length + (16 * 1024)) > CONFIG_USBHOST_CDC_NCM_ETH_MAX_RX_SIZE) {
#endif
                USB_LOG_ERR("Rx packet is overflow, please reduce tcp window size or increase CONFIG_USBHOST_CDC_NCM_ETH_MAX_RX_SIZE\r\n");
                while (1) {
                }
            }
        }
    }
    // clang-format off
delete:
    USB_LOG_INFO("Delete cdc ncm rx thread\r\n");
    usb_osal_thread_delete(NULL);
    // clang-format on
}

uint8_t *usbh_cdc_ncm_get_eth_txbuf(void)
{
    return &g_cdc_ncm_tx_buffer[16];
}

int usbh_cdc_ncm_eth_output(uint32_t buflen)
{
    struct cdc_ncm_ndp16_datagram *ndp16_datagram;

    if (g_cdc_ncm_class.connect_status == false) {
        return -USB_ERR_NOTCONN;
    }

    const uint16_t data_offset = 16;
    const uint16_t data_aligned = USB_ALIGN_UP(buflen, 4);
    const uint16_t first_ndp_offset = data_offset + data_aligned;
    const uint16_t second_ndp_offset = first_ndp_offset + 16;
    const uint16_t block_length = second_ndp_offset + 16;

    struct cdc_ncm_nth16 *nth16 = (struct cdc_ncm_nth16 *)&g_cdc_ncm_tx_buffer[0];

    nth16->dwSignature = CDC_NCM_NTH16_SIGNATURE;
    nth16->wHeaderLength = 12;
    nth16->wSequence = g_cdc_ncm_class.bulkout_sequence++;
    nth16->wBlockLength = block_length;
    nth16->wNdpIndex = first_ndp_offset;

    if (data_aligned > buflen) {
        memset(&g_cdc_ncm_tx_buffer[data_offset + buflen], 0, data_aligned - buflen);
    }
    memset(&g_cdc_ncm_tx_buffer[first_ndp_offset], 0, 32);

    usb_memcpy(&g_cdc_ncm_tx_buffer[data_offset], usbh_cdc_ncm_get_eth_txbuf(), buflen);

    struct cdc_ncm_ndp16 *ndp_std = (struct cdc_ncm_ndp16 *)&g_cdc_ncm_tx_buffer[first_ndp_offset];
    ndp_std->dwSignature = CDC_NCM_NDP16_SIGNATURE;
    ndp_std->wLength = 16;
    ndp_std->wNextNdpIndex = second_ndp_offset;
    ndp16_datagram = (struct cdc_ncm_ndp16_datagram *)&g_cdc_ncm_tx_buffer[first_ndp_offset + 8];
    ndp16_datagram->wDatagramIndex = data_offset;
    ndp16_datagram->wDatagramLength = buflen;

    struct cdc_ncm_ndp16 *ndp_alt = (struct cdc_ncm_ndp16 *)&g_cdc_ncm_tx_buffer[second_ndp_offset];
    ndp_alt->dwSignature = CDC_NCM_NDP16_SIGNATURE_NCM0;
    ndp_alt->wLength = 16;
    ndp_alt->wNextNdpIndex = 0;
    ndp16_datagram = (struct cdc_ncm_ndp16_datagram *)&g_cdc_ncm_tx_buffer[second_ndp_offset + 8];
    ndp16_datagram->wDatagramIndex = data_offset;
    ndp16_datagram->wDatagramLength = buflen;

    USB_LOG_DBG("txlen:%d\r\n", nth16->wBlockLength);
    usb_hexdump(g_cdc_ncm_tx_buffer, MIN(nth16->wBlockLength, 64));

    usbh_bulk_urb_fill(&g_cdc_ncm_class.bulkout_urb, g_cdc_ncm_class.hport, g_cdc_ncm_class.bulkout, g_cdc_ncm_tx_buffer, nth16->wBlockLength, USB_OSAL_WAITING_FOREVER, NULL, NULL);
    int ret = usbh_submit_urb(&g_cdc_ncm_class.bulkout_urb);
    USB_LOG_DBG("bulk OUT submit ret=%d\r\n", ret);
    return ret;
}

__WEAK void usbh_cdc_ncm_run(struct usbh_cdc_ncm *cdc_ncm_class)
{
    (void)cdc_ncm_class;
}

__WEAK void usbh_cdc_ncm_stop(struct usbh_cdc_ncm *cdc_ncm_class)
{
    (void)cdc_ncm_class;
}

const struct usbh_class_driver cdc_ncm_class_driver = {
    .driver_name = "cdc_ncm",
    .connect = usbh_cdc_ncm_connect,
    .disconnect = usbh_cdc_ncm_disconnect
};

CLASS_INFO_DEFINE const struct usbh_class_info cdc_ncm_class_info = {
    .match_flags = USB_CLASS_MATCH_INTF_CLASS | USB_CLASS_MATCH_INTF_SUBCLASS | USB_CLASS_MATCH_INTF_PROTOCOL,
    .bInterfaceClass = USB_DEVICE_CLASS_CDC,
    .bInterfaceSubClass = CDC_NETWORK_CONTROL_MODEL,
    .bInterfaceProtocol = CDC_COMMON_PROTOCOL_NONE,
    .id_table = NULL,
    .class_driver = &cdc_ncm_class_driver
};
