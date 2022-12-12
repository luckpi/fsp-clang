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
#include "tztime.h"
#include "tztype.h"
#include "vsocket.h"

// FspLoad Fsp载入
// mallocTotal malloc内存大小
// frameMaxLen 最大帧长
// fifoItemSum fifo元素和
// timeoutM 超时时间, 单位: Ms
bool FspLoad(int mallocTotal, int frameMaxLen, int fifoItemSum, uint64_t timeout);

// ToolGetTxBytes 读取发送字节流
// 注意:谁调用谁释放空间
TZBufferDynamic *FspGetTxBytes(uint8_t *data, int dataLen, bool isNeedCrc);

// FspReceive Fsp接收
void FspReceive(int pipe, uint8_t *data, int dataLen);

// FspRegisterObserver 注册Fsp观察者
bool FspRegisterObserver(TZPipeDataFunc callback);

#endif
