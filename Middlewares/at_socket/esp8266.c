#include <uart_driver.h>
#include <stdio.h>
#include "at_device.h"
#include "at_socket.h"
#include "at_command.h"

static AT_Device g_esp8266_device = {
    .name = "esp8266",
};

static void esp8266_parser(void *arg);

int esp8266_init(char *uart_dev)
{
    g_esp8266_device.ptUARTDev = UART_GetDevice(uart_dev);

    if (g_esp8266_device.ptUARTDev == NULL) {
        printf("Error: UART_GetDevice failed\n");
        return -1;
    }

    g_esp8266_device.ptUARTDev->Init(g_esp8266_device.ptUARTDev, 115200, UART_MODE_DMA_IDLE);


    // 初始化mutex
    g_esp8266_device.at_lock = xSemaphoreCreateMutex();
    if (g_esp8266_device.at_lock == NULL) {
        printf("Error: Mutex creation failed\n");
        return -1;
    }

    //printf("Free heap before sem: %d\n", (int)xPortGetFreeHeapSize());
    // 初始化semaphore
    g_esp8266_device.at_resp_sem = xSemaphoreCreateBinary();
    if (g_esp8266_device.at_resp_sem == NULL) {
        printf("Error: Semaphore creation failed. Free heap: %d\n", (int)xPortGetFreeHeapSize());
        vSemaphoreDelete(g_esp8266_device.at_lock);
        return -1;
    }

    // 创建后台任务
    BaseType_t ret = xTaskCreate(esp8266_parser, "esp8266_parser", AT_PARSER_TASK_STACK, &g_esp8266_device, osPriorityNormal + 1, NULL);
    if (ret != pdPASS) {
        printf("Error: Parser Task creation failed! Code: %ld\n", ret);
        vSemaphoreDelete(g_esp8266_device.at_resp_sem);
        vSemaphoreDelete(g_esp8266_device.at_lock);
        return -1;
    }

    // 复位esp8266
    at_exec_cmd(&g_esp8266_device, (int8_t *)"AT+RST\r\n", NULL, AT_RESP_BUF_SIZE, NULL, AT_TIMEOUT);

    vTaskDelay(2000);

    // 关闭回显
    at_exec_cmd(&g_esp8266_device, (int8_t *)"ATE0\r\n", NULL, AT_RESP_BUF_SIZE, NULL, AT_TIMEOUT);
    

    return 0;
}

PAT_Device get_at_device(void)
{
    return &g_esp8266_device;
}

int esp8266_connect_ap(char *ssid, char *passwd)
{
	PAT_Device ptDevice = get_at_device();
    static char cmd[128];

    // 参数校验
    if (!ssid || !passwd) {
        return -1; // SSID 和密码不能为空
    }

    // 1.设置WiFi模式为STA
    if (at_exec_cmd(ptDevice, (int8_t *)"AT+CWMODE=1\r\n", NULL, AT_RESP_BUF_SIZE, NULL, AT_TIMEOUT) != 0) {
        return -1;
    }

    // 2.连接WiFi
    if (passwd && strlen(passwd) > 0) {
        snprintf(cmd, sizeof(cmd), "AT+CWJAP=\"%s\",\"%s\"\r\n", ssid, passwd);
    } else {
        snprintf(cmd, sizeof(cmd), "AT+CWJAP=\"%s\"\r\n", ssid);
    }

    if (at_exec_cmd(ptDevice, (int8_t *)cmd, NULL, AT_RESP_BUF_SIZE, NULL, 10000) != 0) {
        return -2;
    }

    // 3.查询IP地址
    if (at_exec_cmd(ptDevice, (int8_t *)"AT+CIFSR\r\n", NULL, AT_RESP_BUF_SIZE, NULL, AT_TIMEOUT) != 0) {
        return -3;
    }

    return 0;
}

