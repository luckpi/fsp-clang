// Copyright 2022-2022 The SUMEC Authors. All rights reserved.
// 帧分割协议
// Authors: Gumy <gumingyang@sumec.com.cn>
#include "fsp.h"
#include "pt.h"

#define TAG "fsp"
// tzmalloc字节数
#define MALLOC_TOTAL 4096

#define FSP_LEN 6

static int mid = -1;

static TZDataFunc fspCallback = NULL;

// FspLoad Fsp载入
bool FspLoad(void) {
    mid = TZMallocRegister(0, TAG, MALLOC_TOTAL);
    if (mid == -1) {
        LE(TAG, "load failed!malloc failed");
        return false;
    }
    LI(TAG, "load success");
    return true;
}

// ToolGetTxBytes 读取发送字节流
// 注意:谁调用谁释放空间
TZBufferDynamic *FspGetTxBytes(uint8_t *data, int dataLen, bool isNeedCrc) {
    TZBufferDynamic *buffer = NULL;
    buffer = TZMalloc(mid, sizeof(TZBufferDynamic) + dataLen + FSP_LEN);
    buffer->len = dataLen + FSP_LEN;
    buffer->buf[0] = FSP_FRAME_HEADER >> 8;
    buffer->buf[1] = FSP_FRAME_HEADER & 0xff;
    buffer->buf[2] = dataLen >> 8;
    buffer->buf[3] = dataLen & 0xff;
    if (isNeedCrc == true) {
        int crc = Crc16Checksum(data, dataLen);
        buffer->buf[4] = crc >> 8;
        buffer->buf[5] = crc & 0xff;
    } else {
        buffer->buf[4] = 0;
        buffer->buf[5] = 0;
    }
    memcpy(buffer->buf + FSP_LEN, data, dataLen);
    return buffer;
}

// FspSend 发送数据
bool FspSend(int pipe, uint8_t *data, int dataLen, bool isNeedCrc) {
    if (VSocketIsAllowSend(pipe) == false) {
        LE(TAG, "tx failed!pipe:%d is not allow send", pipe);
        return false;
    }

    TZBufferDynamic *buffer = FspGetTxBytes(data, dataLen, isNeedCrc);
    if (buffer == NULL) {
        LE(TAG, "tx failed!get bytes failed");
        return false;
    }

    VSocketTxParam txParam;
    txParam.Bytes = buffer->buf;
    txParam.Size = buffer->len;
    txParam.Pipe = pipe;
    VSocketTx(&txParam);

    TZFree(buffer);
    return true;
}

// FspReceive Fsp接收
void FspReceive(uint8_t *data, int dataLen) {
    int i = 0;
    static TZBufferDynamic *buffer = NULL;
    static uint16_t crcNum = 0;
    static int remainderLen = 0;

    // 处理剩余的长度
    if (remainderLen > 0) {
        if (remainderLen > dataLen) {
            memcpy(buffer->buf + (buffer->len - remainderLen), data, dataLen);
            remainderLen -= dataLen;
        } else {
            memcpy(buffer->buf + (buffer->len - remainderLen), data, remainderLen);
            remainderLen = 0;
            if (crcNum == 0) {
                fspCallback(buffer->buf, buffer->len);
            } else if (Crc16Checksum(buffer->buf, buffer->len) == crcNum) {
                fspCallback(buffer->buf, buffer->len);
            } else {
                LE(TAG, "crc error");
            }
            TZFree(buffer);
            buffer = NULL;
        }
        return;
    }

    for (; i < dataLen; i++) {
        if (data[i] != (FSP_FRAME_HEADER >> 8)) {
            continue;
        }

        // 包头不完整，直接丢掉
        if (dataLen - i < FSP_LEN) {
            return;
        }

        if (data[i + 1] != (FSP_FRAME_HEADER & 0xff)) {
            continue;
        }

        int bodyLen = dataLen - i - FSP_LEN;
        int Len = (data[i + 2] << 8) | data[i + 3];

        if (Len == 0 || Len > FSP_FRAME_LEN_MAX) {
            continue;
        }

        // crc校验码
        crcNum = (data[i + 4] << 8) | data[i + 5];

        buffer = TZMalloc(mid, sizeof(TZBufferDynamic) + Len);
        buffer->len = Len;
        memcpy(buffer->buf, &data[i + FSP_LEN], bodyLen);

        if (Len > bodyLen) {
            remainderLen = Len - bodyLen;
            return;
        }

        if (crcNum == 0) {
            fspCallback(buffer->buf, buffer->len);
        } else if (Crc16Checksum(buffer->buf, buffer->len) == crcNum) {
            fspCallback(buffer->buf, buffer->len);
        }

        TZFree(buffer);
        buffer = NULL;
    }
}

// FspReceive Fsp接收
void FspReceive(uint8_t *data, int dataLen);

// FspRegisterObserver() 注册Fsp回调函数
void FspRegisterCallback(TZDataFunc callback) {
    fspCallback = callback;
}
