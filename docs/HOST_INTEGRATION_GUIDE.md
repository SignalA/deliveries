# GaimeGun 上位机接入指南

本文面向 PC / 游戏主机侧开发者，说明如何通过 USB HID 管理端点调用光枪设备：
连接握手、校准档案（玩家 0–3）的切换与标定、状态查询、错误处理与重试策略。

字节级协议的唯一权威来源是 [HID_PROTOCOL.md](HID_PROTOCOL.md)；本文是面向调用方的
使用说明，包含可直接使用的黄金帧、参考实现与完整调用时序。

- 协议版本：与当前固件一致（64 字节 report、Modbus CRC16、little-endian）
- 变更范围：校准档案（`0x05` 的 index 7/8）为新增；`0x0A` 与 8 点标定帧的既有
  语义未改变，老主机代码无需修改即可继续工作。

---

## 1. 角色与端点

| 端点 | 设备节点 | 报告长度 | 用途 |
|---|---|---|---|
| 管理/命令 | `/dev/hidg2` | 64 字节定长 | 本文所有命令与响应 |
| 触摸 | `/dev/hidg1` | 6 字节 | 瞄准坐标 + 扳机（射击），与本文无关 |
| 键盘 | `/dev/hidg0` | 8 字节 | 投币/暂停/上膛按键，与本文无关 |

设备固定收发 64 字节 report。上位机应当把管理端点当作**一问一答或一问不答**的
串行通道使用。

---

## 2. 传输层

### 2.1 帧布局

| 偏移 | 长度 | 字段 |
|---:|---:|---|
| 0 | 1 | 功能码 `func` |
| 1 | 1 | 数据长度 `N`（普通命令 0–56） |
| 2 | 4 | `index`，little-endian uint32 |
| 6 | N | 数据 |
| 6+N | 2 | Modbus CRC16，little-endian，覆盖前 `6+N` 字节 |
| 8+N | 剩余 | 补 0 到 64 字节 |

- CRC 覆盖范围是 **功能码 + 长度 + index + 数据**；不含尾部补零，也不含 CRC 自身。
- CRC16 参数：初值 `0xFFFF`，多项式 `0xA001`（反射），逐位 LSB-first。
- `index` 的语义**随功能码变化**（详见 §5），不是全局统一的。
- 例外：功能码 `0x03 FILE_DATA` 没有 index、没有 CRC、也不产生逐帧响应；本文
  不涉及该功能码。

### 2.2 端序

所有多字节整数（`index`、`total_frames`、校准坐标、CRC）均为 little-endian。
16 字节 MD5 为原始字节序，不反转。

### 2.3 响应模型（重要）

设备对一条命令可能是三种行为之一：

| 行为 | 触发条件 | 上位机该怎么做 |
|---|---|---|
| **立即响应** | 参数非法但仍"识别"该命令，或命令本身就是查询 | 等一条 64 字节响应 |
| **effect 后响应** | 命令需要设备侧动作（切档、标定入队、重启等） | 等一条 64 字节响应，内容反映动作结果 |
| **静默（不响应）** | 命令未被识别，或**参数/格式无效** | **不要死等**；按超时处理 |

静默的典型情况：`index` 越界、`data_len` 与命令不匹配、校准帧长度不是 33、
校准点序不连续、非法 `index` 的 `STATUS_QUERY`。协议层的无效输入一律静默丢弃，
这是既有行为。

### 2.4 响应反压

设备发送响应时若端点暂时不可写，会**暂停接收后续命令**直到响应送出或超时。
因此：

- 上位机不应在未收到期望响应前持续灌入大量命令；
- 同一命令重复发送必须是安全的（本功能的 `index=7`、`index=8` 均幂等）；
- 一次命令的超时建议 ≥ 200 ms（设备侧响应等待上限为 200 ms）。

---

## 3. 校准档案模型

设备同时持有 **4 套校准档案**，编号 0–3（对应玩家 0–3）。

