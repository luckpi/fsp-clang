// Copyright 2022-2022 The SUMEC Authors. All rights reserved.
// 帧分割协议
// Authors: Gumy <gumingyang@sumec.com.cn>
#ifndef _FSP_H
#define _FSP_H

#include "async.h"
#include "crc16.h"
#include "lagan.h"
#include "pt.h"
#include "stdbool.h"
#include "stddef.h"
#include "stdint.h"
#include "string.h"
#include "tzfifo.h"
#include "tzlist.h"
#include "tzmalloc.h"
#include "tztype.h"
#include "vsocket.h"

// 帧头部，两个字节
#define FSP_FRAME_HEADER_HIGH 0xC5
#define FSP_FRAME_HEADER_LOW 0x5C

#define FSP_FRAME_LEN_MAX 1600

// FIFO元素数
#define FSP_RX_FIFO_ITEM_SUM 3

// FspLoad Fsp载入
bool FspLoad(void);

// ToolGetTxBytes 读取发送字节流
// 注意:谁调用谁释放空间
TZBufferDynamic *FspGetTxBytes(uint8_t *data, int dataLen, bool isNeedCrc);

// FspReceive Fsp接收
void FspReceive(uint8_t *data, int dataLen);

// FspRegisterObserver() 注册Fsp回调函数
bool FspRegisterObserver(TZDataFunc callback);

#endif
