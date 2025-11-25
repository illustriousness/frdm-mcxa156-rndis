/*
 * Copyright (c) 2025, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2025-01-17     Supperthomas first version
 * 2025-02-25     hydevcode
 */
#include <rtthread.h>
#include <board.h>
#ifdef RT_CHERRYUSB_DEVICE_TEMPLATE_CDC_ACM
/* Register the EMAC device */
static int rt_hw_stm32_cherryusb_cdc_init(void)
{
    extern void cdc_acm_init(uint8_t busid, uintptr_t reg_base);
    cdc_acm_init(0, 0x400A4000u);
    return 0;
}
INIT_COMPONENT_EXPORT(rt_hw_stm32_cherryusb_cdc_init);
#endif

#ifdef RT_CHERRYUSB_DEVICE_TEMPLATE_CDC_RNDIS
/* USB RNDIS device initialization - allows PC to share network via USB */
static int rt_hw_mcx_cherryusb_rndis_init(void)
{
    extern void cdc_rndis_init(uint8_t busid, uintptr_t reg_base);
    cdc_rndis_init(0, 0x400A4000u);
    return 0;
}
INIT_COMPONENT_EXPORT(rt_hw_mcx_cherryusb_rndis_init);
#endif