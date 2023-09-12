// Copyright 2022-2022 The SUMEC Authors. All rights reserved.
// 帧分割协议
// Authors: Gumy <gumingyang@sumec.com.cn>

#include "fsp.h"
#include "async.h"
#include "crc16.h"
#include "lagan.h"
#include "tzfifo.h"
#include "tzlist.h"
#include "tzmalloc.h"
#include "tztime.h"
#include "statistics.h"

#define TAG "fsp"

#pragma pack(1)

typedef enum {
    STATE_WAIT_HEAD_HIGH = 0,
    STATE_WAIT_HEAD_LOW,
    STATE_WAIT_LEN_HIGH,
    STATE_WAIT_LEN_LOW,
    STATE_WAIT_DATA,
} tState;

typedef struct {
    uint8_t* buf;
    int bufSize;

    tState state;
    int len;
    int frameLen;

    // 开始时间.单位:us
    uint64_t timestamp;
} tFrame;

typedef struct {
    bool isValid;
    int pipe;
    int frameLenMax;
    // 过期时间.单位:us
    uint64_t expireTime;

    // 数据缓存
    intptr_t fifo;
    tFrame frame;

    // 额外参数
    uint32_t ip;
    uint16_t port;
} tCache;

typedef struct {
    TZPipeDataFunc callback;
} tItem;

#pragma pack()

// 统计项id
static int gStatRxTimeout = -1;
static int gStatErrorFrameLen = -1;
static int gStatErrorCrc = -1;

static intptr_t gMid = 0;
static intptr_t gObserverList = 0;
static tCache* gCache = NULL;
static int gPipeNum = 0;

static int getFreeCache(void);
static int getCache(int pipe);
static int task(void);
static void checkFspRx(tCache* cache);
static void notifyObserver(uint8_t *bytes, int size, int pipe, uint32_t ip, uint16_t port);
static bool isExistObserver(TZPipeDataFunc callback);

// FspLoad 模块载入
bool FspLoad(int pipeNum, int mallocSize) {
    gStatRxTimeout = StatisticsRegister("fsp_rx_timeout");
    gStatErrorFrameLen = StatisticsRegister("fsp_err_len");
    gStatErrorCrc = StatisticsRegister("fsp_err_crc");
    if (gStatRxTimeout < 0 || gStatErrorFrameLen < 0 || gStatErrorCrc < 0) {
        LE(TAG, "load failed!statistics register failed");
        return false;
    }

    gMid = TZMallocRegister(0, TAG, mallocSize);
    if (gMid == -1) {
        LE(TAG, "load failed!malloc failed");
        return false;
    }

    gObserverList = TZListCreateList(gMid);
    if (gObserverList == 0) {
        LE(TAG, "load failed!create list failed");
        return false;
    }

    gCache = TZMalloc(gMid, sizeof(tCache) * pipeNum);
    if (gCache == NULL) {
        LE(TAG, "load failed!gCache malloc failed");
        return false;
    }
    gPipeNum = pipeNum;

    if (AsyncStart(task, ASYNC_NO_WAIT) == false) {
        LE(TAG, "load failed!async start task failed");
        return false;
    }
    return true;
}

// FspAddPipe 增加管道.过期时间单位:ms
bool FspAddPipe(int pipe, int frameLenMax, int expireTime, int fifoCache) {
    int index = getFreeCache();
    if (index < 0) {
        LE(TAG, "add pipe failed!get free pipe failed!");
        return false;
    }

    gCache[index].pipe = pipe;
    gCache[index].frameLenMax = frameLenMax;
    gCache[index].expireTime = expireTime * TZTIME_MILLISECOND;

    gCache[index].fifo = TZFifoCreate(gMid, fifoCache, 1);
    if (gCache[index].fifo == 0) {
        LE(TAG, "add pipe failed!create fifo failed!");
        return false;
    }

    gCache[index].frame.buf = TZMalloc(gMid, frameLenMax);
    if (gCache[index].frame.buf == NULL)    {
        LE(TAG, "add pipe failed!frame buf malloc failed!");
        TZFifoDelete(gCache[index].fifo);
        return false;
    }

    gCache[index].frame.bufSize = frameLenMax;
    gCache[index].isValid = true;
    return true;
}

