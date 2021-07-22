#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
/* #include <errno.h> */
#include <signal.h>
#include <sys/select.h>

#define SOCKET_PATH "/tmp/mysock"
#define BUF_SIZE 1024

int server_fd = -1;
int client_fd = -1;

void cleanup_and_exit(int sig) {
    if (client_fd != -1) close(client_fd);
    if (server_fd != -1) close(server_fd);
    unlink(SOCKET_PATH); // 删除套接字文件
    printf("\n[Server] Exit.\n");
    exit(0);
}

int main() {
    struct sockaddr_un addr;
    char buf[BUF_SIZE];

    signal(SIGINT, cleanup_and_exit);

    // 创建 Unix 域套接字
    if ((server_fd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
        perror("socket error");
        exit(EXIT_FAILURE);
    }

    unlink(SOCKET_PATH); // 删除旧的 socket 文件

    memset(&addr, 0, sizeof(struct sockaddr_un));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(struct sockaddr_un)) == -1) {
        perror("bind error");
        cleanup_and_exit(0);
    }

    if (listen(server_fd, 5) == -1) {
        perror("listen error");
        cleanup_and_exit(0);
    }

    printf("[Server] Listening on %s ...\n", SOCKET_PATH);

    // 等待客户端连接
    if ((client_fd = accept(server_fd, NULL, NULL)) == -1) {
        perror("accept error");
        cleanup_and_exit(0);
    }
    printf("[Server] Client connected.\n");

    // 主循环：同时监听 stdin 和 client_fd
    while (1) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(client_fd, &readfds);
        FD_SET(STDIN_FILENO, &readfds);

        int max_fd = (client_fd > STDIN_FILENO) ? client_fd : STDIN_FILENO;
        int ret = select(max_fd + 1, &readfds, NULL, NULL, NULL);
        if (ret == -1) {
            perror("select error");
            break;
        }

        // 客户端发来的消息
        if (FD_ISSET(client_fd, &readfds)) {
            ssize_t n = read(client_fd, buf, BUF_SIZE - 1);
            if (n > 0) {
                buf[n] = '\0';
                printf("[Client]: %s\n", buf);
            } else if (n == 0) {
                printf("[Server] Client disconnected.\n");
                break;
            } else {
                perror("read error");
                break;
            }
        }

        // 服务器控制台输入
        if (FD_ISSET(STDIN_FILENO, &readfds)) {
            if (!fgets(buf, BUF_SIZE, stdin)) break;
            size_t len = strlen(buf);
            if (buf[len - 1] == '\n') buf[len - 1] = '\0';

            if (write(client_fd, buf, strlen(buf)) == -1) {
                perror("write error");
                break;
            }
        }
    }

    cleanup_and_exit(0);
}

