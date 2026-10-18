#include "../../example/user.pb.h"
#include "rpc_application.h"
#include "rpc_logger.h"
#include "rpc_provider.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>
#include <utility>
#include <unistd.h>

class ClusterService final:public fixbug::UserServiceRpc{
public:
	explicit ClusterService(std::string endpoint):endpoint_(std::move(endpoint)){}
	void login(google::protobuf::RpcController*,const fixbug::LoginRequst* request,
		fixbug::LoginResponse* response,google::protobuf::Closure* done) override{
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
		const bool valid=!request->username().empty()&&!request->password().empty();
		response->set_errcode(valid?0:1);
		// Return the actual node identity so the client can measure request distribution.
		response->set_msg(endpoint_);
		// This counts prepared responses, not confirmed delivery; it resets on restart.
		const auto completed=completed_.fetch_add(1,std::memory_order_relaxed)+1;
		if(completed%1000==0){
			LOG(INFO)<<"cluster_server endpoint="<<endpoint_<<" pid="<<getpid()
				<<" business_completed="<<completed;
		}
		if(done!=nullptr){
			// The framework closure may release request/response; do not access them afterward.
			done->Run();
		}
	}
private:
	const std::string endpoint_;
	std::atomic<std::uint64_t> completed_{0};
};

int main(int argc,char** argv){
	RpcApplication::Init(argc,argv);
	RpcLogger logger("cluster_test_server");
	const auto endpoint=RpcApplication::GetConfig().Load("rpcserverip")+":"
		+RpcApplication::GetConfig().Load("rpcserverport");
	std::cout<<"TEST SERVER endpoint="<<endpoint<<" pid="<<getpid()<<std::endl;
	RpcProvider provider;
	// RpcProvider takes ownership of the registered service.
	provider.NotifyService(new ClusterService(endpoint));
	provider.Run();
	return 0;
}
