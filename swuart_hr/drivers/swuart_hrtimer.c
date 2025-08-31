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
static ktime_t bit_period;     // 1.0 비트 주기
static ktime_t bit_1p5;        // 1.5 비트 주기
static ktime_t bit_0p2;        

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
static volatile bool is_rx_active;
/********************************************************/

/**
 * rx_irq_handler() - RX 스타트 비트 검출 ISR
 * @irq:   요청된 인터럽트 번호
 * @dev_id: 공유 IRQ 시 식별용 포인터이나 본 드라이버에서는 사용하지 않음
 *
 * 1) GPIO_RX에서 falling edge가 발생했을 때 호출됨(스타트 비트 시작을 의미)
 * 2) 이미 프레임을 수신 중이 아니면(is_rx_active == false) 수신 상태로 전환
 * 3) 수신 중 재진입을 막기 위해 해당 GPIO IRQ를 disable_irq_nosync()로 비활성화
 * 4) 비트 카운터와 RX 누적 변수 초기화 후, hrtimer를 1.0비트 지연(bit_period)으로
 *    스케줄링하여 다음 샘플링 시점을 예약
 */
static irqreturn_t rx_irq_handler(int irq, void *dev_id) {

        // IDLE 상태일 때만 수신 프로세스 시작
        if (!is_rx_active) {
                printk(KERN_DEBUG "RX: FALLING EDGE (START BIT: %d)\n", GPIO_READ(GPIO_RX));
                is_rx_active = true;

                // 진행 중 재진입 방지
                disable_irq_nosync(irq_number);

                rx_bit_count = 0;
                rx_data = 0;

                // D0 실제 샘플링 시점 = (하강 엣지) + (IRQ latnecy) + (?? 비트)
                // 가장 이상적인 타이밍 기준점은 D0 비트의 정중앙
                // hrtimer_start(&rx_timer, bit_1p5, HRTIMER_MODE_REL);
                hrtimer_start(&rx_timer, bit_period, HRTIMER_MODE_REL);
        }
        
        return IRQ_HANDLED;
}

/**
 * buffer_push() - RX 링버퍼에 1바이트를 push
 * @data:  저장할 1바이트 데이터
 *
 * - spin_lock_irqsave로 RX 버퍼 보호(IRQ 컨텍스트 포함 동시 접근 대비)
 * - 버퍼가 가득 찬 경우 가장 오래된 데이터를 한 칸 버리고(head 덮어쓰기) 진행
 * - 헤드를 한 칸 전진시킴
 */
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

/**
 * buffer_pop() - RX 링버퍼에서 1바이트를 pop
 * @data:  유저에게 반환할 바이트를 저장할 포인터
 *
 * - spin_lock_irqsave로 동시성 보호
 * - 비어 있으면 -1을 반환
 * - 데이터가 있으면 *data에 대입 후 테일을 한 칸 전진
 *
 * 반환:
 * - 0: 성공적으로 1바이트 pop
 * - -1: 버퍼 비어 있음
 */
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

/**
 * tx_timer_callback() - TX 비트 타이머 콜백
 * @timer: hrtimer 포인터
 *
 * - tx_bit_idx 상태에 따라 다음 동작을 수행
 *   - -1: 스타트 비트(LOW) 출력
 *   - 0~7: LSB-first로 데이터 비트 출력
 *   - 8: 스톱 비트(HIGH) 출력
 *   - 9이상: 전송 완료로 간주하여 complete(&tx_done) 호출하고 타이머 중지
 * - hrtimer_forward로 이전 만료 시각 기준 정확히 1비트 주기만큼 전진시켜
 *   위상 드리프트를 방지(지터 누적 최소화 목적)
 *
 * 반환:
 * - HRTIMER_RESTART: 다음 비트를 위해 타이머 재시작
 * - HRTIMER_NORESTART: 바이트 전송 종료
 */
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

