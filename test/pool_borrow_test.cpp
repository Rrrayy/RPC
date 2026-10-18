#include "rpc_connect_pool.h"
#include <arpa/inet.h>
#include <cerrno>
#include <iostream>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>

void require(bool condition,const char* message){
	if(!condition){throw std::runtime_error(message);}
}

struct TestSocket{
	int descriptor=socket(AF_INET,SOCK_STREAM,0);
	~TestSocket(){if(descriptor!=-1){close(descriptor);}}
	TestSocket()=default;
	TestSocket(const TestSocket&)=delete;
	TestSocket& operator=(const TestSocket&)=delete;
};

int main(){
	try{
		require(!BorrowFailure{BorrowError::pool_full,0}.can_failover(),"pool saturation must not trigger failover");
		require(!BorrowFailure{BorrowError::socket_failed,EMFILE}.can_failover(),"local descriptor exhaustion must not trigger failover");
		require(!BorrowFailure{BorrowError::connect_failed,EADDRNOTAVAIL}.can_failover(),"local address exhaustion must not trigger failover");
		require(!BorrowFailure{BorrowError::connect_failed,EACCES}.can_failover(),"permission errors must not trigger failover");
		for(const int reason:{ECONNREFUSED,ETIMEDOUT,EHOSTUNREACH,ENETUNREACH,ECONNRESET}){
			require(BorrowFailure{BorrowError::connect_failed,reason}.can_failover(),"expected connection error must permit failover");
		}
		TestSocket server;
		require(server.descriptor!=-1,"create test socket failed");
		sockaddr_in address{};
		address.sin_family=AF_INET;
		address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
		require(bind(server.descriptor,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0,"bind failed");
		socklen_t address_length=sizeof(address);
		require(getsockname(server.descriptor,reinterpret_cast<sockaddr*>(&address),&address_length)==0,"getsockname failed");
		const auto port=ntohs(address.sin_port);
		auto& pool=RpcConnectPool::GetInstance();
		BorrowFailure failure;
		// Reserve a local port without listening: Linux returns ECONNREFUSED deterministically.
		for(int attempt=0;attempt<1030;++attempt){
			const int descriptor=pool.BorrowConnection("127.0.0.1",port,&failure);
			if(descriptor!=-1){pool.ReturnConnection("127.0.0.1",port,descriptor,true);}
			require(descriptor==-1&&failure.error==BorrowError::connect_failed&&failure.system_error==ECONNREFUSED,
				"failed connect must preserve errno and roll back its pool slot");
		}
		const auto counts=pool.GetFailureCounts();
		const auto key="127.0.0.1:"+std::to_string(port)+" "+failure.describe();
		require(counts.at(key)==1030,"failure histogram must count each connection attempt");
		require(listen(server.descriptor,4)==0,"listen failed");
		const int descriptor=pool.BorrowConnection("127.0.0.1",port,&failure);
		if(descriptor==-1){throw std::runtime_error("pool must recover after refused connections");}
		pool.ReturnConnection("127.0.0.1",port,descriptor);
		require(failure.error==BorrowError::none&&failure.system_error==0,"successful borrow must clear old diagnostics");
		const int reused=pool.BorrowConnection("127.0.0.1",port,&failure);
		if(reused!=-1){pool.ReturnConnection("127.0.0.1",port,reused,true);}
		require(reused==descriptor&&failure.error==BorrowError::none,"healthy pooled connection must remain reusable");
		std::cout<<"Pool borrow test passed"<<std::endl;
		return 0;
	}catch(const std::exception& error){
		std::cerr<<error.what()<<std::endl;
		return 1;
	}
}