int esp8266_socket(int domain, int type, int protocol)
{
	int i;
    for (i = 0; i < AT_DEVICE_SOCKETS_NUM; i++) {
        if (g_esp8266_device.sockets[i].used == 0) {
            break;
        }
    }

    if (i >= AT_DEVICE_SOCKETS_NUM) {
        return -1;
    }

    g_esp8266_device.sockets[i].used = 1;
    g_esp8266_device.sockets[i].type = type;

    g_esp8266_device.sockets[i].user_data = (void *)-1;

    if (g_esp8266_device.sockets[i].at_packet_sem == NULL) {
        g_esp8266_device.sockets[i].at_packet_sem = xSemaphoreCreateBinary();
        if (g_esp8266_device.sockets[i].at_packet_sem == NULL) {
            return -1;
        }
    }

    if (g_esp8266_device.sockets[i].recv_queue == NULL) {
        g_esp8266_device.sockets[i].recv_queue = xQueueCreate(AT_RECV_BUF_SIZE, sizeof(uint8_t));
        if (g_esp8266_device.sockets[i].recv_queue == NULL) {
            return -1;
        }
    }

    return i;
}

int esp8266_listen(int socket, int backlog)
{
    PAT_Device ptDevice = get_at_device();
    PAT_Socket ptSocket = &g_esp8266_device.sockets[socket];

	// 执行：AT+CIPMUX=1 使能多连接
    if (at_exec_cmd(ptDevice, (int8_t *)"AT+CIPMUX=1\r\n", NULL, AT_RESP_BUF_SIZE, NULL, AT_TIMEOUT) != 0) {
        return -1;
    }

    // 执行：AT+CIPSERVERMAXCONN=5 允许最大连接数为5
    at_exec_cmd(ptDevice, (int8_t *)"AT+CIPSERVERMAXCONN=5\r\n", NULL, AT_RESP_BUF_SIZE, NULL, AT_TIMEOUT);
    
    // 执行：AT+CIPSERVER=1,888 使能TCP服务器
    int8_t cmdbuf[128];
    struct sockaddr_in *ptAddr = (struct sockaddr_in *)&ptSocket->local;
    uint16_t port = ptAddr->sin_port;
    snprintf((char *)cmdbuf, sizeof(cmdbuf), "AT+CIPSERVER=1,%d\r\n", ntohs(port));
    if (at_exec_cmd(ptDevice, cmdbuf, NULL, AT_RESP_BUF_SIZE, NULL, AT_TIMEOUT) != 0) {
        return -1;
    }

    return 0;
}


static PAT_Socket get_socket_for_hw_socket(int hw_socket)
{
    PAT_Device ptDevice = get_at_device();
    
    for (int i = 0; i < AT_DEVICE_SOCKETS_NUM; i++) {
        if (ptDevice->sockets[i].used && (int)ptDevice->sockets[i].user_data == hw_socket) {
            return &ptDevice->sockets[i];
        }
    }
    return NULL;
}

int esp8266_accept(int socket, struct sockaddr *name, socklen_t *namelen)
{
	PAT_Device ptDevice = get_at_device();
    PAT_Socket ptSocket = &g_esp8266_device.sockets[socket];
    struct sockaddr_in *ptAddr = (struct sockaddr_in *)&ptSocket->local;
    uint16_t server_port = ntohs(ptAddr->sin_port);

    // 执行：AT+CIPSTATUS 查询网络连接信息
    if (at_exec_cmd(ptDevice, (int8_t *)"AT+CIPSTATUS\r\n", NULL, AT_RESP_BUF_SIZE, NULL, AT_TIMEOUT) != 0) {
        return -1;
    }

    char *line = strstr((char *)&ptDevice->resp, "+CIPSTATUS:");

    // 返回值
    // STATUS:<stat>
    // +CIPSTATUS:<link ID>,<type>,<remote IP>,<remote port>,<local port>,<tetype>
    if (line != NULL) {
        //printf("\n--- CIPSTATUS DETECTED ---\n%s\n", line);

        uint16_t hw_socket;
        char type[10];
        char remote_ip[32];
        uint16_t remote_port;
        uint16_t local_port;
        int tetype;

        int parsed = sscanf(line, "+CIPSTATUS:%hu,\"%9[^\"]\",\"%31[^\"]\",%hu,%hu,%d",
                &hw_socket, type, remote_ip, &remote_port, &local_port, &tetype);

        if (parsed == 6) {
            if (get_socket_for_hw_socket(hw_socket) != NULL) {
                return -1;
            }

            if (local_port != server_port) {
                return -1;
            }

            // 记录客户端的信息
            struct sockaddr_in *ptAddr = (struct sockaddr_in *)name;
            ptAddr->sin_family = AF_INET;
            ptAddr->sin_port = htons(remote_port);
            inet_pton(AF_INET, remote_ip, &ptAddr->sin_addr);
            *namelen = sizeof(struct sockaddr_in);

            // 为这个新连入的客户端分配一个软件 Socket
            int sw_socket = esp8266_socket(AF_INET, SOCK_STREAM, 0);
            PAT_Socket ptSwSocket = &g_esp8266_device.sockets[sw_socket];
            ptSwSocket->user_data = (void *)&hw_socket;

            //ptSwSocket->remote = *ptAddr;
            memcpy(&ptSwSocket->remote, ptAddr, sizeof(struct sockaddr));

            ptAddr = (struct sockaddr_in *)&ptSwSocket->local;
            ptAddr->sin_family = AF_INET;
            ptAddr->sin_port = htons(local_port);

            return sw_socket;   // 返回专属这个客户端的 Socket ID
        }
    }

    return -1;
}

