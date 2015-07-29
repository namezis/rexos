/*
    RExOS - embedded RTOS
    Copyright (c) 2011-2015, Alexey Kramarenko
    All rights reserved.
*/

#include "lpc_otg.h"
#if (MONOLITH_USB)
#include "lpc_core_private.h"
#endif
#include "../../userspace/usb.h"
#include "../../userspace/irq.h"

#if (MONOLITH_USB)

#define ack_pin                 lpc_pin_request_inside
#define ack_power               lpc_power_request_inside

#else //!MONOLITH_USB

#define ack_pin                 lpc_core_request_outside
#define ack_power               lpc_core_request_outside

void lpc_otg();

const REX __LPC_OTG = {
    //name
    "LPC OTG",
    //size
    LPC_USB_PROCESS_SIZE,
    //priority - driver priority
    92,
    //flags
    PROCESS_FLAGS_ACTIVE | REX_HEAP_FLAGS(HEAP_PERSISTENT_NAME),
    //ipc size
    LPC_DRIVERS_IPC_COUNT,
    //function
    lpc_otg
};

#endif //MONOLITH_USB

void lpc_otg_on_isr(int vector, void* param)
{
//    int i;
    SHARED_OTG_DRV* drv = (SHARED_OTG_DRV*)param;
//    uint32_t sta = LPC_USB->INTSTAT;
//    EP* ep;

    iprintd("OTG ISR!!!\n\r");
/*
#if (USB_DEBUG_ERRORS)
    IPC ipc;
    switch (LPC_USB->INFO & USB_INFO_ERR_CODE_MASK)
    {
    case USB_INFO_ERR_CODE_NO_ERROR:
    case USB_INFO_ERR_CODE_IONAK:
        //no error
        break;
    default:
        ipc.process = process_iget_current();
        ipc.cmd = HAL_CMD(HAL_USB, LPC_USB_ERROR);
        ipc.param1 = USB_HANDLE_DEVICE;
        ipc.param2 = (LPC_USB->INFO & USB_INFO_ERR_CODE_MASK) >> USB_INFO_ERR_CODE_POS;
        ipc_ipost(&ipc);
        LPC_USB->INFO &= ~USB_INFO_ERR_CODE_MASK;
    }
#endif

    if (sta & USB_INTSTAT_DEV_INT)
    {
        sta = LPC_USB->DEVCMDSTAT;
        //Don't care on connection change, just clear pending bit
        if (sta & USB_DEVCMDSTAT_DCON_C)
            LPC_USB->DEVCMDSTAT |= USB_DEVCMDSTAT_DCON_C;
        if (sta & USB_DEVCMDSTAT_DSUS_C)
        {
            if (sta & USB_DEVCMDSTAT_DSUS)
                lpc_usb_suspend(drv);
            else
                lpc_usb_wakeup(drv);
            LPC_USB->DEVCMDSTAT |= USB_DEVCMDSTAT_DSUS_C;
        }
        if (sta & USB_DEVCMDSTAT_DRES_C)
        {
            lpc_usb_reset(drv);
            LPC_USB->DEVCMDSTAT |= USB_DEVCMDSTAT_DRES_C;
        }
        LPC_USB->INTSTAT = USB_INTSTAT_DEV_INT;
        return;
    }
    if ((sta & USB_INTSTAT_EP0OUT) && (LPC_USB->DEVCMDSTAT & USB_DEVCMDSTAT_SETUP))
    {
        lpc_usb_setup(drv);
        LPC_USB->DEVCMDSTAT |= USB_DEVCMDSTAT_SETUP;
        LPC_USB->INTSTAT = USB_INTSTAT_EP0OUT;
        return;
    }

    for (i = 0; i < USB_EP_COUNT_MAX; ++i)
    {
        if (sta & USB_EP_INT_BIT(i))
        {
            ep = drv->usb.out[i];
            if (ep && ep->io_active)
                lpc_usb_out(drv, i);
            LPC_USB->INTSTAT = USB_EP_INT_BIT(i);
        }
        if (sta & USB_EP_INT_BIT(USB_EP_IN | i))
        {
            ep = drv->usb.in[i];
            if (ep && ep->io_active)
                lpc_usb_in(drv, i | USB_EP_IN);
            LPC_USB->INTSTAT = USB_EP_INT_BIT(USB_EP_IN | i);
        }
    }
    */
}

void lpc_otg_init(SHARED_OTG_DRV* drv)
{
    int i;
    drv->otg.device = INVALID_HANDLE;
    drv->otg.addr = 0;
    for (i = 0; i < USB_EP_COUNT_MAX; ++i)
    {
        drv->otg.out[i] = NULL;
        drv->otg.in[i] = NULL;
    }
}

