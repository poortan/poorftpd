#ifndef _POORSOCK_H
#define _POORSOCK_H

#include <stdint.h>

int poor_init();
void poor_uninit();
void *poor_create_session();
void poor_delete_session(void *session);
int poor_listen(void *session, int port);
int poor_accept(void *session);
int poor_connect(const char *address, uint16_t remote_port, uint16_t local_port, void *session);
int poor_recv(void *session, char *buffer, int max_length, int *out_length);
int poor_send(void *session, const char *buffer, int length);
void poor_close(void *session);
int poor_get_myaddress(void *session, uint8_t *h1, uint8_t *h2, uint8_t *h3, uint8_t *h4);

#endif