static int get_unused_hw_socket(void)
{
    PAT_Device ptDevice = get_at_device();
    int used_hw_sockets[5] = {0};

    // 1.遍历所有已用socket，记录已占用的hw_socket
    for (int i = 0; i < AT_DEVICE_SOCKETS_NUM; i++) {
        if (ptDevice->sockets[i].used) {
            int hw_socket = (int)ptDevice->sockets[i].user_data;
            if (hw_socket >= 0 && hw_socket < 5) {
                used_hw_sockets[hw_socket] = 1;
            }
        }
    }

    // 2.从0~4中找出第一个未使用的hw_socket
    for (int i = 0; i < 5; i++) {
        if (used_hw_sockets[i] == 0) {
            return i;
        }
    }

    return -1;
}

int esp8266_bind(int socket, const struct sockaddr *name, socklen_t namelen)
{
	PAT_Device ptDevice = get_at_device();
    PAT_Socket ptSocket = &ptDevice->sockets[socket];

    ptSocket->local = *name;
    return 0;
}

int esp8266_connect(int socket, const struct sockaddr *name, socklen_t namelen)
{
    PAT_Device ptDevice = get_at_device();
    PAT_Socket ptSocket = &g_esp8266_device.sockets[socket];
    // AT+CIPSTART=<link ID>,<type>,<remote IP>,<remote port>[,<TCP keep alive>]
	// 1.找到一个空闲的link ID
    int link_id = get_unused_hw_socket();
    if (link_id == -1) {
        return -1;
    }

    at_exec_cmd(ptDevice, (int8_t *)"AT+CIPMUX=1\r\n", NULL, AT_RESP_BUF_SIZE, NULL, AT_TIMEOUT);

    // 2.构造AT命令
    char at_cmd[64];
    char ipstr[16];
    struct sockaddr_in *addr = (struct sockaddr_in *)name;
    uint16_t port = ntohs(addr->sin_port);
    ipaddr_to_ipstr(name, ipstr);

    if (ptSocket->type == SOCK_STREAM) {
        snprintf(at_cmd, sizeof(at_cmd), "AT+CIPSTART=%d,\"%s\",\"%s\",%d\r\n", link_id, "TCP", ipstr, port);
    } else if (ptSocket->type == SOCK_DGRAM) {
        snprintf(at_cmd, sizeof(at_cmd), "AT+CIPSTART=%d,\"%s\",\"%s\",%d,8080,2\r\n", link_id, "UDP", ipstr, port);
    }

    static char rx_buf[128];
    uint32_t rx_len = 0;
    memset(rx_buf, 0, sizeof(rx_buf));

    // 3.发送AT命令
    int ret = at_exec_cmd(ptDevice, (int8_t *)at_cmd, (uint8_t *)rx_buf, sizeof(rx_buf)-1, &rx_len, 5000);
    if (ret != 0) {
        printf("CIPSTART Failed! ESP8266 Resp: \n%s\n", rx_buf);
        return -1;
    }

    ptSocket->user_data = (void *)(uintptr_t)link_id;

    return 0;
}