/**
 * rx_timer_callback() - RX 비트 샘플링 타이머 콜백
 * @timer: hrtimer 포인터
 *
 * - rx_bit_count 기준으로 8개 데이터 비트를 LSB-first로 수집
 * - 각 비트마다 GPIO_READ(GPIO_RX) 샘플 후 rx_data의 해당 비트에 OR 연산으로 누적
 * - 8비트 수집 완료 후 스톱 비트를 1로 확인하면 buffer_push(rx_data) 수행
 *   스톱 비트가 0이면 프레이밍 에러로 간주하여 폐기
 * - 프레임 종료 시 is_rx_active=false로 전환하고 enable_irq()로 다음 프레임 엣지 검출을
 *   재허용
 *
 * 타이밍:
 * - rx_irq_handler에서 예약한 최초 샘플링 시점 이후, 매 비트마다 bit_period만큼 forward하여
 *   중앙 샘플링에 가깝게 유지하려고 함
 *
 * 반환:
 * - 데이터 비트 수집 중: HRTIMER_RESTART
 * - 프레임 종료: HRTIMER_NORESTART
 */
static enum hrtimer_restart rx_timer_callback(struct hrtimer *timer) {
        
        int cur_bit;
        cur_bit = GPIO_READ(GPIO_RX);
        
        // 데이터 비트 샘플링 (D0 ~ D7)
        if (rx_bit_count < 8) {
                rx_data |= (cur_bit << rx_bit_count);
                rx_bit_count++;
                printk(KERN_DEBUG "RX: data bit%d recv%d\n", rx_bit_count, cur_bit);
                hrtimer_forward(timer, hrtimer_get_expires(timer), bit_period);
                return HRTIMER_RESTART;
        }
        
        // Stop 비트 샘플링 (rx_bit_count == 8)
        if (cur_bit == 1) {
                buffer_push(rx_data);
                printk(KERN_DEBUG "RX: 8bit data RX complete: 0x%02x\n", rx_data);
        } else {
                printk(KERN_DEBUG "RX: framing error (stop=%d), drop 0x%02x\n", cur_bit, rx_data);
        }
        
        // 수신 종료 및 다음 프레임을 위한 초기화
        is_rx_active = false;
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

/**
 * swuart_hr_read() - 사용자 공간으로 수신 바이트를 복사
 * @file: VFS file
 * @buf:  사용자 버퍼 포인터
 * @len:  요청 길이
 * @off:  파일 오프셋(미사용)
 *
 * 동작:
 * - 내부 링버퍼에서 최대 min(len, BUF_SIZE) 바이트까지 pop하여 temp_buf에 모음
 * - 가용 데이터가 0바이트면 즉시 0을 반환
 * - 가용 바이트 수만큼 copy_to_user로 전달
 *
 * 반환:
 * - 전달한 바이트 수
 * - copy_to_user 실패 시 -EFAULT
 */
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

/**
 * swuart_hr_write() - 사용자 공간에서 바이트열을 받아 비트뱅잉으로 송신
 * @file: VFS file
 * @buf:  사용자 버퍼 포인터
 * @len:  전송할 길이
 * @off:  파일 오프셋(미사용)
 *
 * 동작:
 * - 최대 msg 배열 크기만큼만 수용
 * - copy_from_user로 커널 버퍼로 복사
 * - 각 바이트마다:
 *   1) tx_byte에 저장하고 tx_bit_idx=-1로 초기화
 *   2) init_completion(&tx_done) 후 hrtimer로 비트 전송 시작
 *   3) wait_for_completion(&tx_done)로 해당 바이트가 끝날 때까지 블로킹
 *
 * 반환:
 * - 성공 시 전송한 바이트 수 len
 * - copy_from_user 실패 시 -EFAULT
 *
 * 동기 블로킹 전송이므로 긴 버퍼를 쓰면 호출 스레드가 그만큼 오래 대기
 */
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

        
        // 타이머 및 completion 초기화
        hrtimer_init(&tx_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL_PINNED);
        tx_timer.function = tx_timer_callback;
        init_completion(&tx_done);
        
        hrtimer_init(&rx_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL_PINNED);
        rx_timer.function = rx_timer_callback;
        
        // RX 관련 초기화
        memset(rx_buffer, 0, BUF_SIZE);
        rx_buffer_head = 0;      
        rx_buffer_tail = 0;  

        bit_period = ns_to_ktime(BIT_DELAY_NS);
        bit_1p5 = ns_to_ktime(BIT_DELAY_NS * 3 / 2);
        bit_0p2 = ns_to_ktime(BIT_DELAY_NS * 2 / 10);
        is_rx_active = false;

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