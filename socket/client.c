#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>
#include <sys/select.h>

#define SOCKET_PATH "/tmp/mysock"
#define BUF_SIZE 1024

int main() {
    struct sockaddr_un addr;
    char buf[BUF_SIZE];
    int fd;

    if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
        perror("socket error");
        exit(EXIT_FAILURE);
    }

    memset(&addr, 0, sizeof(struct sockaddr_un));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr*)&addr, sizeof(struct sockaddr_un)) == -1) {
        perror("connect error");
        exit(EXIT_FAILURE);
    }

    printf("[Client] Connected to server.\n");

    while (1) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(fd, &readfds);
        FD_SET(STDIN_FILENO, &readfds);

        int max_fd = (fd > STDIN_FILENO) ? fd : STDIN_FILENO;
        int ret = select(max_fd + 1, &readfds, NULL, NULL, NULL);
        if (ret == -1) {
            perror("select error");
            break;
        }

        // 服务器发来的消息
        if (FD_ISSET(fd, &readfds)) {
            ssize_t n = read(fd, buf, BUF_SIZE - 1);
            if (n > 0) {
                buf[n] = '\0';
                printf("[Server]: %s\n", buf);
            } else if (n == 0) {
                printf("[Client] Server closed connection.\n");
                break;
            } else {
                perror("read error");
                break;
            }
        }

        // 客户端输入
        if (FD_ISSET(STDIN_FILENO, &readfds)) {
            if (!fgets(buf, BUF_SIZE, stdin)) break;
            size_t len = strlen(buf);
            if (buf[len - 1] == '\n') buf[len - 1] = '\0';

            if (write(fd, buf, strlen(buf)) == -1) {
                perror("write error");
                break;
            }
        }
    }

    close(fd);
    return 0;
}

