# fsp

## 1. 介绍

Frame Segmentation Protocol(FSP)：帧分割协议。

## 2. 功能

在串口或者4G等一些通信介质中，数据会出现粘包的情况。此时可以应用本协议分割帧。

## 3. 帧格式

| 帧头 | 帧长 | 校验 | 数据 |
| ---- | ---- | ---- | ---- |

| 内容 | 字节 | 说明                           |
| ---- | ---- | ------------------------------ |
| 帧头 | 2    | 固定0xC5 0x5C                  |
| 帧长 | 2    | 整个一帧字节数，包括帧头和帧尾 |
| 校验 | 2    | CRC16校验，校验的部分是数据    |
| 数据 | N    |                                |

协议是大端传输。

## 4. 校验说明

- 如果校验值为全0，表示无需校验
- CRC16算法类型：CRC-16/MODBUS,多项式是8005

校验示例：

| 二进制数据            | 校验值(大端) |
| --------------------- | ------------ |
| 0x1 0x2 0x3           | 0x61 0x61    |
| 0x1 0x2 0x3 0x56 0x87 | 0x97 0xba    |
| 0x9 0x12              | 0x87 0xed    |

## 5. API

```c
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
void FspReceive(uint8_t *data, int dataLen, int pipe);

// FspRegisterObserver 注册Fsp观察者
bool FspRegisterObserver(TZPipeDataFunc callback);
```
