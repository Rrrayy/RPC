#include "rpc_connect_pool.h"
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <cerrno>
#include <sys/socket.h>
#include <unistd.h>
#include "rpc_logger.h"

bool BorrowFailure::can_failover()const{
	// Resource exhaustion is not evidence that another service endpoint is healthy.
	return error==BorrowError::connect_failed&&
		(system_error==ECONNREFUSED||system_error==ETIMEDOUT||
		 system_error==EHOSTUNREACH||system_error==ENETUNREACH||system_error==ECONNRESET);
}

std::string BorrowFailure::describe()const{
	const char* reason="none";
	switch(error){
		case BorrowError::pool_full:reason="pool_full";break;
		case BorrowError::socket_failed:reason="socket_failed";break;
		case BorrowError::connect_failed:reason="connect_failed";break;
		case BorrowError::none:break;
	}
	return std::string(reason)+" errno="+std::to_string(system_error);
}

std::map<std::string,std::uint64_t> RpcConnectPool::GetFailureCounts(){
	std::lock_guard<std::mutex> lock(m_global_mtx);
	return m_failures;
}

// 假设每个 IP:Port 最多建立 1024 个长连接（必须是 2 的幂）
static const int MAX_CONN_SIZE = 1024;

RpcConnectPool &RpcConnectPool::GetInstance() {
    static RpcConnectPool instance;
    return instance;
}

RpcConnectPool::RpcConnectPool() : m_max_conn_per_node(MAX_CONN_SIZE) {}

RpcConnectPool::~RpcConnectPool() {
    std::lock_guard<std::mutex> lock(m_global_mtx);
    for (auto &pair : m_pools) {
        int fd;
        while (pair.second->free_fds->pop(fd)) {
            close(fd);
        }
        delete pair.second;
    }
}

//预热连接
void RpcConnectPool::WarmUp(const std::string &ip, uint16_t port, int count) {
    std::string key = ip + ":" + std::to_string(port);
    ConnectionBucket *bucket = nullptr;

    // 1. 确保 Bucket 存在
    {
        std::lock_guard<std::mutex> lock(m_global_mtx);
        auto it = m_pools.find(key);
        if (it == m_pools.end()) {
            bucket = new ConnectionBucket(MAX_CONN_SIZE);
            m_pools[key] = bucket;
        } else {
            bucket = it->second;
        }
    }

    int success_count = 0;
    // 2. 循环建立连接
    for (int i = 0; i < count; ++i) {
        // 检查是否超过最大连接限制
        if (bucket->active_count.load() >= m_max_conn_per_node) break;

        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd == -1) continue;

        // 设置非阻塞/禁用Nagle
        int opt = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

        struct sockaddr_in addr;
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = inet_addr(ip.c_str());

        // 执行握手
        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            // 成功后，原子增加计数并推入无锁队列
            bucket->active_count.fetch_add(1);
            if (!bucket->free_fds->push(fd)) {
                // 队列若满（理论上由 active_count 控制不会发生），则回滚
                close(fd);
                bucket->active_count.fetch_sub(1);
            } else {
                success_count++;
            }
        } else {
            close(fd);
        }
    }
    LOG(INFO) << "[ConnectPool] WarmUp complete: " << success_count << " connections ready for " << key;
}

int RpcConnectPool::BorrowConnection(const std::string &ip,uint16_t port,BorrowFailure* failure) {
    std::string key = ip + ":" + std::to_string(port);
	if(failure!=nullptr){*failure=BorrowFailure{};}
	auto fail=[&](BorrowError error,int system_error){
		const BorrowFailure result{error,system_error};
		if(failure!=nullptr){*failure=result;}
		std::lock_guard<std::mutex> lock(m_global_mtx);
		++m_failures[key+" "+result.describe()];
		return -1;
	};

    ConnectionBucket *bucket = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_global_mtx);
        auto it = m_pools.find(key);
        if (it == m_pools.end()) {
            bucket = new ConnectionBucket(MAX_CONN_SIZE);
            m_pools[key] = bucket;
        } else {
            bucket = it->second;
        }
    }

    int fd = -1;
    // 优先从无锁队列获取（WarmUp 过的连接会在这里被直接拿到）
    if (bucket->free_fds->pop(fd)) {
        return fd;
    }

    // 队列为空时才尝试动态创建
    if (bucket->active_count.load(std::memory_order_relaxed) < m_max_conn_per_node) {
        if (bucket->active_count.fetch_add(1) < m_max_conn_per_node) {
            fd = socket(AF_INET, SOCK_STREAM, 0);
			if(fd==-1){
				const int system_error=errno;
				bucket->active_count.fetch_sub(1);
				return fail(BorrowError::socket_failed,system_error);
			}
            int opt = 1;
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

            struct sockaddr_in addr;
            addr.sin_family = AF_INET;
            addr.sin_port = htons(port);
            addr.sin_addr.s_addr = inet_addr(ip.c_str());

            if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
                return fd;
            } else {
				// Capture errno before close or any bookkeeping can overwrite it.
				const int system_error=errno;
                close(fd);
                bucket->active_count.fetch_sub(1);
                return fail(BorrowError::connect_failed,system_error);
            }
        } else {
            bucket->active_count.fetch_sub(1);
        }
    }
    return fail(BorrowError::pool_full,0);
}

void RpcConnectPool::ReturnConnection(const std::string &ip, uint16_t port, int fd, bool is_bad) {
    std::string key = ip + ":" + std::to_string(port);
    
    ConnectionBucket *bucket = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_global_mtx);
        auto it = m_pools.find(key);
        if (it != m_pools.end()) bucket = it->second;
    }

    if (!bucket) {
        close(fd);
        return;
    }

    if (is_bad) {
        close(fd);
        bucket->active_count.fetch_sub(1);
    } else {
        if (!bucket->free_fds->push(fd)) {
            close(fd);
            bucket->active_count.fetch_sub(1);
        }
    }
}
