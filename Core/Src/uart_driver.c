#include "uart_driver.h"

#define RX_QUEUE_SIZE 128       // 接收队列深度 (字节数)
#define DMA_RX_BUFFER_SIZE 256  // DMA 接收缓冲区大小 (仅 DMA 模式用)

extern UART_HandleTypeDef huart1;
extern UART_HandleTypeDef huart2;

// --- 私有数据结构定义 ---
struct UART_Data {
    UART_HandleTypeDef *handle;
    UART_Mode_t mode;
    uint8_t rx_data;

#ifdef USE_FREERTOS
    // OS 资源
    QueueHandle_t rx_queue;         // 接收队列
    SemaphoreHandle_t tx_semaphore; // 同步：通知发送完成
    SemaphoreHandle_t tx_mutex;     // 互斥：保护发送函数
#endif

    // DMA 模式专用
    uint8_t *dma_rx_buffer;
    uint16_t dma_rx_size;
};

struct UART_Device g_stm32_uart1;

static int UART_Init(struct UART_Device *pDev, int baud_rate, UART_Mode_t mode);

//  实现策略 1: 轮询 (Polling)
static int UART_PollingTransmit(struct UART_Device *pDev, const void *data, uint16_t length, uint32_t timeout)
{
    struct UART_Data *uart_data = (struct UART_Data *)pDev->privdata;

    // 发送数据
    if (HAL_UART_Transmit(uart_data->handle, data, length, timeout) != HAL_OK) {
        return -1;
    }

    return 0;
}

static int UART_PollingReceive(struct UART_Device *pDev, uint8_t *data, uint32_t timeout)
{
    struct UART_Data *uart_data = (struct UART_Data *)pDev->privdata;

    // 接收数据
    if (HAL_UART_Receive(uart_data->handle, data, 1, timeout) != HAL_OK) {
        return -1;
    }

    return 0;
}

#ifdef USE_FREERTOS
//  实现策略 2 & 3: OS 支持 (IT 或 DMA)

// 通用的 OS 发送函数 (支持 IT 和 DMA)
static int UART_Transmit_OS(struct UART_Device *pDev, const void *data, uint16_t length, uint32_t timeout)
{
    struct UART_Data *uart_data = (struct UART_Data *)pDev->privdata;
    int ret = 0;

    if (!uart_data || !uart_data->tx_mutex || !uart_data->tx_semaphore) {
        return -4;
    }

    // 获取互斥锁
    if (xSemaphoreTake(uart_data->tx_mutex, timeout) != pdTRUE) {
        return -1;
    }

    // 清理残留信号量 (防御性编程)
    while(xSemaphoreTake(uart_data->tx_semaphore, 0) == pdTRUE);

    // 仅开启中断
    HAL_StatusTypeDef status = HAL_ERROR;
    if (uart_data->mode == UART_MODE_IT) {
        status = HAL_UART_Transmit_IT(uart_data->handle, data, length);
    } else if (uart_data->mode == UART_MODE_DMA_IDLE) {
        status = HAL_UART_Transmit_DMA(uart_data->handle, data, length);
    }

    if (status != HAL_OK) {
        xSemaphoreGive(uart_data->tx_mutex);
        return -2;
    }

    // 等待信号量
    if (xSemaphoreTake(uart_data->tx_semaphore, timeout) == pdTRUE) {
        ret = 0;
    } else {
        // 超时处理：终止传输
        HAL_UART_AbortTransmit(uart_data->handle);
        ret = -3; 
    }

    // 释放互斥锁
    xSemaphoreGive(uart_data->tx_mutex);
    return ret;
}

// 通用的 OS 接收函数 (从队列读)
static int UART_Receive_OS(struct UART_Device *pDev, uint8_t *data, uint32_t timeout)
{
    struct UART_Data *uart_data = (struct UART_Data *)pDev->privdata;
    // 读取队列
    if (xQueueReceive(uart_data->rx_queue, data, timeout) != pdPASS) {
        return -1;
    }
    return 0;
}
#endif // USE_FREERTOS

