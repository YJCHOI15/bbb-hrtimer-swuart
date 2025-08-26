/* swuart_hrtimer.c */
/* 커널 hrtimer로 비트뱅잉 UART(TX/RX) 구현 */

#include <linux/device.h> 
#include <linux/fs.h> 
#include <linux/module.h> 
 
#define SWUART_CLASS_NAME      "sw_uart" 
#define COMENTO_DEVICE_NAME     "sw_uart_hr" 
 
#define COMENTO_IOCTL_MAGIC     'C' 
#define COMENTO_IOCTL_ADD       _IOW(COMENTO_IOCTL_MAGIC, 0, int) 
#define COMENTO_IOCTL_DEL       _IOW(COMENTO_IOCTL_MAGIC, 1, int) 
 
#define COMENTO_BUF_SIZE 32 
 
static int comento_device_major; 
static struct class *comento_class; 
static struct device *comento_device; 
 
struct device_data { 
        int key; 
        char passwd[COMENTO_BUF_SIZE]; 
        struct device *device; 
        struct hlist_node node; 
}; 
 
static DEFINE_HASHTABLE(device_table, 8); 
static DEFINE_RWLOCK(comento_device_rwlock); 
 
static struct device_data *get_device_data(unsigned long key) { 
        struct device_data *data; 
        hash_for_each_possible(device_table, data, node, key) { 
                if (data->key != key) { 
                        continue; 
                } 
 
                return data; 
        } 
 
        return NULL; 
} 

static ssize_t comento_device_read(struct file *fp, char __user *buf, 
                                                                    size_t len, loff_t *ppos) 
{ 
        int minor = iminor(fp->f_inode); 
        int written_bytes = 0; 
        struct device_data *data; 
        read_lock(&comento_device_rwlock); 
        data = get_device_data(minor); 
        if (data == NULL)  { 
                read_unlock(&comento_device_rwlock); 
                return -1; 
        } 
        if (COMENTO_BUF_SIZE <= len + *ppos) { 
                len = COMENTO_BUF_SIZE - *ppos; 
        } 
        written_bytes = len - copy_to_user(buf, 
                        data->passwd + *ppos, len); 
        *ppos += written_bytes; 
        read_unlock(&comento_device_rwlock); 
        return written_bytes; 
} 
static ssize_t comento_device_write(struct file *fp, const char __user *buf, 
                                                                    size_t len, loff_t *ppos) 
{ 
        int minor = iminor(fp->f_inode); 
        int read_bytes = 0; 
        struct device_data *data; 
        write_lock(&comento_device_rwlock); 
        data = get_device_data(minor); 
        if (data == NULL)  { 
                write_unlock(&comento_device_rwlock); 
                return -1; 
        } 
        if (COMENTO_BUF_SIZE <= len + *ppos) { 
                len = COMENTO_BUF_SIZE - *ppos; 
        } 
        read_bytes = len - copy_from_user(data->passwd + *ppos, 
                       buf, len); 
        *ppos += read_bytes; 
        write_unlock(&comento_device_rwlock); 
        return read_bytes; 
} 

static long comento_device_ioctl(struct file *fp, unsigned int cmd, unsigned long arg) 
{ 
        int ret = 0, bkt; 
        struct device_data *data; 
        struct hlist_node *tmp; 
        switch (cmd) { 
        case COMENTO_IOCTL_ADD: 
                if (arg >= 1048576) { 
                        ret = -EINVAL; 
                        break; 
                } 
                write_lock(&comento_device_rwlock); 
                data = get_device_data(arg + 1); 
                if (data != NULL) { 
                        ret = -EEXIST; 
                        write_unlock(&comento_device_rwlock); 
                        break; 
                } 
                data = kmalloc(sizeof(struct device_data), GFP_KERNEL); 
                data->device = device_create(comento_class, NULL, 
                        MKDEV(comento_device_major, arg + 1), NULL, 
                        "%s%ld", COMENTO_DEVICE_NAME, arg); 
                if (IS_ERR(data->device)) { 
                        ret = PTR_ERR(data->device); 
                        kfree(data); 
                        write_unlock(&comento_device_rwlock); 
                        break; 
                } 
                data->key = arg + 1; 
                memset(data->passwd, 0, COMENTO_BUF_SIZE); 
                hash_add(device_table, &data->node, data->key); 
                write_unlock(&comento_device_rwlock); 
                break; 
        case COMENTO_IOCTL_DEL: 
                ret = -ENOENT; 
                write_lock(&comento_device_rwlock); 
                hash_for_each_safe(device_table, bkt, tmp, data, node) { 
                        if (data->key == arg + 1) { 
                                hash_del(&data->node); 
                                device_destroy(comento_class, MKDEV(comento_device_major, 
                                                           data->key)); 
                                kfree(data);

                            ret = 0; 
                        } 
                } 
                write_unlock(&comento_device_rwlock); 
                break; 
        default: 
                ret = -EINVAL; 
        } 
        return ret; 
} 
 
static struct file_operations comento_device_fops = { 
        .read = comento_device_read, 
        .write = comento_device_write, 
        .unlocked_ioctl = comento_device_ioctl, 
}; 
 
static int __init comento_module_init(void) 
{ 
        int ret = 0; 
 
        comento_device_major = register_chrdev(0, COMENTO_CTL_DEVICE_NAME, 
                        &comento_device_fops); 
        if (comento_device_major < 0) { 
                printk(KERN_ERR "%s: Failed to get major number", 
                                COMENTO_DEVICE_NAME); 
                ret = comento_device_major; 
                goto err_register_chrdev; 
        } 
 
        comento_class = class_create(THIS_MODULE, COMENTO_CLASS_NAME); 
        if (IS_ERR(comento_class)) { 
                printk(KERN_ERR "%s: Failed to create class", 
                                COMENTO_DEVICE_NAME); 
                ret = PTR_ERR(comento_class); 
                goto err_class; 
        } 
 
        comento_device = device_create(comento_class, NULL, 
                        MKDEV(comento_device_major, 0), NULL, 
                        COMENTO_CTL_DEVICE_NAME);

 if (IS_ERR(comento_device)) { 
                ret = PTR_ERR(comento_device); 
                goto err_device; 
        } 
 
        return ret; 
 
err_device: 
        class_destroy(comento_class); 
err_class: 
        unregister_chrdev(comento_device_major, COMENTO_DEVICE_NAME); 
err_register_chrdev: 
        return ret; 
} 
 
static void __exit comento_module_exit(void) 
{ 
        int bkt; 
        struct device_data *data; 
        struct hlist_node *tmp; 
 
        printk(KERN_DEBUG "%s", __func__); 
 
        hash_for_each_safe(device_table, bkt, tmp, data, node) { 
                hash_del(&data->node); 
                device_destroy(comento_class, MKDEV(comento_device_major, 
                                            data->key)); 
                kfree(data); 
        } 
        device_destroy(comento_class, 
                        MKDEV(comento_device_major, 0)); 
        class_destroy(comento_class); 
        unregister_chrdev(comento_device_major, COMENTO_DEVICE_NAME); 
} 
 
module_init(comento_module_init); 
module_exit(comento_module_exit); 
 
MODULE_AUTHOR("DdunAnt <ddunant@comento.com>"); 
MODULE_DESCRIPTION("An keyring driver for comento"); 
MODULE_LICENSE("GPL v2");