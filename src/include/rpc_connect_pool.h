#ifndef RPC_CONNECT_POOL_H
#define RPC_CONNECT_POOL_H

#include<string>
#include<unordered_map>
#include<atomic>
#include<mutex>
#include<chrono>
#include<condition_variable>
#include<memory>
#include<cstdint>
#include<cstddef>
#include<map>
#include"LockFreeQueue.h"

enum class BorrowError{none,pool_full,socket_failed,connect_failed};

struct BorrowFailure{
	BorrowError error=BorrowError::none;
	int system_error=0;
	bool can_failover()const;
	std::string describe()const;
};

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

    int BorrowConnection(const std::string &ip,uint16_t port,BorrowFailure* failure=nullptr,std::chrono::milliseconds wait_timeout=std::chrono::milliseconds(50));
    std::map<std::string,std::uint64_t> GetFailureCounts();
    void ReturnConnection(const std::string &ip , uint16_t port ,int fd ,bool is_bad = false);
    void WarmUp(const std::string &ip , uint16_t port ,int count);

private:
    RpcConnectPool();
    ~RpcConnectPool();
    int m_max_conn_per_node;
    std::unordered_map<std::string, ConnectionBucket *> m_pools;
    std::mutex m_global_mtx; // Protect bucket lookup/creation and failure snapshots.
	std::map<std::string,std::uint64_t> m_failures;
	std::condition_variable m_connection_returned_cv;
	std::uint64_t m_pool_change_version=0;
};

#endif
