#include <stdio.h>    
#include <unistd.h>  
#include <fcntl.h>    

#define DEVICE_FILE "/dev/sw_uart_hr"  // 디바이스 파일 경로

ssize_t swuart_recieve_data(int fd, char *data, size_t data_len) {
    ssize_t bytes_read;

    // 데이터 읽기
    bytes_read = read(fd, data, data_len);
    if (bytes_read < 0) {
        perror("데이터 읽기 실패");
        return -1;
    }

    return bytes_read;
}

int main() {
    int fd; 
    char msg[256];

    fd = open(DEVICE_FILE, O_RDWR);
    if (fd < 0) {            
        perror("디바이스 파일 열기 실패");  
        return -1;            
    }

    while (1) {  
        ssize_t bytes_read = swuart_recieve_data(fd, msg, sizeof(msg) - 1);
        if (bytes_read > 0) {
            msg[bytes_read] = '\0';

            printf("수신: %s\n", msg);
        }
        else {
            usleep(100000); // 100ms
        }
    }

    close(fd);
    return 0;  
}
