/* swuart_hrtimer.c */
/* 커널 hrtimer로 비트뱅잉 UART(TX/RX) 구현 */

#include <linux/device.h> 
#include <linux/fs.h> 
#include <linux/module.h> 
#include <linux/hrtimer.h>
#include <linux/ktime.h>
#include <linux/gpio.h>
#include <linux/ioport.h>
#include <linux/io.h>
#include <linux/completion.h>
#include <linux/interrupt.h>
#include <linux/uaccess.h>
#include <linux/spinlock.h>

#define SWUART_CLASS_NAME    "sw_uart" 
#define SWUART_DEVICE_NAME   "sw_uart_hr" 

#define BAUD_RATE              9600
#define BIT_DELAY_NS           (1000000000 / BAUD_RATE) // (1초 / baudrate)

#define GPIO_TX 12                            // GPIO1_12(44), P8_12
#define GPIO_RX 13                            // GPIO1_13(45), P8_11

#define GPIO_BASE              0x4804C000     // AM335x GPIO1 base address
#define GPIO_END               0x4804CFFF
#define GPIO_SIZE              GPIO_END-GPIO_BASE

#define GPIO_OE_OFFSET         0x134          // Input/Output 결정 레지스터
#define GPIO_DATAIN_OFFSET     0x138          // 핀에 들어오는 전압값에 따라 0과 1
#define GPIO_CLEAR_OFFSET      0x190
#define GPIO_SET_OFFSET        0x194

static void __iomem *gpio_base;
// volatile unsigned int* gpio_base;

#define GPIO_IN(g)    writel(readl(gpio_base + GPIO_OE_OFFSET) | (1 << g), gpio_base + GPIO_OE_OFFSET)
#define GPIO_OUT(g)   writel(readl(gpio_base + GPIO_OE_OFFSET) & ~(1 << g), gpio_base + GPIO_OE_OFFSET)
#define GPIO_SET(g)   writel(1 << g, gpio_base + GPIO_SET_OFFSET)
#define GPIO_CLEAR(g) writel(1 << g, gpio_base + GPIO_CLEAR_OFFSET)
#define GPIO_READ(g)  ((readl(gpio_base + GPIO_DATAIN_OFFSET) >> g) & 1)

// #define GPIO_IN(g)       ((*(gpio_base+GPIO_OE_OFFSET/4)) |= (1<<g))
// #define GPIO_OUT(g)      ((*(gpio_base+GPIO_OE_OFFSET/4)) &= ~(1<<g))
// #define GPIO_SET(g)      ((*(gpio_base+GPIO_SET_OFFSET/4)) |= (1<<g))
// #define GPIO_CLEAR(g)    ((*(gpio_base+GPIO_CLEAR_OFFSET/4)) |= (1<<g))
// #define GPIO_READ(g)     (*(gpio_base+GPIO_DATAIN_OFFSET/4) & (1<<g)) ? 1 : 0

/******************** 함수 선언 *************************/
static irqreturn_t rx_irq_handler(int irq, void *dev_id);
static void buffer_push(char data);
static int buffer_pop(char *data);

static enum hrtimer_restart tx_timer_callback(struct hrtimer *timer);
static enum hrtimer_restart rx_timer_callback(struct hrtimer *timer);

static int swuart_hr_open(struct inode* inode, struct file* file);
static int swuart_hr_release(struct inode* inode, struct file* file);
static ssize_t swuart_hr_read(struct file* file, char* buf, size_t len, loff_t* off);
static ssize_t swuart_hr_write(struct file* file, const char* buf, size_t len, loff_t* off);

static int swuart_device_major; 
static struct class *swuart_class; 
static struct device *swuart_device; 
/********************************************************/

/******************** 변수 선언 *************************/
static ktime_t bit_0p5;        //  비트
static ktime_t bit_period;     // 1.0 비트

// tx 변수
#define BUF_SIZE 256
static char msg[BUF_SIZE];
static struct hrtimer tx_timer;
static struct completion tx_done;
static int tx_bit_idx;
static char tx_byte;

// rx 변수
#define BUFFER_FULL ((rx_buffer_head + 1) % BUF_SIZE == rx_buffer_tail)
#define BUFFER_EMPTY (rx_buffer_head == rx_buffer_tail)
static DEFINE_SPINLOCK(rx_lock);
static struct hrtimer rx_timer;
static int irq_number;
static char rx_buffer[BUF_SIZE];               // 수신 데이터 저장 버퍼
static int rx_buffer_head;                     // ring buffer 헤드
static int rx_buffer_tail;                     // ring buffer 테일
static int rx_bit_count;                       // 0~8 (8이면 stop 샘플 타이밍)
static char rx_data;                           // 현재 수신 중인 1바이트
static volatile bool timer_running;            // 타이머 상태 확인
/********************************************************/

static irqreturn_t rx_irq_handler(int irq, void *dev_id) {

        // 스타트 비트 감지
        if (!timer_running) {

                // 프레임 시작 즉시 초기화
                rx_bit_count = 0;
                rx_data = 0;
                timer_running = true;

                // 진행 중 재진입 방지
                disable_irq_nosync(irq_number);

                // 얼마나 지연시키냐가 관건 -> bit0 중앙에서 첫 샘플
                hrtimer_start(&rx_timer, bit_0p5, HRTIMER_MODE_REL);
        }
        
        return IRQ_HANDLED;
}

