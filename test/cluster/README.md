# 双节点分布与节点下线、恢复实验

这是独立的集成测试工具，使用现有 RPC 框架、Protobuf 协议、ZooKeeper 服务发现和连接池。原来的示例 `client`、`server` 源文件保持不变；同步调用目前支持发送前最多一次备用节点建连，详见下文。

## 文件职责

| 文件 | 职责 |
| --- | --- |
| `server.cc` | 测试专用服务，保留 5ms 模拟业务延迟；响应 `msg` 返回本节点地址；原子计数器每完成 1000 次业务处理输出一个里程碑 |
| `client.cc` | 通过真实 RPC Stub 发起同步调用，按响应节点和错误原因汇总，支持两个测试模式 |
| `node-8000.conf` / `node-8001.conf` | 两个服务端的地址及 ZooKeeper 配置 |
| `distribution.conf` | 100 线程，每线程 1000 次，共 100000 次调用，无额外异步探测 |
| `failover.conf` | 20 个持续调用线程，手动触发节点下线与重新上线，分五段统计 |

**不能用原来的服务端代替测试服务端。** 原服务返回 `login success`，无法证明响应来自哪个节点；本测试会将此标记为 `unexpected server identity`，而不是误判成功。

## 1. 编译

以下命令在 Ubuntu 虚拟机执行，每条命令独占一行，避免把反斜杠与参数粘在同一行。

```bash
cd /mnt/hgfs/RPC-git
cmake -S /mnt/hgfs/RPC-git -B /home/rr/build/rpc-release -DCMAKE_BUILD_TYPE=Release
cmake --build /home/rr/build/rpc-release --target cluster_test_server cluster_test_client -j"$(nproc)"
```

当前项目在 CMake 中显式设置 Release 为 `-O2`。不要仅凭缓存中的默认值判断实际编译参数。

## 2. 准备两个测试实例

先在各自服务终端停止旧的 8000、8001 示例服务，避免端口冲突或混用服务。保留本机 ZooKeeper 运行。

检查旧监听是否已经退出：

```bash
ss -lntp '( sport = :8000 or sport = :8001 )'
```

确认旧的临时注册节点已消失，再启动新实例。会话清理可能有延迟，不能仅凭端口释放就假定 ZooKeeper 节点已清理：

```bash
/usr/share/zookeeper/bin/zkCli.sh -server 127.0.0.1:2181 ls /UserServiceRpc
```

终端 A：

```bash
/home/rr/build/rpc-release/cluster_test_server -i /mnt/hgfs/RPC-git/test/cluster/node-8000.conf
```

终端 B：

```bash
/home/rr/build/rpc-release/cluster_test_server -i /mnt/hgfs/RPC-git/test/cluster/node-8001.conf
```

终端 C 再查询节点列表，必须同时出现 `127.0.0.1:8000` 和 `127.0.0.1:8001`。若缺少节点，不进入正式测试；先查对应服务的注册日志。重启恢复注册不等于修复注册生命周期缺陷。

## 3. 实验一：双节点请求分布

终端 C：

```bash
timeout --signal=TERM --kill-after=5s 60s /home/rr/build/rpc-release/cluster_test_client -i /mnt/hgfs/RPC-git/test/cluster/distribution.conf
echo "exit_code=$?"
```

输出字段：

- `RESULT phase=distribution`：这一次客户端观察窗口的秒数、成功数、失败数。
- `attempt_qps`：成功与失败的合计调用数 / 实际窗口时长。
- `success_qps`：成功响应数 / 实际窗口时长。
- `NODE responses`：客户端真正收到并校验成功、且归属于该节点的响应数。
- `success_share_pct`：该节点成功响应数占本窗口全部成功响应数的比例。
- `delivered_qps`：该节点成功响应数 / 同一个客户端窗口时长，不是节点独立处理能力上限。
- `ERROR`：按原因聚合的失败次数，不逐请求刷日志。

