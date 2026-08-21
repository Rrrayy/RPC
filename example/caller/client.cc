#include "rpc_application.h"
#include "../user.pb.h"
#include "rpc_controller.h"
#include <iostream>
#include <atomic>
#include <thread>
#include <chrono>
#include "rpc_logger.h"
#include "rpc_connect_pool.h"
#include "rpc_channel.h"

void send_request(int thread_id, std::atomic<int> &success_count, std::atomic<int> &fail_count, int requests_per_thread)
{
    // 增加 STUB_OWNS_CHANNEL 自动管理 Channel 内存
    fixbug::UserServiceRpc_Stub stub(new RpcChannel(false), google::protobuf::Service::STUB_OWNS_CHANNEL);

    fixbug::LoginRequst request;                       // 你的请求类（proto 拼写 LoginRequst，保持）
    request.set_username("zhangsan");                  // 你的字段
    request.set_password("123456");

    fixbug::LoginResponse response;
    RpcController controller;

    for (int i = 0; i < requests_per_thread; ++i)
    {
        controller.Reset();

        stub.login(&controller, &request, &response, nullptr);   // 小写 login（你的 proto 定义）

        if (controller.Failed())
        {
            fail_count++;
        }
        else
        {
            if (0 == response.errcode())              
            {
                success_count++;
            }
            else
            {
                fail_count++;
            }
        }
    }
}

int main(int argc, char **argv)
{
    RpcApplication::Init(argc, argv);
    FLAGS_logbufsecs = 5;
    RpcLogger logger("MyRPC");
    std::string ip = RpcApplication::GetInstance().GetConfig().Load("rpcserverip");
    uint16_t port = atoi(RpcApplication::GetInstance().GetConfig().Load("rpcserverport").c_str());

    // 预热 100 个长连接
    const int thread_count = 100;
    RpcConnectPool::GetInstance().WarmUp(ip, port, thread_count);

    const int requests_per_thread = 1000;

    std::vector<std::thread> threads;
    std::atomic<int> success_count(0);
    std::atomic<int> fail_count(0);

    auto start_time = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < thread_count; i++)
    {
        threads.emplace_back([argc, argv, i, &success_count, &fail_count, requests_per_thread]()
                             { send_request(i, success_count, fail_count, requests_per_thread); });
    }

    for (auto &t : threads)
    {
        t.join();
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> elapsed = end_time - start_time;

    LOG(INFO) << "Total requests: " << thread_count * requests_per_thread;
    LOG(INFO) << "Success count: " << success_count;
    LOG(INFO) << "Fail count: " << fail_count;
    LOG(INFO) << "Elapsed time: " << elapsed.count() << " seconds";
    LOG(INFO) << "QPS: " << (thread_count * requests_per_thread) / elapsed.count();

    return 0;
}