| 性质 | 说明 |
|---|---|
| 内容 | 每档一个 3×3 单应矩阵，由 8 点标定数据求解得到 |
| 默认状态 | 档案为空 = **无校准**，坐标不经投影直接输出（不是错误） |
| 生效方式 | 设备始终使用"当前档案"投影；`index=7` 切换当前档案，立即生效 |
| 标定写入 | 8 点标定写入"**第 8 帧处理时的当前档案**" |
| 重复标定 | 在同一档案上再次标定 = 在该档已有矩阵上继续叠加修正 |
| 持久化 | **仅内存**：设备重启后 4 档全部为空，需要重新标定 |
| 复位 | `0x09` 清除**当前档案**的内存矩阵 |

**关键约定**：标定帧里**不携带档案号**（为保持既有 wire 兼容）。因此正确顺序
永远是"**先选档，再上传**"。若在上传 8 帧的过程中插入 `index=7`，归属按第 8 帧
处理时的当前档案判定——请避免这种并发操作。

---

## 4. 命令速查

| func | index | 名称 | 方向 | 响应 |
|---:|---:|---|---|---|
| `0x01` | — | CONNECT_CONFIRM | 双向 | 空响应；开始新文件传输会话 |
| `0x02` | 0/2/4 | FILE_INFO | 双向 | 原样回显 |
| `0x03` | — | FILE_DATA | PC→枪 | **无响应** |
| `0x04` | 0–3 | STATUS_QUERY | 双向 | 见 HID_PROTOCOL.md |
| `0x05` | 0 | 重启 | 双向 | 1 字节；成功重启不会返回 |
| `0x05` | 1 | 恢复出厂 | 双向 | 1 字节 `0`（已识别未实现） |
| `0x05` | 2 | 设备信息 | 双向 | NUL 结尾文本 `model_ver: X, fw_ver: Y` |
| `0x05` | 3 | 清缓存 | 双向 | 1 字节 `0`（已识别未实现） |
| `0x05` | 4 | 踏板状态 | 双向 | 1 字节 `0`（无领域消费者；缺数据时静默） |
| `0x05` | 5 | 射击模式 | 双向 | 1 字节：`1` 成功、`0` 失败/非法值 |
| `0x05` | 6 | **校准点（8 帧）** | PC→枪 | 前 7 帧无响应；第 8 帧回一次 `0x0A` |
| `0x05` | 7 | **选择校准档案** | 双向 | 1 字节：`1` 成功、`0` 失败 |
| `0x05` | 8 | **查询校准档案** | 双向 | 4 字节：当前档案/有效掩码/上次结果/上次结果档案 |
| `0x06` | 0 | SET_INFO | 双向 | 成功回显请求 JSON |
| `0x07` | 0 | GET_INFO | 双向 | NUL 结尾值，或 `NULL` |
| `0x08` | — | COM_TEST | 双向 | 原样回显任意数据 |
| `0x09` | — | RESET_CALIBRATION | 双向 | 成功回空响应 |
| `0x0A` | 回显 6 | CALIBRATION_RESULT | 枪→PC | 无（设备主动上报） |

---

## 5. 详细命令规格

### 5.1 选择校准档案（`0x05`, index = 7）

| 项 | 内容 |
|---|---|
| 请求数据 | 1 字节：`data[0]` = 档案号 0–3 |
| 成功响应 | `func=0x05`、`index=7`、1 字节、`data[0]=1` |
| 失败响应 | 1 字节、`data[0]=0`；条件是 `data_len < 1` 或 `data[0] > 3`；**当前档案不变** |
| 幂等 | 是（重复选择同一档案无副作用） |
| 生效时机 | 立即。此后坐标投影使用该档案；该档为空则直通 |

### 5.2 查询校准档案（`0x05`, index = 8）

| 项 | 内容 |
|---|---|
| 请求数据 | 空（`data_len=0`；多余数据被忽略） |
| 响应 | `func=0x05`、`index=8`、`data_len=4` |

响应 4 个字节：

| 字节 | 含义 | 取值 |
|---:|---|---|
| `data[0]` | 当前档案 | 0–3 |
| `data[1]` | 有效档案掩码 | bit0 = 玩家 0，bit1 = 玩家 1，…… |
| `data[2]` | 上次标定结果 | `0` 已生效、`1` 被设备拒绝、`2` 错误、`3` 从未标定 |
| `data[3]` | 上次结果所属档案 | 0–3；从未标定为 `0xFF` |

`data[2]` 的确切含义：