static int getFreeCache(void) {
    for (int i = 0; i < gPipeNum; i++) {
        if (gCache[i].isValid == false) {
            return i;
        }
    }
    return -1;
}

// FspReceive 接收FSP帧
void FspReceive(uint8_t* data, int dataLen, int pipe, uint32_t ip, uint16_t port) {
    int index = getCache(pipe);
    if (index == -1) {
        return;
    }

    TZFifoWriteBatch(gCache[index].fifo, data, dataLen);
    gCache[index].ip = ip;
    gCache[index].port = port;
}

static int getCache(int pipe) {
    for (int i = 0; i < gPipeNum; i++) {
        if (gCache[i].pipe == pipe && gCache[i].isValid == true) {
            return i;
        }
    }
    return -1;
}

static int task(void) {
    static struct pt pt = {0};
    static int i = 0;
    
    PT_BEGIN(&pt);

    for (i = 0; i < gPipeNum; i++) {
        if (gCache[i].isValid == false) {
            break;
        }
        checkFspRx(&gCache[i]);
        PT_YIELD(&pt);
    }
    
    PT_END(&pt);
}

static void checkFspRx(tCache* cache) {
    uint64_t now = TZTimeGet();
    if (cache->frame.state != STATE_WAIT_HEAD_HIGH && now - cache->frame.timestamp > cache->expireTime) {
        // 超时则清空
        LW(TAG, "pipe:%d wait timeout.len:%d.now clear frame data", cache->pipe, cache->frame.len);
        cache->frame.state = STATE_WAIT_HEAD_HIGH;
        cache->frame.len = 0;
        StatisticsAdd(gStatRxTimeout);
    }

    int count = TZFifoReadableItemCount(cache->fifo);
    if (count == 0) {
        return;
    }
    
    if (count > cache->frame.bufSize - cache->frame.len) {
        count = cache->frame.bufSize - cache->frame.len;
    }

    if (TZFifoReadBatch(cache->fifo, cache->frame.buf + cache->frame.len, count, count) == false) {
        LW(TAG, "fifo read batch failed!");
        return;
    }

    cache->frame.timestamp = now;
    uint8_t* data = cache->frame.buf + cache->frame.len;
    tFrame* frame = &cache->frame;
    for (int i = 0; i < count; i++) {
        switch(cache->frame.state) {
            case STATE_WAIT_HEAD_HIGH: {
                if (data[i] == 0xC5) {
                    frame->buf[cache->frame.len] = data[i];
                    frame->len++;
                    frame->state++;
                } else {
                    frame->state = STATE_WAIT_HEAD_HIGH;
                    frame->len = 0;
                }
                break;
            }
            case STATE_WAIT_HEAD_LOW: {
                if (data[i] == 0x5C) {
                    frame->buf[cache->frame.len] = data[i];
                    frame->len++;
                    frame->state++;
                } else {
                    frame->state = STATE_WAIT_HEAD_HIGH;
                    frame->len = 0;
                }
                break;
            }
            case STATE_WAIT_LEN_HIGH: {
                frame->buf[cache->frame.len] = data[i];
                frame->len++;
                frame->state++;
                break;
            }
            case STATE_WAIT_LEN_LOW: {
                frame->buf[cache->frame.len] = data[i];
                frame->len++;

                frame->frameLen = (frame->buf[2] << 8) + frame->buf[3];
                if (frame->frameLen > cache->frameLenMax || frame->frameLen <= 6) {
                    LE(TAG, "pipe:%d frame len is wrong.%d", cache->pipe, frame->frameLen);
                    frame->state = STATE_WAIT_HEAD_HIGH;
                    frame->len = 0;
                    StatisticsAdd(gStatErrorFrameLen);
                } else {
                    frame->state++;
                }
                break;
            }
            case STATE_WAIT_DATA: {
                frame->buf[cache->frame.len] = data[i];
                frame->len++;

                if (frame->len >= frame->frameLen) {
                    uint16_t crcGet = (frame->buf[4] << 8) + frame->buf[5];
                    if (crcGet != 0) {
                        uint16_t crcCalc = Crc16Checksum(frame->buf + 6, frame->len - 6);
                        if (crcGet != crcCalc) {
                            LE(TAG, "pipe:%d frame crc is wrong.0x%04x 0x%04x", cache->pipe, crcGet, crcCalc);
                            frame->state = STATE_WAIT_HEAD_HIGH;
                            frame->len = 0;
                            StatisticsAdd(gStatErrorCrc);
                            break;
                        }
                    }

                    LD(TAG, "pipe:%d receive frame:%d crc:0x%x", cache->pipe, frame->len, crcGet);
                    notifyObserver(frame->buf + 6, frame->len - 6, cache->pipe, cache->ip, cache->port);
                    frame->state = STATE_WAIT_HEAD_HIGH;
                    frame->len = 0;
                }
            }
        }
    }
}

