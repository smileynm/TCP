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

#define VIDEO_DEVICE        "/dev/video0" // 카메라 장치 경로
#define SERVER_IP           "127.0.0.1"   // 서버 IP 주소
#define SERVER_PORT         7777          // 서버 포트
#define WIDTH               640           // 비디오 프레임 너비 및 높이 설정
#define HEIGHT              480

int main() 
{
  // open()함수를 사용하여 카메라 장치 파일을 O_RDWR 모드로 연다
  // 실패 시 에러 출력 후 종료
  int fd = open(VIDEO_DEVICE, O_RDWR);
  if (fd == -1) {
    perror("Failed to open video device");
    return 1;
  }

  // v4l2 구조체를 통해 비디오 설정 구성
  // 처음 해석이 좀 헷갈렸으나, 구조체 구조를 자세히 뜯어본 결과
  // fmt 유니온이 따로 존재 한다는 것을 깨달았다.
  // 비디오 캡처 타입, 해상도, 픽셀 형식, 필드 설정
  struct v4l2_format fmt;
  fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  fmt.fmt.pix.width = WIDTH;
  fmt.fmt.pix.height = HEIGHT;
  fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
  fmt.fmt.pix.field = V4L2_FIELD_INTERLACED;

  // ioctl(): VIDIOC_S_FMT 명령어 활용하여 카메라에 설정내용 적용
  // 실패 시 종료
  if (ioctl(fd, VIDIOC_S_FMT, &fmt) == -1) {
    perror("Failed to set format");
    close(fd);
    return 1;
  }

  // 버퍼 할당
  // ioctl() 호출 후 드라이버가 설정한 이미지 버퍼 크기를 그대로 사용
  // 버퍼가 제대로 생성되지 않을 경우
  // 오류 출력 후 종료
  uint8_t *buffer = malloc(fmt.fmt.pix.sizeimage);
  if (!buffer) {
    perror("Failed to allocate buffer");
    close(fd);
    return 1;
  }

  // TCP 소켓 생성
  int clientSocket;
  struct sockaddr_in serverAddr;

  // client 소켓 생성: ipv4, tcp, 기본프로토콜
  // 실패 시 buffer 해제 후 종료
  if ((clientSocket = socket(PF_INET, SOCK_STREAM, 0)) < 0) {
    perror("socket() failed");
    free(buffer);
    close(fd);
    return 1;
  }

  // 연결할 server의 주소 정보 설정: IPv4, IP, Port
  memset(&serverAddr, 0x00, sizeof(serverAddr));
  serverAddr.sin_family = AF_INET;
  serverAddr.sin_addr.s_addr = inet_addr(SERVER_IP);
  serverAddr.sin_port = htons(SERVER_PORT);

  // connect(): clientSocket을 serverAddr로 지정된 서버에 연결 시도
  // 실패 시 할당자원 해제 후 종료
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
    // 읽기 실패 시 루프 탈출
    int ret = read(fd, buffer, fmt.fmt.pix.sizeimage);
    if (ret == -1) {
      perror("Failed to read frame");
      break;
    }

    // 프레임 크기 전송 (4바이트 정수: uint32_t)
    // 실패 시 루프 탈출
    uint32_t frameSize = fmt.fmt.pix.sizeimage;
    if (send(clientSocket, &frameSize, sizeof(frameSize), 0) < 0) {
      perror("Failed to send frame size");
      break;
    }

    // 프레임 데이터 전송
    // 실패 시 루프 탈출
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
