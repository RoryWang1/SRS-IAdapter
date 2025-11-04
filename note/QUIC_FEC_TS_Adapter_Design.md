# QUIC+FEC TS Adapter 设计与开发计划（复用优先版）


> **修订记录（2025-10-30）**  
> 本版在“复用优先 + 最小闭环”的前提下，补充/强化：① 线协议探测与**裸TS**兜底；② **双阈值**（FEC修复截止 + 重排窗口）与**关键帧宽限**；③ **Stream 可靠兜底**与 `datagram_only` 开关；④ **PMTU 黑洞回退**（失败回落 1200B）；⑤ **背压高/低水位 + 丢最老非关键帧**；⑥ **观测性指标**与 qlog；⑦ **多节目（program_id）路由**；⑧ 资源上限（CPU/内存）与默认SLO分档；⑨ 测试与验收矩阵细化；⑩ 回放工具与最近N秒datagram切片。


---

## 一、设计目标与原则

### 目标说明
- 在现有 SRS Adapter 框架（IAdapter/FrameBus/FrameToSourceBridge）下，实现原生接入“QUIC+FEC TS流”。
- **最大限度复用 SRS 原生 TS 管线与 adapter 能力**，仅把“可靠还原、稳序输出”做到位。
- 所有 TS 流、音视频解析、关键帧处理等核心能力，均交给 SRS 的已实现模块处理。

### 设计原则
1. **不重复造轮子，不自造 TS Demux/解析/推流链路**。
2. 仅新建“网络输入端 + FEC 解码/乱序重排 + 最终推流接口对接”。
3. 坚持与已存在 Adapter 代码风格一致，充分用好 FrameBus/FrameToSourceBridge、SrsSource、SrsMpegtsOverUdp 等。
4. 配置/参数、生命周期管理紧贴 repo 内部 adapter/style 体系。

---

## 二、演进框架再梳理

### 2.1 本适配器新增部分
- QUIC/UDP 输入解包与会话管理（推荐复用社区库，如 quiche/lsquic，**不手写协议栈**）。
- FEC 解码/组块还原，**仅聚合修复，不处理 TS 内容本身**。
- 简单乱序缓冲及完整 TS 包识别（188B 对齐或打包对齐，保持同步）。
- 将最终聚合并校验通过的 TS 数据包，
  - **直接送入 SRS 原生 TS 管线输入接口**（如调用 SrsMpegtsOverUdp::on_udp_packet/...），
  - 或通过 FrameBus/FrameToSourceBridge 交由后续流程。
- 仅在新输入环节（网络协程+FEC缓冲+正确投喂）做创新与补全。

### 2.2 明确“只做自己的部分”
- **不自行拆解/组装 TS 包**（除非抓到显式跨帧粘包，常用已实现，优先聚合转发）
- **不自写 CRC/PCR/CC 检查**——SRS TS Parser 已有相关健壮代码。
- **不分发/广播/事件通知**，沿用 FrameBus/Listener 机制。
- **不自造 StdFrame 分发主链**，可按需调用 FrameToSourceBridge。（在 TS 流需要多格式并发转出时用）

### 2.3 高复用点
- **FrameBus/FrameToSourceBridge/StdFrame**：直接构建帧结构需求时用，无需额外数据结构。
- **TS 推流对接**：Adapter 层可直接模拟 mpegts-over-udp/SRT 的输入回调调用链，效果等同实际 UDP/SRT 推流。
- **TS Parser/Source**：解流、demux、音视频同步全部由 SRS 原有接口完成。

---

## 三、关键开发步骤

### 3.1 Adapter 框架集成
- 新建 `QuicFecTsAdapter` 类，继承 IAdapter，注册到 AdapterManager。
- 读配置、初始化并绑定指定监听端口或QUIC地址。

### 3.2 QUIC+FEC 网络收流集成
- 用三方库建立QUIC会话/数据通道。
- 提供 `feed()`/`parseFrame()`，持续读取并聚合 datagram/stream。
- FEC 分组还原（RS/XOR，可用简单第三方实现）
- 在窗口或组完整后，输出“已还原 TS 段”至投喂接口。