| 值 | 名称 | 设备侧发生了什么 | 该档矩阵 |
|---:|---|---|---|
| 0 | 已生效 | 全部质量校验通过，新矩阵已合成进该档案，投影立即使用 | 已更新 |
| 1 | 被拒 | 设备质量校验不通过（非有限值、采样离散 > 5%、凸包退化、单应求解失败、不可逆、重投影误差 > 5% 等） | **不变** |
| 2 | 错误 | 应用过程内部错误（当前实现会让 HID 输出线程置 FAILED 并停止） | 不变 |
| 3 | 从未标定 | 本次开机后没有任何一次标定尝试 | 空 |

注意区分：

- `data[1]`（掩码）是**每档独立**的当前状态，随时查询都准确；
- `data[2]`/`data[3]` 是**全局"最近一次尝试"**的记录，不按档案保存。若连续为
  多个玩家标定，只会看到最后一次的结果。判断"某玩家是否有可用标定"应使用掩码位。

### 5.3 上传 8 点标定（`0x05`, index = 6）

每帧数据固定 **33 字节**：

| 偏移 | 长度 | 内容 |
|---:|---:|---|
| 0 | 1 | 点序号 0–7（**点号在这里，不在帧头 index**） |
| 1 | 8 | 采样 1：`x, y` 各 int32 LE |
| 9 | 8 | 采样 2：`x, y` 各 int32 LE |
| 17 | 8 | 采样 3：`x, y` 各 int32 LE |
| 25 | 8 | 基准点（期望坐标）：`x, y` 各 int32 LE |

规则：

- 必须按点号 **0 → 7 连续**发送；点号 0 的帧会**重置**设备端累积器（重标即从 0 开始）。
- 帧长不是 33、点号 ≥ 8、点序不连续 → **静默丢弃**（无任何响应）。
- 点 0–6 **没有响应**；点 7 处理后设备**只回一次** `0x0A`：

| `0x0A` 的 `data[0]` | 含义 |
|---:|---|
| `0` | 线级检查通过且已入队。**不等于矩阵已生效** |
| `1` | 整体退化（8×3 实测点在两轴跨度均 < 1%）或队列满/发送失败 |

- 设备的队列入队后由 HID 线程异步求解与校验，因此"是否真的生效"必须用
  §5.2 的查询确认。
- 坐标标度：设备把 int32 坐标除以 **32767** 归一化，即 0…32767 对应归一化 0…1。
  请沿用现有主机实现的标度，**不要**改用 `INT32_MAX`（该分母来源见
  HID_PROTOCOL.md 的说明）。

### 5.4 复位校准（`0x09`）

| 项 | 内容 |
|---|---|
| 请求数据 | 空 |
| 成功响应 | `func=0x09`、空数据 |
| 失败 | 静默（不响应） |
| 作用范围 | 仅**当前档案**：清除其内存矩阵并把掩码对应位置 0 |

`0x09` 不改变 `data[2]`/`data[3]`（它们记录的是"上次标定尝试"，不是矩阵状态）。

---

## 6. 黄金帧（可直接用于联调与自测）

以下帧由**设备自身的协议编码器**生成，并已用独立实现交叉验证 CRC 与布局。
`...` 表示补 0 到 64 字节。

```
# 选择档案 2（func=05, N=01, index=7, data=02）
05 01 07 00 00 00 02 7B D0 ...

# 查询档案状态（func=05, N=00, index=8）
05 00 08 00 00 00 02 2E ...

# 校准点 3：采样 (10000,20000)、(10010,20010)、(9990,19990)，基准 (10000,20000)
05 21 06 00 00 00 03
   10 27 00 00 20 4E 00 00
   1A 27 00 00 2A 4E 00 00
   06 27 00 00 16 4E 00 00
   10 27 00 00 20 4E 00 00
   91 2B ...

# 复位（func=09, N=00, index=0）
09 00 00 00 00 00 00 82 ...

# 设备上报的标定结果帧（func=0A, N=01, index 回显 6, data=00 表示已入队）
0A 01 06 00 00 00 00 38 D1 ...
```

CRC 校验向量（可用于验证上位机实现）：

