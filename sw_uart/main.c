#include <stdio.h>    
#include <unistd.h>  
#include <fcntl.h>    

#define DEVICE_FILE "/dev/led_buttonpush"  // 디바이스 파일 경로 정의

int main() {
    int fd;                   // 디바이스 파일의 파일 디스크립터를 저장할 변수
    char buffer[2];           // 버튼 상태를 저장할 버퍼, 문자열 끝을 고려하여 2바이트로 선언
    ssize_t bytes_read;       // 읽은 바이트 수를 저장할 변수

    // 디바이스 파일을 읽기 전용으로 열기
    fd = open(DEVICE_FILE, O_RDONLY);
    if (fd < 0) {            
        perror("디바이스 파일 열기 실패");  
        return -1;            
    }

    while (1) {               // 무한 루프 시작
        // 디바이스 파일에서 버튼 상태 읽기
        bytes_read = read(fd, buffer, sizeof(buffer));
        if (bytes_read < 0) { // 읽기에 실패한 경우
            perror("디바이스 파일 읽기 실패");  // 에러 메시지 출력
            close(fd);        // 열린 파일 닫기
            return -1;        // 비정상 종료
        }

        // 읽어온 버튼 상태를 출력
        printf("버튼 상태: %s\n", buffer);

        // 간단한 지연을 주기 위해 빈 루프 실행
        for (int delay = 0; delay < 1000000; delay++);
    }

    // 파일 닫기
    close(fd);
    return 0;                 // 프로그램 정상 종료
}
