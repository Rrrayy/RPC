#ifndef RPC_CONNECT_POOL_H
#define RPC_CONNECT_POOL_H

#include<string>
#include<unordered_map>
#include<atomic>
#include<mutex>
#include<memory>
#include"LockFreeQueue.h"

struct ConnectionBucket
{
    //无锁队列存放空闲fd
    std::unique_ptr<MPMCQueue<int>>  free_fds;
    //记录该地址当前已建立的连接总数
    std::atomic<int> active_count;

    ConnectionBucket(size_t capacity):
        free_fds(new MPMCQueue<int>(capacity)), active_count(0){}
};

class RpcConnectPool{
public:
    static RpcConnectPool &GetInstance();

    int BorrowConnection(const std::string &ip ,uint16_t port);
    void ReturnConnection(const std::string &ip , uint16_t port ,int fd ,bool is_bad = false);
    void WarmUp(const std::string &ip , uint16_t port ,int count);

private:
    RpcConnectPool();
    ~RpcConnectPool();
    int m_max_conn_per_node;
    std::unordered_map<std::string, ConnectionBucket *> m_pools;
    std::mutex m_global_mtx; // 仅用于创建新 Bucket 时加锁
};

#endif