// Copyright 2022-2022 The SUMEC Authors. All rights reserved.
// 帧分割协议
// Authors: Gumy <gumingyang@sumec.com.cn>
#include "fsp.h"

#define TAG "fsp"

// 帧头部，两个字节
#define FSP_FRAME_HEADER_HIGH 0xC5
#define FSP_FRAME_HEADER_LOW 0x5C

#define FSP_LEN 6

typedef struct
{
    TZPipeDataFunc callback;
} tItem;

typedef enum {
    HEADER_HIGH,
    HEADER_LOW,
    LEN_HIGH,
    LEN_LOW,
    DATA_CPY,
} tFspState;

typedef struct {
    uint64_t rxTime;
    int pipe;
} tRxTag;

typedef struct {
    uint64_t rxTime;
    int pipe;
    tFspState state;
    int framelen;
    int dataCpyOffsetAddr;
    TZBufferDynamic *gBuffer;
} tFspParam;

static int mid = -1;

// 观察者列表
static intptr_t gObserverList = 0;

// 参数列表
static intptr_t gParamList = 0;

// 接收fifo
static intptr_t rxFifo;
static TZBufferDynamic *rxBuffer;

// 接收最大长度
static int gFrameMaxLen = 0;

static uint64_t gTimeout = 0;

static int task(void);
static bool fspDataCrc(uint16_t crcNum, uint8_t *data, int size);
static void notifyObserver(uint8_t *bytes, int size, int pipe);
static bool isExistObserver(TZPipeDataFunc callback);
static void fspRun(uint8_t *data, int dataLen, tFspParam *param);
static bool rxFifoCreate(int itemSum, int itemSize);
static tFspParam *pipeParamGet(int pipe);
static tFspParam *pipeParamCreate(int pipe);
static void fspTimeOutCheck(tFspParam *param, uint64_t timestamp);

