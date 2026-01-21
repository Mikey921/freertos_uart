#ifndef UART_DRIVER_H
#define UART_DRIVER_H

#include "main.h"
#include "stdint.h"
#include "string.h"

#define USE_FREERTOS

#ifdef USE_FREERTOS
#include "FreeRTOS.h"
#include "queue.h"
#include "semphr.h"
#endif

typedef enum {
    UART_MODE_POLLING,
    UART_MODE_IT,
    UART_MODE_DMA_IDLE
} UART_Mode_t;

struct UART_Device;

struct UART_Device {
    char *name;
    int (*Init)(struct UART_Device *pDev, int baud_rate, UART_Mode_t mode);
    int (*Transmit)(struct UART_Device *pDev, const void *data, uint16_t length, uint32_t timeout);
    int (*Receive)(struct UART_Device *pDev, uint8_t *data, uint32_t timeout);
    void *privdata;
};

struct UART_Device *UART_GetDevice(const char *name);

void UART_DMA_IdleHandler(struct UART_Device *pDev);

#endif
