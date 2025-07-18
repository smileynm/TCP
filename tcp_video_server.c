#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>
#include <string.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#define PORT            7777                // 서버 포트: 7777
#define FRAMEBUFFER_DEVICE  "/dev/fb0"      // 프레임 버퍼: fb0
#define WIDTH               640             // 비디오 프레임 너비 및 높이
#define HEIGHT              480
#define BUFF_SIZE           1024 * 1024     // 1MB 버퍼 (넉넉하게 할당)

// 프레임버퍼의 정보 구조체
static struct fb_var_screeninfo vinfo;

void display_frame(uint16_t *fbp, uint8_t *data, int width, int height) 
{
  // 화면 중앙에 표시하기 위해 전체 값의 2를 나누어 계산
  int x_offset = (vinfo.xres - width) / 2;
  int y_offset = (vinfo.yres - height) / 2;

  // YUYV -> RGB565 변환하여 프레임버퍼에 출력
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; x += 2) {
      uint8_t Y1 = data[(y * width + x) * 2];
      uint8_t U = data[(y * width + x) * 2 + 1];
      uint8_t Y2 = data[(y * width + x + 1) * 2];
      uint8_t V = data[(y * width + x + 1) * 2 + 1];

      int R1 = Y1 + 1.402 * (V - 128);
      int G1 = Y1 - 0.344136 * (U - 128) - 0.714136 * (V - 128);
      int B1 = Y1 + 1.772 * (U - 128);

      int R2 = Y2 + 1.402 * (V - 128);
      int G2 = Y2 - 0.344136 * (U - 128) - 0.714136 * (V - 128);
      int B2 = Y2 + 1.772 * (U - 128);

      // RGB565 포맷으로 변환 (R: 5비트, G: 6비트, B: 5비트)
      uint16_t pixel1 = ((R1 & 0xF8) << 8) | ((G1 & 0xFC) << 3) | (B1 >> 3);
      uint16_t pixel2 = ((R2 & 0xF8) << 8) | ((G2 & 0xFC) << 3) | (B2 >> 3);

      fbp[(y + y_offset) * vinfo.xres + (x + x_offset)] = pixel1;
      fbp[(y + y_offset) * vinfo.xres + (x + x_offset + 1)] = pixel2;
    }
  }
}