int esp8266_sendto(int socket, const void *data, size_t size, int flags, const struct sockaddr *to, socklen_t tolen)
{
	// 对于TCP连接: AT+CIPSEND=<link ID>,<length>
    // 对于UDP: AT+CIPSEND=[<link ID>,]<length>[,<remote IP>,<remote port>]
    PAT_Device ptDevice = get_at_device();
    PAT_Socket ptSocket = &g_esp8266_device.sockets[socket];
    uint16_t hw_socket = (uint16_t)(uintptr_t)ptSocket->user_data;
    char cmd[128];

    if (ptSocket->type == SOCK_STREAM || (ptSocket->type == SOCK_DGRAM && to == NULL)) {
        snprintf(cmd, sizeof(cmd), "AT+CIPSEND=%d,%d\r\n", hw_socket, (int)size);
    } else if (ptSocket->type == SOCK_DGRAM && to != NULL) {
        struct sockaddr_in *addr = (struct sockaddr_in *)to;
        char ipstr[16];
        uint16_t port = ntohs(addr->sin_port);
        ipaddr_to_ipstr((struct sockaddr *)addr, ipstr);
        snprintf(cmd, sizeof(cmd), "AT+CIPSEND=%d,%d,\"%s\",%d\r\n", hw_socket, (int)size, ipstr, port);
    } else {
        return -1;
    }

    printf("Sending: %s\n", cmd);
    static char rx_buf[128];
    uint32_t rx_len = 0;
    memset(rx_buf, 0, sizeof(rx_buf));

    int ret = at_exec_cmd(ptDevice, (int8_t *)cmd, (uint8_t *)rx_buf, sizeof(rx_buf)-1, &rx_len, AT_TIMEOUT);

    if (ret != 0) {
        printf("CIPSEND Refused! Resp: \n%s\n", rx_buf);
        return -1;
    }

    if (at_send_datas(ptDevice, (uint8_t *)data, size, AT_TIMEOUT) != 0) {
        printf("Data send failed!\n");
        return -1;
    }

    printf("Send Success: %d bytes\n", size);

    return size;
}

int esp8266_recvfrom(int socket, void *mem, size_t len, int flags, struct sockaddr *from, socklen_t *fromlen)
{
    PAT_Socket ptSocket = &g_esp8266_device.sockets[socket];
    uint8_t data;
    size_t recv_len = 0;
    uint8_t *pdata = (uint8_t *)mem;

	 // 1.对于UDP，先发起AT命令来连接
     /*
    if (ptSocket->type == SOCK_DGRAM && from != NULL) {
        if (esp8266_connect(socket, (const struct sockaddr *)from, *fromlen) != 0) {
            return -1;
        }
    }
    */

     // 2.尝试从接收队列读上一次遗留的数据
     while (xQueueReceive(ptSocket->recv_queue, &data, 0) == pdTRUE) {
         pdata[recv_len++] = data;
         if (recv_len >= len) {
             return recv_len;
         }
     }

     if (recv_len > 0) {
         return recv_len;
     }

     // 3.无遗留数据就等待信号量 at_packet_sem
     if (xSemaphoreTake(ptSocket->at_packet_sem, portMAX_DELAY) != pdTRUE) {
         return -1;
     }

     // 4.读队列recv_queue
     while (xQueueReceive(ptSocket->recv_queue, &data, 0) == pdTRUE) {
         pdata[recv_len++] = data;
         if (recv_len >= len) {
             return recv_len;
         }
     }

     if (recv_len > 0) {
         return recv_len;
     }

     return -1;
}