// FspLoad Fsp载入
// mallocTotal malloc内存大小
// frameMaxLen 最大帧长
// fifoItemSum fifo元素和
// timeoutM 超时时间, 单位: Ms
bool FspLoad(int mallocTotal, int frameMaxLen, int fifoItemSum, uint64_t timeout) {
    gFrameMaxLen = frameMaxLen;
    gTimeout = timeout;
    mid = TZMallocRegister(0, TAG, mallocTotal);
    if (mid == -1) {
        LE(TAG, "load failed!malloc failed");
        return false;
    }

    gObserverList = TZListCreateList(mid);
    if (gObserverList == 0) {
        LE(TAG, "load failed!create list failed");
        return false;
    }

    gParamList = TZListCreateList(mid);
    if (gParamList == 0) {
        LE(TAG, "load failed!create list failed");
        return false;
    }

    rxBuffer = TZMalloc(mid, sizeof(TZBufferDynamic) + frameMaxLen);
    if (rxBuffer == NULL) {
        LE(TAG, "load failed!malloc rx buffer failed");
        return false;
    }

    if (rxFifoCreate(fifoItemSum, frameMaxLen) == false) {
        return false;
    }

    if (AsyncStart(task, ASYNC_NO_WAIT) == false) {
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
    if (buffer == NULL) {
        LE(TAG, "tx failed!buffer malloc failed");
        return NULL;
    }
    buffer->len = dataLen + FSP_LEN;
    buffer->buf[0] = FSP_FRAME_HEADER_HIGH;
    buffer->buf[1] = FSP_FRAME_HEADER_LOW;
    buffer->buf[2] = buffer->len >> 8;
    buffer->buf[3] = buffer->len & 0xff;
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

static int task(void) {
    static struct pt pt = {0};

    PT_BEGIN(&pt);

    PT_WAIT_UNTIL(&pt, TZFifoReadable(rxFifo));

    tRxTag tag;

    rxBuffer->len = TZFifoReadMix(rxFifo, (uint8_t *)&tag, sizeof(tag), rxBuffer->buf, gFrameMaxLen);

    tFspParam *param = pipeParamGet(tag.pipe);
    fspTimeOutCheck(param, tag.rxTime);
    fspRun(rxBuffer->buf, rxBuffer->len, param);

    PT_END(&pt);
}

static void fspTimeOutCheck(tFspParam *param, uint64_t timestamp) {
    if (timestamp - param->rxTime > gTimeout && param->state != HEADER_HIGH) {
        LW(TAG, "timeout");
        param->state = HEADER_HIGH;
        param->framelen = 0;
        param->dataCpyOffsetAddr = 0;
        param->gBuffer->len = 0;
    }
    param->rxTime = timestamp;
}

static void fspRun(uint8_t *data, int dataLen, tFspParam *param) {
    int i = 0;

    while (i < dataLen) {
        switch (param->state) {
        case HEADER_HIGH:
            if (data[i] == FSP_FRAME_HEADER_HIGH) {
                param->state = HEADER_LOW;
            }
            i++;
            break;
        case HEADER_LOW:
            if (data[i] == FSP_FRAME_HEADER_LOW) {
                param->state = LEN_HIGH;
                i++;
            } else {
                param->state = HEADER_HIGH;
            }
            break;
        case LEN_HIGH:
            param->framelen = data[i] << 8;
            if (param->framelen > gFrameMaxLen) {
                param->state = HEADER_HIGH;
            } else {
                param->state = LEN_LOW;
                i++;
            }
            break;
        case LEN_LOW:
            param->framelen |= data[i];
            if (param->framelen > gFrameMaxLen || param->framelen < FSP_LEN) {
                param->state = HEADER_HIGH;
            } else {
                // 减去FSP帧头长度，但需要加上crc的两个字节
                param->framelen -= FSP_LEN - 2;
                param->gBuffer->len = param->framelen;
                param->state = DATA_CPY;
                i++;
            }
            break;
        case DATA_CPY: {
            int dataCpyLen = dataLen - i;

            if (param->framelen > dataCpyLen) {
                param->framelen -= dataCpyLen;
            } else {
                dataCpyLen = param->framelen;
                param->framelen = 0;
            }

            memcpy(param->gBuffer->buf + param->dataCpyOffsetAddr, &data[i], dataCpyLen);

            // 数据完整性检测
            if (param->framelen == 0) {
                uint16_t crcNum = param->gBuffer->buf[0] << 8 | param->gBuffer->buf[1];
                if (fspDataCrc(crcNum, &param->gBuffer->buf[2], param->gBuffer->len - 2) == true) {
                    notifyObserver(&param->gBuffer->buf[2], param->gBuffer->len - 2, param->pipe);
                    i += dataCpyLen;
                }
                param->dataCpyOffsetAddr = 0;
                param->gBuffer->len = 0;
                param->state = HEADER_HIGH;
            } else {
                param->dataCpyOffsetAddr = param->gBuffer->len - param->framelen;
                return;
            }
            break;
        }
        default:
            param->state = HEADER_HIGH;
            break;
        }
    }
}

static bool fspDataCrc(uint16_t crcNum, uint8_t *data, int size) {
    if (crcNum != 0x0) {
        uint16_t calcCrc = Crc16Checksum(data, size);
        if (crcNum != calcCrc) {
            LE(TAG, "crc err, crcNum = %d, clacCrc = %d", crcNum, calcCrc);
            return false;
        }
    }
    return true;
}

// FspReceive Fsp接收
void FspReceive(uint8_t *data, int dataLen, int pipe) {
    if (TZFifoWriteable(rxFifo) == false) {
        LW(TAG, "deal data is too slow.throw frame!");
        return;
    }

    tRxTag tag;
    tag.pipe = pipe;
    tag.rxTime = TZTimeGet();
    TZFifoWriteMix(rxFifo, (uint8_t *)&tag, sizeof(tag), data, dataLen);
}

// FspRegisterObserver 注册Fsp回调函数
bool FspRegisterObserver(TZPipeDataFunc callback) {
    if (gObserverList == 0 || callback == NULL) {
        return false;
    }
    if (isExistObserver(callback)) {
        return true;
    }

    TZListNode *node = TZListCreateNode(gObserverList);
    if (node == NULL) {
        return false;
    }
    node->Data = TZMalloc(mid, sizeof(tItem));
    if (node->Data == NULL) {
        TZFree(node);
        return false;
    }
    node->Size = sizeof(tItem);

    tItem *item = (tItem *)node->Data;
    item->callback = callback;
    TZListAppend(gObserverList, node);
    return true;
}

static void notifyObserver(uint8_t *bytes, int size, int pipe) {
    TZListNode *node = TZListGetHeader(gObserverList);
    tItem *item = NULL;
    for (;;) {
        if (node == NULL) {
            break;
        }

        item = (tItem *)node->Data;
        if (item->callback) {
            item->callback(bytes, size, 0, 0, pipe);
        }

        node = node->Next;
    }
}

static bool isExistObserver(TZPipeDataFunc callback) {
    TZListNode *node = TZListGetHeader(gObserverList);
    tItem *item = NULL;
    for (;;) {
        if (node == NULL) {
            break;
        }

        item = (tItem *)node->Data;
        if (item->callback == callback) {
            return true;
        }

        node = node->Next;
    }
    return false;
}

static bool rxFifoCreate(int itemSum, int itemSize) {
    // 多4个字节是因为fifo存储混合结构体需增加4字节长度
    rxFifo = TZFifoCreate(mid, itemSum, itemSize + 4);
    if (rxFifo == 0) {
        LE(TAG, "create failed!create rx fifo failed");
        return false;
    }
    return true;
}

static tFspParam *pipeParamGet(int pipe) {
    TZListNode *node = TZListGetHeader(gParamList);
    tFspParam *param = NULL;
    for (;;) {
        if (node == NULL) {
            param = pipeParamCreate(pipe);
            break;
        }

        param = (tFspParam *)node->Data;
        if (param->pipe == pipe) {
            break;
        }

        node = node->Next;
    }

    return param;
}

static tFspParam *pipeParamCreate(int pipe) {
    if (gParamList == 0) {
        return NULL;
    }

    TZListNode *node = TZListCreateNode(gParamList);
    if (node == NULL) {
        return NULL;
    }
    node->Data = TZMalloc(mid, sizeof(tFspParam));
    if (node->Data == NULL) {
        TZFree(node);
        return NULL;
    }
    node->Size = sizeof(tItem);

    tFspParam *item = (tFspParam *)node->Data;
    item->pipe = pipe;
    item->framelen = 0;
    item->rxTime = 0;
    item->state = HEADER_HIGH;
    item->dataCpyOffsetAddr = 0;

    item->gBuffer = TZMalloc(mid, sizeof(TZBufferDynamic) + gFrameMaxLen);
    if (item->gBuffer == NULL) {
        LE(TAG, "load failed!malloc buffer failed");
        TZFree(node);
        return NULL;
    }

    TZListAppend(gParamList, node);

    return item;
}