自动通过条件：两个预期节点均返回成功响应、没有失败、成功数与失败数之和恰好为 100000。通过时 `VERDICT PASS`，退出码为 0；不通过为 1；配置或启动异常为 2。

一致性哈希不保证严格 50:50，不设置凭空编造的均匀度合格线。记录实际比例，必要时再分析虚拟节点和哈希环行为。

服务端 `business_completed` 是当前进程准备好的业务响应累计计数，每 1000 次打印一次；并不代表响应已送达客户端，也不是最终精确总数。精确的成功响应分布以客户端统计为准。服务端重启后计数归零，不把旧进程和新进程的数字直接相减。

## 4. 实验二：同一客户端中的节点下线与恢复

保持 A、B 两个测试实例在线。终端 C 运行以下命令，必须保留标准输入，不要将其放在后台或关闭 stdin：

```bash
timeout --signal=TERM --kill-after=5s 180s /home/rr/build/rpc-release/cluster_test_client -i /mnt/hgfs/RPC-git/test/cluster/failover.conf
echo "exit_code=$?"
```

整个过程保持同一客户端进程、同一组线程和 Stub，不通过重启客户端来绕过 watcher 更新。

| 阶段 | 操作及观察 | 默认时长 | 自动判定 |
| --- | --- | --- | --- |
| `baseline` | 两节点均在线 | 10 秒 | A、B 均有成功响应且零失败 |
| `down_transition` | 出现提示后，在终端 B 按 Ctrl+C；回到 C 输入 `yes` 并回车 | 人工操作耗时 + 10 秒收敛观察 | 记录失败原因和次数，不要求零失败 |
| `down_stable` | B 保持下线，继续请求 A | 10 秒 | 仅 A 返回成功响应且零失败 |
| `up_transition` | 出现提示后，在终端 B 用原命令启动测试服务；确认 B 已注册，再回 C 输入 `yes` | 人工操作耗时 + 10 秒收敛观察 | 记录重新注册和连接恢复期间的结果 |
| `recovered_stable` | A、B 保持在线 | 10 秒 | A、B 均有成功响应且零失败 |

重新启动 B 的命令：

```bash
/home/rr/build/rpc-release/cluster_test_server -i /mnt/hgfs/RPC-git/test/cluster/node-8001.conf
```

提示等待期间工作线程仍在请求，不是暂停客户端等操作。输入非 `yes` 或关闭 stdin 会终止实验并判定失败。

观察客户端哈希环日志是否出现 `2→1→2`，并与各阶段的实际节点响应分布一起保存。自动判定检查响应行为，不解析日志中的节点数量。`PASS` 不证明特定毫秒级恢复时间，也不证明过渡期零丢失。

阶段按**调用完成时刻**归属：跨越阶段边界的在途请求计入完成时所在阶段。最终阶段包含停止提交后等待在途请求返回的排空时间。汇总按实际执行顺序输出，与 `PHASE` 日志及上表一致。

当前失败路径可能快速返回，因此故障测试对每次最终失败后的下一次调用增加 10ms 测试侧节流，防止错误空转。这不是测试工具重试；此模式的 QPS 不能与原始性能压测直接比较。

## 5. 发送前备用节点优化与回归

同步调用借连接时，框架保存失败类别、目标地址和失败当时的系统错误码。仅对 `connect_failed` 中的连接拒绝、连接超时、主机/网络不可达、连接重置，在尚未发送请求字节时，沿当前哈希环跳过原节点的所有虚拟节点，最多尝试另一个节点一次。不修改共享哈希环，不循环拉取 ZooKeeper，不重发已发送的业务请求。单节点无备用时直接保留原错误；连接池满、本机 socket 资源或权限错误不触发切换。

先编译并执行不依赖 ZooKeeper 的边界测试：

```bash
cmake --build /home/rr/build/rpc-release --target hash_alternative_test pool_borrow_test cluster_test_client cluster_test_server -j"$(nproc)"
/home/rr/build/rpc-release/hash_alternative_test
/home/rr/build/rpc-release/pool_borrow_test
```

