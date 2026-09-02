#ifndef _RpcProvider_H
#define _RpcProvider_H
#include "google/protobuf/service.h"
#include "zookeeperutil.h"
#include <muduo/net/TcpServer.h>
#include <muduo/net/EventLoop.h>
#include <muduo/net/InetAddress.h>
#include <muduo/net/TcpConnection.h>
#include <muduo/base/ThreadPool.h>
#include <google/protobuf/descriptor.h>
#include <functional>
#include <string>
#include <unordered_map>

class RpcProvider{
public:
    void NotifyService(google::protobuf::Service* service);
    ~RpcProvider();
    void Run();
private:
    muduo::net::EventLoop event_loop;
    muduo::ThreadPool m_thread_pool;
    struct ServiceInfo{
        google::protobuf::Service* service;
        std::unordered_map<std::string, const google::protobuf::MethodDescriptor*> method_map;
    };
    std::unordered_map<std::string, ServiceInfo>service_map;
    void OnConnection(const muduo::net::TcpConnectionPtr& conn);
    void OnMessage(const muduo::net::TcpConnectionPtr& conn , muduo::net::Buffer* buffer, muduo::Timestamp receive_time);
    void SendRpcResponse(const muduo::net::TcpConnectionPtr& conn, google::protobuf::Message* response, google::protobuf::Message* request);

};
#endif

// RpcProvider
// ├── event_loop       主事件循环
// ├── m_thread_pool    业务线程池
// ├── service_map      服务名 → ServiceInfo
// │   ├── service      业务服务对象
// │   └── method_map   方法名 → MethodDescriptor
// ├── OnConnection     连接回调
// ├── OnMessage        消息回调
// └── SendRpcResponse  响应发送