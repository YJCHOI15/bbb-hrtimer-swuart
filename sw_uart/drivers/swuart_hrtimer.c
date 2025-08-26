/* swuart_hrtimer.c */
/* 커널 hrtimer로 비트뱅잉 UART(TX/RX) 구현 */

#include <linux/device.h> 
#include <linux/fs.h> 
#include <linux/module.h> 

#define SWUART_CLASS_NAME      "sw_uart" 
#define SWUART_DEVICE_NAME     "sw_uart_hr" 
 
static int swuart_device_major; 
static struct class *swuart_class; 
static struct device *swuart_device; 

static struct file_operations swuart_device_fops = { 
        // .read = swuart_device_read, 
        // .write = swuart_device_write, 
}; 
 
static int __init swuart_module_init(void) { 
        int ret = 0; 
 
        swuart_device_major = register_chrdev(0, SWUART_DEVICE_NAME, &swuart_device_fops); 
        if (swuart_device_major < 0) { 
                printk(KERN_ERR "%s: Failed to get major number", SWUART_DEVICE_NAME); 
                ret = swuart_device_major; 
                goto err_register_chrdev; 
        } 
 
        swuart_class = class_create(THIS_MODULE, SWUART_CLASS_NAME); 
        if (IS_ERR(swuart_class)) { 
                printk(KERN_ERR "%s: Failed to create class", SWUART_DEVICE_NAME); 
                ret = PTR_ERR(swuart_class); 
                goto err_class; 
        } 
 
        swuart_device = device_create(swuart_class, NULL, 
                        MKDEV(swuart_device_major, 0), NULL, 
                        SWUART_DEVICE_NAME);
        if (IS_ERR(swuart_device)) { 
                ret = PTR_ERR(swuart_device); 
                goto err_device; 
        } 
 
        return ret; 
 
err_device: 
        class_destroy(swuart_class); 
err_class: 
        unregister_chrdev(swuart_device_major, SWUART_DEVICE_NAME); 
err_register_chrdev: 
        return ret; 
} 
 
static void __exit swuart_module_exit(void) { 
 
        printk(KERN_DEBUG "%s", __func__); 

        device_destroy(swuart_class, MKDEV(swuart_device_major, 0)); 
        class_destroy(swuart_class); 
        unregister_chrdev(swuart_device_major, SWUART_DEVICE_NAME); 
} 
 
module_init(swuart_module_init); 
module_exit(swuart_module_exit); 
 
MODULE_AUTHOR("YJCHOI15(ccyjin15@gmail.com)"); 
MODULE_DESCRIPTION("An sw uart device driver using kernel hrtimer"); 
MODULE_LICENSE("GPL v2");