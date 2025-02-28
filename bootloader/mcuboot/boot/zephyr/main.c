/*
 * Copyright (c) 2012-2014 Wind River Systems, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */


#include <assert.h>
#include <zephyr.h>
#include <drivers/gpio.h>
#include <sys/__assert.h>
#include <drivers/flash.h>
#include <drivers/timer/system_timer.h>
#include <usb/usb_device.h>
#include <soc.h>
#include <linker/linker-defs.h>

#include "target.h"

#include "bootutil/bootutil_log.h"
#include "bootutil/image.h"
#include "bootutil/bootutil.h"
#include "flash_map_backend/flash_map_backend.h"

#if(CONFIG_BOOT_IOTEX_BOARD_VERSION==3)
#define  POWER_ON_PIN  06
#define  LED_RED_PIN  16
#define  LED_GREEN_PIN  18
#define  LED_BLUE_PIN  17
#define  OLED_RST_PIN  27
Z_GENERIC_SECTION(.openocd_dbg.5) __attribute__((used)) const  uint8_t AppVersion[]= "3.0_1.4.0_boot:1.1.1>";

#elif (CONFIG_BOOT_IOTEX_BOARD_VERSION==2)
#define  POWER_ON_PIN  15
#define  LED_RED_PIN  30
#define  LED_GREEN_PIN  26
#define  LED_BLUE_PIN  27
Z_GENERIC_SECTION(.openocd_dbg.5) __attribute__((used)) const  uint8_t AppVersion[]="2.0_1.4.0_boot:1.1.1>";
#endif



#define  POWER_ON_IO_VAL   1
#define  POWER_OFF_IO_VAL  0
//  5s  power key , entry firmware update  proc.
#define  PWK_ENTRY_UPDATE_TIM    5000     
//  2s   power key active time
#define  PWK_ACTIVE_TIME    500

#define GPIO_DIR_OUT  GPIO_OUTPUT
#define GPIO_DIR_IN   GPIO_INPUT
#define GPIO_INT  GPIO_INT_ENABLE
#define GPIO_INT_DOUBLE_EDGE  GPIO_INT_EDGE_BOTH
#define gpio_pin_write  gpio_pin_set

#ifdef CONFIG_FW_INFO
#include <fw_info.h>
#endif

#ifdef CONFIG_MCUBOOT_SERIAL
#include "boot_serial/boot_serial.h"
#include "serial_adapter/serial_adapter.h"

const struct boot_uart_funcs boot_funcs = {
    .read = console_read,
    .write = console_write
};
#endif

#ifdef CONFIG_BOOT_WAIT_FOR_USB_DFU
#include <usb/class/usb_dfu.h>
#endif

#if CONFIG_MCUBOOT_CLEANUP_ARM_CORE
#include <arm_cleanup.h>
#endif

#ifdef CONFIG_SOC_NRF5340_CPUAPP
#include <dfu/pcd.h>
#endif

#if defined(CONFIG_LOG) && !defined(CONFIG_LOG_IMMEDIATE)
#ifdef CONFIG_LOG_PROCESS_THREAD
#warning "The log internal thread for log processing can't transfer the log"\
         "well for MCUBoot."
#else
#include <logging/log_ctrl.h>

#define BOOT_LOG_PROCESSING_INTERVAL K_MSEC(30) /* [ms] */

/* log are processing in custom routine */
K_THREAD_STACK_DEFINE(boot_log_stack, CONFIG_MCUBOOT_LOG_THREAD_STACK_SIZE);
struct k_thread boot_log_thread;
volatile bool boot_log_stop = false;
K_SEM_DEFINE(boot_log_sem, 1, 1);

/* log processing need to be initalized by the application */
#define ZEPHYR_BOOT_LOG_START() zephyr_boot_log_start()
#define ZEPHYR_BOOT_LOG_STOP() zephyr_boot_log_stop()
#endif /* CONFIG_LOG_PROCESS_THREAD */
#else
/* synchronous log mode doesn't need to be initalized by the application */
#define ZEPHYR_BOOT_LOG_START() do { } while (false)
#define ZEPHYR_BOOT_LOG_STOP() do { } while (false)
#endif /* defined(CONFIG_LOG) && !defined(CONFIG_LOG_IMMEDIATE) */

#if USE_PARTITION_MANAGER && CONFIG_FPROTECT
#include <fprotect.h>
#include <pm_config.h>

#endif