//  初始化函数 (多态工厂)
static int UART_Init(struct UART_Device *pDev, int baud_rate, UART_Mode_t mode)
{
    struct UART_Data *uart_data = (struct UART_Data *)pDev->privdata;
    uart_data->mode = mode;

    // 挂载函数指针
    if (mode == UART_MODE_POLLING) {
        pDev->Transmit = UART_PollingTransmit;
        pDev->Receive = UART_PollingReceive;
        return 0;   // 轮询模式无需后续初始化
    }

#ifdef USE_FREERTOS

    // OS 资源创建
    // 创建二值信号量
    if (!uart_data->tx_semaphore) uart_data->tx_semaphore = xSemaphoreCreateBinary();
    // 创建一个互斥锁,防止多任务同时写
    if (!uart_data->tx_mutex) uart_data->tx_mutex = xSemaphoreCreateMutex();
    // 创建接收队列
    if (!uart_data->rx_queue) uart_data->rx_queue = xQueueCreate(RX_QUEUE_SIZE, sizeof(uint8_t));

    if (!uart_data->tx_semaphore || !uart_data->tx_mutex || !uart_data->rx_queue) {
        return -1;
    }

    // 挂载 OS 版本的实现
    pDev->Transmit = UART_Transmit_OS;
    pDev->Receive = UART_Receive_OS;

    if (uart_data->mode == UART_MODE_IT) {
        // IT 模式：开启单字节中断接收
        HAL_UART_Receive_IT(uart_data->handle, &uart_data->rx_data, 1);
    }
    else if (uart_data->mode == UART_MODE_DMA_IDLE) {
        // DMA 模式：分配缓冲区并开启 DMA + IDLE
        static uint8_t dma_buffer[DMA_RX_BUFFER_SIZE];
        uart_data->dma_rx_buffer = dma_buffer;
        uart_data->dma_rx_size = DMA_RX_BUFFER_SIZE;

        // 启动 DMA 搬运到 Buffer
        HAL_UART_Receive_DMA(uart_data->handle, uart_data->dma_rx_buffer, uart_data->dma_rx_size);
        // 开启 IDLE 中断
        __HAL_UART_ENABLE_IT(uart_data->handle, UART_IT_IDLE);
    }
#endif
    return 0;
}

//  中断回调处理
void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
    struct UART_Data *uart_data = (struct UART_Data *)g_stm32_uart1.privdata;
    
    if (huart == uart_data->handle) {
#ifdef USE_FREERTOS
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;
        // 释放信号量
        xSemaphoreGiveFromISR(uart_data->tx_semaphore, &xHigherPriorityTaskWoken);
        // 强制上下文切换
        portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
#endif
    }
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    struct UART_Data *uart_data = (struct UART_Data *)g_stm32_uart1.privdata;
    
    if (huart == uart_data->handle) {
#ifdef USE_FREERTOS
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;
        // 写队列
        xQueueSendFromISR(uart_data->rx_queue, &uart_data->rx_data, &xHigherPriorityTaskWoken);
        // 再次启动接收
        HAL_UART_Receive_IT(huart, &uart_data->rx_data, 1);
        // 强制上下文切换
        portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
#endif
    }
}

// DMA IDLE 处理函数
void UART_DMA_IdleHandler(struct UART_Device *pDev)
{
    struct UART_Data *uart_data = (struct UART_Data *)pDev->privdata;
    UART_HandleTypeDef *huart = uart_data->handle;

    if (__HAL_UART_GET_FLAG(huart, UART_FLAG_IDLE)) {
        __HAL_UART_CLEAR_IDLEFLAG(huart);
        HAL_UART_DMAStop(huart);    // 暂停 DMA 以读取计数

        // 计算接收到的数据长度
        // CNDTR 是递减计数器
        uint16_t remain = __HAL_DMA_GET_COUNTER(huart->hdmarx);
        uint16_t received_len = uart_data->dma_rx_size - remain;

        if (received_len > 0) {
#ifdef USE_FREERTOS
            BaseType_t xHigherPriorityTaskWoken = pdFALSE;
            // 将数据推入队列
            for (int i = 0; i < received_len; i++) {
                xQueueSendFromISR(uart_data->rx_queue, &uart_data->dma_rx_buffer[i], &xHigherPriorityTaskWoken);
            }
            portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
#endif
        }

        // 重启 DMA 准备下一包
        HAL_UART_Receive_DMA(huart, uart_data->dma_rx_buffer, uart_data->dma_rx_size);
    }
}

//  实例定义
static struct UART_Data g_stm32_uart1_data = {
    .handle = &huart1,  // 绑定硬件句柄
    .rx_queue = NULL,
    .tx_semaphore = NULL,
    .tx_mutex = NULL
};

struct UART_Device g_stm32_uart1 = {
    .name = "STM32_UART1",
    .Init = UART_Init,
    .Transmit = NULL,   // 运行时动态绑定
    .Receive = NULL,
    .privdata = &g_stm32_uart1_data
};

static struct UART_Data g_stm32_uart2_data = {
    .handle = &huart2,  // 绑定硬件句柄
    .rx_queue = NULL,
    .tx_semaphore = NULL,
    .tx_mutex = NULL
};

struct UART_Device g_stm32_uart2 = {
    .name = "STM32_UART2",
    .Init = UART_Init,
    .Transmit = NULL,   // 运行时动态绑定
    .Receive = NULL,
    .privdata = &g_stm32_uart2_data
};

static struct UART_Device *g_uart_devices[] = {
    &g_stm32_uart1,
    &g_stm32_uart2
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