| 帧头（不含 CRC） | 期望 CRC（LE 字节序） |
|---|---|
| `05 01 07 00 00 00 02` | `7B D0` |
| `05 00 08 00 00 00` | `02 2E` |
| `09 00 00 00 00 00` | `00 82` |
| `0A 01 06 00 00 00 00` | `38 D1` |
| `0A 01 06 00 00 00 01` | `F9 11` |

---

## 7. 参考实现（Python）

以下代码与设备协议一致，可直接复制使用：

```python
import time

PROFILE_COUNT = 4
FUNC_SYSTEM_COMMAND = 0x05
FUNC_RESET_CALIBRATION = 0x09
FUNC_CALIBRATION_RESULT = 0x0A

IDX_REBOOT, IDX_DEVICE_INFO, IDX_SHOOT_MODE = 0, 2, 5
IDX_CALIBRATION_DATA, IDX_CALIBRATION_PROFILE, IDX_CALIBRATION_STATUS = 6, 7, 8

RESULT_APPLIED, RESULT_REJECTED, RESULT_ERROR, RESULT_NEVER = 0, 1, 2, 3
PROFILE_NONE = 0xFF

REPORT_SIZE = 64


def crc16(data: bytes) -> int:
    """Modbus CRC16: init 0xFFFF, poly 0xA001, LSB first."""
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc


def build_frame(func: int, index: int, data: bytes = b"") -> bytes:
    assert len(data) <= 56, "payload too long"
    frame = bytearray(REPORT_SIZE)
    frame[0] = func
    frame[1] = len(data)
    frame[2:6] = index.to_bytes(4, "little")
    frame[6:6 + len(data)] = data
    crc = crc16(bytes(frame[:6 + len(data)]))
    frame[6 + len(data):8 + len(data)] = crc.to_bytes(2, "little")
    return bytes(frame)


def parse_frame(frame: bytes) -> dict:
    """校验 CRC 并拆出字段；crc_ok=False 时其余字段不可信。"""
    assert len(frame) == REPORT_SIZE
    length = frame[1]
    body = frame[:6 + length]
    received = int.from_bytes(frame[6 + length:8 + length], "little")
    return {
        "func": frame[0],
        "length": length,
        "index": int.from_bytes(frame[2:6], "little"),
        "data": frame[6:6 + length],
        "crc_ok": crc16(body) == received,
    }


def i32(value: int) -> bytes:
    return int(value).to_bytes(4, "little", signed=True)


def select_profile(profile: int) -> bytes:
    assert 0 <= profile < PROFILE_COUNT
    return build_frame(FUNC_SYSTEM_COMMAND, IDX_CALIBRATION_PROFILE,
                       bytes([profile]))


def query_calibration() -> bytes:
    return build_frame(FUNC_SYSTEM_COMMAND, IDX_CALIBRATION_STATUS)


def calibration_point(point: int, samples, base) -> bytes:
    """samples: 3 组 (x, y)；base: (x, y)；坐标标度 0..32767。"""
    assert 0 <= point < 8 and len(samples) == 3
    payload = bytes([point])
    for x, y in samples:
        payload += i32(x) + i32(y)
    payload += i32(base[0]) + i32(base[1])
    assert len(payload) == 33
    return build_frame(FUNC_SYSTEM_COMMAND, IDX_CALIBRATION_DATA, payload)


def reset_calibration() -> bytes:
    return build_frame(FUNC_RESET_CALIBRATION, 0)


def parse_calibration_status(data: bytes) -> dict:
    assert len(data) == 4
    return {
        "active_profile": data[0],
        "valid_mask": data[1],
        "last_result": data[2],
        "last_profile": data[3],
    }


def profile_is_calibrated(status: dict, profile: int) -> bool:
    return bool(status["valid_mask"] & (1 << profile))
```

### 7.1 与设备交互的会话封装

