#include "ccidd.h"
#include "../userspace/ccid.h"
#include "../userspace/usb.h"
#include "../userspace/stdlib.h"
#include "../userspace/io.h"
#include "../userspace/stdio.h"
#include "../userspace/ipc.h"
#include "usb.h"
#include <string.h>
#include "sys_config.h"

typedef enum {
    CCIDD_CARD_STATE_NOT_PRESENT = 0,
    CCIDD_CARD_STATE_INSERTED,
    CCIDD_CARD_STATE_POWERED
} CCIDD_CARD_STATE;

typedef enum {
    CCIDD_STATE_IDLE = 0,
    CCIDD_STATE_CARD_REQUEST,
    CCIDD_STATE_TX,
    CCIDD_STATE_TX_ZLP
} CCIDD_STATE;

typedef struct {
    IO* io;
    IO* status_io;
    CCIDD_STATE state;
    unsigned int request;
    uint8_t data_ep, data_ep_size, status_ep, iface, seq, status_busy, card_state, aborting;
} CCIDD;

static void ccidd_destroy(CCIDD* ccidd)
{
    io_destroy(ccidd->io);
    io_destroy(ccidd->status_io);
    free(ccidd);
}

static void ccidd_rx(USBD* usbd, CCIDD* ccidd)
{
    io_reset(ccidd->io);
    usbd_usb_ep_read(usbd, ccidd->data_ep, ccidd->io, io_get_free(ccidd->io));
}

static void ccidd_notify_slot_change(USBD* usbd, CCIDD* ccidd, unsigned int change_mask)
{
    CCID_NOTIFY_SLOT_CHANGE* notify;
    unsigned int mask;
    if (ccidd->status_ep)
    {
        if (ccidd->status_busy)
            usbd_usb_ep_flush(usbd, USB_EP_IN | ccidd->status_ep);

        notify = io_data(ccidd->status_io);
        notify->bMessageType = RDR_TO_PC_NOTIFY_SLOT_CHANGE;
        mask = change_mask;
        if (ccidd->card_state != CCIDD_CARD_STATE_NOT_PRESENT)
            mask |= (1 << 0);
        notify->bmSlotICCState = mask;
        ccidd->status_io->data_size = sizeof(CCID_NOTIFY_SLOT_CHANGE);
        usbd_usb_ep_write(usbd, USB_EP_IN | ccidd->status_ep, ccidd->status_io);
        ccidd->status_busy = true;
    }
}

static inline uint8_t ccidd_slot_status_register(CCIDD* ccidd, unsigned int command_status)
{
    return ((ccidd->card_state & 3) << 0) | (command_status & 3) << 6;
}

static void ccidd_send_slot_status(USBD* usbd, CCIDD* ccidd, uint8_t seq, uint8_t error, uint8_t status)
{
    io_reset(ccidd->io);
    CCID_SLOT_STATUS* msg = io_data(ccidd->io);
    msg->bMessageType = RDR_TO_PC_SLOT_STATUS;
    msg->dwLength = 0;
    msg->bSlot = 0;
    msg->bSeq = seq;
    msg->bStatus = ccidd_slot_status_register(ccidd, status);
    msg->bError = error;
    msg->bClockStatus = CCID_CLOCK_STATUS_RUNNING;
    ccidd->io->data_size = sizeof(CCID_SLOT_STATUS);
    usbd_usb_ep_write(usbd, USB_EP_IN | ccidd->data_ep, ccidd->io);
    ccidd->state = CCIDD_STATE_TX;
}

static void ccidd_send_data_block(USBD* usbd, CCIDD* ccidd, uint8_t error, uint8_t status)
{
    CCID_DATA_BLOCK* msg = io_data(ccidd->io);
    if (error)
        ccidd->io->data_size = sizeof(CCID_MESSAGE);
    msg->bMessageType = RDR_TO_PC_DATA_BLOCK;
    msg->dwLength = ccidd->io->data_size - sizeof(CCID_MESSAGE);
    msg->bSlot = 0;
    msg->bSeq = ccidd->seq;
    msg->bStatus = ccidd_slot_status_register(ccidd, status);
    msg->bError = error;
    msg->bChainParameter = 0;
    usbd_usb_ep_write(usbd, USB_EP_IN | ccidd->data_ep, ccidd->io);
    ccidd->state = CCIDD_STATE_TX;
}

