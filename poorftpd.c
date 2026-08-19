#if defined(_WIN32)
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "poorsock.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <string.h>

enum MODE {
    UNKNOWN = 0,
    PASSIVE_MODE = 1,
    ACTIVE_MODE = 2
};

static void *ctrl_session;
static void *pasv_session;
static char file_info[256];
static const char *filename = NULL;
static const char *leafname = NULL;
static unsigned int pasv_port = 60000;

static int reply(const char *msg) {
    fprintf(stderr, ">%s", msg);
    return poor_send(ctrl_session, msg, 0);
}

/* LIST/NLST に応答 (ファイル情報送信) */
static void send_list(int list_mode, void *session) {
    reply("150 Opening ASCII mode data connection for file list\r\n");
    if (filename != NULL) {
        if (list_mode) {
            poor_send(session, file_info, 0);
        } else {
            poor_send(session, leafname, 0);
            poor_send(session, "\r\n", 2);
        }
    }
    poor_close(session);
    reply("226 Transfer complete\r\n");
}

/* ファイル送信 (FTPクライアント側でのダウンロード) */
static void send_file(void *session) {
    FILE *fp;

    reply("150 Opening BINARY mode data connection for file\r\n");
    fp = fopen(filename, "rb");
    if (fp == 0) {
        perror("Cannot open the file");
        reply("426 Connection closed; transfer aborted\r\n");
        return;
    }
    while (!feof(fp)) {
        char buffer[1460];
        int bytesRead = (int)fread(buffer, 1, sizeof buffer, fp);
        if (bytesRead > 0) {
            if (!poor_send(session, buffer, bytesRead)) break;
        }
        if (ferror(fp)) {
            perror("fread()");
            break;
        }
    }
    poor_close(session);
    if (feof(fp)) {
        reply("226 Transfer complete\r\n");
    } else {
        reply("426 Connection closed; transfer aborted\r\n");
    }
    fclose(fp);
}

/* ファイル受信 (FTPクライアント側からのアップロード) */
static void recv_file(void *session, const char *name) {
    FILE *fp;
    char buffer[2048];
    int size;

    reply("150 Opening BINARY mode data connection for file\r\n");

    if (*name == '/' || *name == '\\') name++;
    fp = fopen(name, "wb");
    if (fp == 0) {
        perror("Cannot open the file");
        reply("426 Connection closed; transfer aborted\r\n");
        return;
    }
    while (poor_recv(session, buffer, sizeof buffer, &size) > 0) {
        fwrite(buffer, 1, size, fp);
        if (ferror(fp)) {
            perror("fwrite()");
            reply("426 Connection closed; transfer aborted\r\n");
            return;
        }
    }
    fclose(fp);
    poor_close(session);
    reply("226 Transfer complete\r\n");
}

static int AUTH() {
    reply("534 Request denied for policy reasons\r\n");
    return 1;
}

static int USER() {
    reply("230 User logged in, proceed\r\n");
    return 1;
}

static int TYPE(const char *arg) {
    char reply_str[20];
    if (*arg == '\0') {
        reply("500 Syntax error, command unrecognized\r\n");
    } else {
        snprintf(reply_str, sizeof reply_str, "200 Type set to %c\r\n", arg[0]);
        reply(reply_str);
    }
    return 1;
}

static int CDUP() {
    reply("250 CDUP command successful\r\n");
    return 1;
}

static int PWD() {
    reply("257 \"/\" is current directory\r\n");
    return 1;
}

static int PORT(const char *arg, char *active_addr, int addr_size, uint16_t *active_port, enum MODE *transfer_mode) {
    uint8_t h1, h2, h3, h4, p1, p2;
    int cnt;
    poor_close(pasv_session);
    cnt = sscanf(arg, "%hhu,%hhu,%hhu,%hhu,%hhu,%hhu", &h1, &h2, &h3, &h4, &p1, &p2);
    if (cnt != 6) {
        reply("501 Syntax error in parameters or arguments\r\n");
        return 1;
    }
    snprintf(active_addr, addr_size, "%u.%u.%u.%u", h1, h2, h3, h4);
    *active_port = p1 << 8 | p2;
    *transfer_mode = ACTIVE_MODE;
    reply("200 PORT command successful\r\n");
    return 1;
}

