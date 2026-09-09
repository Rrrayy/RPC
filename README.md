# RPC Framework

基于 C++17、muduo、Protobuf 和 Zookeeper 实现的轻量级 RPC 框架，覆盖客户端调用、协议编解码、服务端分发、服务注册发现、负载均衡和容器化部署。

## Features

- 自定义二进制协议，支持 TCP 粘包与半包处理
- Protobuf 序列化、反射分发与 Stub 生成
- 基于 muduo 的 Reactor 网络模型
- IO 线程与业务线程池解耦
- Zookeeper 服务注册、发现与实例变更监听
- 一致性哈希服务实例选择
- TCP 长连接池与连接预热
- Docker Compose 编排 Zookeeper、RPC Server 和 RPC Client

## Architecture

```text
Client
  └─ Stub
      └─ Service Discovery
          └─ Connection Pool
              └─ Serialize and Send
                  └─ TCP
                      └─ RPC Server
                          ├─ Receive and Decode
                          ├─ Method Dispatch
                          ├─ Business Thread Pool
                          └─ Response Callback

Zookeeper: service registration and discovery
```

一次调用的主要流程：客户端通过 Stub 发起调用，服务发现模块选择实例，连接池提供长连接，请求经过序列化和协议组包后发送到服务端。服务端网络线程负责接收和拆包，业务线程池完成反序列化与业务执行，最后通过回调返回响应。

## Protocol

```text
+----------------+----------------+----------------+----------------+
| total_len (4B) | header_len(4B) | RpcHeader      | args           |
+----------------+----------------+----------------+----------------+
```

- `total_len`：协议头之后的完整消息长度
- `header_len`：序列化后的 RPC 元数据长度
- `RpcHeader`：服务名、方法名、参数长度等元数据
- `args`：Protobuf 序列化后的业务参数

服务端根据长度字段循环拆包。数据不完整时保留缓冲区并等待下一次读取，完整解析后继续处理同一缓冲区中的后续请求。

## Service Discovery and Load Balancing

服务端向 Zookeeper 注册服务路径和实例地址。实例地址使用临时节点保存，进程退出或会话断开后由 Zookeeper 自动清理。客户端通过 Watcher 感知实例列表变化，并使用一致性哈希选择服务节点。

一致性哈希采用虚拟节点和有序哈希环，节点变化时只影响局部 key 的映射，减少路由迁移范围。

## Technology Stack

| Component | Purpose |
| --- | --- |
| C++17 | Framework implementation |
| muduo | Reactor network layer |
| Protobuf | Serialization, reflection and Stub generation |
| Zookeeper | Service registration and discovery |
| glog | Logging |
| CMake | Build system |
| Docker Compose | Container orchestration |

## Project Structure

```text
rpc/
├── src/
│   ├── include/
│   │   ├── rpc_provider.h
│   │   ├── rpc_channel.h
│   │   ├── service_discovery.h
│   │   ├── consistent_hash.h
│   │   ├── rpc_connect_pool.h
│   │   ├── rpc_controller.h
│   │   ├── zookeeperutil.h
│   │   ├── rpc_config.h
│   │   └── rpc_application.h
│   └── *.cc
├── example/
├── third_party/muduo/
├── Dockerfile
├── docker-compose.yml
├── test.conf
├── test.docker.conf
└── CMakeLists.txt
```

## Build and Run

```bash
mkdir -p build
cd build
cmake ..
make -j$(nproc)
```

启动 Zookeeper、服务端和客户端：

```bash
sudo /usr/share/zookeeper/bin/zkServer.sh start
./server -i ../test.conf
./client -i ../test.conf
```

## Docker Deployment

Docker Compose 将 Zookeeper、RPC Server 和 RPC Client 放入同一个 bridge 网络。当前配置使用固定容器 IP，以兼容项目现有的地址解析和服务注册逻辑。

```bash
docker compose build
docker compose up -d zookeeper rpc-server
docker compose run --rm rpc-client
docker compose ps
docker compose logs --tail=50 rpc-server
docker compose down
```

## Benchmark

测试配置为 100 个并发线程，每个线程执行 1000 次 RPC 调用，共 100000 次请求；服务端包含约 5 ms 的模拟业务耗时。

| Environment | Requests | Success | Failures | QPS |
| --- | ---: | ---: | ---: | ---: |
| Ubuntu native | 100000 | 100000 | 0 | 约 16952 |
| Docker Compose | 100000 | 100000 | 0 | 约 16633 |

两种环境均完成 100000 次调用且失败数为 0。测试结果仅用于当前实现和测试环境下的对比参考。

## Future Improvements

- 使用服务名解析替代固定容器 IP
- 增加 Zookeeper 健康检查和服务就绪等待
- 增加请求超时、重试、取消和熔断机制
- 完善连接失效检测与连接池动态扩缩容
- 增加协议版本、校验和及统一错误码
- 补充单元测试、集成测试和持续集成配置

## License

This project is intended for learning and engineering practice.

