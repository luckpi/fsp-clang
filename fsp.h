// Copyright 2022-2022 The SUMEC Authors. All rights reserved.
// 帧分割协议
// Authors: Gumy <gumingyang@sumec.com.cn>
#ifndef _FSP_H
#define _FSP_H

#include "tztype.h"

// malloc空间字节数
#define FSP_MALLOC_SIZE 20480

// FspLoad 模块载入
bool FspLoad(int pipeNum);

// FspAddPipe 增加管道.过期时间单位:ms
bool FspAddPipe(int pipe, int frameLenMax, int expireTime, int fifoCache);

// FspReceive 接收FSP帧
void FspReceive(uint8_t* data, int dataLen, int pipe, uint32_t ip, uint16_t port);

// FspRegisterObserver 注册Fsp观察者
bool FspRegisterObserver(TZPipeDataFunc callback);

// FspBytesToFrame 字节流转FSP帧
// 返回值是FSP帧字节数.返回0表示转换失败.帧存储在dst中
// 如果src长度足够长,可以设置dst等于src,这样转换后的帧会存放在src中
int FspBytesToFrame(uint8_t *src, int srcLen, bool isNeedCrc, uint8_t* dst, int dstSize);

// FspFrameToBytes FSP帧转换为字节流
// 返回值是字节流字节数.返回0表示转换失败.字节流存储在dst中
// 如果不需要src帧,为节约空间,可将dst设置为src的地址
int FspFrameToBytes(uint8_t *src, int srcLen, bool isNeedCrc, uint8_t* dst, int dstSize);

// FspIsFrameValid 是否有效的FSP帧
bool FspIsFrameValid(uint8_t* frame, int len);

#endif
