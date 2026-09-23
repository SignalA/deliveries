# GaimeGun HID 管理协议

## 报告格式

管理端点固定收发 64 字节 HID report。

除 `FILE_DATA (0x03)` 外，命令和响应使用同一格式：

| 偏移 | 长度 | 字段 |
|---:|---:|---|
| 0 | 1 | 功能码 |
| 1 | 1 | 数据长度，0–56 |
| 2 | 4 | index，little-endian `uint32_t` |
| 6 | N | 数据 |
| 6+N | 2 | Modbus CRC16，little-endian；覆盖前 `6+N` 字节 |
| 8+N | 剩余 | 0 填充到 64 字节 |

`FILE_DATA (0x03)` 是唯一例外：偏移 0 为功能码、偏移 1 为数据长度
（1–62）、偏移 2 起为原始数据；没有 index、CRC 或逐帧响应。

## 功能码

| 功能码 | 名称 | 方向 | index / 数据 | 响应 |
|---:|---|---|---|---|
| `0x01` | CONNECT_CONFIRM | PC→Gun | index=0，data_len=0 | `0x01` 空响应；开始新会话并清理旧临时文件 |
| `0x02` | FILE_INFO | PC→Gun | index 0：2 字节总帧数；2：16 字节 MD5；4：目标路径 | 有效帧原样回显 |
| `0x03` | FILE_DATA | PC→Gun | 1–62 字节文件数据 | 无响应 |
| `0x04` | STATUS_QUERY | 双向 | 见下表 | 返回查询结果 |
| `0x05` | SYSTEM_COMMAND | 双向 | 见下表 | 返回结果；校准除外 |
| `0x06` | SET_INFO | 双向 | index=0，JSON 更新对象 | 成功时回显 |
| `0x07` | GET_INFO | 双向 | index=0，点分 JSON 路径 | NUL 结尾值或 `NULL\0` |
| `0x08` | COM_TEST | 双向 | 任意合法命令数据 | 原样回显 |
| `0x09` | RESET_CALIBRATION | 双向 | 通常 index=0、无数据 | 领域回调接受后回显 |
| `0x0A` | CALIBRATION_RESULT | Gun→PC | data[0]：0 协议接收成功，1 协议接收失败 | 无 |

### FILE_INFO 顺序

1. index 0：`uint16_t total_frames`，little-endian，必须大于 0；
2. index 2：16 字节原始 MD5；
3. index 4：非空目标路径，长度小于 56，不能含内嵌 NUL。

路径帧到达后才进入数据接收状态。有效元数据或数据连续 5 秒未到达时，
会话转为失败并删除 `/app/temp_file.bin`。

### STATUS_QUERY

| index | 返回数据 |
|---:|---|
| 0 | 1 字节会话状态：0 idle、1 接收信息、2 接收数据、3 校验、4 成功、5 失败 |
| 1 | `received_frames` 和 `total_frames`，各 4 字节 little-endian |
| 2 | 1 字节结果：成功为 1，其余为 0 |
| 3 | 16 字节声明的 MD5 |

完成后仍保留进度和 MD5 查询数据，直到下一次 CONNECT 或 Processor 销毁。

### SYSTEM_COMMAND

| index | 含义 | 请求数据 | 结果 |
|---:|---|---|---|
| 0 | 重启 | 空 | `reboot()` 成功不会返回；调用失败才响应 0 |
| 1 | 恢复出厂 | 空 | 已识别但未实现，响应 0 |
| 2 | 设备信息 | 空 | NUL 结尾的 model/fw 文本 |
| 3 | 清缓存 | 空 | 已识别但未实现，响应 0 |
| 4 | 踏板状态 | 至少 1 字节 | 当前无领域消费者，响应 0 |
| 5 | 射击模式 | 1 字节，0=single，1=continuous | 领域回调结果；非法值响应 0 |
| 6 | 校准点 | 33 字节 | 前 7 帧无响应；第 8 帧只返回一次 `0x0A` |

校准点必须按 0–7 连续发送。每帧包含：1 字节点索引、3 组 `(x,y)`
有符号 32 位坐标和 1 组基准 `(x,y)`；所有整数为 little-endian。缺帧、
乱序、整体退化或领域回调失败都不能产生接受结果。

当前实现为兼容历史 host 行为，仍使用 `32767` 作为坐标归一化分母；该分母与
上述 signed-32-bit 字段描述的协议来源尚未确认，因此不能据此推断应改为
`INT32_MAX`。

`0x0A` 只表示 app 已完成帧顺序、长度和整体退化检查，并已将数据
投递给 vision。它不表示 vision 生成的标定矩阵有效或已经生效。

### SET_INFO / GET_INFO

SET_INFO 的数据是 JSON：

```json
{"path":"fw.ver","value":"v1.2.3"}
```

GET_INFO 的数据是点分路径，例如 `fw.ver`。字符串、数字、布尔值和对象都
转换为文本返回；响应数据区超过 56 字节时截断并保留结尾 NUL。

## 响应发送

合法命令先执行，64 字节响应随后由同一 Worker 发送；响应端点暂时不可写时，
Worker 等待端点恢复并暂停接收后续命令。单次等待超时不会丢弃响应或停止 Worker；
Worker 完成文件传输维护后重试发送。等待或写入发生不可恢复错误时停止 Worker。
effect 已执行时不会因为响应未送达而回滚。上位机不能把响应送达作为命令是否执行的
唯一依据；需要确认状态时应重新查询或重发幂等命令。
`FILE_DATA` 按协议不发送逐帧响应。