static int PASV(enum MODE *transfer_mode) {
    poor_close(pasv_session);
    if (poor_listen(pasv_session, pasv_port)) {
        uint8_t h1, h2, h3, h4, p1, p2;
        char reply_str[54];
        poor_get_myaddress(ctrl_session, &h1, &h2, &h3, &h4);
        p1 = (uint8_t)(pasv_port >> 8);
        p2 = (uint8_t)(pasv_port & 0xff);
        snprintf(reply_str, sizeof reply_str, "227 Entering Passive Mode (%u,%u,%u,%u,%u,%u)\r\n", h1, h2, h3, h4, p1, p2);
        reply(reply_str);
        if (poor_accept(pasv_session)) {
            *transfer_mode = PASSIVE_MODE;
        } else {
            poor_close(pasv_session);
        }
    }
    return 1;
}

static int LIST(int list_mode, const char *active_addr, uint16_t active_port, enum MODE transfer_mode) {
    switch (transfer_mode) {
        case PASSIVE_MODE:
            send_list(list_mode, pasv_session);
            break;
        case ACTIVE_MODE: {
            void *active_session = poor_create_session();
            if (poor_connect(active_addr, active_port, 20, active_session)) {
                send_list(list_mode, active_session);
            }
            poor_delete_session(active_session);
            break;
        }
        default:
            reply("425 Can't open data connection\r\n");
            break;
    }
    return 1;
}

static int NLST(const char *arg, const char *active_addr, uint16_t active_port, enum MODE transfer_mode) {
    int list_mode = strcmp(arg, "-alL") == 0;
    LIST(list_mode, active_addr, active_port, transfer_mode);
    return 1;
}

static int RETR(const char *active_addr, uint16_t active_port, enum MODE transfer_mode) {
    switch (transfer_mode) {
        case PASSIVE_MODE:
            send_file(pasv_session);
            break;
        case ACTIVE_MODE: {
            void *active_session = poor_create_session();
            if (!poor_connect(active_addr, active_port, 20, active_session)) return 0;
            send_file(active_session);
            poor_delete_session(active_session);
            break;
        }
        default:
            reply("425 Can't open data connection\r\n");
            break;
    }
    return 1;
}

static int STOR(const char *arg, const char *active_addr, uint16_t active_port, enum MODE transfer_mode) {
    if (*arg == '\0') {
        reply("500 Syntax error, command unrecognized\r\n");
    } else {
        switch (transfer_mode) {
            case PASSIVE_MODE:
                recv_file(pasv_session, arg);
                break;
            case ACTIVE_MODE: {
                void *active_session = poor_create_session();
                if (!poor_connect(active_addr, active_port, 20, active_session)) return 0;
                recv_file(active_session, arg);
                poor_delete_session(active_session);
                break;
            }
            default:
                reply("425 Can't open data connection\r\n");
                break;
        }
    }
    return 1;
}

static int QUIT() {
    reply("221 Service closing control connection\r\n");
    return 0;
}

/* FTPコマンド実行 */
static int exec_command(const char *line) {
    static enum MODE transfer_mode = UNKNOWN;
    static char active_addr[16];   /* PORT コマンドで受け取ったアドレス */
    static uint16_t active_port;   /* とポート */

    char cmd[5];
    char arg[256] = "";
    int count;

    fprintf(stderr, "%s\n", line);
    count = sscanf(line, "%4s %255s", cmd, arg);
    if (count < 1) return 1;
    cmd[sizeof cmd - 1] = '\0';
    arg[sizeof arg - 1] = '\0';

    if (strcmp(cmd, "AUTH") == 0) return AUTH();
    if (strcmp(cmd, "USER") == 0) return USER();
    if (strcmp(cmd, "TYPE") == 0) return TYPE(arg);
    if (strcmp(cmd, "CDUP") == 0) return CDUP();
    if (strcmp(cmd, "PWD" ) == 0) return PWD();
    if (strcmp(cmd, "XPWD") == 0) return PWD();
    if (strcmp(cmd, "PORT") == 0) return PORT(arg, active_addr, sizeof active_addr, &active_port, &transfer_mode);
    if (strcmp(cmd, "PASV") == 0) return PASV(&transfer_mode);
    if (strcmp(cmd, "LIST") == 0) return LIST(1, active_addr, active_port, transfer_mode);
    if (strcmp(cmd, "NLST") == 0) return NLST(arg, active_addr, active_port, transfer_mode);
    if (strcmp(cmd, "RETR") == 0) return RETR(active_addr, active_port, transfer_mode);
    if (strcmp(cmd, "STOR") == 0) return STOR(arg, active_addr, active_port, transfer_mode);
    if (strcmp(cmd, "QUIT") == 0) return QUIT();

    reply("502 Command not implemented\r\n");
    return 1;
}

