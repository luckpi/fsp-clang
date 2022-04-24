// Copyright 2022-2022 The SUMEC Authors. All rights reserved.
// 帧分割协议
// Authors: Gumy <gumingyang@sumec.com.cn>
#include "fsp.h"
#include "pt.h"

#define TAG "fsp"
// tzmalloc字节数
#define MALLOC_TOTAL 4096

#define FSP_LEN 6

typedef struct {
    int len;
    uint8_t buf[FSP_FRAME_LEN_MAX];
} tBuffer;

typedef enum {
    HEADERHIGH,
    HEADERLOW,
    LENHIGH,
    LENLOW,
    CRCHIGH,
    CRCLOW,
    DATACPY,
    DATACRC,
} FspState;

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
    static tBuffer buffer;
    static int len = 0;
    static uint16_t crcNum = 0;
    static int remainderLen = 0;
    static FspState fspState;

    for (; i < dataLen;) {
        switch (fspState) {
        case HEADERHIGH:
            if (data[i] == (FSP_FRAME_HEADER >> 8)) {
                fspState = HEADERLOW;
            }
            i++;
            break;
        case HEADERLOW:
            if (data[i] == (FSP_FRAME_HEADER & 0xff)) {
                fspState = LENHIGH;
                i++;
            } else {
                fspState = HEADERHIGH;
            }
            break;
        case LENHIGH:
            len = data[i] << 8;
            if (len > FSP_FRAME_LEN_MAX) {
                fspState = HEADERHIGH;
            } else {
                fspState = LENLOW;
                i++;
            }
            break;
        case LENLOW:
            len |= data[i];
            if (len > FSP_FRAME_LEN_MAX || len == 0) {
                fspState = HEADERHIGH;
            } else {
                buffer.len = len;
                fspState = CRCHIGH;
                i++;
            }
            break;
        case CRCHIGH:
            crcNum = data[i] << 8;
            fspState = CRCLOW;
            i++;
            break;
        case CRCLOW:
            crcNum |= data[i];
            fspState = DATACPY;
            i++;
            break;
        case DATACPY: {
            int bodyLen = dataLen - i;
            // 单帧数据拼接
            if (remainderLen > 0) {
                if (remainderLen > bodyLen) {
                    memcpy(buffer.buf + (buffer.len - remainderLen), &data[i], bodyLen);
                    remainderLen -= bodyLen;
                    return;
                } else {
                    memcpy(buffer.buf + (buffer.len - remainderLen), &data[i], remainderLen);
                    remainderLen = 0;
                    fspState = DATACRC;
                    i += remainderLen;
                }
            } else if (len > bodyLen) {
                remainderLen = len - bodyLen;
                memcpy(buffer.buf, &data[i], bodyLen);
                return;
            } else {
                memcpy(buffer.buf, &data[i], len);
                i += len;
                fspState = DATACRC;
            }
        }
        case DATACRC:
            if (crcNum == 0x0) {
                fspCallback(buffer.buf, buffer.len);
            } else if (crcNum == Crc16Checksum(buffer.buf, buffer.len)) {
                fspCallback(buffer.buf, buffer.len);
            }
            fspState = HEADERHIGH;
            break;
        default:
            break;
        }
    }
}

// FspReceive Fsp接收
void FspReceive(uint8_t *data, int dataLen);

// FspRegisterObserver() 注册Fsp回调函数
void FspRegisterCallback(TZDataFunc callback) {
    fspCallback = callback;
}
