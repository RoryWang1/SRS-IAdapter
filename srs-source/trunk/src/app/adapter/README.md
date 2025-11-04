# Adapter 模块目录结构

```
adapter/
├── core/                      # 核心接口和框架
│   ├── iadapter.hpp          # Adapter抽象接口定义
│   ├── adapter_manager.hpp/cpp    # Adapter管理器
│   ├── adapter_listener.hpp/cpp   # Adapter监听器
│   └── adapter_stats.hpp/cpp      # Adapter统计信息
│
├── common/                    # 通用数据结构
│   ├── std_frame.hpp         # 标准帧定义
│   └── e2e_tester.hpp        # 端到端测试工具
│
├── components/                # 功能组件
│   ├── frame/                # 帧处理组件
│   │   ├── frame_bus.hpp/cpp         # 帧总线
│   │   ├── frame_to_source_bridge.hpp # 帧到Source桥接
│   │   └── jitter_buffer.hpp          # 抖动缓冲
│   │
│   ├── fec/                  # FEC相关组件
│   │   ├── fec_group_buffer.hpp/cpp   # FEC组缓冲
│   │
│   ├── reorder/              # 乱序处理组件
│   │   ├── reorder_buffer.hpp/cpp     # 乱序缓冲区
│   │
│   └── parameter/            # 参数管理组件
│       └── parameter_set_manager.hpp  # 参数集管理器
│
└── adapters/                 # 具体适配器实现
    ├── myproto_adapter.cpp            # MyProto协议适配器
    └── quic_fec_ts_adapter.hpp/cpp    # QUIC+FEC TS适配器
```