static void esp8266_recv_packet(PAT_Device ptDevice)
{
    struct UART_Device *ptUARTDev = ptDevice->ptUARTDev;
    uint8_t data;
    int hw_socket = 0;
    int len = 0;
    int state = 0; //0:等待hw_socket, 1:等待len, 2:读取data
    int received = 0;
    PAT_Socket ptSocket = NULL;

    //数据格式：“+IPD,<link id>,<len>:<data>”。
    //函数功能：
    // 1.读取串口数据
    // 2.根据 link id 找到 socket，把数据写入 socket 的队列，释放信号量
    while (1) {
        if (ptUARTDev->Receive(ptUARTDev, &data, portMAX_DELAY) != 0) {
            return;
        }

        switch (state) {
            case 0: //等待hw_socket
                if (data == ',') {
                    state = 1;
                    ptSocket = get_socket_for_hw_socket(hw_socket);

                }
                else if (data >= '0' && data <= '9') {
                    hw_socket = hw_socket * 10 + (data - '0');
                }
                else if (data == ':') {
                    state = 3;
                    len = hw_socket;
                    ptSocket = get_socket_for_hw_socket(hw_socket);
                }
                else {
                    return;
                }
                break;

            case 1: //等待len
                if (data == ',') {
                    state = 2;
                    if (len == 0) return;
                }
                else if (data == ':') {
                    state = 3;
                    if (len == 0) return;
                }
                else if (data >= '0' && data <= '9') {
                    len = len * 10 + (data - '0');
                }
                else {
                    return;
                }
                break;

            case 2: 
                if (data == ':') {
                    state = 3;
                }
                break;
            case 3: //读取data
                if (received < len) {
                    if (ptSocket && ptSocket->recv_queue) {
                        xQueueSend(ptSocket->recv_queue, &data, 0);
                    }
                    received++;
                    if (received >= len) {
                        if (ptSocket && ptSocket->at_packet_sem) {
                            xSemaphoreGive(ptSocket->at_packet_sem);
                        }
                        return;
                    }
                    break;
                }
        }
    }
}

static void esp8266_parser(void *arg)
{
    PAT_Device ptDevice = (PAT_Device)arg;
    struct UART_Device *ptUARTDev = ptDevice->ptUARTDev;
    uint8_t data;
    uint8_t line[AT_RECV_BUF_SIZE];
    int32_t len = 0;

    while (1) {
        // 1.读取串口数据
        if (ptUARTDev->Receive(ptUARTDev, &data, portMAX_DELAY) != 0) {
            continue;
        }
        line[len++] = data;
        if (len >= AT_RECV_BUF_SIZE) {
            len = 0;
        }
        line[len] = '\0';

        // 2.解析是否为"网络数据",即"+IPD,"
        // 找到对应的socket
        // 将数据写入socket的接收队列
        if (strstr((const char *)line, "+IPD,")) {
            esp8266_recv_packet(ptDevice);
            len = 0;
            continue;
        }

        // 3.执行到这，读到的数据就是AT命令的回应 (也可能是其他状态信息，如：0,CONNECT)
        //   存储这些多行数据
        //   解析最后的"OK\r\n"或者"ERROR\r\n"
        if (data == '\n') {
            if (ptDevice->resp_len + len < AT_RESP_BUF_SIZE) {
                // 保存当前行到resp数组
                memcpy(&ptDevice->resp[ptDevice->resp_len], line, len);
        
                ptDevice->resp_len += len;

                ptDevice->resp[ptDevice->resp_len] = '\0';

                ptDevice->resp_line_counts++;
            }
            len = 0;
        }

        if (strstr((const char *)line, "OK\r\n") || strstr((const char *)line, "ERROR\r\n")) {
            if (strstr((const char *)line, "OK\r\n")) {
                ptDevice->resp_status = AT_RESP_OK;
            } else {
                ptDevice->resp_status = AT_RESP_ERROR;
            }
            // 释放信号量通知命令完成
            xSemaphoreGive(ptDevice->at_resp_sem);
            len = 0;
        }
    }
}

int esp8266_closesocket(int socket)
{
	PAT_Device ptDevice = get_at_device();
    PAT_Socket ptSocket = &ptDevice->sockets[socket];

    int hw_socket = (int)ptSocket->user_data;

    char cmd[64];
    sprintf(cmd, "AT+CIPCLOSE=%d\r\n", hw_socket);
    if (at_exec_cmd(ptDevice, (int8_t *)cmd, NULL, AT_RESP_BUF_SIZE, NULL, AT_TIMEOUT) != 0) {
        return -1;
    }
    ptSocket->used = 0;
    
    return 0;
}