#if CONFIG_MCUBOOT_NRF_CLEANUP_PERIPHERAL
#include <nrf_cleanup.h>
#endif

#ifdef CONFIG_SOC_FAMILY_NRF
#include <hal/nrf_power.h>

static inline bool boot_skip_serial_recovery()
{
#if NRF_POWER_HAS_RESETREAS
    uint32_t rr = nrf_power_resetreas_get(NRF_POWER);

    return !(rr == 0 || (rr & NRF_POWER_RESETREAS_RESETPIN_MASK));
#else
    return false;
#endif
}
#else
static inline bool boot_skip_serial_recovery()
{
    return false;
}
#endif

MCUBOOT_LOG_MODULE_REGISTER(mcuboot);

void os_heap_init(void);

#if defined(CONFIG_ARM)

#ifdef CONFIG_SW_VECTOR_RELAY
extern void *_vector_table_pointer;
#endif

struct arm_vector_table {
    uint32_t msp;
    uint32_t reset;
};

extern void sys_clock_disable(void);

static void do_boot(struct boot_rsp *rsp)
{
    struct arm_vector_table *vt;
    uintptr_t flash_base;
    int rc;

    /* The beginning of the image is the ARM vector table, containing
     * the initial stack pointer address and the reset vector
     * consecutively. Manually set the stack pointer and jump into the
     * reset vector
     */
    rc = flash_device_base(rsp->br_flash_dev_id, &flash_base);
    assert(rc == 0);

    vt = (struct arm_vector_table *)(flash_base +
                                     rsp->br_image_off +
                                     rsp->br_hdr->ih_hdr_size);
    irq_lock();
#ifdef CONFIG_SYS_CLOCK_EXISTS
    sys_clock_disable();
#endif
#ifdef CONFIG_USB
    /* Disable the USB to prevent it from firing interrupts */
    usb_disable();
#endif

#if defined(CONFIG_FW_INFO) && !defined(CONFIG_EXT_API_PROVIDE_EXT_API_UNUSED)
    bool provided = fw_info_ext_api_provide(fw_info_find((uint32_t)vt), true);

#ifdef PM_S0_ADDRESS
    /* Only fail if the immutable bootloader is present. */
    if (!provided) {
        BOOT_LOG_ERR("Failed to provide EXT_APIs\n");
        return;
    }
#endif
#endif
#if CONFIG_MCUBOOT_NRF_CLEANUP_PERIPHERAL
    nrf_cleanup_peripheral();
#endif
#if CONFIG_MCUBOOT_CLEANUP_ARM_CORE
    cleanup_arm_nvic(); /* cleanup NVIC registers */
#endif

#if defined(CONFIG_BUILTIN_STACK_GUARD) && \
    defined(CONFIG_CPU_CORTEX_M_HAS_SPLIM)
    /* Reset limit registers to avoid inflicting stack overflow on image
     * being booted.
     */
    __set_PSPLIM(0);
    __set_MSPLIM(0);
#endif

#ifdef CONFIG_BOOT_INTR_VEC_RELOC
#if defined(CONFIG_SW_VECTOR_RELAY)
    _vector_table_pointer = vt;
#ifdef CONFIG_CPU_CORTEX_M_HAS_VTOR
    SCB->VTOR = (uint32_t)__vector_relay_table;
#endif
#elif defined(CONFIG_CPU_CORTEX_M_HAS_VTOR)
    SCB->VTOR = (uint32_t)vt;
#endif /* CONFIG_SW_VECTOR_RELAY */
#else /* CONFIG_BOOT_INTR_VEC_RELOC */
#if defined(CONFIG_CPU_CORTEX_M_HAS_VTOR) && defined(CONFIG_SW_VECTOR_RELAY)
    _vector_table_pointer = _vector_start;
    SCB->VTOR = (uint32_t)__vector_relay_table;
#endif
#endif /* CONFIG_BOOT_INTR_VEC_RELOC */

    __set_MSP(vt->msp);
#if CONFIG_MCUBOOT_CLEANUP_ARM_CORE
    __set_CONTROL(0x00); /* application will configures core on its own */
#endif
    ((void (*)(void))vt->reset)();
}

#elif defined(CONFIG_XTENSA)
#define SRAM_BASE_ADDRESS	0xBE030000

