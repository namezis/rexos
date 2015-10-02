/*
    RExOS - embedded RTOS
    Copyright (c) 2011-2015, Alexey Kramarenko
    All rights reserved.
*/

#ifndef IP_H
#define IP_H

#include <stdint.h>
#include "ipc.h"

#pragma pack(push, 1)

typedef union {
    uint8_t u8[4];
    struct {
        uint32_t ip;
    }u32;
} IP;

#pragma pack(pop)

#define IP_MAKE(a, b, c, d)                             (((a) << 0) | ((b) << 8) | ((c) << 16) | ((d) << 24))

#define LOCALHOST                                       (IP_MAKE(127, 0, 0, 1))

typedef enum {
    IP_SET = IPC_USER,
    IP_GET,
    //notification of ip interface up/down
    IP_UP,
    IP_DOWN
}IP_IPCS;

void ip_print(const IP* ip);
uint16_t ip_checksum(void *buf, unsigned int size);
void ip_set(HANDLE tcpip, const IP* ip);
void ip_get(HANDLE tcpip, IP* ip);

#endif // IP_H