```python
class GaimeGunLink:
    """管理端点的一次问答。send/recv 由调用方按平台实现（HID 写入/读取）。"""

    def __init__(self, send_report, recv_report, timeout_s=0.3):
        self._send = send_report      # callable(bytes64) -> None
        self._recv = recv_report      # callable(timeout_s) -> bytes64 | None
        self._timeout = timeout_s

    def request(self, frame: bytes):
        self._send(frame)
        response = self._recv(self._timeout)
        if response is None:
            return None
        parsed = parse_frame(response)
        assert parsed["crc_ok"], "CRC error on response"
        return parsed

    # ---- 校准档案 ----

    def select_profile(self, profile: int) -> bool:
        parsed = self.request(select_profile(profile))
        return parsed is not None and parsed["data"][0] == 1

    def query_status(self):
        parsed = self.request(query_calibration())
        if parsed is None:
            return None
        return parse_calibration_status(parsed["data"])

    def calibrate(self, profile: int, points, poll_after=True):
        """
        points: 长度为 8 的列表，每项为 (samples, base)；
                samples 是 3 组 (x, y)，base 是 (x, y)。
        返回 (ok: bool, detail: str)
        """
        if not self.select_profile(profile):
            return False, "select_profile_failed"

        # 点 0..6 无响应，可连发；第 8 帧后等一次 0x0A
        for point_index, (samples, base) in enumerate(points):
            self._send(calibration_point(point_index, samples, base))
            if point_index < 7:
                continue
            deadline = time.monotonic() + self._timeout
            while True:
                report = self._recv(max(0.0, deadline - time.monotonic()))
                if report is None:
                    return False, "no_calibration_result"
                parsed = parse_frame(report)
                if parsed["func"] == FUNC_CALIBRATION_RESULT:
                    if parsed["data"][0] != 0:
                        return False, "calibration_rejected_by_device"
                    break
                # 其它响应（例如前一条命令的迟到响应）忽略

        if not poll_after:
            return True, "queued"

        # 结果异步产生：轮询查询命令直到出现明确结论
        for _ in range(10):
            status = self.query_status()
            if status is None:
                time.sleep(0.02)
                continue
            if (status["last_result"] == RESULT_APPLIED and
                    status["last_profile"] == profile and
                    profile_is_calibrated(status, profile)):
                return True, "applied"
            if status["last_result"] in (RESULT_REJECTED, RESULT_ERROR) and \
                    status["last_profile"] == profile:
                return False, f"apply_failed_{status['last_result']}"
            time.sleep(0.02)
        return False, "apply_result_timeout"
```

---

## 8. 调用时序

### 8.1 连接与初始化（建议）

```
1. 打开管理端点（64 字节 report）
2. COM_TEST (0x08) 任意数据          → 期望原样回显，确认链路与 CRC 正常
3. GET_INFO (0x07) 路径 "fw.ver"     → 可选，读固件版本用于兼容判断
4. 查询档案状态 (index=8)             → 得到当前档案与有效掩码
5. 按游戏需要选择玩家档案 (index=7)
```

### 8.2 给玩家 N 标定

```
1. index=7, data[0]=N                → 响应 data[0]==1 才继续
2. 依次发送 index=6 的 8 帧（点号 0..7）
   - 连发即可，不要逐帧等响应
   - 坐标标度 0..32767，3 组采样 + 1 组基准点
3. 等一次 0x0A（index 回显 6）
   - data[0]==1 → 判定失败（整体退化/队列满），提示重标，结束
   - data[0]==0 → 继续
4. 查询 index=8（建议延时 20–50 ms 或轮询数次）
   成功条件：last_result==0 且 last_profile==N 且 (mask & (1<<N))!=0
   last_result==1/2 且 last_profile==N → 本次标定失败，提示重标
5. 记录本地状态：玩家 N 已标定
```

### 8.3 玩家切换

```
1. index=7, data[0]=M
2. 查询 index=8（可选）
   (mask & (1<<M))==0 → 该玩家没有标定，当前为"无校准"直通，可提示标定
   否则直接进入游戏
```

### 8.4 开局/进入会话判断

```
查询 index=8 → 用 mask 判断该玩家的标定是否存在
（不要用 last_result 判断"某玩家是否标定过"，它是全局最近一次记录）
```

### 8.5 清除某玩家标定

```
1. index=7, data[0]=N
2. 0x09（空数据）→ 空响应表示成功
3. 查询 index=8 → 掩码对应位应变为 0
```

---

## 9. 错误处理与重试