int main() {
    int serverSocket;               // 서버 소켓 파일디스크립터 선언
    struct sockaddr_in serverAddr;  // 서버 정보 저장 구조체
    int clientSocket;               // 클라이언트 소켓 파일디스크립터
    struct sockaddr_in clientAddr;  // 클라이언트 정보 저장 구조체
    unsigned int clientAddrLength;  // 클라이언트 주소 구조체 크기
    uint8_t *frameBuffer = NULL;    // 동적 할당될 프레임 데이터 버퍼

    // 프레임버퍼 초기화: O_RDWR
    // 실패 시 종료
    int fb_fd = open(FRAMEBUFFER_DEVICE, O_RDWR);
    if (fb_fd == -1) {
        perror("Error opening framebuffer device");
        exit(1);
    }

    // 프레임 버퍼의 화면 정보를 구조체에 저장
    // 실패 시 종료
    if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &vinfo)) {
        perror("Error reading variable information");
        close(fb_fd);
        exit(1);
    }

    // 프레임 버퍼해상도 설정
    uint32_t fb_width = vinfo.xres;
    uint32_t fb_height = vinfo.yres;

    // 프레임버퍼 전체 크기 계산: 가로 * 세로 * 픽셀당 비트 수 / 8 (2byte)
    uint32_t screensize = fb_width * fb_height * vinfo.bits_per_pixel / 8;

    // mmap 함수를 활용하여 프레임 버퍼 메모리 매핑
    uint16_t *fbp = mmap(0, screensize, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
    if ((intptr_t)fbp == -1) {
        perror("Error mapping framebuffer device to memory");
        close(fb_fd);
        exit(1);
    }

    printf("Server start\n");

    // IPv4(PF_INET) TPC 서버 소켓 생성
    // 실패 시 종료
    if ((serverSocket = socket(PF_INET, SOCK_STREAM, 0)) < 0) {
        perror("socket() failed.");
        return -1;
    }

    // 서버 재시작 시 오류 방지 (SO_REUSEADDR)
    // 실패 시 종료
    int opt = 1;
    if (setsockopt(serverSocket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt() failed.");
        close(serverSocket);
        return -1;
    }

    // 소켓 구조체 초기화 후, 서버 정보 설정
    // IPv4, 모든 네트워크 허용, 서버 포트 번호 설정
    memset(&serverAddr, 0x00, sizeof(serverAddr));
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    serverAddr.sin_port = htons(PORT);

    // bind()를 사용하여 서버 소켓을 지정된 IP와 포트에 바인딩
    // 실패 시 종료
    if (bind(serverSocket, (struct sockaddr *)&serverAddr, sizeof(serverAddr))) {
        perror("bind() failed.");
        close(serverSocket);
        return -2;
    }

    // listen(): 수신 대기 상태 돌입
    // 연결 요청 개수 1개로 제한 (어차피 하나만 할 것이기에)
    // 실패 시 종료
    if (listen(serverSocket, 1) < 0) {
        perror("listen() failed.");
        close(serverSocket);
        return -3;
    }

    printf("Waiting for client connection...\n");

    // 클라이언트의 주소 정보 수신 및 저장
    // 클라이언트의 요청 수락 및 클라이언트 소켓 파일 디스크립터 생성
    clientAddrLength = sizeof(clientAddr);
    clientSocket = accept(serverSocket, (struct sockaddr *)&clientAddr, &clientAddrLength);
    
    // 클라이언트 소켓 생성 실패 시 종료
    if (clientSocket < 0) {
        perror("accept() failed.");
        close(serverSocket);
        return -4;
    }

    printf("Client connected from %s\n", inet_ntoa(clientAddr.sin_addr));
    
    // 프레임 데이터 수신 루프
    while (1) {
        uint32_t frameSize;
        int bytesRead;

        // recv(): 클라이언트가 전송한 프레임 크기 정보 수신(4byte)
        // MSG_WAITALL 플래그로 요청 바이트 수만큼 수신될 때까지 recv를 블록
        // recv() 반환 값이 0이면 연결 오류 출력
        // recv() 반환 값이 음수이면 오류출력 후 while루프 break
        bytesRead = recv(clientSocket, &frameSize, sizeof(frameSize), MSG_WAITALL);
        if (bytesRead <= 0) {
            if (bytesRead == 0) printf("Client disconnected.\n");
            else perror("recv frame size failed");
            break;
        }

        // 수신할 프레임 크기에 맞게 버퍼 동적 할당 또는 재할당
        // 프레임버퍼가 할당되지 않았거나
        // 예상 버퍼 크기보다 큰 경우
        // 프레임버퍼를 해제하고 재 할당
        // 그럼에도 할당이 되지 않으면
        // 오류 출력 후 while루프 break
        // BUFF_SIZE는 최대 예상 크기로 임의설정 (1MB)
        if (frameBuffer == NULL || frameSize > BUFF_SIZE) { 
            free(frameBuffer);
            frameBuffer = (uint8_t *)malloc(frameSize);
            if (frameBuffer == NULL) {
                perror("Failed to allocate frame buffer");
                break;
            }
        }

        // recv() 함수를 다시 사용하여 실제 전송 데이터 수신
        // 위에서 수신하여 확인한 frameSize 크기만큼의 데이터를 frameBuffer에 저장
        // MSG_WAITALL 플래그를 통해 데이터가 모두 수신될 때까지 블록
        // recv() 반환 갑시 0일 경우 오류 출력
        // recv() 반환 값이 음수일 경우 오류 출력 후 루프 break
        bytesRead = recv(clientSocket, frameBuffer, frameSize, MSG_WAITALL);
        if (bytesRead <= 0) {
            if (bytesRead == 0) printf("Client disconnected.\n");
            else perror("recv frame data failed");
            break;
        }

        // 한 루프에 수신된 데이터의 크기를 표시
        printf("Received frame: %d bytes\n", bytesRead);

        // 수신된 YUYV 데이터를 프레임버퍼에 출력
        display_frame(fbp, frameBuffer, WIDTH, HEIGHT);
    }

    if (frameBuffer) free(frameBuffer);
    munmap(fbp, screensize);
    close(fb_fd);
    close(clientSocket);
    close(serverSocket);

    printf("Server ended.\n");
    return 0;
}
