#ifndef UART_DRIVER_H
#define UART_DRIVER_H

#include "main.h"
#include "FreeRTOS.h"
#include "queue.h"
#include "semphr.h"
#include "stdint.h"
#include "string.h"

struct UART_Data {
    UART_HandleTypeDef *handle;
    uint8_t rx_data;
    QueueHandle_t rx_queue;
    SemaphoreHandle_t tx_semaphore;
    SemaphoreHandle_t tx_mutex;
};

struct UART_Device *UART_GetDevice(const char *name);

struct UART_Device {
    char *name;
    int (*Init)(struct UART_Device *pDev, int baud_rate, int data_bits, int stop_bits, int parity);
    int (*Transmit)(struct UART_Device *pDev, const char *data, int length, int timeout);
    int (*Receive)(struct UART_Device *pDev, char *data, int timeout);
    void *privdata;
};

#endif
