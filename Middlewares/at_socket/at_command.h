#ifndef AT_COMMAND_H
#define AT_COMMAND_H

#include "at_device.h"
#include "stdint.h"

int at_exec_cmd(PAT_Device ptDev, int8_t *cmd, uint8_t *resp, uint32_t max_len, uint32_t *resp_len, uint32_t timeout);

int at_send_datas(PAT_Device ptDev, uint8_t *data, uint32_t data_len, uint32_t timeout);

#endif // AT_COMMAND_H