| 现象 | 可能原因 | 建议处理 |
|---|---|---|
| 无响应（超时） | 命令/参数无效被静默丢弃；端点被反压占用 | 检查 `index`、`data_len`、帧长与 CRC；稍后重试（幂等命令可安全重发） |
| `0x0A` 的 `data[0]==1` | 8 点整体退化（跨度 < 1%）或队列满 | 检查基准点是否分布在全屏、坐标是否全为 0；重新标定 |
| `0x0A` 始终不来 | 上传中断（丢帧/点序乱） | 从点 0 重发整套 8 帧 |
| 查询 `last_result==1` | 设备质量校验拒绝（共线、采样离散、重投影超限） | 提示玩家重新按 8 个位置采集；确认基准点覆盖屏幕四角 |
| 查询 `last_result==3` 且掩码为 0 | 标定从未生效；或 HID 输出线程未运行 | 检查设备是否已启用（见 §11）；重试一次并核对设备日志 |
| `index=7` 响应 `0` | 档案号 > 3 或数据长度不为 1 | 修正帧内容 |
| CRC 校验失败 | 帧布局/端序错误 | 用 §6 的校验向量自测 |

重试原则：`index=7` 与 `index=8` 幂等，可重发；8 点标定只有在**从点 0 重发整套**
时才安全（中途重发单帧会被静默丢弃，因为点序不连续）。

---

## 10. 常见坑

1. **点号在数据里，不在帧头**：8 点标定的帧头 `index` 恒为 `6`，点号是 `data[0]`。
2. **`0x0A` 的 index 回显 6**：不是 0；按功能码 `0x0A` 识别更稳妥。
3. **`0x0A=0` ≠ 标定成功**：它只表示已入队，必须用 `index=8` 复查。
4. **不要逐帧等响应**：点 0–6 永远不会有响应。
5. **先选档再上传**：标定帧不带档案号。
6. **坐标分母是 32767**：不要改成 `INT32_MAX`，否则标定会整体偏移。
7. **`last_result` 是全局的**：多玩家连续标定时只反映最近一次；判断"某玩家是否有标定"要用掩码位。
8. **设备重启后档案全空**：掩码为 0，需重新标定。
9. **`0x09` 只清当前档案**：清其他玩家需先切档。
10. **设备未启用时命令通道仍可用**：设备开机后先只启动命令通道，等上位机把
    `main.status` 设为 `enable` 才启动视觉/跟踪/HID。因此在"未启用"状态下，
    标定可以入队但不会被应用（HID 线程尚未运行）——请先确认设备已启用。

---

## 11. 设备启用与联调观测

### 11.1 启用设备

设备启动后处于等待状态，只有命令端点工作。上位机需先把 `main.status` 置为
`enable`（`0x06 SET_INFO`，数据为 JSON）：

```
{"path":"main.status","value":"enable"}
```

设备每秒轮询该字段，读到 `enable` 后才启动视觉/跟踪/HID/PWM/按键线程。因此在
启用之前，`index=8` 查询会看到 `last_result==3`、掩码为 0。

### 11.2 设备侧日志（联调期）

默认固件关闭日志。联调时用 DEBUG 级别重编（`logging/config/gaime_log_config.h`
中 `GAIME_LOG_LEVEL_NUM` 改为 `GAIME_LOG_DEBUG_LEVEL`），可获得与主机记录逐条
对齐的日志：

| 标签 | 内容 |
|---|---|
| `cmd.calibration profile` | 收到并执行 `index=7` 后的档案快照 |
| `cmd.calibration status` | 应答 `index=8` 时回给主机的 4 个字节 |
| `hid.calibration apply_applied` / `apply_rejected` / `apply_error` | 设备对本次标定的实际处理结局 |
| `hid.calibration apply_reset` | `0x09` 复位某档案 |
| `hid.calibration profile_active` | 当前档案发生变化（该行只在变化时输出） |

每行都带 `active=<当前档案> mask=0x<掩码> last_result=<结果> last_profile=<档案>`。

---

## 12. 兼容性说明

| 项 | 状态 |
|---|---|
| 64 字节 report、CRC16、端序、设备节点 | 未变更 |
| 8 点标定帧格式（33 字节、index=6） | 未变更 |
| `0x0A` 语义 | 未变更（仍只表示"协议级接受并入队"） |
| `0x09` 语义 | 未变更（仍清一个矩阵；多档案下作用于当前档案） |
| `0x05` index 7/8 | **新增**；旧主机不使用这两个 index 时行为完全不变 |
| 校准持久化 | 无（仅内存） |
