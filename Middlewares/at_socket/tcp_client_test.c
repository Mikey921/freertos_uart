// tcp_client_test.cpp : 此文件包含 "main" 函数。程序执行将在此处开始并结束。
//

#include "at_socket.h"
#include <stdio.h>

#define WIFI_SSID     "CMCC-JyN5"     
#define WIFI_PWD      "13724448209"     
#define SERVER_IP     "192.168.1.100"  
#define SERVER_PORT 8888

/* ./tcp_client_test 127.0.0.1 */
void tcp_client_test_task(void *arg)
{
    //printf("Task Started\n");
    if (at_init("STM32_UART1") != 0) {
        printf("FATAL: AT Init Fail\n");
        while(1) vTaskDelay(1000); // 挂起而不是删除，方便你观察错误
    }

    while (1) {
        int ret = at_connect_ap(WIFI_SSID, WIFI_PWD);
        if (ret == 0) break;
        else printf("Error: %d\n", ret);
        vTaskDelay(1000);
    }

    int iSocketClient;
    struct sockaddr_in tSocketServerAddr;
    int count = 0;
    static char ucSendBuf[64];

    iSocketClient = (int)socket(AF_INET, SOCK_STREAM, 0);

    tSocketServerAddr.sin_family = AF_INET;
    tSocketServerAddr.sin_port = htons(SERVER_PORT);  /* host to net, short */
    int rc = inet_pton(tSocketServerAddr.sin_family, SERVER_IP, &(tSocketServerAddr.sin_addr));
    if (rc <= 0) {
        printf("invalid server_ip\n");
        closesocket(iSocketClient);
        vTaskDelete(NULL);
        return;
    }
    memset(tSocketServerAddr.sin_zero, 0, 8);

    int iRet = connect(iSocketClient, (const struct sockaddr*)&tSocketServerAddr, sizeof(struct sockaddr));
    if (-1 == iRet)
    {
            printf("connect error!\n");
            closesocket(iSocketClient);
            vTaskDelete(NULL);
            return;
    }

    while (1)
    {
        // 自动生成数据
        sprintf(ucSendBuf, "Hello STM32 TCP: %d\n", count++);
        
        // 发送
        int iSendLen = send(iSocketClient, ucSendBuf, strlen(ucSendBuf), 0);
        
        if (iSendLen <= 0) {
            printf("Send failed or connection closed.\n");
            closesocket(iSocketClient);
            break;
        }

        // 延时 1 秒
        vTaskDelay(1000);
    }

    vTaskDelete(NULL);
}