static void copy_img_to_SRAM(int slot, unsigned int hdr_offset)
{
    const struct flash_area *fap;
    int area_id;
    int rc;
    unsigned char *dst = (unsigned char *)(SRAM_BASE_ADDRESS + hdr_offset);

    BOOT_LOG_INF("Copying image to SRAM");

    area_id = flash_area_id_from_image_slot(slot);
    rc = flash_area_open(area_id, &fap);
    if (rc != 0) {
        BOOT_LOG_ERR("flash_area_open failed with %d\n", rc);
        goto done;
    }

    rc = flash_area_read(fap, hdr_offset, dst, fap->fa_size - hdr_offset);
    if (rc != 0) {
        BOOT_LOG_ERR("flash_area_read failed with %d\n", rc);
        goto done;
    }

done:
    flash_area_close(fap);
}

/* Entry point (.ResetVector) is at the very beginning of the image.
 * Simply copy the image to a suitable location and jump there.
 */
static void do_boot(struct boot_rsp *rsp)
{
    void *start;

    BOOT_LOG_INF("br_image_off = 0x%x\n", rsp->br_image_off);
    BOOT_LOG_INF("ih_hdr_size = 0x%x\n", rsp->br_hdr->ih_hdr_size);

    /* Copy from the flash to HP SRAM */
    copy_img_to_SRAM(0, rsp->br_hdr->ih_hdr_size);

    /* Jump to entry point */
    start = (void *)(SRAM_BASE_ADDRESS + rsp->br_hdr->ih_hdr_size);
    ((void (*)(void))start)();
}

#else
/* Default: Assume entry point is at the very beginning of the image. Simply
 * lock interrupts and jump there. This is the right thing to do for X86 and
 * possibly other platforms.
 */
static void do_boot(struct boot_rsp *rsp)
{
    uintptr_t flash_base;
    void *start;
    int rc;

    rc = flash_device_base(rsp->br_flash_dev_id, &flash_base);
    assert(rc == 0);

    start = (void *)(flash_base + rsp->br_image_off +
                     rsp->br_hdr->ih_hdr_size);

    /* Lock interrupts and dive into the entry point */
    irq_lock();
    ((void (*)(void))start)();
}
#endif

#if defined(CONFIG_LOG) && !defined(CONFIG_LOG_IMMEDIATE) &&\
    !defined(CONFIG_LOG_PROCESS_THREAD)
/* The log internal thread for log processing can't transfer log well as has too
 * low priority.
 * Dedicated thread for log processing below uses highest application
 * priority. This allows to transmit all logs without adding k_sleep/k_yield
 * anywhere else int the code.
 */

/* most simple log processing theread */
void boot_log_thread_func(void *dummy1, void *dummy2, void *dummy3)
{
    (void)dummy1;
    (void)dummy2;
    (void)dummy3;

     log_init();

     while (1) {
             if (log_process(false) == false) {
                    if (boot_log_stop) {
                        break;
                    }
                    k_sleep(BOOT_LOG_PROCESSING_INTERVAL);
             }
     }

     k_sem_give(&boot_log_sem);
}

void zephyr_boot_log_start(void)
{
        /* start logging thread */
        k_thread_create(&boot_log_thread, boot_log_stack,
                K_THREAD_STACK_SIZEOF(boot_log_stack),
                boot_log_thread_func, NULL, NULL, NULL,
                K_HIGHEST_APPLICATION_THREAD_PRIO, 0,
                BOOT_LOG_PROCESSING_INTERVAL);

        k_thread_name_set(&boot_log_thread, "logging");
}

void zephyr_boot_log_stop(void)
{
    boot_log_stop = true;

    /* wait until log procesing thread expired
     * This can be reworked using a thread_join() API once a such will be
     * available in zephyr.
     * see https://github.com/zephyrproject-rtos/zephyr/issues/21500
     */
    (void)k_sem_take(&boot_log_sem, K_FOREVER);
}
#endif/* defined(CONFIG_LOG) && !defined(CONFIG_LOG_IMMEDIATE) &&\
        !defined(CONFIG_LOG_PROCESS_THREAD) */


struct device *detect_port;

void LedCtrl(uint8_t port, uint32_t val)
{
    gpio_pin_write(detect_port, port,val ); 
}

void TimdeDelay(int32_t ms)
{
    int32_t time_stamp;
    volatile int32_t milliseconds_spent;
    time_stamp = k_uptime_get(); 
    while(true)
    {
        milliseconds_spent = k_uptime_get();
        if((milliseconds_spent - time_stamp) >=ms)
            break;
    }
}

