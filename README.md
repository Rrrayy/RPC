# Lightweight RPC Framework

<div align="center">

基于 C++17 的学习型 RPC 框架，覆盖网络通信、Protobuf 编解码、服务注册发现、连接池和一致性哈希。

![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)
![CMake](https://img.shields.io/badge/build-CMake-064F8C.svg)
![Linux](https://img.shields.io/badge/platform-Linux-orange.svg)
![Status](https://img.shields.io/badge/status-learning-green.svg)

</div>

## 项目简介

本项目用于学习和实践 RPC 框架的核心设计：客户端通过 Stub 发起调用，服务发现模块选择服务实例，连接池复用 TCP 长连接，服务端通过网络线程接收请求并交给业务线程池执行。

当前项目已经完成基础同步调用、统一错误响应和主要异常路径验证，定位为“可运行、可扩展、持续完善”的学习型工程。

## 核心能力

| 模块 | 当前能力 |
| --- | --- |
| 网络层 | 基于 muduo 的 Reactor 模型，支持 TCP 粘包/半包拆包 |
| 序列化 | Protobuf 消息序列化、反射分发和 Stub 调用 |
| 服务治理 | Zookeeper 服务注册、发现和节点变更监听 |
| 负载均衡 | 基于虚拟节点的一致性哈希 |
| 连接管理 | TCP 长连接池和连接预热 |
| 错误处理 | `RpcResponse` 统一错误码、错误信息和业务 payload |
| 构建部署 | CMake 构建，提供 Docker Compose 示例 |

## 调用流程

```text
Client Stub
    │
    ▼
Service Discovery ──► Consistent Hash
    │
    ▼
Connection Pool ──► Serialize and Send
    │
    ▼
TCP / muduo
    │
    ▼
RpcProvider ──► Decode ──► Dispatch ──► Business Thread Pool
    │
    ▼
RpcResponse

Zookeeper：服务注册、实例发现和节点变更通知
```

## 协议概览

请求帧结构：

```text
+----------------+----------------+----------------+----------------+
| total_len (4B) | header_len(4B) | RpcHeader      | args           |
+----------------+----------------+----------------+----------------+
```

响应使用统一的 Protobuf 信封：

```protobuf
message RpcResponse {
    int32 error_code = 1;
    string error_message = 2;
    bytes payload = 3;
}
```

当前错误码约定：

| 错误码 | 含义 |
| ---: | --- |
| `0` | 成功 |
| `1` | `SERVICE_NOT_EXIST` |
| `2` | `METHOD_NOT_EXIST` |
| `3` | `INVALID_REQUEST` |

## 技术栈

`C++17` · `muduo` · `Protobuf` · `Zookeeper` · `glog` · `CMake` · `Docker Compose`

## 目录结构

```text
RPC-git/
├── src/                 # RPC 核心实现和协议文件
│   ├── include/         # 公共头文件
│   └── *.cc / *.proto
├── example/             # 客户端、服务端和示例 Protobuf
├── test/                # 单元测试和故障验证
├── third_party/         # 第三方依赖
├── CMakeLists.txt
├── test.conf
├── Dockerfile
└── docker-compose.yml
```

## 快速开始

### 构建

需要 Ubuntu/Linux、CMake、C++17 编译器、Protobuf、Zookeeper、glog 和 muduo 环境。

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j"$(nproc)"
```

### 启动本地示例

先确保 Zookeeper 已启动，并根据本机环境检查 `test.conf`：

```bash
./build/server -i ./test.conf
```

另开终端运行客户端：

```bash
./build/client -i ./test.conf
```

### 运行测试

```bash
./build/config_test -i ./test.conf
./build/controller_test
```

## Docker 部署

```bash
docker compose build
docker compose up -d zookeeper rpc-server
docker compose run --rm rpc-client
docker compose down
```

Docker 配置用于本地演示，正式部署前仍需完善服务就绪检查、服务名解析和运行参数管理。

## 当前验证结果

当前基线为 Ubuntu Debug 构建、单服务实例、约 5 ms 模拟业务耗时：

| 场景 | 请求数 | 成功 | 失败 | 结果 |
| --- | ---: | ---: | ---: | --- |
| 正常 RPC 调用 | 100000 | 100000 | 0 | 通过 |
| 未知服务 | 100000 | 0 | 100000 | 收到 `SERVICE_NOT_EXIST` |
| 未知方法 | 100000 | 0 | 100000 | 收到 `METHOD_NOT_EXIST` |
| 非法请求参数 | 100000 | 0 | 100000 | 收到 `INVALID_REQUEST` |

正常调用本次实测 QPS 约为 `6738`。该数据仅代表当前机器、构建模式、并发度和业务耗时下的结果，不作为通用性能承诺。

## 开发状态

已完成：

- `RpcController` 基础实现和单元测试
- 统一 `RpcResponse` 错误响应
- 未知服务、未知方法和非法请求错误处理
- 基础服务发现、连接池和一致性哈希调用链

计划完善：

- 请求超时、取消、重试和真正的 `request_id` 响应匹配
- 连接池状态治理、坏连接剔除和服务端重启恢复
- Zookeeper 快照、健康检查和优雅摘流
- 协议边界测试、Sanitizer、Release 基准和 CI


## 贡献方式

```text
main
  └── fix/<feature-or-bug>
        └── test → commit → push → Pull Request → merge
```

建议每个改动独立分支、独立提交，并在 PR 描述中记录问题、方案和验证结果。

## License

本项目用于学习和工程实践。
