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

#define PORT            7777
#define FRAMEBUFFER_DEVICE  "/dev/fb0"
#define WIDTH               640
#define HEIGHT              480
#define BUFF_SIZE           1024 * 1024  // 1MB 버퍼 (충분히 큰 크기)

static struct fb_var_screeninfo vinfo;

void display_frame(uint16_t *fbp, uint8_t *data, int width, int height) 
{
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
    int serverSocket;
    struct sockaddr_in serverAddr;
    int clientSocket;
    struct sockaddr_in clientAddr;
    unsigned int clientAddrLength;
    uint8_t *frameBuffer = NULL; // 동적 할당될 프레임 데이터 버퍼

    // 프레임버퍼 초기화
    int fb_fd = open(FRAMEBUFFER_DEVICE, O_RDWR);
    if (fb_fd == -1) {
        perror("Error opening framebuffer device");
        exit(1);
    }

    if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &vinfo)) {
        perror("Error reading variable information");
        close(fb_fd);
        exit(1);
    }

    uint32_t fb_width = vinfo.xres;
    uint32_t fb_height = vinfo.yres;
    uint32_t screensize = fb_width * fb_height * vinfo.bits_per_pixel / 8;
    uint16_t *fbp = mmap(0, screensize, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
    if ((intptr_t)fbp == -1) {
        perror("Error mapping framebuffer device to memory");
        close(fb_fd);
        exit(1);
    }

    printf("Server start\n");

    if ((serverSocket = socket(PF_INET, SOCK_STREAM, 0)) < 0) {
        perror("socket() failed.");
        return -1;
    }

    int opt = 1;
    if (setsockopt(serverSocket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt() failed.");
        close(serverSocket);
        return -1;
    }

    memset(&serverAddr, 0x00, sizeof(serverAddr));
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    serverAddr.sin_port = htons(PORT);

    if (bind(serverSocket, (struct sockaddr *)&serverAddr, sizeof(serverAddr))) {
        perror("bind() failed.");
        close(serverSocket);
        return -2;
    }

    if (listen(serverSocket, 1) < 0) { // QUEUE_LIMIT 1
        perror("listen() failed.");
        close(serverSocket);
        return -3;
    }

    printf("Waiting for client connection...\n");

    clientAddrLength = sizeof(clientAddr);
    clientSocket = accept(serverSocket, (struct sockaddr *)&clientAddr, &clientAddrLength);
    
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

        // 1. 프레임 크기 수신 (4바이트)
        bytesRead = recv(clientSocket, &frameSize, sizeof(frameSize), MSG_WAITALL);
        if (bytesRead <= 0) {
            if (bytesRead == 0) printf("Client disconnected.\n");
            else perror("recv frame size failed");
            break;
        }

        // 수신할 프레임 크기에 맞게 버퍼 동적 할당 또는 재할당
        if (frameBuffer == NULL || frameSize > BUFF_SIZE) { // BUFF_SIZE는 최대 예상 크기로 설정
            free(frameBuffer);
            frameBuffer = (uint8_t *)malloc(frameSize);
            if (frameBuffer == NULL) {
                perror("Failed to allocate frame buffer");
                break;
            }
        }

        // 2. 실제 프레임 데이터 수신
        bytesRead = recv(clientSocket, frameBuffer, frameSize, MSG_WAITALL);
        if (bytesRead <= 0) {
            if (bytesRead == 0) printf("Client disconnected.\n");
            else perror("recv frame data failed");
            break;
        }

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

