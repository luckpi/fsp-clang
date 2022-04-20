// Copyright 2022-2022 The SUMEC Authors. All rights reserved.
// 帧分割协议
// Authors: Gumy <gumingyang@sumec.com.cn>
#ifndef _FSP_H
#define _FSP_H

#include "stdint.h"
#include "string.h"
#include "crc16.h"
#include "tztype.h"
#include "lagan.h"
#include "tzmalloc.h"
#include "stddef.h"
#include "vsocket.h"

// 帧头部，两个字节
#define FSP_FRAME_HEADER 0xc55c

#define FSP_FRAME_LEN_MAX 1024

// FspLoad Fsp载入
bool FspLoad(void);

// FspSend 发送数据
bool FspSend(int pipe, uint8_t *data, int dataLen, bool isNeedCrc);

// FspReceive Fsp接收
void FspReceive(uint8_t *data, int dataLen);

// FspRegisterObserver() 注册Fsp回调函数
void FspRegisterCallback(TZDataFunc callback);

#endif
