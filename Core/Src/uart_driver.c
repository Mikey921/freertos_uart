#include "uart_driver.h"

#define RX_QUEUE_SIZE 100

extern UART_HandleTypeDef huart1;

static struct UART_Device g_stm32_uart1;

void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    struct UART_Data *uart_data = g_stm32_uart1.privdata;
    if (huart == uart_data->handle) {
        /* 释放信号量 */
        xSemaphoreGiveFromISR(uart_data->tx_semaphore, &xHigherPriorityTaskWoken);
        /* 强制上下文切换 */
        portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
    }
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    struct UART_Data *uart_data = g_stm32_uart1.privdata;
    if (huart == uart_data->handle) {
        /* 写队列 */
        xQueueSendFromISR(uart_data->rx_queue, &uart_data->rx_data, &xHigherPriorityTaskWoken);
        /* 再次启动接收 */
        HAL_UART_Receive_IT(huart, &uart_data->rx_data, 1);
        /* 强制上下文切换 */
        portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
    }
}

static int UART_Init(struct UART_Device *pDev, int baud_rate, int data_bits, int stop_bits, int parity)
{
    struct UART_Data *uart_data = pDev->privdata;
    /* 创建二值信号量 */
    uart_data->tx_semaphore = xSemaphoreCreateBinary();
    if (uart_data->tx_semaphore == NULL) return -1;
     /* 创建一个互斥锁,防止多任务同时写 */
     uart_data->tx_mutex = xSemaphoreCreateMutex();
    if (uart_data->tx_mutex == NULL) return -1;
    /* 创建接收队列 */
    uart_data->rx_queue = xQueueCreate(RX_QUEUE_SIZE, sizeof(uint8_t));
    if (uart_data->rx_queue == NULL) return -1;

    /* 启动第一次接收 */
    HAL_UART_Receive_IT(uart_data->handle, &uart_data->rx_data, 1);
    return 0;
}

static int UART_Transmit(struct UART_Device *pDev, const char *data, int length, int timeout)
{
    struct UART_Data *uart_data = pDev->privdata;
    int ret = 0;

    /* 获得互斥锁 */
    if (xSemaphoreTake(uart_data->tx_mutex, timeout) != pdTRUE) {
        return -1;
    }

    xSemaphoreTake(uart_data->tx_semaphore, 0);

    /* 仅开启中断 */
    if (HAL_UART_Transmit_IT(uart_data->handle, (uint8_t *)data, length) != HAL_OK) {
        xSemaphoreGive(uart_data->tx_mutex);
        return -2;
    }

    /* 等待信号量 */
    if (xSemaphoreTake(uart_data->tx_semaphore, timeout) == pdTRUE) {
        ret = 0;
    } else {
        HAL_UART_AbortTransmit(uart_data->handle);
        ret = -3; // 发送超时
    }
    /* 释放互斥锁 */
    xSemaphoreGive(uart_data->tx_mutex);

    return ret;
}

static int UART_Receive(struct UART_Device *pDev, char *data, int timeout)
{
    struct UART_Data *uart_data = pDev->privdata;
    /* 读取队列 */
    if (xQueueReceive(uart_data->rx_queue, data, timeout) != pdPASS) {
        return -1;
    }
    return 0;
}

static struct UART_Data g_stm32_uart1_data = {
    .handle = &huart1,
    .rx_data = 0,
    .rx_queue = NULL,
    .tx_semaphore = NULL,
    .tx_mutex = NULL
};

static struct UART_Device g_stm32_uart1 = {
    .name = "STM32_UART1",
    .Init = UART_Init,
    .Transmit = UART_Transmit,
    .Receive = UART_Receive,
    .privdata = &g_stm32_uart1_data
};

static struct UART_Device *g_uart_devices[] = {
    &g_stm32_uart1
};

struct UART_Device *UART_GetDevice(const char *name)
{
    int count = sizeof(g_uart_devices) / sizeof(g_uart_devices[0]);
    for (int i = 0; i < count; i++) {
        if (strcmp(g_uart_devices[i]->name, name) == 0) {
            return g_uart_devices[i];
        }
    }
    return NULL;
}