### 3.3 TS输出与原生流程对接
- **推荐路径：调用 SrsMpegtsOverUdp 的对应输入函数**（如 on_udp_packet/on_ts_message），投递修复好的 TS 数据包。
- 或：推送到 FrameBus, 唤起 FrameToSourceBridge，由 SRS 内部转发/推协议。
- 不在adapter中重复写“TS解流/打包”逻辑。

### 3.4 配置与运维
- 采用当前 Adapter 层参数风格，增加如下配置项：
    - quic_listen/quic_remote
    - fec_mode/k/n/repair_deadline（如adapter参数）
    - 输出模式（ts_passthrough/stdframe），以兼容不同后端对接
    - 指标与观测性配置，沿用 adapter stats

### 3.5 观测与测试
- 统计接口复用 AdapterStatsManager，按输入流量、FEC修复、丢包、推流成功数打点。
- 保证相关异常均有日志与详细事件抛出（如修复超时/丢帧告警等）。
- 测试方案应聚焦于“乱序+丢包FEC”与投给 SRS TS 流负责的端到端验证。

---

## 四、复用与扩展点（任务分工示意）
- `QUIC 绑定与解析`：新建
- `FEC聚包/修复`：新建封装，支持多算法（调用第三方库/已有简易实现）
- `TS聚合及推流`：仅在 TS 粘包等场景下聚合，其余全部调用原生 SRS TS/FrameBus 投递接口，无须重复解析
- `FrameBus/ToSourceBridge/AdapterListener`：全部复用，不重写核心数据结构
- `统计与指标`：直接挂接 adapterStats/全局观测接口
- `异常&慢消费防护`：沿用 adapter 现有策略，可增加特殊场景优化回调。

---

## 五、风险与对策（优化版）
- **上游 wire-format 多变**：主协议探测失败时可 fallback 到裸 TS模式。TS聚合如依赖协议头、可自适应拆包分组（机制用已实现或只做hook触发，无需主逻辑自造）
- **FEC 窗口/修复调优**：超时短降伤修复率、超时长拉高时延，窗口可参数化，调试阶段暴露并细化合理区间。
- **QUIC 库集成与安全**：避免直接用原生 socket，隔离线程/事件循环，通过事件/信号告知主 adapter 协程。
- **压力流控与资源释放**：慢消费、偶发乱序抖动可沿用 FrameBus/AdapterListener drap-old/drop-nonkey 现有策略，无须再造。
- **观测性**：全部安全埋点与告警，建议对接全局 metrics/stats，与 SRS 现有后台一致。

---

## 六、任务里程碑&交付划分（按周/功能小模块细分）

1. 环境准备&社区库集成（QUIC、FEC Demo验证）
2. 新建 QuicFecTsAdapter 类 & AdapterManager 挂载
3. 基础收流、粘包聚合、简单丢包仿真端到端（TS流推送流程贯通）
4. FEC 修复集成，参数化 k/n、deadline，端到端恢复率实测
5. 业务参数、观测性配置与主流程落地升级
6. 深度测试&慢消费/丢包极端场景联调

---

## 七、总结
- 本适配器仅**补齐 SRS 对 QUIC+FEC TS 流的输入环节，后端全部复用原生推流/解析全链路**。
- 避免冗余代码、保证风格统一，持续可维护。
- 后续如需扩展新协议（WebTransport、可靠 UDP 等）可直接套用该模式。

> 如需补充代码样例/对接主流程接口文档或子模块分工，可随时细化阵列。

---

## 二、详细开发分解

### 1. Adapter基础结构
- 目标：
  - 实现`QuicFecTsAdapter`类，继承`IAdapter`，符合AdapterManager注册机制，支持标准配置/启动/生命周期。
- 复用点：
  - `IAdapter`、`AdapterManager`、`FrameBus`、`FrameToSourceBridge`，所有通用生命周期和事件不需重复设计。
- 方法设计：
  - `start/init/close/feed/parseFrame/flush`等标准IAdapter接口方法
  - 构造时读取全局配置（如fec策略、乱序窗口、背压策略等）
- 新增部分：
  - 配套QUIC socket监听/连接管理，fd/事件分发到主协程+解析。

### 2. QUIC输入与事件集成
- 目标：
  - 监听或主动连接QUIC端口，收取上游datagram/stream负载。
- 复用点：
  - 推荐用`quiche/lsquic`等外部库，不自造协议细节。