static void ccidd_send_params(USBD* usbd, CCIDD* ccidd, uint8_t error, uint8_t status, CCID_PROTOCOL protocol)
{
    CCID_PARAMS* msg = io_data(ccidd->io);
    if (error)
        ccidd->io->data_size = sizeof(CCID_MESSAGE);
    msg->bMessageType = RDR_TO_PC_PARAMETERS;
    msg->dwLength = ccidd->io->data_size - sizeof(CCID_MESSAGE);
    msg->bSlot = 0;
    msg->bSeq = ccidd->seq;
    msg->bStatus = ccidd_slot_status_register(ccidd, status);
    msg->bError = error;
    msg->bProtocolNum = protocol;
    usbd_usb_ep_write(usbd, USB_EP_IN | ccidd->data_ep, ccidd->io);
    ccidd->state = CCIDD_STATE_TX;
}

static void ccidd_user_request(USBD* usbd, CCIDD* ccidd, unsigned int req, uint8_t param)
{
    //hide ccid message to user
    ccidd->io->data_offset = sizeof(CCID_MESSAGE);
    ccidd->io->data_size -= sizeof(CCID_MESSAGE);
    ccidd->request = req;
    usbd_io_user(usbd, ccidd->iface, 0, HAL_CMD(HAL_USBD_IFACE, req), ccidd->io, param);
    ccidd->state = CCIDD_STATE_CARD_REQUEST;
}

void ccidd_class_configured(USBD* usbd, USB_CONFIGURATION_DESCRIPTOR_TYPE* cfg)
{
    USB_INTERFACE_DESCRIPTOR_TYPE* iface;
    USB_ENDPOINT_DESCRIPTOR_TYPE* ep;
    CCID_DESCRIPTOR_TYPE* ccid_descriptor;
    CCIDD* ccidd;
    unsigned int status_ep_size;

    for (iface = usb_get_first_interface(cfg); iface != NULL; iface = usb_get_next_interface(cfg, iface))
    {
        if (iface->bInterfaceClass == CCID_INTERFACE_CLASS)
        {
            ccid_descriptor = (CCID_DESCRIPTOR_TYPE*)usb_interface_get_first_descriptor(cfg, iface, USB_FUNCTIONAL_DESCRIPTOR);
            ccidd = malloc(sizeof(CCIDD));
            if (ccidd == NULL || ccid_descriptor == NULL)
                return;

            ccidd->iface = iface->bInterfaceNumber;

            ccidd->data_ep = ccidd->status_ep = 0;
            ccidd->io = ccidd->status_io = NULL;
            status_ep_size = 0;
            for (ep = (USB_ENDPOINT_DESCRIPTOR_TYPE*)usb_interface_get_first_descriptor(cfg, iface, USB_ENDPOINT_DESCRIPTOR_INDEX); ep != NULL;
                 ep = (USB_ENDPOINT_DESCRIPTOR_TYPE*)usb_interface_get_next_descriptor(cfg, (USB_DESCRIPTOR_TYPE*)ep, USB_ENDPOINT_DESCRIPTOR_INDEX))
            {
                switch (ep->bmAttributes & USB_EP_BM_ATTRIBUTES_TYPE_MASK)
                {
                case USB_EP_BM_ATTRIBUTES_BULK:
                    if (ccidd->data_ep == 0)
                    {
                        ccidd->data_ep = USB_EP_NUM(ep->bEndpointAddress);
                        ccidd->data_ep_size = ep->wMaxPacketSize;
                        ccidd->io = io_create(ccid_descriptor->dwMaxCCIDMessageLength + sizeof(CCID_MESSAGE));
                    }
                    break;
                case USB_EP_BM_ATTRIBUTES_INTERRUPT:
                    if (ccidd->status_ep == 0)
                    {
                        ccidd->status_ep = USB_EP_NUM(ep->bEndpointAddress);
                        status_ep_size = ep->wMaxPacketSize;
                        ccidd->status_io = io_create(status_ep_size);
                    }
                    break;
                default:
                    break;
                }
            }
            //invalid configuration
            if (ccidd->data_ep == 0 || ccidd->io == NULL || (ccidd->status_ep != 0 && ccidd->status_io == NULL))
            {
                ccidd_destroy(ccidd);
                continue;
            }
#if (USBD_CCID_REMOVABLE_CARD)
            ccidd->card_state = CCID_SLOT_STATUS_ICC_NOT_PRESENT;
#else
            ccidd->card_state = CCID_SLOT_STATUS_ICC_PRESENT_AND_INACTIVE;
#endif //USBD_CCID_REMOVABLE_CARD
            ccidd->state = CCIDD_STATE_IDLE;
            ccidd->aborting = false;

#if (USBD_CCID_DEBUG_REQUESTS)
            printf("Found USB CCID device class, data: EP%d, iface: %d\n\r", ccidd->data_ep, ccidd->iface);
            if (ccidd->status_ep)
                printf("Status: EP%d\n\r", ccidd->status_ep);
#endif //USBD_CCID_DEBUG_REQUESTS

            usbd_register_interface(usbd, ccidd->iface, &__CCIDD_CLASS, ccidd);
            usbd_register_endpoint(usbd, ccidd->iface, ccidd->data_ep);
            usbd_usb_ep_open(usbd, ccidd->data_ep, USB_EP_BULK, ccidd->data_ep_size);
            usbd_usb_ep_open(usbd, USB_EP_IN | ccidd->data_ep, USB_EP_BULK, ccidd->data_ep_size);
            ccidd_rx(usbd, ccidd);

            if (ccidd->status_ep)
            {
                usbd_register_endpoint(usbd, ccidd->iface, ccidd->status_ep);
                ccidd->status_busy = false;
                usbd_usb_ep_open(usbd, USB_EP_IN | ccidd->status_ep, USB_EP_INTERRUPT, status_ep_size);
                ccidd_notify_slot_change(usbd, ccidd, 1 << 1);
            }
        }
    }
}

