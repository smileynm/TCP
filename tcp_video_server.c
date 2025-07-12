#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>
#include <linux/videodev2.h>
#include <string.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#define VIDEO_DEVICE        "/dev/video0"
#define SERVER_IP           "127.0.0.1"  // 서버 IP 주소
#define SERVER_PORT         7777         // 서버 포트
#define WIDTH               640
#define HEIGHT              480

int main() 
{
  int fd = open(VIDEO_DEVICE, O_RDWR);
  if (fd == -1) {
    perror("Failed to open video device");
    return 1;
  }

  struct v4l2_format fmt;
  fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  fmt.fmt.pix.width = WIDTH;
  fmt.fmt.pix.height = HEIGHT;
  fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
  fmt.fmt.pix.field = V4L2_FIELD_INTERLACED;

  if (ioctl(fd, VIDIOC_S_FMT, &fmt) == -1) {
    perror("Failed to set format");
    close(fd);
    return 1;
  }

  // 버퍼 할당
  uint8_t *buffer = malloc(fmt.fmt.pix.sizeimage);
  if (!buffer) {
    perror("Failed to allocate buffer");
    close(fd);
    return 1;
  }

  // TCP 소켓 생성
  int clientSocket;
  struct sockaddr_in serverAddr;

  if ((clientSocket = socket(PF_INET, SOCK_STREAM, 0)) < 0) {
    perror("socket() failed");
    free(buffer);
    close(fd);
    return 1;
  }

  memset(&serverAddr, 0x00, sizeof(serverAddr));
  serverAddr.sin_family = AF_INET;
  serverAddr.sin_addr.s_addr = inet_addr(SERVER_IP);
  serverAddr.sin_port = htons(SERVER_PORT);

  if (connect(clientSocket, (struct sockaddr *)&serverAddr, sizeof(serverAddr)) < 0) {
    perror("connect() failed");
    free(buffer);
    close(fd);
    close(clientSocket);
    return 1;
  }

  printf("Connected to server. Starting video streaming...\n");

  // 프레임 캡처 및 전송 루프
  while (1) {
    // 카메라에서 프레임 읽기
    int ret = read(fd, buffer, fmt.fmt.pix.sizeimage);
    if (ret == -1) {
      perror("Failed to read frame");
      break;
    }

    // 프레임 크기 전송 (4바이트 정수)
    uint32_t frameSize = fmt.fmt.pix.sizeimage;
    if (send(clientSocket, &frameSize, sizeof(frameSize), 0) < 0) {
      perror("Failed to send frame size");
      break;
    }

    // 프레임 데이터 전송
    if (send(clientSocket, buffer, frameSize, 0) < 0) {
      perror("Failed to send frame data");
      break;
    }

    printf("Frame sent: %d bytes\n", frameSize);
    
    // 프레임 레이트 제어 (약 15fps)
    usleep(66666);  // 약 1/15초
  }

  free(buffer);
  close(fd);
  close(clientSocket);

  return 0;
}
