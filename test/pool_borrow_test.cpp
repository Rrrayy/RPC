#include "rpc_connect_pool.h"

#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace{

void require(bool condition,const char* message){
	if(!condition){
		throw std::runtime_error(message);
	}
}

struct TestSocket{
	int descriptor=socket(AF_INET,SOCK_STREAM,0);

	~TestSocket(){
		if(descriptor!=-1){
			close(descriptor);
		}
	}

	TestSocket()=default;
	TestSocket(const TestSocket&)=delete;
	TestSocket& operator=(const TestSocket&)=delete;
};

std::uint16_t bind_loopback_port(int descriptor){
	sockaddr_in address{};
	address.sin_family=AF_INET;
	address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);

	require(
		bind(
			descriptor,
			reinterpret_cast<sockaddr*>(&address),
			sizeof(address)
		)==0,
		"bind test socket failed"
	);

	socklen_t address_length=sizeof(address);

	require(
		getsockname(
			descriptor,
			reinterpret_cast<sockaddr*>(&address),
			&address_length
		)==0,
		"get test port failed"
	);

	return ntohs(address.sin_port);
}

void test_pool_wait_and_wakeup(RpcConnectPool& pool){
	constexpr int connection_limit=1024;

	TestSocket server;
	require(server.descriptor!=-1,"create wait test server socket failed");

	const std::uint16_t port=bind_loopback_port(server.descriptor);
	require(listen(server.descriptor,128)==0,"listen wait test server failed");

	std::vector<int> accepted_sockets;
	accepted_sockets.reserve(connection_limit);

	std::thread accept_thread([&]{
		for(int index=0;index<connection_limit;++index){
			const int accepted=accept(server.descriptor,nullptr,nullptr);

			if(accepted==-1){
				return;
			}

			accepted_sockets.push_back(accepted);
		}
	});

	pool.WarmUp("127.0.0.1",port,connection_limit);
	accept_thread.join();

	require(
		accepted_sockets.size()==connection_limit,
		"server must accept every warmed-up connection"
	);

	std::vector<int> borrowed_sockets;
	borrowed_sockets.reserve(connection_limit);

	for(int index=0;index<connection_limit;++index){
		const int descriptor=pool.BorrowConnection("127.0.0.1",port);

		require(
			descriptor!=-1,
			"borrow all warmed-up connections failed"
		);

		borrowed_sockets.push_back(descriptor);
	}

	BorrowFailure timeout_failure;

	const int timeout_result=pool.BorrowConnection(
		"127.0.0.1",
		port,
		&timeout_failure,
		std::chrono::milliseconds(100)
	);

	require(
		timeout_result==-1&&
		timeout_failure.error==BorrowError::pool_full,
		"borrow must return pool_full when no connection is returned before timeout"
	);

	std::atomic<bool> waiter_started{false};
	int waiter_result=-1;
	BorrowFailure waiter_failure;

	std::thread waiting_thread([&]{
		waiter_started.store(true,std::memory_order_release);

		waiter_result=pool.BorrowConnection(
			"127.0.0.1",
			port,
			&waiter_failure,
			std::chrono::milliseconds(2000)
		);
	});

	while(!waiter_started.load(std::memory_order_acquire)){
		std::this_thread::yield();
	}

	std::this_thread::sleep_for(std::chrono::milliseconds(100));

	pool.ReturnConnection(
		"127.0.0.1",
		port,
		borrowed_sockets.back()
	);
	borrowed_sockets.pop_back();

	waiting_thread.join();

	require(
		waiter_result!=-1,
		"waiting borrower must get the returned connection"
	);

	require(
		waiter_failure.error==BorrowError::none,
		"successful waiting borrow must not report an error"
	);

	borrowed_sockets.push_back(waiter_result);

	for(const int descriptor:borrowed_sockets){
		pool.ReturnConnection("127.0.0.1",port,descriptor,true);
	}

	for(const int descriptor:accepted_sockets){
		close(descriptor);
	}
}

}

int main(){
	try{
		require(
			!BorrowFailure{BorrowError::pool_full,0}.can_failover(),
			"pool saturation must not trigger failover"
		);

		require(
			!BorrowFailure{BorrowError::socket_failed,EMFILE}.can_failover(),
			"local descriptor exhaustion must not trigger failover"
		);

		require(
			!BorrowFailure{BorrowError::connect_failed,EADDRNOTAVAIL}.can_failover(),
			"local address exhaustion must not trigger failover"
		);

		require(
			!BorrowFailure{BorrowError::connect_failed,EACCES}.can_failover(),
			"permission errors must not trigger failover"
		);

		for(const int reason:{
			ECONNREFUSED,
			ETIMEDOUT,
			EHOSTUNREACH,
			ENETUNREACH,
			ECONNRESET
		}){
			require(
				BorrowFailure{BorrowError::connect_failed,reason}.can_failover(),
				"expected connection error must permit failover"
			);
		}

		TestSocket server;
		require(server.descriptor!=-1,"create connection test server failed");

		const std::uint16_t port=bind_loopback_port(server.descriptor);
		auto& pool=RpcConnectPool::GetInstance();
		BorrowFailure failure;

		for(int attempt=0;attempt<1030;++attempt){
			const int descriptor=pool.BorrowConnection(
				"127.0.0.1",
				port,
				&failure
			);

			if(descriptor!=-1){
				pool.ReturnConnection(
					"127.0.0.1",
					port,
					descriptor,
					true
				);
			}

			require(
				descriptor==-1&&
				failure.error==BorrowError::connect_failed&&
				failure.system_error==ECONNREFUSED,
				"failed connect must preserve errno and release its pool slot"
			);
		}

		const auto counts=pool.GetFailureCounts();
		const std::string failure_key=
			"127.0.0.1:"+std::to_string(port)+" "+failure.describe();

		require(
			counts.at(failure_key)==1030,
			"failure histogram must count each connection attempt"
		);

		require(
			listen(server.descriptor,8)==0,
			"listen connection test server failed"
		);

		const int descriptor=pool.BorrowConnection(
			"127.0.0.1",
			port,
			&failure
		);

		require(
			descriptor!=-1,
			"pool must recover after refused connections"
		);

		pool.ReturnConnection("127.0.0.1",port,descriptor);

		require(
			failure.error==BorrowError::none&&failure.system_error==0,
			"successful borrow must clear old diagnostics"
		);

		const int reused=pool.BorrowConnection(
			"127.0.0.1",
			port,
			&failure
		);

		require(
			reused==descriptor&&failure.error==BorrowError::none,
			"healthy pooled connection must be reusable"
		);

		pool.ReturnConnection("127.0.0.1",port,reused,true);

		test_pool_wait_and_wakeup(pool);

		std::cout<<"Pool borrow test passed"<<std::endl;
		return 0;
	}catch(const std::exception& error){
		std::cerr<<error.what()<<std::endl;
		return 1;
	}
}