void ccidd_class_reset(USBD* usbd, void* param)
{
    CCIDD* ccidd = (CCIDD*)param;

    usbd_usb_ep_close(usbd, ccidd->data_ep);
    usbd_usb_ep_close(usbd, USB_EP_IN | ccidd->data_ep);
    usbd_unregister_endpoint(usbd, ccidd->iface, ccidd->data_ep);
    if (ccidd->status_ep)
    {
        usbd_usb_ep_close(usbd, USB_EP_IN | ccidd->status_ep);
        usbd_unregister_endpoint(usbd, ccidd->iface, ccidd->status_ep);
    }

    usbd_unregister_interface(usbd, ccidd->iface, &__CCIDD_CLASS);
    ccidd_destroy(ccidd);
}

void ccidd_class_suspend(USBD* usbd, void* param)
{
    CCIDD* ccidd = (CCIDD*)param;
    usbd_usb_ep_flush(usbd, ccidd->data_ep);
    usbd_usb_ep_flush(usbd, USB_EP_IN | ccidd->data_ep);
    ccidd->state = CCIDD_STATE_IDLE;
    ccidd->aborting = false;
    if (ccidd->status_ep && ccidd->status_busy)
    {
        usbd_usb_ep_flush(usbd, USB_EP_IN | ccidd->status_ep);
        ccidd->status_busy = false;
    }
}

void ccidd_class_resume(USBD* usbd, void* param)
{
    CCIDD* ccidd = (CCIDD*)param;
    ccidd_rx(usbd, ccidd);
    ccidd_notify_slot_change(usbd, ccidd, 1 << 1);
}

int ccidd_class_setup(USBD* usbd, void* param, SETUP* setup, IO* io)
{
    CCIDD* ccidd = (CCIDD*)param;
    unsigned int res = -1;
    switch (setup->bRequest)
    {
    case CCID_CMD_ABORT:
        if (ccidd->state == CCIDD_STATE_CARD_REQUEST)
            usbd_post_user(usbd, ccidd->iface, 0, HAL_CMD(HAL_USBD_IFACE, IPC_CANCEL_IO), 0, 0);
        ccidd->state = CCIDD_STATE_IDLE;
        ccidd->aborting = true;
        ccidd->seq = setup->wValue >> 8;
        res = 0;
#if (USBD_CCID_DEBUG_REQUESTS)
        printf("CCIDD Abort request\n\r");
#endif //USBD_CCID_DEBUG_REQUESTS
        break;
    default:
        break;
    }
    return res;
}