void PowerOfIndicator(void)
{  
    int i;
    for(i=0; i < 3; i++)  
    {
        gpio_pin_write(detect_port, LED_RED_PIN,0);
        TimdeDelay(1000);
        gpio_pin_write(detect_port, LED_RED_PIN,1);
        TimdeDelay(1000);
    }
    gpio_pin_write(detect_port, POWER_ON_PIN,0);
    TimdeDelay(5000);
}

void fatalError(void)
{
    int i;
    for(i=0; i < 3; i++)  
    {
        gpio_pin_write(detect_port, LED_RED_PIN,0);
        gpio_pin_write(detect_port, LED_BLUE_PIN,0 );
        TimdeDelay(1000);
        gpio_pin_write(detect_port, LED_RED_PIN,1);
        gpio_pin_write(detect_port, LED_BLUE_PIN,1 );
        TimdeDelay(1000);
    }
    gpio_pin_write(detect_port, POWER_ON_PIN,0);
    TimdeDelay(5000);    
}

void main(void)
{
    struct boot_rsp rsp;
    int rc;

    BOOT_LOG_INF("Starting bootloader");

    os_heap_init();

    ZEPHYR_BOOT_LOG_START();

#if (!defined(CONFIG_XTENSA) && defined(DT_CHOSEN_ZEPHYR_FLASH_CONTROLLER_LABEL))
    if (!flash_device_get_binding(DT_CHOSEN_ZEPHYR_FLASH_CONTROLLER_LABEL)) {
        BOOT_LOG_ERR("Flash device %s not found",
		     DT_CHOSEN_ZEPHYR_FLASH_CONTROLLER_LABEL);
        while (1)
            ;
    }
#elif (defined(CONFIG_XTENSA) && defined(JEDEC_SPI_NOR_0_LABEL))
    if (!flash_device_get_binding(JEDEC_SPI_NOR_0_LABEL)) {
        BOOT_LOG_ERR("Flash device %s not found", JEDEC_SPI_NOR_0_LABEL);
        while (1)
            ;
    }
#endif

#ifdef CONFIG_MCUBOOT_SERIAL

    //struct device *detect_port;
    uint32_t detect_value = !CONFIG_BOOT_SERIAL_DETECT_PIN_VAL;

    detect_port = device_get_binding(CONFIG_BOOT_SERIAL_DETECT_PORT);
    __ASSERT(detect_port, "Error: Bad port for boot serial detection.\n");

    /* The default presence value is 0 which would normally be
     * active-low, but historically the raw value was checked so we'll
     * use the raw interface.
     */
    rc = gpio_pin_configure(detect_port, CONFIG_BOOT_SERIAL_DETECT_PIN,
#ifdef GPIO_INPUT
                            GPIO_INPUT | GPIO_PULL_UP
#else
                            GPIO_DIR_IN | GPIO_PUD_PULL_UP
#endif
           );
    __ASSERT(rc == 0, "Error of boot detect pin initialization.\n");
    gpio_pin_configure(detect_port, POWER_ON_PIN, GPIO_DIR_OUT);

#ifdef GPIO_INPUT
    rc = gpio_pin_get_raw(detect_port, CONFIG_BOOT_SERIAL_DETECT_PIN);
    detect_value = rc;
#else
    rc = gpio_pin_read(detect_port, CONFIG_BOOT_SERIAL_DETECT_PIN,
                       &detect_value);
#endif

    __ASSERT(rc >= 0, "Error of the reading the detect pin.\n"); 
    gpio_pin_configure(detect_port, LED_RED_PIN,GPIO_DIR_OUT);    //qiuhm 0526
    gpio_pin_configure(detect_port, LED_GREEN_PIN,GPIO_DIR_OUT);    //qiuhm 0526
    gpio_pin_configure(detect_port, LED_BLUE_PIN,GPIO_DIR_OUT);    //qiuhm 0526
    gpio_pin_configure(detect_port, POWER_ON_PIN, GPIO_DIR_OUT);
#if(CONFIG_BOOT_IOTEX_BOARD_VERSION==3)
    gpio_pin_configure(detect_port, OLED_RST_PIN, GPIO_DIR_OUT);
    gpio_pin_write(detect_port, OLED_RST_PIN,0);    
#endif
    gpio_pin_write(detect_port, POWER_ON_PIN,1);
    gpio_pin_write(detect_port, LED_RED_PIN,1);
    gpio_pin_write(detect_port, LED_GREEN_PIN,1 ); 
    gpio_pin_write(detect_port, LED_BLUE_PIN,1 );    
    // check voltage
    adc_module_init();
    power_check();  
#if(CONFIG_BOOT_IOTEX_BOARD_VERSION==3)     
    gpio_pin_write(detect_port, OLED_RST_PIN,1);
#endif
    gpio_pin_write(detect_port, LED_BLUE_PIN,0 );    
   // gpio_pin_write(detect_port, 26,1 ); 
   // gpio_pin_write(detect_port, 30,1 );
#if(CONFIG_BOOT_IOTEX_BOARD_VERSION==3) 
    Timer_Initialize();
	ssd1306_init();
    closeTimer2();
#endif
    //k_sleep(K_MSEC(2000));
    int32_t time_stamp;
    int32_t milliseconds_spent;
    time_stamp = k_uptime_get();
    rc = 0;
     milliseconds_spent = k_uptime_get();
    while((detect_value == CONFIG_BOOT_SERIAL_DETECT_PIN_VAL)&&((milliseconds_spent-time_stamp) < PWK_ENTRY_UPDATE_TIM)){
        milliseconds_spent = k_uptime_get();
        if((!rc)&&((milliseconds_spent-time_stamp) >= PWK_ACTIVE_TIME)){
            gpio_pin_write(detect_port, POWER_ON_PIN, POWER_ON_IO_VAL);
            rc++;
        }
        //gpio_pin_read(detect_port, CONFIG_BOOT_SERIAL_DETECT_PIN, &detect_value); 
        detect_value = gpio_pin_get(detect_port, CONFIG_BOOT_SERIAL_DETECT_PIN);
    }    
    //if (detect_value == CONFIG_BOOT_SERIAL_DETECT_PIN_VAL &&
    if (((milliseconds_spent-time_stamp) >=PWK_ENTRY_UPDATE_TIM ) &&
        !boot_skip_serial_recovery()) {
        BOOT_LOG_INF("Enter the serial recovery mode");
        rc = boot_console_init();
        __ASSERT(rc == 0, "Error initializing boot console.\n");

gpio_pin_write(detect_port,LED_BLUE_PIN, 1 );//qiuhm 0526
gpio_pin_write(detect_port, LED_RED_PIN,0 );

        boot_serial_start(&boot_funcs);
        __ASSERT(0, "Bootloader serial process was terminated unexpectedly.\n");
    }
#endif

#ifdef CONFIG_BOOT_WAIT_FOR_USB_DFU
    rc = usb_enable(NULL);
    if (rc) {
        BOOT_LOG_ERR("Cannot enable USB");
    } else {
        BOOT_LOG_INF("Waiting for USB DFU");
        wait_for_usb_dfu();
        BOOT_LOG_INF("USB DFU wait time elapsed");
    }
#endif

    rc = boot_go(&rsp);
    if (rc != 0) {
        BOOT_LOG_ERR("Unable to find bootable image");
        while (1)
            ;
    }

    BOOT_LOG_INF("Bootloader chainload address offset: 0x%x",
                 rsp.br_image_off);

    BOOT_LOG_INF("Jumping to the first image slot");

#if USE_PARTITION_MANAGER && CONFIG_FPROTECT

#ifdef PM_S1_ADDRESS
/* MCUBoot is stored in either S0 or S1, protect both */
#define PROTECT_SIZE (PM_MCUBOOT_PRIMARY_ADDRESS - PM_S0_ADDRESS)
#define PROTECT_ADDR PM_S0_ADDRESS
#else
/* There is only one instance of MCUBoot */
#define PROTECT_SIZE (PM_MCUBOOT_PRIMARY_ADDRESS - PM_MCUBOOT_ADDRESS)
#define PROTECT_ADDR PM_MCUBOOT_ADDRESS
//#define PROTECT_SIZE (PM_MCUBOOT_PRIMARY_ADDRESS - 0x4000)
//#define PROTECT_ADDR 0x4000
#endif

    rc = fprotect_area(PROTECT_ADDR, PROTECT_SIZE);

    if (rc != 0) {
        BOOT_LOG_ERR("Protect mcuboot flash failed, cancel startup.");
        while (1)
            ;
    }
#endif /* USE_PARTITION_MANAGER && CONFIG_FPROTECT */
#if defined(CONFIG_SOC_NRF5340_CPUAPP) && defined(PM_CPUNET_B0N_ADDRESS)
    pcd_lock_ram();
#endif

    ZEPHYR_BOOT_LOG_STOP();

    do_boot(&rsp);

    BOOT_LOG_ERR("Never should get here");
    while (1)
        ;
}