/* FTP制御ポート受信バッファから1文字取得 */
static int read_char(char *data) {
    static char recv_buffer[1024];
    static char *bp = recv_buffer;
    static char *ep = recv_buffer;

    int received_length;
    if (bp == ep) {
        /* 受信バッファが空なら追加受信 */
        if (!poor_recv(ctrl_session, recv_buffer, sizeof recv_buffer, &received_length)) return 0;
        bp = recv_buffer;
        ep = bp + received_length;
    }
    *data = *bp;
    bp++;
    return 1;
}

static void main_loop() {
    char line_buff[256];
    char *lp = line_buff;

    if (!reply("220 poorftpd ready.\r\n")) return;

    for (;;) {
        if (!read_char(lp)) break;
        if (*lp == '\n') {
            *lp = 0;
            lp--;
            if (*lp == '\r') *lp = 0;
            if (!exec_command(line_buff)) break;
            lp = line_buff;
        } else {
            lp++;
            if (lp >= &line_buff[sizeof line_buff]) {
                fprintf(stderr, "Too long.\n");
                break;
            }
        }
    }

    fprintf(stderr, "Disconnected\n");
}

static const char *get_leafname(const char *path) {
    char *leaf = strrchr(path, '\\');
    if (leaf == 0) {
        leaf = strrchr(path, '/');
    }
    if (leaf != 0) {
        return leaf + 1;
    }
    return path;
}

static void build_file_info() {
    int year;
    char month[4];
    int day;
    long size;
    FILE *fp;
    time_t now = time(NULL);
    struct tm *local = localtime(&now);
    strftime(month, 4, "%b", local);
    year = local->tm_year + 1900;
    day = local->tm_mday;

    /* ファイルサイズ取得 */
    fp = fopen(filename, "rb");
    if (fp == 0) {
        perror(filename);
        exit(1);
    }
    fseek(fp, 0, SEEK_END);
    size = ftell(fp);
    fclose(fp);

    snprintf(file_info, sizeof file_info, "-rwxrwxrwx 1 you group %ld %s %d %d %s\r\n", size, month, day, year, leafname);
}

static void usage() {
    fprintf(stderr, "Usage: poorftpd [-p port] [-pp port] [filename]\n");
    exit(1);
}

static int get_argv(int argc, char *argv[], int *i, const char **value) {
    if (argc <= *i) return 0;
    *value = argv[*i];
    (*i)++;
    return 1;
}
int main(int argc, char *argv[]) {
    int listen_port = 21;

    /* コマンドライン引数解析 */
    int i = 1;
    const char *value;
    while (get_argv(argc, argv, &i, &value)) {
        if (strcmp(value, "-p") == 0) {
            /* 待ち受けポート番号 */
            if (!get_argv(argc, argv, &i, &value)) usage();
            listen_port = atoi(value);
            if (listen_port == 0) usage();
        } else if (strcmp(value, "-pp") == 0) {
            /* パッシブモードのポート番号 */
            if (!get_argv(argc, argv, &i, &value)) usage();
            pasv_port = atoi(value);
            if (pasv_port == 0) usage();
        } else {
            /* クライアントに送るファイル名 */
            filename = value;
            leafname = get_leafname(filename);
            build_file_info();
        }
    }

    /* サーバー開始 */
    ctrl_session = poor_create_session();
    pasv_session = poor_create_session();
    if (!poor_init()) return 1;
    if (poor_listen(ctrl_session, listen_port)) {
        if (poor_accept(ctrl_session)) {
            main_loop();
            poor_close(ctrl_session);
        }
    }
    poor_delete_session(ctrl_session);
    poor_delete_session(pasv_session);
    poor_uninit();

    return 0;
}