static inline void ccidd_power_on(USBD* usbd, CCIDD* ccidd)
{
    CCID_MESSAGE* msg = io_data(ccidd->io);
#if (USBD_CCID_DEBUG_REQUESTS)
    printf("CCIDD: ICC slot%d power on\n\r", msg->bSlot);
#endif //USBD_CCID_DEBUG_REQUESTS
    ccidd_user_request(usbd, ccidd, USB_CCID_POWER_ON, msg->msg_specific[0]);
}

static inline void ccidd_power_off(USBD* usbd, CCIDD* ccidd)
{
#if (USBD_CCID_DEBUG_REQUESTS)
    CCID_MESSAGE* msg = io_data(ccidd->io);
    printf("CCIDD: ICC slot%d power off\n\r", msg->bSlot);
#endif //USBD_CCID_DEBUG_REQUESTS
    ccidd_user_request(usbd, ccidd, USB_CCID_POWER_OFF, 0);
}

static inline void ccidd_get_slot_status(USBD* usbd, CCIDD* ccidd)
{
#if (USBD_CCID_DEBUG_REQUESTS)
    CCID_MESSAGE* msg = io_data(ccidd->io);
    printf("CCIDD: get slot%d status\n\r", msg->bSlot);
#endif //USBD_CCID_DEBUG_REQUESTS
    ccidd_send_slot_status(usbd, ccidd, ccidd->seq, 0, CCID_SLOT_STATUS_COMMAND_NO_ERROR);
}

static inline void ccidd_xfer_block(USBD* usbd, CCIDD* ccidd)
{
    CCID_MESSAGE* msg = io_data(ccidd->io);
#if (USBD_CCID_DEBUG_REQUESTS)
    printf("CCIDD: Xfer block to slot%d, size: %d\n\r", msg->bSlot, msg->dwLength);
#endif //USBD_CCID_DEBUG_REQUESTS
#if (USBD_CCID_DEBUG_IO)
    usbd_dump(io_data(ccidd->io) + sizeof(CCID_MESSAGE), msg->dwLength, "CCIDD C-APDU");
#endif
    ccidd_user_request(usbd, ccidd, USB_CCID_APDU, msg->msg_specific[0]);
}

static inline void ccidd_get_params(USBD* usbd, CCIDD* ccidd)
{
#if (USBD_CCID_DEBUG_REQUESTS)
        printf("CCIDD: get params\n\r");
#endif //USBD_CCID_DEBUG_REQUESTS
    ccidd_user_request(usbd, ccidd, USB_CCID_GET_PARAMS, 0);
}

static inline void ccidd_set_params(USBD* usbd, CCIDD* ccidd)
{
    CCID_MESSAGE* msg = io_data(ccidd->io);
#if (USBD_CCID_DEBUG_REQUESTS)
    printf("CCIDD: set params - T%d\n\r", msg->msg_specific[0]);
#endif //USBD_CCID_DEBUG_REQUESTS
    ccidd_user_request(usbd, ccidd, USB_CCID_SET_PARAMS, msg->msg_specific[0]);
}

static inline void ccidd_reset_params(USBD* usbd, CCIDD* ccidd)
{
#if (USBD_CCID_DEBUG_REQUESTS)
        printf("CCIDD: reset params\n\r");
#endif //USBD_CCID_DEBUG_REQUESTS
    ccidd_user_request(usbd, ccidd, USB_CCID_RESET_PARAMS, 0);
}

static inline void ccidd_msg_process(USBD* usbd, CCIDD* ccidd)
{
    CCID_MESSAGE* msg = io_data(ccidd->io);
    ccidd->seq = msg->bSeq;
    switch (msg->bMessageType)
    {
    case PC_TO_RDR_ICC_POWER_ON:
        ccidd_power_on(usbd, ccidd);
        break;
    case PC_TO_RDR_ICC_POWER_OFF:
        ccidd_power_off(usbd, ccidd);
        break;
    case PC_TO_RDR_GET_SLOT_STATUS:
        ccidd_get_slot_status(usbd, ccidd);
        break;
    case PC_TO_RDR_XFER_BLOCK:
        ccidd_xfer_block(usbd, ccidd);
        break;
    case PC_TO_RDR_GET_PARAMETERS:
        ccidd_get_params(usbd, ccidd);
        break;
    case PC_TO_RDR_RESET_PARAMETERS:
        ccidd_reset_params(usbd, ccidd);
        break;
    case PC_TO_RDR_SET_PARAMETERS:
        ccidd_set_params(usbd, ccidd);
        break;
    default:
        ccidd_send_slot_status(usbd, ccidd, msg->bSeq, CCID_SLOT_ERROR_CMD_NOT_SUPPORTED, CCID_SLOT_STATUS_COMMAND_FAIL);
#if (USBD_CCID_DEBUG_ERRORS)
        printf("CCIDD: unsupported CMD: %#X\n\r", msg->bMessageType);
#endif //USBD_DEBUG_CLASS_REQUESTS
    }
}

