# RPC — 基于 muduo 的轻量级 RPC 框架

C++17 实现的高性能 RPC 框架：Protobuf 序列化 + 自定义二进制协议 + muduo 多 Reactor 网络层 + Zookeeper 服务注册与发现 + 一致性哈希负载均衡 + 无锁连接池。

## 性能

| 指标 | 数据 |
|------|------|
| 总请求 | 100,000 次 |
| 成功率 | 100%（0 失败） |
| QPS | 6,700+（单机 Debug，含模拟业务耗时 5ms/请求） |

## 架构

```
客户端
  Kclient -> Stub（代理）-> RpcChannel::CallMethod
    1. ServiceDiscovery：ZK 拉实例 + 一致性哈希选节点
    2. ConnectPool：无锁队列借长连接
    3. 组包 [total_len|header_len|RpcHeader|args] -> send
    4. recv_exact 收 [len|body] -> 解析 -> 还连接
服务端
  RpcProvider：muduo（1 main loop + 4 IO 线程）
    1. OnMessage：while 拆包 -> 双层查表 -> 反射 New()
    2. 业务线程池（100 线程）执行 CallMethod
    3. done 闭包回调 -> SendRpcResponse 跨线程回包
    4. 启动时注册 ZK：持久节点 + 临时实例节点
注册中心：Zookeeper（服务注册 / Watcher 动态感知）
```

## 技术栈

- C++17（智能指针 / 线程池 / 原子操作 / 读写锁）
- Protobuf（消息序列化 + 反射 + stub 生成）
- muduo（Reactor 网络库，多线程事件循环）
- Zookeeper（服务注册与发现）
- glog（日志系统，RAII 封装）
- CMake（工程化构建）

## 核心特性

1. 自定义二进制协议：`[total_len(4B) | header_len(4B) | RpcHeader | args]` 定长头拆包，解决 TCP 粘包/半包
2. Protobuf 反射分发：服务注册表 + `GetRequestPrototype().New()` 原型创建 + `CallMethod` 虚函数分发——框架不认识业务类也能调用
3. IO 与业务解耦：muduo IO 线程只管收发拆包，业务逻辑丢 100 线程池执行，done 闭包回调跨线程回包
4. ZK 服务注册发现：持久节点存服务路径、临时节点（ZOO_EPHEMERAL）存实例地址，进程断线自动摘除；Watcher 监听实例变化动态更新
5. 一致性哈希负载均衡：虚拟节点 + 有序哈希环 + 二分查找，增删节点只影响局部；后台线程检测负载偏差动态调参
6. 无锁连接池：MPMC 无锁环形队列（CAS 序列号法，防伪共享）管理长连接，预热 100 条，借还 O(1) 零锁竞争

## 目录结构

```
rpc/
├── src/
│   ├── include/          # 头文件（框架全部组件）
│   │   ├── rpc_provider.h        # 服务端：注册/启动/拆包/反射分发/回包
│   │   ├── rpc_channel.h         # 客户端：CallMethod 组包/收发
│   │   ├── service_discovery.h   # 服务发现：ZK 拉实例 + Watcher
│   │   ├── consistent_hash.h     # 一致性哈希 + 负载均衡
│   │   ├── rpc_connect_pool.h    # 无锁连接池
│   │   ├── LockFreeQueue.h       # MPMC 无锁队列（模板）
│   │   ├── rpc_controller.h      # RPC 调用状态控制器
│   │   ├── zookeeperutil.h       # ZK 客户端封装
│   │   ├── rpc_config.h          # 配置文件解析
│   │   ├── rpc_application.h     # 框架入口（单例）
│   │   └── rpc_logger.h          # glog 日志封装
│   └── *.cc               # 对应实现
├── example/
│   ├── callee/server.cc   # 服务端示例（UserService.Login）
│   └── caller/client.cc   # 客户端压测示例（100 线程 x 1000 次）
├── test.conf              # 配置文件
└── CMakeLists.txt
```

## 快速开始

```bash
# 1. 启动 Zookeeper
cd /usr/share/zookeeper/bin && sudo ./zkServer.sh start

# 2. 编译
cd build && cmake .. && make -j$(nproc)

# 3. 启动服务端
./server -i ../test.conf
# -> service_name=UserServiceRpc / zookeeper_init success / RpcProvider start...

# 4. 启动客户端（100 线程 x 1000 次 = 10 万次调用）
./client -i ../test.conf
# -> Total requests: 100000 / Success: 100000 / Fail: 0 / QPS: 6700+
```

## 压测方式

- 100 线程并发 x 每线程 1000 次 = 10 万次真实 RPC 调用
- 连接池预热 100 条长连接，避免握手开销
- 业务端模拟查库 5ms/请求
- 结果：10 万次 0 失败，QPS 6700+


