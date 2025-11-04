# QUIC库集成计划

## 目标
集成lsquic库到SRS构建系统，替换QUIC+FEC adapter中的QUIC协议占位实现。

## 步骤

### 1. 构建系统集成
- [x] 在`options.sh`中添加`SRS_QUIC`选项
- [ ] 在`depends.sh`中添加lsquic构建逻辑
- [ ] 在`setup_variables.sh`中添加lsquic目录创建
- [ ] 在`configure`中添加lsquic链接选项

### 2. QUIC会话包装类
- [ ] 创建`QuicSessionWrapper`类封装lsquic API
- [ ] 实现连接建立和管理
- [ ] 实现datagram接收
- [ ] 实现错误处理和超时

### 3. 替换占位代码
- [ ] 修改`QuicUdpHandler`使用真实QUIC库
- [ ] 修改`QuicFecTsAdapter`使用真实QUIC会话
- [ ] 更新协议解析逻辑

## 技术选择

### 选择lsquic的原因
1. 纯C库，易于C++集成
2. 活跃维护，文档完善
3. 支持datagram和stream
4. 性能优异

### 依赖项
- lsquic需要：zlib, BoringSSL/OpenSSL
- SRS已有OpenSSL，可直接复用

## 实现细节

### lsquic基本API使用
```c
// 创建引擎
struct lsquic_engine_api api;
api.ea_packets_out = packets_out;
api.ea_packets_out_ctx = ctx;
engine = lsquic_engine_new(LSENG_SERVER, &api);

// 接收UDP数据包
lsquic_engine_packet_in(engine, data, size, &peer);

// 发送数据
lsquic_conn_send(conn, data, size);

// datagram接收回调
int on_datagram(void *ctx, const void *data, size_t size) {
    // 处理接收到的datagram
}
```

