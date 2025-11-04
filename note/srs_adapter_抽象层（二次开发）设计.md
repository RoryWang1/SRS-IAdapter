# SRS Adapter 抽象层（二次开发）设计文档

---

## 目录

- [背景与目标](#背景与目标)
- [总体方案](#总体方案)
- [模块结构与命名](#模块结构与命名)
- [关键调用链](#关键调用链)
- [统一“标准帧”协议](#统一标准帧协议)
- [与 SRS 核心的粘合点](#与-srs-核心的粘合点)
- [时间戳、缓冲与背压策略](#时间戳缓冲与背压策略)
- [配置与路由](#配置与路由)
- [实现步骤与里程碑](#实现步骤与里程碑)
- [观测、排障与测试](#观测排障与测试)
- [风险与非目标](#风险与非目标)
- [最小样例骨架（伪代码）](#最小样例骨架伪代码)
- [Checklist](#checklist)

---

## 背景与目标

在 `trunk/src/app/` 下新增 `adapter/` 抽象层，通过 **AdapterManager + IAdapter 插件** 机制，把外部自研或第三方协议接入为 **可插拔的 Stream Caster**，统一转为“标准帧”后喂给 SRS 核心分发（RTMP/HTTP-FLV/HLS/WebRTC/SRT 等）。这样可以：

- 快速导入自研协议，无需改动 SRS 核心；
- 复用 SRS 的 Source/GOP/转封装/多协议输出能力；
- 延续现有 `stream_caster` 配置范式，管理员零额外学习成本；
- 便于灰度、旁路和逐步增强（低风险）。

---

## 总体方案

```
外部输入(网络/文件/自定义IO)
        │
        ▼
  AdapterListener  ←  统一协程/监听封装（可复用 srs_app_listener.*）
        │
        ▼
  AdapterManager   ←  注册表/工厂：协议名 → IAdapter
        │                         ├─ myproto_adapter
        │                         ├─ rtp_ps_adapter（示例）
        │                         └─ vendorX_adapter（示例）
        ▼
      IAdapter  —— parseFrame()/start()/feed()/flush()/close()
        │
        ▼
      FrameBus  —— 产出统一“标准帧”
        │
        ▼
  SrsSource / LiveSource —— on_video()/on_audio()/on_metadata()
        │
        └→ 多协议输出：RTMP / HTTP-FLV / HLS / WebRTC / SRT / DASH...
```

---

## 模块结构与命名

建议在 `trunk/src/app/` 形成如下布局（文件名可微调，但请保持职责单一）：

```
trunk/src/app/
  adapter/
    iadapter.hpp            # 抽象接口与初始化参数
    adapter_manager.hpp/.cpp# 注册表/工厂/生命周期与路由
    adapter_listener.hpp/.cpp# 监听器封装（复用 SRS 协程与网络栈）
    frame_bus.hpp/.cpp      # 标准帧缓冲/派发（轻量，不做大队列）
    std_frame.hpp           # 统一媒体帧定义（音/视频/事件）
    adapters/
      myproto_adapter.cpp   # 最小可跑的样例协议插件
      rtp_ps_adapter.cpp    # 示例：RTP/PS 解析插件（可选）

  srs_app_caster_myproto.cpp # 薄桥接：把 adapter 挂入 SRS Caster 工厂
  # … 其余 SRS 既有模块（rtmp/hls/http_stream/gb28181/...）
```

> 说明：
>
> - `srs_app_caster_myproto.cpp` 是**薄桥接文件**，用于将 `adapter` 注册到 SRS 现有的 Caster 创建流程，复用 `stream_caster` 配置项（避免另起一套配置语法）。
> - 若后续新增更多协议，推荐以 `srs_app_caster_xxx.cpp` 的方式分别挂接，适配器实现仍放在 `adapter/` 下。

---

## 关键调用链

```
外部流入数据 → AdapterManager → 对应协议 IAdapter
                             ↓
                        parseFrame()/start()
                             ↓
                     输出标准帧 → FrameBus → SrsSource
```

- **AdapterListener**：监听端口/接入连接，交由 AdapterManager 路由到目标 IAdapter；每连接/会话一条协程。
- **AdapterManager**：注册表（协议名 → 工厂方法），负责 IAdapter 的创建、复用与销毁，并维护路由（vhost/app/stream）。
- **IAdapter**：协议插件核心，完成原始数据的**拆包、纠偏、组帧**，产出“标准帧”。
- **FrameBus**：极薄缓冲层，按 DTS 排序并喂给 `SrsSource`，避免双缓冲放大延迟。

---

## 统一“标准帧”协议

为确保所有适配器都能走同一条核心链路，约束“标准帧”字段如下（**统一时基：毫秒**）：

### 公共字段（所有帧）

- `codec`：`H264 | H265 | AAC | OPUS | PCM_ALAW | PCM_ULAW | ...`
- `dts_ms` / `pts_ms`：`int64`，单调递增；若源为 90kHz（PS/TS/RTCP），换算规则 `ms = ts / 90`。
- `stream_id`：路由键（`vhost/app/stream`）。
- `extradata`：可选，编解码初始化参数（H.264 SPS/PPS；H.265 VPS/SPS/PPS；AAC ASC 等）。

### 视频负载

- **H.264/H.265**：Annex-B NALU（前缀 0x00000001）；关键帧须携带参数集；B 帧需保证 `dts < pts`。
- `keyframe`：布尔，标记 IDR/CRA 等关键帧。

### 音频负载

- **AAC**：去 ADTS，RAW frame；ASC 通过 `extradata` 在 sequence header 携带一次。
- **Opus/G.711**：按采样率/声道入库，确保时间戳连续与对齐。

### 流事件

- `on_start_stream(vhost, app, stream)`：触发创建/查找 `SrsSource` 并对接 `on_publish` 流程。
- `on_stop_stream()`：清理资源与路由，关闭消费者。

---

## 与 SRS 核心的粘合点

- **入口复用**：沿用“*Caster → SrsSource*”主路，适配器只需把标准帧封成 SRS 内部消息调用 `on_video()`/`on_audio()` 等接口；
- **网络与协程**：优先复用 `srs_app_listener.*` 与 ST 协程模型，以继承统一的日志/上下文 ID/超时控制；
- **配置**：通过 `stream_caster` 扩展一个 `caster myproto`（或多个），保持与 TS/UDP、HTTP-FLV/POST、GB28181/TCP 相同的使用体验。

---

## 时间戳、缓冲与背压策略

- **时基统一**：所有来源统一换算为 **毫秒**；缺失 PTS 的场景按编码规则回推，或以解码顺序近似（可配置）。
- **JitterBuffer**：实现一个小窗口（例如 200–500ms）的乱序重排与抖动缓冲；窗口外的极端乱序直接丢弃或矫正。
- **首包/GOP 策略**：
  - 默认等到**首个关键帧**后触发 `on_start_stream()`；
  - 允许“热启动 + 参数集补发”（在弱网/低延迟模式下可选）。
- **背压控制**：适配器不做大队列；把背压交给 `SrsSource` 与下游消费者，避免双重缓冲导致延迟放大。

---

## 配置与路由

复用 `conf` 的 `stream_caster` 语义，以最小变更引入新协议：

```conf
stream_caster {
    enabled on;
    caster   myproto;            # 你的协议名（由桥接文件注册）
    listen   9000;               # 监听端口/传输（如 tcp://0.0.0.0:9000）
    output   rtmp://127.0.0.1/[app]/[stream];

    # 可选：协议私有配置
    # vendor_ext   on;
    # input_mode   framed|raw;
    # route        vhost=__defaultVhost__;app=live;stream=${from_proto}
}
```

**路由策略**（建议三种来源都支持）：

1. **固定配置**：`vhost/app/stream` 在 conf 中写死，适合网关接入；
2. **协议字段映射**：从自研协议头中抽取并映射到路由；
3. **端口维度默认值**：同一端口的连接默认路由到某个 `app` 或 `vhost`，减少配置量。

---

## 实现步骤与里程碑

**Phase 1 — 最小可用（1–2 周）**

- 完成 `iadapter.hpp`、`adapter_manager`、`adapter_listener`、`std_frame.hpp`；
- 实现 `myproto_adapter`：仅支持 TCP 单路、固定路由、H.264/AAC；
- 薄桥接 `srs_app_caster_myproto.cpp`，可通过 conf 启用并推流到 SRS 核心；
- 基础观测：连接日志、首帧耗时、帧率/码率统计。

**Phase 2 — 协议增强（2–3 周）**

- 完善拆包/重组、B 帧时间戳与 `dts/pts` 关系、参数集管理；
- 异常恢复：心跳/keepalive、重连、re-invite（如适用）；
- JitterBuffer 与乱序窗口；
- 路由：支持协议内字段映射与端口默认值。

**Phase 3 — 观测与稳定（持续）**

- 接入 HTTP API 统计/自检（如 `/api/v1/streams` 扩展项）；
- 录制回放/pcap 回放工具链（便于问题复现）；
- 兼容性回归：RTMP/HLS/HTTP-FLV/WebRTC/SRT 全链路打通与回退策略。

---

## 观测、排障与测试

**Metrics/Logs**（建议）：

- 连接生命周期：建立时间、首帧时间、平均/峰值码率、流持续时长；
- 质量指标：丢包/乱序（若适用 UDP）、JitterBuffer 命中率、重传/重邀次数；
- 输出侧：进入 `SrsSource` 后的消费者数、GOP 命中率、HLS 切片时延等。

**测试清单**：

- 单元：拆包/组帧、时戳换算（90kHz→ms）、B 帧 `dts/pts` 序；
- 集成：端到端推拉（RTMP/HTTP-FLV/HLS/WebRTC/SRT），首帧与稳定性；
- 鲁棒：断网/抖动/乱序/时间回退/参数集缺失/关键帧丢失等边界；
- 兼容：浏览器、播放器（VLC/ffplay/ExoPlayer/Safari/OBS）。

---

## 风险与非目标

**风险**

- 协议层复杂度导致“适配器内大缓冲”，放大端到端延迟；
- 非严格时戳（特别是 B 帧）引发播放端花屏/卡顿；
- 多协议并存时的路由冲突与资源抢占。

**非目标**

- 在适配器中实现完整的控制面（如 ONVIF/对讲/回放）；这些应通过外部服务/旁路实现，仅把媒体面接入 SRS。

---

## 最小样例骨架（伪代码）

### `adapter/iadapter.hpp`

```cpp
#pragma once
#include <string>
#include <vector>
#include <map>
#include <stdint.h>

struct AdapterInit {
    std::string vhost;
    std::string app;
    std::string stream;
    std::map<std::string,std::string> kv; // 协议私参
};

class IAdapter {
public:
    virtual ~IAdapter() {}
    virtual srs_error_t start(const AdapterInit& init) = 0;
    virtual srs_error_t feed(const uint8_t* data, size_t nbytes) = 0; // 原始字节/包
    virtual srs_error_t parseFrame() = 0;  // 组帧→产出标准帧
    virtual srs_error_t flush() = 0;
    virtual void close() = 0;
};
```

### `adapter/std_frame.hpp`（简化示意）

```cpp
struct StdFrameCommon {
    std::string codec;      // "H264"/"H265"/"AAC"/...
    int64_t dts_ms = 0;
    int64_t pts_ms = 0;
    bool keyframe = false;  // 视频
    std::vector<uint8_t> extradata; // SPS/PPS/ASC
};

struct StdFrame {
    StdFrameCommon h;
    std::vector<uint8_t> payload; // Annex-B NALU 或 AAC RAW 等
};
```

### `adapter/adapter_manager.hpp`（核心接口）

```cpp
using AdapterFactory = std::function<IAdapter*()>;

class AdapterManager {
public:
    static AdapterManager& instance();
    void register_factory(const std::string& name, AdapterFactory f);
    IAdapter* create(const std::string& name);

    srs_error_t route_and_start(const std::string& caster,
                                const AdapterInit& init);
};
```

### `adapter/adapters/myproto_adapter.cpp`（核心流程）

```cpp
class MyProtoAdapter : public IAdapter {
public:
    srs_error_t start(const AdapterInit& init) override {
        // 初始化路由/状态，校验 extradata 等
        return srs_success;
    }
    srs_error_t feed(const uint8_t* data, size_t nbytes) override {
        // 拆包/缓冲，积累到完整帧
        return srs_success;
    }
    srs_error_t parseFrame() override {
        // 组 H264/AAC 标准帧，换算 dts/pts（ms）
        // 首个关键帧前补齐 SPS/PPS 或 VPS/SPS/PPS
        // 调用 FrameBus::push(frame)
        return srs_success;
    }
    srs_error_t flush() override { return srs_success; }
    void close() override {}
};

// 注册到工厂
static bool _ = [](){
    AdapterManager::instance().register_factory("myproto", [](){return new MyProtoAdapter();});
    return true;
}();
```

### `srs_app_caster_myproto.cpp`（桥接示意）

```cpp
// 在 SRS 的 caster 创建流程里：
// if (conf.caster == "myproto") { auto* a = AdapterManager::instance().create("myproto"); a->start(init); }
// 复用 SRS 的 listener/协程与 HTTP API
```

---


---

## 架构决策记录（ADR-001）：为何选择 Adapter/Caster 插件化路径

**背景**：我们有两种实现思路——
1) *自建“统一流入接口”层*：桥接器 → 统一抽象帧 → 统一流入接口 → SRS 内部封装（SrsMediaPacket）→ 核心；
2) *对齐 SRS 正路*：Adapter 插件直接产出“标准帧”，通过 **stream_caster → SrsSource** 进入核心分发。

**决策**：采用 **Adapter/Caster 插件化** 路线（本设计）。

**理由**：
- **更小侵入**：完全复用 stream_caster 与 SrsSource，避免新造“统一接口”层与重复封装；
- **更好延迟**：少一次潜在内存拷贝/排队点；Jitter/背压统一由核心处理；
- **长期可维护**：配置/日志/HTTP API/监控与上游保持一致，新协议只需新增一个 IAdapter；
- **风险更低**：问题可在适配器边界回退，不影响核心分发链。

**备选方案**（权衡）：
- **Sidecar/IPC 网关**（进程外解析后喂入）：隔离最好，但多一次 IPC；适合闭源库/高安全场景。
- **Ingress 深度重构**（把 Caster 上提为第一公民）：控制力最强但重构面大，维护成本高。

**影响**：
- 初期需要一层“桥接文件”（如 `srs_app_caster_myproto.cpp`）以挂入工厂；
- 适配器需严格遵守“标准帧”契约（毫秒时基、参数集、关键帧门控）。

**验证与量化目标（KPI）**：
- 首帧时间 ≤ 800ms（RTMP 拉流基准环境）；
- 直播稳态端到端延迟：低延迟预设 0.8–2.5s、常规 3–5s；
- 乱序窗口 ≤ 100–200ms（可配），丢帧率 < 0.5%；
- 适配器 CPU 开销 < 10%（以 1080p@4Mbps 单路为基准）；
- 零/近零拷贝命中率 ≥ 95%；
- 回归：RTMP/HLS/HTTP-FLV/WebRTC/SRT 全链路通过。

**回滚策略**：
- 以协议为粒度可以随时关闭某个 `stream_caster { caster myproto; }`；
- 预留旁路：适配器可切换到文件录制/pcap 回放模式辅助排障。

---

## 针对原方案的改进（吸取教训）

结合最初“自建统一接口”的思路，我们总结如下优化并纳入实施：

1. **删除自建“统一流入接口”层** → 直接对接 `SrsSource` 的 `on_video/on_audio`，避免重复封装与潜在拷贝。
2. **统一时基为毫秒**，明确 `90kHz → ms = ts/90`；在适配器内完成，核心无需再换算。
3. **关键帧门控**：默认等 IDR（或 CRA），必要时“热启动 + 参数集补发”，减少花屏与首帧飘移。
4. **最小缓冲原则**：适配器仅保留组帧与极薄乱序窗口（默认 ≤100ms），严禁大队列。
5. **近零拷贝**：StdFrame payload 直接引用解析缓冲（引用计数/内存池），与核心通过共享消息传递。
6. **背压外显**：以 `SrsSource` enqueue 结果为反馈，适配器在低延迟模式下可策略性丢 B 帧或限速。
7. **参数集管理**：维护最新 SPS/PPS（或 VPS/SPS/PPS）缓存，IDR 时附带，流切换时主动补发。
8. **错误恢复**：keepalive/重连/重邀（如适用），时间回退或戳震荡时自愈到最近关键帧。
9. **配置对齐**：严格复用 `stream_caster` 语义，所有私参置于 `caster myproto` 作用域下；支持端口级默认路由。
10. **观测完善**：为每路连接打日志上下文 ID，暴露首帧耗时、抖动窗口命中率、近零拷贝命中率、丢帧率等指标。

---

## 二次开发方案（优化版要点）

- **接口**：`IAdapter.start/feed/parseFrame/flush/close` 保持不变；`StdFrame` 明确 `codec/dts_ms/pts_ms/keyframe/extradata/payload`。
- **FrameBus**：固定容量环形缓冲 + 高/低水位；溢出优先丢非关键视频帧，避免回拖。
- **Jitter 策略**：默认 80–120ms；低延迟预设可降到 0–60ms；可配置关闭（实验/电竞场景）。
- **时间戳**：B 帧纠偏保证 `dts < pts`；音频按采样精度累加避免漂移；跨源切换时做对齐。
- **内存管理**：内存池与分片复用；StdFrame 与网络缓冲共享生命周期（RAII + 引用计数）。
- **事件流**：`on_start_stream`/`on_stop_stream` 明确状态迁移；异常重连后自动等待关键帧再恢复推送。
- **安全与健壮**：长度/边界校验、异常 NALU 过滤、速率限制防洪；
- **HTTP API**：`/api/v1/adapters` 暴露每路指标与最近错误（后续版本）。

---

## 里程碑（优化版）与验收标准

**M1 最小可用**（目标：跑通）
- H.264/AAC、TCP 输入、固定路由；
- 首帧时间 ≤ 1s；端到端延迟 ≤ 3s（RTMP 基准）；
- 完成日志上下文与基础指标。

**M2 协议增强**（目标：稳定）
- JitterBuffer 与 B 帧纠偏；参数集管理完善；
- 乱序窗口 ≤ 100–200ms；丢帧率 < 0.5%；
- 异常恢复（重连/回退到关键帧）。

**M3 观测与性能**（目标：可运营）
- 近零拷贝命中率 ≥ 95%；
- 适配器 CPU 开销 < 10%（1080p@4Mbps 单路）；
- HTTP API/metrics 完成，回归全协议分发通过。

---

## 对照表（原方案 vs 现方案）

| 维度 | 原方案：自建统一流入接口 | 现方案：Adapter/Caster 插件化 |
|---|---|---|
| 架构侵入 | 中（新增一层） | 低（复用 stream_caster + SrsSource） |
| 延迟 | 中（多一处封装/排队） | 低（路径更短） |
| 开发成本 | 首轮低，后续高（每协议需碰统一层） | 首轮中，后续低（新增一个 IAdapter 即可） |
| 可维护性 | 中（自有接口需长期维护） | 高（对齐上游配置/日志/监控） |
| 回退/隔离 | 中 | 高（协议粒度可关闭） |

> 结论：在“开发难度 × 延迟 × 可维护性”的综合目标下，现方案优于原方案；仅当需要强隔离/异语言时，才考虑 Sidecar/IPC 备选。

---

## 追加 Checklist（优化版）

- [ ] 鼓励在适配器内启用 **IDR 门控** 与 **参数集补发**
- [ ] **近零拷贝**：StdFrame payload 引用解析缓冲（引用计数/内存池）
- [ ] **乱序窗口**：默认 100ms，可配 0–200ms
- [ ] **背压策略**：低延迟模式可丢 B 帧/限速；常规模式平滑排队
- [ ] **时戳对齐**：90kHz→ms 与 B 帧 `dts<pts` 校验
- [ ] **错误恢复**：心跳/重连/关键帧恢复
- [ ] **观测**：首帧时间/近零拷贝命中率/丢帧率/乱序命中率
- [ ] **API**：`/api/v1/adapters` 统计输出
- [ ] **回归**：RTMP/HLS/HTTP-FLV/WebRTC/SRT 全链路