static void buffer_push(char data) {

        unsigned long flags;
        spin_lock_irqsave(&rx_lock, flags);
        if (BUFFER_FULL) {
                // 가장 오래된 데이터를 덮어쓰기
                rx_buffer_tail = (rx_buffer_tail + 1) % BUF_SIZE;
        }
        rx_buffer[rx_buffer_head] = data;
        rx_buffer_head = (rx_buffer_head + 1) % BUF_SIZE;
        spin_unlock_irqrestore(&rx_lock, flags);
}

static int buffer_pop(char *data) {

        unsigned long flags;        
        int ret;
        spin_lock_irqsave(&rx_lock, flags);
        if (BUFFER_EMPTY) {
                ret = -1;
        } else {
                *data = rx_buffer[rx_buffer_tail];
                rx_buffer[rx_buffer_tail] = 0; // 데이터 초기화
                rx_buffer_tail = (rx_buffer_tail + 1) % BUF_SIZE;
                ret = 0;
        }
        spin_unlock_irqrestore(&rx_lock, flags);
        return ret;
}

static enum hrtimer_restart tx_timer_callback(struct hrtimer *timer) {
        if (tx_bit_idx == -1) {
                GPIO_CLEAR(GPIO_TX);              // Start bit
                // printk(KERN_DEBUG "TX: Start bit\n");
        } else if (tx_bit_idx < 8) {
                if (tx_byte & (1 << tx_bit_idx))
                        GPIO_SET(GPIO_TX);        // Data bit 1
                else
                        GPIO_CLEAR(GPIO_TX);      // Data bit 0
        } else if (tx_bit_idx == 8) {
                GPIO_SET(GPIO_TX);                // Stop bit
        } else {
                complete(&tx_done);
                // printk(KERN_DEBUG "TX: Transmission complete\n");
                return HRTIMER_NORESTART;
        }

        tx_bit_idx++;

        // 위상 고정: 이전 만료시각 기준으로 정확히 1비트 전진
        hrtimer_forward(timer, hrtimer_get_expires(timer), bit_period);
        return HRTIMER_RESTART;
}

static enum hrtimer_restart rx_timer_callback(struct hrtimer *timer) {
        
        int cur_bit;
        cur_bit = GPIO_READ(GPIO_RX);
        
        if (rx_bit_count < 8) {
                rx_data |= (cur_bit << rx_bit_count);
                rx_bit_count++;
                printk(KERN_DEBUG "RX: data bit%d recv%d\n", rx_bit_count, cur_bit);
                
                // 위상 고정: 비트 주기만큼 정확히 전진
                hrtimer_forward(timer, hrtimer_get_expires(timer), bit_period);
                return HRTIMER_RESTART;
        }
        
        // stop 비트 중앙 샘플 타이밍 (바이트 확정)
        if (cur_bit == 1) {
                buffer_push(rx_data);
                printk(KERN_DEBUG "RX: 8bit data RX complete: 0x%02x\n", rx_data);
        } else {
                printk(KERN_DEBUG "RX: framing error (stop=%d), drop 0x%02x\n", cur_bit, rx_data);
        }

        // 프레임 종료
        timer_running = false;
        rx_bit_count = 0;
        rx_data = 0;

        // 다음 프레임을 위한 RX IRQ 재활성화
        enable_irq(irq_number);
        return HRTIMER_NORESTART;
}

 
static int swuart_hr_open(struct inode *inode, struct file *file) {
        try_module_get(THIS_MODULE);       // 모듈 참조 카운트 증가 -> 모듈 언로드 불가
        printk("OPEN - swuart_hr device\n");
        return 0;
}

static int swuart_hr_release(struct inode* inode, struct file* file) {
        module_put(THIS_MODULE);          // 모듈 참조 카운트 감소 -> 모듈 언로드 가능
        printk("CLOSE - swuart_hr device\n");
        return 0;
}

static ssize_t swuart_hr_read(struct file* file, char* buf, size_t len, loff_t* off) {
        ssize_t i = 0;
        char temp_buf[BUF_SIZE];
        char data;
        size_t to_copy = min_t(size_t, len, BUF_SIZE);

        // 바로 pop 시도 (별도 BUFFER_EMPTY 사전 체크 불필요)
        for (; i < to_copy; i++) {
                if (buffer_pop(&data) < 0)
                break;
                temp_buf[i] = data;
        }

        if (i == 0)
                return 0;

        if (copy_to_user(buf, temp_buf, i))
                return -EFAULT;

        return i;
}

