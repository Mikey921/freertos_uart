#ifndef UART_DRIVER_H
#define UART_DRIVER_H

#include "main.h"
#include "stdint.h"
#include "string.h"

//若为裸机环境，请注释下面这行
#define USE_FREERTOS

#ifdef USE_FREERTOS
#include "FreeRTOS.h"
#include "queue.h"
#include "semphr.h"
#endif

/**
 * @brief UART 工作模式
 */
typedef enum {
    UART_MODE_POLLING,
    UART_MODE_IT,
    UART_MODE_DMA_IDLE
} UART_Mode_t;

struct UART_Device;

/**
 * @brief UART 设备对象接口 (公开)
 */
struct UART_Device {
    char *name;     // 设备名称，如 "STM32_UART1"
    /**
     * @brief 初始化函数
     * @param mode 指定工作模式 (POLLING / IT / DMA_IDLE)
     */
    int (*Init)(struct UART_Device *pDev, int baud_rate, UART_Mode_t mode);

    /**
     * @brief 发送数据
     * @param timeout 超时时间 (FreeRTOS Ticks 或 HAL Delay)
     */
    int (*Transmit)(struct UART_Device *pDev, const void *data, uint16_t length, uint32_t timeout);

    /**
     * @brief 接收数据 (从队列读取)
     * @note 仅在 IT 和 DMA 模式下有效，Polling 模式请自行实现或直接调 HAL
     */
    int (*Receive)(struct UART_Device *pDev, uint8_t *data, uint32_t timeout);

    void *privdata; // 私有数据指针 (外部无需关心)
};

// --- 公开 API ---
/**
 * @brief 通过名称获取设备句柄
 */
struct UART_Device *UART_GetDevice(const char *name);

/**
 * @brief 专门处理 IDLE 中断的 Handler
 * @note 需要在 stm32f1xx_it.c 的 USARTx_IRQHandler 中调用
 */
void UART_DMA_IdleHandler(struct UART_Device *pDev);

#endif
