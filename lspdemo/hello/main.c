#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <fcntl.h>

ssize_t read_full(int fd, void *buf, size_t count) {
    size_t seen = 0;
    while (seen < count) {
        ssize_t r = read(fd, (char*)buf + seen, count - seen);
        if (r < 0) {
            if (errno == EINTR) continue;
            perror("read error");
            return -1;
        }
        if (r == 0) break;
        seen += r;
    }
    return seen;
}

char* read_lsp(int fd, size_t *out_len) {
    char hdr[8192];
    size_t pos = 0;
    int state = 0;
    while (pos + 1 < sizeof(hdr)) {
        char c;
        ssize_t r = read_full(fd, &c, 1);
        if (r != 1) {
            fprintf(stderr, "read_lsp: failed to read header byte, r=%zd\n", r);
            return NULL;
        }
        hdr[pos++] = c;
        if (state == 0 && c == '\r') state = 1;
        else if (state == 1 && c == '\n') state = 2;
        else if (state == 2 && c == '\r') state = 3;
        else if (state == 3 && c == '\n') break;
        else state = 0;
    }
    hdr[pos] = '\0';

    fprintf(stderr, "Received header:\n%s\n", hdr);

    size_t content_length = 0;
    char *saveptr;
    char *line = strtok_r(hdr, "\r\n", &saveptr);
    while (line) {
        if (strncasecmp(line, "Content-Length:", 15) == 0) {
            char *p = line + 15;
            while (*p && isspace((unsigned char)*p)) p++;
            content_length = strtoul(p, NULL, 10);
            break;
        }
        line = strtok_r(NULL, "\r\n", &saveptr);
    }
    if (content_length == 0) {
        fprintf(stderr, "Content-Length is zero or missing\n");
        return NULL;
    }

    char *body = malloc(content_length + 1);
    if (!body) {
        fprintf(stderr, "malloc failed\n");
        return NULL;
    }
    ssize_t r = read_full(fd, body, content_length);
    if (r != (ssize_t)content_length) {
        fprintf(stderr, "read_lsp: failed to read body, read %zd, expected %zu\n", r, content_length);
        free(body);
        return NULL;
    }
    body[content_length] = '\0';

    if (out_len) *out_len = content_length;
    return body;
}

int send_lsp(int fd, const char *json) {
    size_t len = strlen(json);
    char header[128];
    int hlen = snprintf(header, sizeof(header), "Content-Length: %zu\r\n\r\n", len);
    ssize_t w;

    w = write(fd, header, hlen);
    if (w != hlen) {
        perror("write header");
        return -1;
    }
    w = write(fd, json, len);
    if (w != (ssize_t)len) {
        perror("write json");
        return -1;
    }
    fprintf(stderr, "Sent message:\n%s\n", json);
    return 0;
}

char* json_escape(const char *str) {
    size_t len = strlen(str);
    char *buf = malloc(len * 6 + 1);
    if (!buf) return NULL;

    char *p = buf;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = str[i];
        switch (c) {
            case '"':  strcpy(p, "\\\""); p += 2; break;
            case '\\': strcpy(p, "\\\\"); p += 2; break;
            case '\b': strcpy(p, "\\b");  p += 2; break;
            case '\f': strcpy(p, "\\f");  p += 2; break;
            case '\n': strcpy(p, "\\n");  p += 2; break;
            case '\r': strcpy(p, "\\r");  p += 2; break;
            case '\t': strcpy(p, "\\t");  p += 2; break;
            default:
                if (c < 32 || c == 127) {
                    sprintf(p, "\\u%04x", c);
                    p += 6;
                } else {
                    *p++ = c;
                }
        }
    }
    *p = '\0';
    return buf;
}