void lpc_otg_open_device(SHARED_OTG_DRV* drv, HANDLE device)
{
    int i;
    drv->otg.device = device;

    //power on. Turn USB0 PLL 0n
    LPC_CGU->PLL0USB_CTRL = CGU_PLL0USB_CTRL_PD_Msk;
    LPC_CGU->PLL0USB_CTRL |= CGU_PLL0USB_CTRL_DIRECTI_Msk | CGU_CLK_HSE;
    LPC_CGU->PLL0USB_MDIV = USBPLL_M;
    LPC_CGU->PLL0USB_NP_DIV = USBPLL_P;
    LPC_CGU->PLL0USB_CTRL &= ~CGU_PLL0USB_CTRL_PD_Msk;

    //power on. USBPLL must be turned on even in case of SYS PLL used. Why?
    //wait for PLL lock
    for (i = 0; i < PLL_LOCK_TIMEOUT; ++i)
        if (LPC_CGU->PLL0USB_STAT & CGU_PLL0USB_STAT_LOCK_Msk)
            break;
    if ((LPC_CGU->PLL0USB_STAT & CGU_PLL0USB_STAT_LOCK_Msk) == 0)
    {
        error(ERROR_HARDWARE);
        return;
    }
    //enable PLL clock
    LPC_CGU->PLL0USB_CTRL |= CGU_PLL0USB_CTRL_CLKEN_Msk;

    //set device mode
    LPC_USB0->USBMODE_D = USB0_USBMODE_CM_DEVICE;

    //clear any spurious pending interrupts
    //TODO: endpoints
    LPC_USB0->USBSTS_D = USB0_USBSTS_D_UI_Msk | USB0_USBSTS_D_UEI_Msk | USB0_USBSTS_D_PCI_Msk | USB0_USBSTS_D_SEI_Msk | USB0_USBSTS_D_URI_Msk |
                         USB0_USBSTS_D_SLI_Msk | USB0_USBSTS_D_NAKI_Msk;

    //enable interrupts
    irq_register(USB0_IRQn, lpc_otg_on_isr, drv);
    NVIC_EnableIRQ(USB0_IRQn);
    NVIC_SetPriority(USB0_IRQn, 1);

    //Unmask common interrupts
    LPC_USB0->USBINTR_D = USB0_USBINTR_D_UE_Msk | USB0_USBINTR_D_PCE_Msk | USB0_USBINTR_D_URE_Msk | USB0_USBINTR_D_SLE_Msk;
#if (USB_DEBUG_ERRORS)
    LPC_USB0->USBINTR_D |= USB0_USBINTR_D_UEE_Msk | USB0_USBINTR_D_SEE_Msk;
#endif

    //start
    LPC_USB0->USBCMD_D |= USB0_USBCMD_D_RS_Msk;
}

static inline bool lpc_otg_device_request(SHARED_OTG_DRV* drv, IPC* ipc)
{
    bool need_post = false;
    switch (HAL_ITEM(ipc->cmd))
    {
    case USB_GET_SPEED:
//TODO:        ipc->param2 = lpc_otg_get_speed(drv);
        need_post = true;
        break;
    case IPC_OPEN:
        lpc_otg_open_device(drv, ipc->process);
        need_post = true;
        break;
    case IPC_CLOSE:
//TODO:        lpc_otg_close_device(drv);
        need_post = true;
        break;
    case USB_SET_ADDRESS:
//TODO:        lpc_otg_set_address(drv, ipc->param2);
        need_post = true;
        break;
    //TODO: test mode
    //TODO:
/*#if (USB_DEBUG_ERRORS)
    case LPC_USB_ERROR:
        printd("USB driver error: %#x\n\r", ipc->param2);
        //posted from isr
        break;
#endif*/
    default:
        error(ERROR_NOT_SUPPORTED);
        need_post = true;
        break;
    }
    return need_post;
}

static inline bool lpc_otg_ep_request(SHARED_OTG_DRV* drv, IPC* ipc)
{
    bool need_post = false;
    if (USB_EP_NUM(ipc->param1) >= USB_EP_COUNT_MAX)
    {
        switch (HAL_ITEM(ipc->cmd))
        {
        case IPC_READ:
        case IPC_WRITE:
            io_send_error(ipc, ERROR_INVALID_PARAMS);
            break;
        default:
            error(ERROR_INVALID_PARAMS);
            need_post = true;
        }
    }
    else
        switch (HAL_ITEM(ipc->cmd))
        {
        case IPC_OPEN:
//TODO:            lpc_otg_open_ep(drv, ipc->param1, ipc->param2, ipc->param3);
            need_post = true;
            break;
        case IPC_CLOSE:
//TODO:            lpc_otg_close_ep(drv, ipc->param1);
            need_post = true;
            break;
        case IPC_FLUSH:
//TODO:            lpc_otg_ep_flush(drv, ipc->param1);
            need_post = true;
            break;
        case USB_EP_SET_STALL:
//TODO:            lpc_otg_ep_set_stall(drv, ipc->param1);
            need_post = true;
            break;
        case USB_EP_CLEAR_STALL:
//TODO:            lpc_otg_ep_clear_stall(drv, ipc->param1);
            need_post = true;
            break;
        case USB_EP_IS_STALL:
//TODO:            ipc->param2 = lpc_otg_ep_is_stall(ipc->param1);
            need_post = true;
            break;
        case IPC_READ:
//TODO:            lpc_otg_read(drv, ipc);
            //posted with io, no return IPC
            break;
        case IPC_WRITE:
//TODO:            lpc_otg_write(drv, ipc);
            //posted with io, no return IPC
            break;
        default:
            error(ERROR_NOT_SUPPORTED);
            need_post = true;
            break;
        }
    return need_post;
}

bool lpc_otg_request(SHARED_OTG_DRV* drv, IPC* ipc)
{
    bool need_post = false;
    if (ipc->param1 == USB_HANDLE_DEVICE)
        need_post = lpc_otg_device_request(drv, ipc);
    else
        need_post = lpc_otg_ep_request(drv, ipc);
    return need_post;
}

#if !(MONOLITH_USB)
void lpc_usb()
{
    IPC ipc;
    SHARED_OTG_DRV drv;
    bool need_post;
    object_set_self(SYS_OBJ_USB);
    lpc_otg_init(&drv);
    for (;;)
    {
        ipc_read(&ipc);
        need_post = lpc_otg_request(&drv, &ipc);
        if (need_post)
            ipc_post_or_error(&ipc);
    }
}
#endif //!MONOLITH_USB