- 方法设计：
  - 在Map管理会话（ConnectionId->Session对象），每会话配一“输入协程”
  - 仅负责数据报收集、流量调度和超时重连。
- 新增部分：
  - QUICSession/Handler类，封装库回调到适配器主协程。
- 注意事项：
  - 线程安全/事件同步
- 测试点：
  - 模拟多路/并发收流、突发掉线。

### 3. FEC修复与乱序缓冲
- 目标：
  - 对每个FEC group/k/n，完成丢包重组，还原出完整TS负载包。
- 复用点：
  - 若业务内已有RS/XOR模块优先用，无则选用高成熟度三方库。
- 方法设计：
  - 按group id分组缓存，k块齐or时窗截止则解修。
  - 可配置group最大数量及修复deadline（adapter参数）
- 新增部分：
  - `FecGroupBuffer`, `FecRepairManager`, 仅缓冲与调度，不解析TS包体。
- 注意事项：
  - 乱序/timestamp窗口、;超窗后失效回收、热修复延迟（关键帧放宽）。
- 测试点：
  - 仿真各种k/n比例，1~30%丢包多次试验，观测修复率和逻辑正确性。

### 4. TS聚合与原生推流对接
- 目标：
  - 聚合解析复原的TS 188字节对齐包，直接推给SRS已有TS流接口。
- 复用点：
  - 直接调用`SrsMpegtsOverUdp::on_udp_packet`, `on_ts_message`, 或内部TS数据分发api（详查srs_app_mpegts_udp.hpp/cpp）
- 方法设计：
  - 每还原一组 block，遍历数据块，按188对齐切分循环交给TS api。
- 新增部分：
  - 聚合器/投递器，仅聚合粘包，对 TS 包内容不做自解析。
- 注意事项：
  - 静态绑定推流目标流号（如输出rtmp://.../stream=byFecQuic，可配置）
- 测试点：
  - 对比UDP/SRT直连与本Adapter输入效果一致性（首帧/掉帧/马赛克事件）。

### 5. 输出备选：FrameBus链路
- 目标：
  - 可选路径，聚合后如需定制音视频同步/旁路处理可先解TS为StdFrame，再走FrameBus->FrameToSourceBridge->SrsSource（适配多协议）。
- 复用点：
  - 直接使用`FrameBus`, `StdFrame`，按原有音视频帧/pts/dts字段填写、推送。
- 新增部分：
  - Converter，调用SRS内部现有TS解复用（勿重复造解析器）。
- 测试点：
  - 多协议输出（HLS/RTMP/RTC等）与原生TS输入一致性比对。

### 6. 配置与可观测性
- 目标：
  - 支持adapter参数化配置，复用现有adapter范式、adapter stats/metrics。
- 方法设计：
  - 磁盘+热加载兼容，限指标、日志自动接入。
- 新增部分：
  - FEC与QUIC参数，必要时暴露到HTTP API进行查看。
- 测试点：
  - 常态统计、故障注入、慢消费者告警链路

### 7. 测试验证与交付标准
- 目标：
  - 单元、集成、系统，端到端（流生成→丢包/乱序→SRS输出）通路全覆盖
- 验收标准：
  - 端到端流畅首帧（<800ms）、丢包≤30%修复率>90%；所有场景接入SRS后与UDP输入一致。

---

## 三、接口与结构参考样例

列出主要新增与复用类/方法的接口定义和调用流程参考（按最新adapter风格），重点标明推流对接点。

...（此处如需具体C++伪代码、注册点、关键API调用代码行示例，可补充补全）...


## 12.1 资源与上限（新增）
- **CPU**：1080p@4–6Mbps、丢包 5–10% 时，RS 解码 + 重排约 **0.2 核/路**（视 SIMD 优化而定）；
- **内存**：取决于 `fec_repair_deadline_ms + reorder_window_ms`，常规 **10–30 MB/路**；
- **FD/端口**：限制新连接速率，启用 `idle_timeout`，防放大与资源泄露；
- **MTU 黑洞**：探测失败熔断并强制回退到 **1200B**。


**观测补充（新增计数器）**  
- `queue_depth`、`mtu_blackhole_events`、`reconnects`、`parser_fallbacks(bare_ts)`
