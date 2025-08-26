#include <linux/module.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/device.h>

#define GPIO_MAJOR   245
#define GPIO_MINOR   0
#define GPIO_DEVICE   "led_buttonpush"
#define GPIO_BUTTON 12
#define GPIO_LED    13

#define GPIO_BASE   0x4804C000
#define GPIO_END   0x4804CFFF
#define GPIO_SIZE   GPIO_END-GPIO_BASE

#define GPIO_DATAIN     0x138
#define GPIO_DATAOUT    0x13C
#define GPIO_OE_OFF      0x134
#define GPIO_SET_OFF   0x194
#define GPIO_CLEAR_OFF   0x190


#define GPIO_IN(g)      ((*(gpio+GPIO_OE_OFF/4)) |= (1<<g))
#define GPIO_OUT(g)      ((*(gpio+GPIO_OE_OFF/4)) &= ~(1<<g))
#define GPIO_SET(g)      ((*(gpio+GPIO_SET_OFF/4)) |= (1<<g))
#define GPIO_CLEAR(g)   ((*(gpio+GPIO_CLEAR_OFF/4)) |= (1<<g))

static int gpio_open(struct inode* inode, struct file* file);
static int gpio_release(struct inode* inode, struct file* file);
static ssize_t gpio_read(struct file* file, char* buf, size_t len, loff_t* off);
static ssize_t gpio_write(struct file* file, const char* buf, size_t len, loff_t* off);

volatile unsigned* gpio;
static char msg[BLOCK_SIZE] = {0};
struct cdev gpio_cdev;
static struct class *gpio_class;    // ********** udev용 class 구조체
static struct device *gpio_device;  // ********** udev용 device 구조체

static struct file_operations gpio_fop={
   .owner = THIS_MODULE,
   .open = gpio_open,
   .release = gpio_release,
   .read = gpio_read,
   .write = gpio_write,
};

int start_module(void){
   unsigned int cnt = 1;             // 관리할 장치의 개수를 1로 설정
   static void* map;                 // 메모리 매핑된 주소를 저장할 포인터 map을 선언
   int add;                          // cdev_add 함수의 반환 값을 저장할 변수 add
   dev_t devno;                      // 장치 번호를 저장할 변수 devno

   printk(KERN_INFO "START MODULE\n"); // 커널 로그에 "START MODULE" 메시지를 출력

   devno = MKDEV(0, GPIO_MINOR); // ****** 메이저 번호 0으로
   register_chrdev_region(devno, 1, GPIO_DEVICE); // 생성된 장치 번호를 커널에 등록하여 사용 가능하도록 설정

   cdev_init(&gpio_cdev, &gpio_fop); // 캐릭터 디바이스 구조체 gpio_cdev 초기화하고 file_operations 구조체 연결
   gpio_cdev.owner = THIS_MODULE;     // 이 캐릭터 디바이스가 현재 모듈에 속함을 지정

   add = cdev_add(&gpio_cdev, devno, cnt); // 장치 번호 devno로 gpio_cdev를 커널에 등록

   // ★ udev class/device 등록
   gpio_class = class_create(THIS_MODULE, "gpio_class");
   if (IS_ERR(gpio_class)) {
      printk(KERN_ERR "class_create failed\n");
      cdev_del(&gpio_cdev);
      unregister_chrdev_region(devno, 1);
      return PTR_ERR(gpio_class);
   }

   gpio_device = device_create(gpio_class, NULL, devno, NULL, GPIO_DEVICE);
   if (IS_ERR(gpio_device)) {
      printk(KERN_ERR "device_create failed\n");
      class_destroy(gpio_class);
      cdev_del(&gpio_cdev);
      unregister_chrdev_region(devno, 1);
      return PTR_ERR(gpio_device);
   }

   map = ioremap(GPIO_BASE, GPIO_SIZE);    // GPIO 레지스터의 물리 주소를 가상 주소로 매핑하여 map에 저장
   gpio = (volatile unsigned int*)map;     // map을 gpio로 캐스팅하여 GPIO 레지스터에 접근할 수 있게 설정

   GPIO_IN(GPIO_BUTTON); // GPIO_BUTTON 핀을 입력 모드로 설정
   GPIO_OUT(GPIO_LED);   // GPIO_LED 핀을 출력 모드로 설정

   return 0;           
}

void end_module(void){
   dev_t devno = MKDEV(GPIO_MAJOR, GPIO_MINOR);

   device_destroy(gpio_class, devno);   // ★ udev 노드 삭제
   class_destroy(gpio_class);           // ★ class 해제

   unregister_chrdev_region(devno, 1);
   cdev_del(&gpio_cdev);

   if(gpio){
      iounmap(gpio);
   }
   printk(KERN_INFO "END MODUILE\n");
}

static int gpio_open(struct inode* inode, struct file* file){
   try_module_get(THIS_MODULE);
   printk("OPEN - gpio device\n");
   return 0;
}

static int gpio_release(struct inode* inode, struct file* file){
   module_put(THIS_MODULE);
   printk("CLOSE - gpio device\n");
   return 0;
}

static ssize_t gpio_read(struct file* file, char* buf, size_t len, loff_t* off){
   unsigned int value;              // GPIO 데이터 레지스터 값을 저장할 변수 선언
   char button_status[2];           // 버튼 상태를 문자열로 저장할 배열 선언
   int cnt;                         // copy_to_user 함수의 반환 값을 저장할 변수

   value = *(gpio + GPIO_DATAIN/4); // GPIO 데이터 입력 레지스터(GPIO_DATAIN)에서 현재 값 읽기

   if (value & (1 << GPIO_BUTTON)) { // GPIO_BUTTON에 해당하는 비트가 1인지 확인
       GPIO_CLEAR(GPIO_LED);          // 버튼이 눌린 상태이면 LED를 끄기
       strcpy(button_status, "0");    // 버튼 상태를 "0"으로 설정 (버튼이 눌림을 의미)
   } else {
       GPIO_SET(GPIO_LED);            // 버튼이 눌리지 않은 상태이면 LED를 켜기
       strcpy(button_status, "1");    // 버튼 상태를 "1"로 설정 (버튼이 눌리지 않음을 의미)
   }

   cnt = copy_to_user(buf, button_status, strlen(button_status)+1); // 유저 공간으로 버튼 상태 전달
   printk("GPIO device READ: Button status: %s\n", button_status); // 버튼 상태를 커널 로그에 출력

   return cnt;                       // copy_to_user 함수의 반환 값(복사하지 못한 바이트 수)을 반환
}


static ssize_t gpio_write(struct file* file, const char* buf, size_t len, loff_t* off){
   short cnt;
   memset(msg, 0, BLOCK_SIZE);
   cnt = copy_from_user(msg, buf, len);
   strcmp(msg, "0") ? GPIO_SET(GPIO_LED) : GPIO_CLEAR(GPIO_LED);
   printk("gpio device WRITE: %s \n", msg);
   return cnt;
}

MODULE_LICENSE("GPL");
module_init(start_module);
module_exit(end_module);