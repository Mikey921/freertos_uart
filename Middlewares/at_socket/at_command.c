#include "at_device.h"
#include "at_socket.h"
#include "at_command.h"
#include "stdio.h"

static void at_reset_resp(PAT_Device ptDev)
{
    if (ptDev) {
        ptDev->resp_len = 0;
        ptDev->resp_line_counts = 0;
        ptDev->resp_status = 1;
    }
}

int at_exec_cmd(PAT_Device ptDev, int8_t *cmd, uint8_t *resp, uint32_t max_len, uint32_t *resp_len, uint32_t timeout)
{
    struct UART_Device *ptUART = ptDev->ptUARTDev;
    int ret = -1;

    // 获得锁
    xSemaphoreTake(ptDev->at_lock, portMAX_DELAY);

    at_reset_resp(ptDev);

    xSemaphoreTake(ptDev->at_resp_sem, 0);

    // 通过串口发送AT命令
    ptUART->Transmit(ptUART, cmd, strlen((char *)cmd), timeout);

    // 等待信号量
    if (xSemaphoreTake(ptDev->at_resp_sem, timeout) == pdTRUE) {
        if (resp != NULL && max_len > 0) {
            uint32_t copy_len = ptDev->resp_len > max_len ? max_len : ptDev->resp_len;

            if (copy_len > 0) {
                memcpy(resp, ptDev->resp, copy_len);
            }

            if (resp_len != NULL) {
                *resp_len = copy_len;
            }
        }
    } else if (resp_len != NULL) {
        *resp_len = 0;
    }

    ret = ptDev->resp_status;
    // 释放锁
    xSemaphoreGive(ptDev->at_lock);

    return ret;
}

int at_send_datas(PAT_Device ptDev, uint8_t *data, uint32_t data_len, uint32_t timeout)
{
    struct UART_Device *ptUARTDev = ptDev->ptUARTDev;
    int ret = 0;
    // 获得锁
    xSemaphoreTake(ptDev->at_lock, portMAX_DELAY);

    // 通过串口发送AT命令
    ptUARTDev->Transmit(ptUARTDev, data, data_len, timeout);

    // 释放锁
    xSemaphoreGive(ptDev->at_lock);

    return ret;
}