static inline void ccidd_rx_complete(USBD* usbd, CCIDD* ccidd)
{
    CCID_MESSAGE* msg = io_data(ccidd->io);
    if (ccidd->aborting)
    {
        if (msg->bSeq == ccidd->seq && msg->bMessageType == PC_TO_RDR_ABORT)
        {
            ccidd->aborting = false;
            ccidd_send_slot_status(usbd, ccidd, msg->bSeq, 0, CCID_SLOT_STATUS_COMMAND_NO_ERROR);
        }
        else
            ccidd_send_slot_status(usbd, ccidd, msg->bSeq, CCID_SLOT_ERROR_CMD_ABORTED, CCID_SLOT_STATUS_COMMAND_FAIL);
    }
    else if (ccidd->state != CCIDD_STATE_IDLE)
    {
        ccidd->state = CCIDD_STATE_IDLE;
        ccidd_rx(usbd, ccidd);
#if (USBD_CCID_DEBUG_ERRORS)
        printf("CCIDD: invalid state on rx: %d\n\r", ccidd->state);
#endif //USBD_DEBUG_CLASS_REQUESTS
    }
    else
        ccidd_msg_process(usbd, ccidd);
}

static void ccidd_tx_complete(USBD* usbd, CCIDD* ccidd)
{
    switch (ccidd->state)
    {
    case CCIDD_STATE_TX:
        if ((ccidd->io->data_size % ccidd->data_ep_size) == 0)
        {
            ccidd->io->data_size = 0;
            usbd_usb_ep_write(usbd, ccidd->data_ep, ccidd->io);
            ccidd->state = CCIDD_STATE_TX_ZLP;
            return;
        }
        //follow down
    case CCIDD_STATE_TX_ZLP:
        ccidd->state = CCIDD_STATE_IDLE;
        break;
    default:
        ccidd->state = CCIDD_STATE_IDLE;
#if (USBD_CCID_DEBUG_ERRORS)
        printf("CCIDD: invalid state on tx: %d\n\r", ccidd->state);
#endif //USBD_DEBUG_CLASS_REQUESTS
    }
    ccidd_rx(usbd, ccidd);
}

//TODO:
/*
static inline void ccidd_card_insert(USBD* usbd, CCIDD* ccidd)
{
    switch (ccidd->state)
    {
    case CCIDD_STATE_NO_CARD:
        ccidd->state = CCIDD_STATE_CARD_INSERTED;
        ccidd_notify_state_change(usbd, ccidd);
        break;
    case CCIDD_STATE_CARD_INSERTED:
        //no state change
        break;
    default:
        //wrong state, ignore
        break;
    }
}

static inline void ccidd_card_remove(USBD* usbd, CCIDD* ccidd)
{
    switch (ccidd->state)
    {
    case CCIDD_STATE_CARD_POWERED:
    case CCIDD_STATE_CARD_INSERTED:
        ccidd->state = CCIDD_STATE_NO_CARD;
        ccidd_notify_state_change(usbd, ccidd);
        break;
    case CCIDD_STATE_NO_CARD:
        //no state change
        break;
    default:
        //wrong state, ignore
        break;
    }
}
*/

static inline void ccidd_data_block_response(USBD* usbd, CCIDD* ccidd, int param3)
{
    if (param3 < 0)
    {
        switch (param3)
        {
        case ERROR_HARDWARE:
            ccidd_send_data_block(usbd, ccidd, CCID_SLOT_ERROR_HW_ERROR, CCID_SLOT_STATUS_COMMAND_FAIL);
            break;
        default:
            ccidd_send_data_block(usbd, ccidd, CCID_SLOT_ERROR_CMD_NOT_SUPPORTED, CCID_SLOT_STATUS_COMMAND_FAIL);
        }
        return;
    }
    ccidd_send_data_block(usbd, ccidd, 0, CCID_SLOT_STATUS_COMMAND_NO_ERROR);
}