新增 CMake 目标前需要按第 1 节重新运行配置。哈希测试检查空环、单节点排除、双节点备选和不修改原路由。连接池测试在本机保留未监听端口，重复 1030 次连接拒绝，检查错误码、失败名额回滚及随后恢复监听时的连接复用；它不是完整 RPC 故障转移测试，也不覆盖请求超时。

随后按第 3、4 节重跑两个集成测试，保留正常节点和故障注入方式不变，比较修改前后结果。新增输出：

- `BORROW_FAILURE`：整轮按目标地址、失败类型和 errno 汇总的借连接失败事件，包含被备用节点成功接管的调用，不等于最终失败请求数。
- `FAILOVER attempted`：整轮尝试备用节点建连的次数。
- `FAILOVER connected`：其中成功获得备用连接的次数。
- `FAILOVER completed`：其中最终完成协议校验和业务响应解析、未报告错误的次数。

这些计数是进程级累计值在本轮前后的差值，在工作线程全部退出后读取；不要把它们当成按阶段完成时刻归属的统计。应满足 `completed<=connected<=attempted`。若过渡期未实际触发备用节点，计数为零，不能据此声称优化路径已验证。

旧基线下线过渡期为 6789 次最终失败（6771 次笼统借连接失败、18 次响应头接收失败）。旧日志无法追溯 errno；不能将旧的 6771 次直接断言为全部连接拒绝。优化后只记录真实结果，不预填下降比例。

边界：本次未增加节点熔断、连接超时或冷却机制，仍可能在 ZooKeeper 收敛前反复尝试故障节点；备用建连也可能阻塞或失败。接收响应失败不重试，防止业务重复执行。本次仅增强同步路径，异步路径未加入备用节点重选。

## 边界和失败处理

- 这次测试验证请求分发和动态路由恢复；同步路径支持发送前一次备用建连，不提供已发送请求的重放、请求 deadline、熔断或幂等保证。
- 两个实验都使用 `done=nullptr` 的同步调用路径。保持 Channel 存活不等于验证了异步读取线程和 pending 请求的故障处理；异步容错需要独立验收。
- 当前同步网络 I/O 仍可能阻塞。外层 `timeout` 是实验级看门狗，不是 RPC 请求超时机制；退出码 124 通常表示总时长超限，137 可能表示被强制结束，应保留日志分析，不算测试通过。
- 这里只做受控进程停止/重启，不用 `tc` 制造黑洞网络，不用 `SIGSTOP` 挂起服务。
- `Ctrl+C` 结束的是进程，不代表框架已经实现优雅停机。以后若做 `SIGKILL` 实验，要单独记录注入方式和 ZooKeeper 会话过期时间，不能混为一个结果。
- 必须在同一注册命名空间里只运行这两个预期服务实例；并发的其他客户端会污染服务端累计计数。
- 两实例都在同一台虚拟机内，竞争 CPU 等资源；不能据此声称跨机器部署、高可用 SLA 或线性横向扩容。
- 测试新增节点身份、计数和汇总锁，且未预热连接，包含首次发现/建连成本。它是功能证据工具，不替换原来的吞吐基准。
- 若单节点稳定期仍有失败，或恢复稳定期只收到一个节点的响应，保存证据并定位缺陷，不通过放宽断言来制造通过结果。

## 结果记录与简历边界

每次记录：Git 提交号、构建参数、虚拟机 CPU/内存、ZooKeeper 版本、线程数、请求量、各阶段完整输出、节点停止/启动操作时间、服务端进程 PID 和注册日志。至少重复几轮，不能只选最好的一次。

本文件不预填实测成绩。只有实际执行通过后，才可以写“完成双实例请求分发及节点上下线动态路由验证”。若过渡期存在失败，应如实说明，不写“无损故障转移”或“自动重试”。