static void notifyObserver(uint8_t *bytes, int size, int pipe, uint32_t ip, uint16_t port) {
    TZListNode *node = TZListGetHeader(gObserverList);
    tItem *item = NULL;
    for (;;) {
        if (node == NULL) {
            break;
        }

        item = (tItem *)node->Data;
        if (item->callback) {
            item->callback(bytes, size, ip, port, pipe);
        }

        node = node->Next;
    }
}

// FspRegisterObserver 注册回调函数
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
    node->Data = TZMalloc(gMid, sizeof(tItem));
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

// FspBytesToFrame 字节流转FSP帧
// 返回值是FSP帧字节数.返回0表示转换失败.帧存储在dst中
// 如果src长度足够长,可以设置dst等于src,这样转换后的帧会存放在src中
int FspBytesToFrame(uint8_t *src, int srcLen, bool isNeedCrc, uint8_t* dst, int dstSize) {
    uint16_t frameLen = srcLen + 6;
    if (dstSize < frameLen) {
        LW(TAG, "bytes to frame failed!dst size is too short");
        return 0;
    }

    int j = 0;
    dst[j++] = 0xC5;
    dst[j++] = 0x5C;
    dst[j++] = frameLen >> 8;
    dst[j++] = frameLen;

    uint16_t crc = 0;
    if (isNeedCrc == true) {
        crc = Crc16Checksum(src, srcLen);
    }
    dst[j++] = crc >> 8;
    dst[j++] = crc;
    memmove(dst + j, src, srcLen);
    j += srcLen;
    return j;
}

// FspFrameToBytes FSP帧转换为字节流
// 返回值是字节流字节数.返回0表示转换失败.字节流存储在dst中
// 如果不需要src帧,为节约空间,可将dst设置为src的地址
int FspFrameToBytes(uint8_t *src, int srcLen, bool isNeedCrc, uint8_t* dst, int dstSize) {
    if (FspIsFrameValid(src, srcLen) == false) {
        LW(TAG, "frame to bytes failed!frame is invalid");
        return 0;
    }
    if (dstSize < srcLen - 6) {
        LW(TAG, "frame to bytes failed!dst size is too short");
        return 0;
    }

    memmove(dst, src + 6, srcLen - 6);
    return srcLen - 6;
}

// FspIsFrameValid 是否有效的FSP帧
bool FspIsFrameValid(uint8_t* frame, int len) {
    if (len < 6) {
        return false;
    }
    if (frame[0] != 0xC5 || frame[1] != 0x5C) {
        return false;
    }
    uint16_t payloadLen = (frame[2] << 8) + frame[3];
    if (payloadLen != len) {
        return false;
    }
    uint16_t crcGet = (frame[4] << 8) + frame[5];
    if (crcGet != 0) {
        uint16_t crcCalc = Crc16Checksum(frame + 6, len - 6);
        if (crcGet != crcCalc) {
            return false;
        }
    }
    return true;
}
