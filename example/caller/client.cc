#include "rpc_application.h"
#include "../user.pb.h"
#include "rpc_controller.h"
#include <iostream>
#include <atomic>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <chrono>
#include <vector>
#include "rpc_logger.h"
#include "rpc_connect_pool.h"
#include "rpc_channel.h"

class AsyncDone final : public google::protobuf::Closure {
public:
	AsyncDone(
		RpcController* controller,
		fixbug::LoginResponse* response,
		std::promise<void>* completed
	)
		: controller_(controller),
		  response_(response),
		  completed_(completed) {
	}

	void Run() override {
		if(controller_->Failed()){
			std::cerr<<"async rpc failed: "
					 <<controller_->ErrorText()
					 <<std::endl;
		}
		else{
			std::cout<<"async rpc completed, errcode="
					 <<response_->errcode()
					 <<std::endl;
		}

		completed_->set_value();
		delete this;
	}

private:
	RpcController* controller_;
	fixbug::LoginResponse* response_;
	std::promise<void>* completed_;
};

struct AsyncBatchState {
	std::mutex mutex;
	std::condition_variable condition;
	int completed=0;
	int failed=0;
};

struct AsyncCallContext {
	std::shared_ptr<RpcController> controller=
		std::make_shared<RpcController>();
	std::shared_ptr<fixbug::LoginResponse> response=
		std::make_shared<fixbug::LoginResponse>();
};

class AsyncBatchDone final : public google::protobuf::Closure {
public:
	AsyncBatchDone(
		std::shared_ptr<AsyncCallContext> call,
		std::shared_ptr<AsyncBatchState> state
	)
		: call_(std::move(call)),
		  state_(std::move(state)) {
	}

	void Run() override {
		{
			std::lock_guard<std::mutex> lock(state_->mutex);

			if(call_->controller->Failed()){
				++state_->failed;
			}
			else{
				++state_->completed;
			}
		}

		state_->condition.notify_one();
		delete this;
	}

private:
	std::shared_ptr<AsyncCallContext> call_;
	std::shared_ptr<AsyncBatchState> state_;
};

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
			std::cerr<<"RPC failed: "<<controller.ErrorText()<<std::endl;
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
	{
		fixbug::UserServiceRpc_Stub stub(
			new RpcChannel(false),
			google::protobuf::Service::STUB_OWNS_CHANNEL
		);

		fixbug::LoginRequst request;
		request.set_username("zhangsan");
		request.set_password("123456");

		fixbug::LoginResponse response;
		RpcController controller;

		std::promise<void> completed;
		std::future<void> completed_future=completed.get_future();

		auto start=std::chrono::steady_clock::now();

		stub.login(
			&controller,
			&request,
			&response,
			new AsyncDone(&controller,&response,&completed)
		);

		auto return_time=std::chrono::steady_clock::now();

		std::cout<<"CallMethod returned in "
				<<std::chrono::duration_cast<std::chrono::microseconds>(
						return_time-start
					).count()
				<<" us"
				<<std::endl;

		if(completed_future.wait_for(std::chrono::seconds(3))==
		std::future_status::timeout){
			std::cerr<<"async rpc timeout"<<std::endl;
		}
	}

	{
		fixbug::UserServiceRpc_Stub stub(
			new RpcChannel(false),
			google::protobuf::Service::STUB_OWNS_CHANNEL
		);

		const int async_request_count=100;
		fixbug::LoginRequst request;
		request.set_username("zhangsan");
		request.set_password("123456");

		std::vector<std::shared_ptr<AsyncCallContext>> calls;
		calls.reserve(async_request_count);

		auto state=std::make_shared<AsyncBatchState>();

		for(int i=0;i<async_request_count;++i){
			auto call=std::make_shared<AsyncCallContext>();
			calls.push_back(call);

			stub.login(
				call->controller.get(),
				&request,
				call->response.get(),
				new AsyncBatchDone(
					call,
					state
				)
			);
		}

		std::unique_lock<std::mutex> lock(state->mutex);
		bool finished=state->condition.wait_for(
			lock,
			std::chrono::seconds(5),
			[&state,async_request_count](){
				return state->completed+state->failed==async_request_count;
			}
		);

		std::cout<<"Async batch completed="<<state->completed
				 <<" failed="<<state->failed
				 <<" expected="<<async_request_count
				 <<" finished="<<finished
				 <<std::endl;
	}
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