int main() {
    // 获取当前目录及文件路径
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd))) {
        perror("getcwd");
        return 1;
    }
    char filepath[PATH_MAX];
    snprintf(filepath, sizeof(filepath), "%s/hello.c", cwd);

    // 构造根目录 URI（目录要以斜杠结尾）
    char root_uri[PATH_MAX * 3];
    snprintf(root_uri, sizeof(root_uri), "file://%s/", cwd);

    // 构造文件 URI
    char file_uri[PATH_MAX * 3];
    snprintf(file_uri, sizeof(file_uri), "file://%s", filepath);

    fprintf(stderr, "Project root URI: %s\n", root_uri);
    fprintf(stderr, "File URI: %s\n", file_uri);

    // 读取文件内容
    FILE *f = fopen(filepath, "rb");
    if (!f) {
        perror("fopen");
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long flen = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *source = malloc(flen + 1);
    if (!source) {
        fprintf(stderr, "malloc source failed\n");
        fclose(f);
        return 1;
    }
    if (fread(source, 1, flen, f) != (size_t)flen) {
        fprintf(stderr, "fread failed\n");
        free(source);
        fclose(f);
        return 1;
    }
    source[flen] = '\0';
    fclose(f);

    char *escaped_source = json_escape(source);
    free(source);
    if (!escaped_source) {
        fprintf(stderr, "json_escape failed\n");
        return 1;
    }

    // 创建管道
    int to_child[2], from_child[2];
    if (pipe(to_child) < 0 || pipe(from_child) < 0) {
        perror("pipe");
        free(escaped_source);
        return 1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        free(escaped_source);
        return 1;
    }

    if (pid == 0) {
        // 子进程启动 clangd
        close(to_child[1]);
        close(from_child[0]);
        dup2(to_child[0], STDIN_FILENO);
        dup2(from_child[1], STDOUT_FILENO);
        dup2(from_child[1], STDERR_FILENO);
        close(to_child[0]);
        close(from_child[1]);
        execlp("clangd", "clangd", NULL, NULL);
        perror("execlp clangd");
        _exit(1);
    }

    // 父进程关闭无用端
    close(to_child[0]);
    close(from_child[1]);
    int write_fd = to_child[1];
    int read_fd = from_child[0];

    // 1. 发送 initialize 请求
    char init_msg[1024];
    snprintf(init_msg, sizeof(init_msg),
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{"
        "\"processId\":null,\"rootUri\":\"%s\",\"capabilities\":{}}}", root_uri);

    if (send_lsp(write_fd, init_msg) < 0) {
        fprintf(stderr, "send initialize failed\n");
        free(escaped_source);
        return 1;
    }

    char *resp = read_lsp(read_fd, NULL);
    if (resp) {
        fprintf(stderr, "initialize response:\n%s\n", resp);
        free(resp);
    } else {
        fprintf(stderr, "No response for initialize\n");
    }

    // 2. 发送 initialized 通知
    const char *initialized = "{\"jsonrpc\":\"2.0\",\"method\":\"initialized\",\"params\":{}}";
    if (send_lsp(write_fd, initialized) < 0) {
        fprintf(stderr, "send initialized failed\n");
        free(escaped_source);
        return 1;
    }

    // 3. 发送 didOpen 通知
    char didopen[8192];
    snprintf(didopen, sizeof(didopen),
        "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\",\"params\":{"
        "\"textDocument\":{\"uri\":\"%s\",\"languageId\":\"c\",\"version\":1,\"text\":\"%s\"}}}",
        file_uri, escaped_source);
    free(escaped_source);

    if (send_lsp(write_fd, didopen) < 0) {
        fprintf(stderr, "send didOpen failed\n");
        return 1;
    }

    resp = read_lsp(read_fd, NULL);
    if (resp) {
        fprintf(stderr, "diagnostics or didOpen response:\n%s\n", resp);
        free(resp);
    } else {
        fprintf(stderr, "No response for didOpen\n");
    }

    // 4. 发送 completion 请求，第3行第5字符（从0开始计数）
    char completion_msg[1024];
    snprintf(completion_msg, sizeof(completion_msg),
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"textDocument/completion\",\"params\":{"
        "\"textDocument\":{\"uri\":\"%s\"},\"position\":{\"line\":3,\"character\":4},\"context\":{\"triggerKind\":1}}}",
        file_uri);

    if (send_lsp(write_fd, completion_msg) < 0) {
        fprintf(stderr, "send completion failed\n");
        return 1;
    }

    resp = read_lsp(read_fd, NULL);
    if (resp) {
        fprintf(stderr, "completion response:\n%s\n", resp);
        free(resp);
    } else {
        fprintf(stderr, "No response for completion\n");
    }

    // 5. shutdown & exit
    const char *shutdown = "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"shutdown\",\"params\":null}";
    send_lsp(write_fd, shutdown);
    resp = read_lsp(read_fd, NULL);
    if (resp) free(resp);

    const char *exitmsg = "{\"jsonrpc\":\"2.0\",\"method\":\"exit\",\"params\":null}";
    send_lsp(write_fd, exitmsg);

    waitpid(pid, NULL, 0);
    close(write_fd);
    close(read_fd);

    return 0;
}