static inline void ccidd_params_response(USBD* usbd, CCIDD* ccidd, int param3)
{
    if (param3 < 0)
    {
        switch (param3)
        {
        case ERROR_HARDWARE:
            ccidd_send_params(usbd, ccidd, CCID_SLOT_ERROR_HW_ERROR, CCID_SLOT_STATUS_COMMAND_FAIL, CCID_PROTOCOL_T1);
            break;
        default:
            ccidd_send_params(usbd, ccidd, CCID_SLOT_ERROR_CMD_NOT_SUPPORTED, CCID_SLOT_STATUS_COMMAND_FAIL, CCID_PROTOCOL_T1);
        }
        return;
    }
    ccidd_send_params(usbd, ccidd, 0, CCID_SLOT_STATUS_COMMAND_NO_ERROR, (CCID_PROTOCOL)param3);
}

static inline void ccidd_power_off_response(USBD* usbd, CCIDD* ccidd, int param3)
{
    if (param3 < 0)
    {
        ccidd_send_slot_status(usbd, ccidd, ccidd->seq, CCID_SLOT_ERROR_CMD_NOT_SUPPORTED, CCID_SLOT_STATUS_COMMAND_FAIL);
        return;
    }
    ccidd_send_slot_status(usbd, ccidd, ccidd->seq, 0, CCID_SLOT_STATUS_COMMAND_NO_ERROR);
}


static inline bool ccidd_driver_event(USBD* usbd, CCIDD* ccidd, IPC* ipc)
{
    bool need_post = false;
    switch (HAL_ITEM(ipc->cmd))
    {
    case IPC_READ:
        ccidd_rx_complete(usbd, ccidd);
        break;
    case IPC_WRITE:
        if (USB_EP_NUM(ipc->param1) == ccidd->data_ep)
            ccidd_tx_complete(usbd, ccidd);
        //status notify complete
        else
            ccidd->status_busy = false;
        break;
    default:
        error(ERROR_NOT_SUPPORTED);
        need_post = true;
    }
    return need_post;
}

static inline void ccidd_user_response(USBD* usbd, CCIDD* ccidd, IPC* ipc)
{
    //ignore on abort
    if (ccidd->aborting)
        return;
    ccidd->io->data_offset -= sizeof(CCID_MESSAGE);
    ccidd->io->data_size += sizeof(CCID_MESSAGE);
    switch (HAL_ITEM(ipc->cmd))
    {
    case USB_CCID_POWER_ON:
    case USB_CCID_APDU:
        ccidd_data_block_response(usbd, ccidd, ipc->param3);
        break;
    case USB_CCID_POWER_OFF:
        ccidd_power_off_response(usbd, ccidd, ipc->param3);
        break;
    case USB_CCID_GET_PARAMS:
    case USB_CCID_SET_PARAMS:
    case USB_CCID_RESET_PARAMS:
        ccidd_params_response(usbd, ccidd, ipc->param3);
        break;
    }
}

bool ccidd_class_request(USBD* usbd, void* param, IPC* ipc)
{
    CCIDD* ccidd = (CCIDD*)param;
    bool need_post = false;
    if (HAL_GROUP(ipc->cmd) == HAL_USB)
        need_post = ccidd_driver_event(usbd, ccidd, ipc);
    else
        switch (HAL_ITEM(ipc->cmd))
        {
        //TODO:
/*        case USB_CCID_CARD_INSERTED:
            ccidd_card_insert(usbd, ccidd);
            break;
        case USB_CCID_CARD_REMOVED:
            ccidd_card_remove(usbd, ccidd);
            break;*/
        case USB_CCID_POWER_ON:
        case USB_CCID_POWER_OFF:
        case USB_CCID_GET_PARAMS:
        case USB_CCID_SET_PARAMS:
        case USB_CCID_RESET_PARAMS:
        case USB_CCID_APDU:
            ccidd_user_response(usbd, ccidd, ipc);
            break;
        default:
            error(ERROR_NOT_SUPPORTED);
            need_post = true;
        }
    return need_post;
}

const USBD_CLASS __CCIDD_CLASS = {
    ccidd_class_configured,
    ccidd_class_reset,
    ccidd_class_suspend,
    ccidd_class_resume,
    ccidd_class_setup,
    ccidd_class_request,
};
