#include "poorsock.h"
#include <stdio.h>
#include <string.h>
#include <malloc.h>

#if defined(_WIN32)

#pragma comment(lib, "ws2_32.lib")
#include <WS2tcpip.h>
#include <windows.h>
#include <ws2def.h>
#include <WinSock2.h>

static void socket_perror(const char *prefix) {
    DWORD error_code = WSAGetLastError();
    LPSTR buff = NULL;
    DWORD size = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL, error_code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPSTR)&buff, 0, NULL
    );
    if (size > 0 && buff != NULL) {
        fprintf(stderr, "%s: 0x%x %s", prefix, error_code, buff);
    } else {
        fprintf(stderr, "%s: 0x%x", prefix, error_code);
    }
    LocalFree(buff);
}

#else

#define INVALID_SOCKET -1
#define SOCKET_ERROR -1
#define SD_BOTH SHUT_RDWR
#define SOCKET int

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>

#define socket_perror(prefix) perror(prefix)
#define closesocket(s) close(s)

#endif

typedef struct session_t {
    SOCKET handle;
    SOCKET listener;
} session_t;

int poor_init() {
#if defined(_WIN32)
    WSADATA wsaData;
    int err = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (err != 0) {
        fprintf(stderr, "WSAStartup failed:0x%x\n", err);
        return 0;
    }
#endif
    return 1;
}

void poor_uninit() {
#if defined(_WIN32)
    WSACleanup();
#endif
}

void *poor_create_session() {
    session_t *session = (session_t *)malloc(sizeof(session_t));
    if (session == NULL) return NULL;
    session->handle = INVALID_SOCKET;
    session->listener = INVALID_SOCKET;
    return session;
}

void poor_delete_session(void *session) {
    free(session);
}

static int reuseaddr_bind(SOCKET sock, uint16_t port) {
    struct sockaddr_in addr;
    int opt = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt)) < 0) {
        socket_perror("setsockopt() failed");
        return 0;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(sock, (struct sockaddr *)&addr, sizeof addr) == SOCKET_ERROR) {
        socket_perror("bind() failed");
        return 0;
    }
    return 1;
}

int poor_listen(void *session, int port) {
    ((session_t *)session)->listener = socket(AF_INET, SOCK_STREAM, 0);
    if (((session_t *)session)->listener == INVALID_SOCKET) {
        socket_perror("socket() failed");
        return 0;
    }

    if (reuseaddr_bind(((session_t *)session)->listener, (uint16_t)port)) {
        if (listen(((session_t *)session)->listener, 1) != SOCKET_ERROR) {
            return 1;
        } else {
            socket_perror("listen() failed");
        }
    }

    closesocket(((session_t *)session)->listener);
    ((session_t *)session)->listener = INVALID_SOCKET;
    return 0;
}

int poor_accept(void *session) {
    ((session_t *)session)->handle = accept(((session_t *)session)->listener, NULL, NULL);
    closesocket(((session_t *)session)->listener);
    ((session_t *)session)->listener = INVALID_SOCKET;
    if (((session_t *)session)->handle != INVALID_SOCKET) return 1;
    socket_perror("accept() failed");
    return 0;
}

int poor_connect(const char *address, uint16_t remote_port, uint16_t local_port, void *session) {
    struct sockaddr_in serv_addr;

    ((session_t *)session)->handle = socket(AF_INET, SOCK_STREAM, 0);
    if (((session_t *)session)->handle == INVALID_SOCKET) {
        socket_perror("socket() failed");
        return 0;
    }

    if (local_port > 0) {
        if (!reuseaddr_bind(((session_t *)session)->handle, local_port)) return 0;
    }

    memset(&serv_addr, 0, sizeof(struct sockaddr_in));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(remote_port);
    inet_pton(AF_INET, address, &serv_addr.sin_addr);
    if (connect(((session_t *)session)->handle, (struct sockaddr *)&serv_addr, sizeof(struct sockaddr_in)) != 0) {
        socket_perror("connect() failed");
        closesocket(((session_t *)session)->handle);
        ((session_t *)session)->handle = INVALID_SOCKET;
        return 0;
    }

    return 1;
}

int poor_recv(void *session, char *buffer, int max_length, int *out_length) {
    *out_length = recv(((session_t *)session)->handle, buffer, max_length, 0);
    if (*out_length > 0) return 1;
    if (*out_length == 0) {
        /* Disconnected */
    } else {
        socket_perror("recv() failed");
    }
    return 0;
}

int poor_send(void *session, const char *buffer, int length) {
    if (length <= 0) {
        length = (int)strlen(buffer);
    }
    if (send(((session_t *)session)->handle, buffer, length, 0) != SOCKET_ERROR) return 1;
    socket_perror("send() failed");
    return 0;
}

void poor_close(void *session) {
    if (((session_t *)session)->handle != INVALID_SOCKET) {
        shutdown(((session_t *)session)->handle, SD_BOTH);
        closesocket(((session_t *)session)->handle);
        ((session_t *)session)->handle = INVALID_SOCKET;
    }
}

int poor_get_myaddress(void *session, uint8_t *h1, uint8_t *h2, uint8_t *h3, uint8_t *h4) {
    struct sockaddr_in addr;
    uint8_t *ip;
    socklen_t addr_len = sizeof(addr);
    if (getsockname(((session_t *)session)->handle, (struct sockaddr *)&addr, &addr_len) == -1) {
        socket_perror("getsockname() failed");
        return 0;
    }
    ip = (uint8_t *)&addr.sin_addr;
    *h1 = ip[0];
    *h2 = ip[1];
    *h3 = ip[2];
    *h4 = ip[3];
    return 1;
}