static ssize_t swuart_hr_write(struct file* file, const char* buf, size_t len, loff_t* off) {
        size_t i;
        
        if (len > sizeof(msg))
        len = sizeof(msg);

        if (copy_from_user(msg, buf, len)) {
                printk(KERN_ERR "swuart_hr device WRITE: copy_from_user failed\n");
                return -EFAULT;
        }

        for (i = 0; i < len; i++) {
                tx_byte = msg[i];
                tx_bit_idx = -1;
                init_completion(&tx_done);
                
                hrtimer_start(&tx_timer, ns_to_ktime(BIT_DELAY_NS), HRTIMER_MODE_REL);
                // printk(KERN_INFO "TX: hrtimer started for byte 0x%02x\n", tx_byte);
                wait_for_completion(&tx_done);
        }

        printk(KERN_INFO "swuart_hr device WRITE: %zu bytes\n", len);

        return len;
}

static struct file_operations swuart_device_fops = { 
        .owner = THIS_MODULE,              // 모듈 참조 카운트 관리
        .open = swuart_hr_open,
        .release = swuart_hr_release,
        .read = swuart_hr_read,
        .write = swuart_hr_write,    
}; 

static int __init swuart_module_init(void) { 
        
        int ret = 0;
        
        printk(KERN_INFO "START SW UART (HRTIMER BASED) MODULE\n");

        // GPIO 레지스터 매핑
        // gpio_base = (volatile unsigned int*)ioremap(GPIO_BASE, GPIO_SIZE);
        gpio_base = ioremap(GPIO_BASE, GPIO_SIZE);
        if (!gpio_base) {
                printk(KERN_ERR "Failed to map GPIO registers\n");
                return -ENOMEM;
        }
        GPIO_OUT(GPIO_TX);
        GPIO_SET(GPIO_TX);
        GPIO_IN(GPIO_RX);

        // 인터럽트 설정 (falling edge)
        irq_number = gpio_to_irq(32*1+GPIO_RX);   // gpio1_13 핀
        if (irq_number < 0) {
                printk(KERN_ERR "Failed to get IRQ for GPIO_RX %d\n", GPIO_RX);
                ret = irq_number;
                goto err_gpio_reg_mapping;
        }

        ret = request_irq(irq_number, rx_irq_handler, IRQF_TRIGGER_FALLING, "swuart_rx_irq", NULL);
        if (ret) {
                printk(KERN_ERR "Failed to request IRQ %d for GPIO_RX\n", irq_number);
                goto err_gpio_reg_mapping;
        }

        // 캐릭터 디바이스 등록
        swuart_device_major = register_chrdev(0, SWUART_DEVICE_NAME, &swuart_device_fops);
        if (swuart_device_major < 0) {
                printk(KERN_ERR "%s: Failed to get major number\n", SWUART_DEVICE_NAME);
                ret = swuart_device_major;
                goto err_irq;
        }

        // 클래스 생성
        swuart_class = class_create(THIS_MODULE, SWUART_CLASS_NAME);
        if (IS_ERR(swuart_class)) {
                printk(KERN_ERR "%s: Failed to create class\n", SWUART_DEVICE_NAME);
                ret = PTR_ERR(swuart_class);
                goto err_register_chrdev;
        }

        // 디바이스 생성
        swuart_device = device_create(swuart_class, NULL, MKDEV(swuart_device_major, 0), NULL, SWUART_DEVICE_NAME);
        if (IS_ERR(swuart_device)) {
                printk(KERN_ERR "%s: Failed to create device\n", SWUART_DEVICE_NAME);
                ret = PTR_ERR(swuart_device);
                goto err_class;
        }

        // 타이밍 상수 초기화
        bit_0p5 = ns_to_ktime(BIT_DELAY_NS / 2);
        bit_period = ns_to_ktime(BIT_DELAY_NS);

        // 타이머 및 completion 초기화
        hrtimer_init(&tx_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL_PINNED);
        tx_timer.function = tx_timer_callback;
        init_completion(&tx_done);

        hrtimer_init(&rx_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL_PINNED);
        rx_timer.function = rx_timer_callback;

        // RX 버퍼 초기화
        memset(rx_buffer, 0, BUF_SIZE);
        rx_buffer_head = 0;      
        rx_buffer_tail = 0;  
        timer_running = false;

        return 0;

err_class:
        class_destroy(swuart_class);
err_register_chrdev:
        unregister_chrdev(swuart_device_major, SWUART_DEVICE_NAME);
err_irq:
        free_irq(irq_number, NULL);
err_gpio_reg_mapping:
        iounmap(gpio_base);
        return ret;
} 
 
static void __exit swuart_module_exit(void) { 

        hrtimer_cancel(&tx_timer);
        hrtimer_cancel(&rx_timer);
        device_destroy(swuart_class, MKDEV(swuart_device_major, 0));
        class_destroy(swuart_class);
        unregister_chrdev(swuart_device_major, SWUART_DEVICE_NAME);
        free_irq(irq_number, NULL);
        iounmap(gpio_base);

        printk(KERN_INFO "END SW UART (HRTIMER BASED) MODULE\n");
} 
 
module_init(swuart_module_init); 
module_exit(swuart_module_exit); 
 
MODULE_AUTHOR("YJCHOI15(ccyjin15@gmail.com)"); 
MODULE_DESCRIPTION("hrtimer-based software UART driver");
MODULE_LICENSE("GPL v2");