#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

#define DEVICE_PATH "/dev/sw_uart_hr"

int main(int argc, char *argv[])
{
    int fd;
    ssize_t bytes_written;
    const char *data = "Hello\n"; // 기본 전송 데이터

    // 명령줄 인자가 있으면 해당 데이터를 사용
    if (argc > 1) {
        data = argv[1];
    }

    // 디바이스 파일 열기
    fd = open(DEVICE_PATH, O_WRONLY);
    if (fd < 0) {
        fprintf(stderr, "Failed to open %s: %s\n", DEVICE_PATH, strerror(errno));
        return EXIT_FAILURE;
    }

    // 데이터 쓰기
    bytes_written = write(fd, data, strlen(data));
    if (bytes_written < 0) {
        fprintf(stderr, "Failed to write to %s: %s\n", DEVICE_PATH, strerror(errno));
        close(fd);
        return EXIT_FAILURE;
    }

    printf("Successfully sent %zd bytes: %s\n", bytes_written, data);

    // 디바이스 파일 닫기
    close(fd);
    return EXIT_SUCCESS;
}