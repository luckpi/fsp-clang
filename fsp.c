// Copyright 2022-2022 The SUMEC Authors. All rights reserved.
// 帧分割协议
// Authors: Gumy <gumingyang@sumec.com.cn>
#include "fsp.h"
#include "pt.h"

#define TAG "fsp"
// tzmalloc字节数
#define MALLOC_TOTAL 8192

#define FSP_LEN 6

typedef struct
{
    TZDataFunc callback;
} tItem;

typedef enum {
    HEADER_HIGH,
    HEADER_LOW,
    LEN_HIGH,
    LEN_LOW,
    DATA_CPY,
} FspState;

static int mid = -1;

// 观察者列表
static intptr_t gList = 0;

// 接收fifo
static intptr_t rxFifo;
static TZBufferDynamic *buffer;

static int task(void);
static bool fspDataCrc(uint16_t crcNum, uint8_t *data, int size);
static void notifyObserver(uint8_t *bytes, int size);
static bool isExistObserver(TZDataFunc callback);
static void FspRun(uint8_t *data, int dataLen);
static bool rxFifoCreate(void);

// FspLoad Fsp载入
bool FspLoad(void) {
    mid = TZMallocRegister(0, TAG, MALLOC_TOTAL);
    if (mid == -1) {
        LE(TAG, "load failed!malloc failed");
        return false;
    }

    gList = TZListCreateList(mid);
    if (gList == 0) {
        LE(TAG, "load failed!create list failed");
        return false;
    }

    buffer = TZMalloc(mid, sizeof(TZBufferDynamic) + FSP_FRAME_LEN_MAX);
    if (buffer == NULL) {
        LE(TAG, "load failed!malloc rx buffer failed");
        return false;
    }

    if (rxFifoCreate() == false) {
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

static int task(void) {
    static struct pt pt = {0};

    PT_BEGIN(&pt);

    PT_WAIT_UNTIL(&pt, TZFifoReadable(rxFifo));

    buffer->len = TZFifoReadBytes(rxFifo, buffer->buf, FSP_FRAME_LEN_MAX);

    FspRun(buffer->buf, buffer->len);

    PT_END(&pt);
}

static void FspRun(uint8_t *data, int dataLen) {
    int i = 0;
    static TZBuffer buffer;
    static int len = 0;
    static int dataCpyOffsetAddr = 0;
    static FspState fspState;

    while (i < dataLen) {
        switch (fspState) {
        case HEADER_HIGH:
            if (data[i] == FSP_FRAME_HEADER_HIGH) {
                fspState = HEADER_LOW;
            }
            i++;
            break;
        case HEADER_LOW:
            if (data[i] == FSP_FRAME_HEADER_LOW) {
                fspState = LEN_HIGH;
                i++;
            } else {
                fspState = HEADER_HIGH;
            }
            break;
        case LEN_HIGH:
            len = data[i] << 8;
            if (len > FSP_FRAME_LEN_MAX) {
                fspState = HEADER_HIGH;
            } else {
                fspState = LEN_LOW;
                i++;
            }
            break;
        case LEN_LOW:
            len |= data[i];
            // 加上crc的两个字节
            len += 2;
            if (len > FSP_FRAME_LEN_MAX || len == 0) {
                fspState = HEADER_HIGH;
            } else {
                buffer.len = len;
                fspState = DATA_CPY;
                i++;
            }
            break;
        case DATA_CPY: {
            int dataCpyLen = dataLen - i;

            if (len > dataCpyLen) {
                len -= dataCpyLen;
            } else {
                dataCpyLen = len;
                len = 0;
            }

            memcpy(buffer.buf + dataCpyOffsetAddr, &data[i], dataCpyLen);

            // 数据完整性检测
            if (len == 0) {
                uint16_t crcNum = buffer.buf[0] << 8 | buffer.buf[1];
                if (fspDataCrc(crcNum, &buffer.buf[2], buffer.len - 2) == true) {
                    i += dataCpyLen;
                }
                dataCpyOffsetAddr = 0;
                fspState = HEADER_HIGH;
            } else {
                dataCpyOffsetAddr = buffer.len - len;
                return;
            }
            break;
        }
        default:
            fspState = HEADER_HIGH;
            break;
        }
    }
}

static bool fspDataCrc(uint16_t crcNum, uint8_t *data, int size) {
    if (crcNum != 0x0) {
        if (crcNum != Crc16Checksum(data, size)) {
            return false;
        }
    }
    notifyObserver(data, size);
    return true;
}

// FspReceive Fsp接收
void FspReceive(uint8_t *data, int dataLen) {
    if (TZFifoWriteable(rxFifo) == false) {
        LW(TAG, "deal data is too slow.throw frame!");
        return;
    }
    TZFifoWriteBytes(rxFifo, data, dataLen);
}

// FspRegisterObserver() 注册Fsp回调函数
bool FspRegisterObserver(TZDataFunc callback) {
    if (gList == 0 || callback == NULL) {
        return false;
    }
    if (isExistObserver(callback)) {
        return true;
    }

    TZListNode *node = TZListCreateNode(gList);
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
    TZListAppend(gList, node);
    return true;
}

static void notifyObserver(uint8_t *bytes, int size) {
    TZListNode *node = TZListGetHeader(gList);
    tItem *item = NULL;
    for (;;) {
        if (node == NULL) {
            break;
        }

        item = (tItem *)node->Data;
        if (item->callback) {
            item->callback(bytes, size);
        }

        node = node->Next;
    }
}

static bool isExistObserver(TZDataFunc callback) {
    TZListNode *node = TZListGetHeader(gList);
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

static bool rxFifoCreate(void) {
    // 多4个字节是因为fifo存储混合结构体需增加4字节长度
    rxFifo = TZFifoCreate(mid, FSP_RX_FIFO_ITEM_SUM, FSP_FRAME_LEN_MAX + 4);
    if (rxFifo == 0) {
        LE(TAG, "create failed!create rx fifo failed");
        return false;
    }
    return true;
}
