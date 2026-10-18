#include "../../example/user.pb.h"
#include "rpc_application.h"
#include "rpc_channel.h"
#include "rpc_controller.h"
#include "rpc_connect_pool.h"
#include "rpc_logger.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <future>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using TestClock=std::chrono::steady_clock;

// Attribute responses by completion time, not by request submission time.
struct PhaseStats{
	TestClock::time_point start;
	TestClock::time_point end;
	std::map<std::string,std::uint64_t> nodes;
	std::map<std::string,std::uint64_t> errors;
};

int read_setting(const std::string& name,int minimum,int maximum){
	const auto value=RpcApplication::GetConfig().Load(name);
	std::size_t parsed=0;
	const int number=std::stoi(value,&parsed);
	if(parsed!=value.size()||number<minimum||number>maximum){
		throw std::invalid_argument("invalid setting: "+name);
	}
	return number;
}

std::uint64_t total(const std::map<std::string,std::uint64_t>& counts){
	std::uint64_t count=0;
	for(const auto& entry:counts){
		count+=entry.second;
	}
	return count;
}

bool confirm(const std::string& instruction){
	std::cout<<instruction<<" Then type yes and press Enter."<<std::endl;
	std::string answer;
	return static_cast<bool>(std::getline(std::cin,answer))&&answer=="yes";
}

int run_test(){
	const auto mode=RpcApplication::GetConfig().Load("test_mode");
	if(mode!="distribution"&&mode!="failover"){
		throw std::invalid_argument("test_mode must be distribution or failover");
	}
	const int thread_count=read_setting("test_threads",1,200);
	const int request_count=read_setting("test_requests_per_thread",1,1000000);
	const int stable_seconds=read_setting("test_stable_seconds",1,120);
	const int settle_seconds=read_setting("test_settle_seconds",1,120);
	const auto node_a=RpcApplication::GetConfig().Load("test_node_a");
	const auto node_b=RpcApplication::GetConfig().Load("test_node_b");
	if(node_a.empty()||node_b.empty()||node_a==node_b){
		throw std::invalid_argument("configure two distinct test nodes");
	}
	std::cout<<"TEST mode="<<mode<<" threads="<<thread_count<<" node_a="<<node_a
		<<" node_b="<<node_b<<" simulated_work_ms=5 request_replays=0 pre_send_alternatives=1"<<std::endl;
	const auto failures_before=RpcConnectPool::GetInstance().GetFailureCounts();
	const auto fallback_before=RpcChannel::GetSyncFailoverStats();
	std::mutex stats_mutex;
	std::map<std::string,PhaseStats> phases;
	std::string phase=mode=="distribution"?"distribution":"baseline";
	std::vector<std::string> phase_order{phase};
	std::atomic<bool> stopping{false};
	// Exclude thread creation from timing and release all workers through one gate.
	std::promise<void> start_gate;
	const auto started=start_gate.get_future().share();
	std::vector<std::thread> workers;
	// Use the same mutex for phase transitions and completion accounting.
	auto change_phase=[&](const std::string& next){
		std::lock_guard<std::mutex> lock(stats_mutex);
		const auto now=TestClock::now();
		phases[phase].end=now;
		phase=next;
		phase_order.push_back(next);
		phases[phase].start=now;
		std::cout<<"PHASE "<<phase<<std::endl;
	};
	// Each worker retains its own channel and mutable messages across all phases.
	auto worker=[&](){
		fixbug::UserServiceRpc_Stub stub(new RpcChannel(false),google::protobuf::Service::STUB_OWNS_CHANNEL);
		fixbug::LoginRequst request;
		request.set_username("cluster_test");
		request.set_password("test_only");
		fixbug::LoginResponse response;
		RpcController controller;
		started.wait();
		for(int submitted=0;!stopping.load()&&(mode=="failover"||submitted<request_count);++submitted){
			controller.Reset();
			response.Clear();
			stub.login(&controller,&request,&response,nullptr);
			std::string error;
			if(controller.Failed()){
				error=controller.ErrorText();
				if(error.empty()){
					error="controller failed without reason";
				}
			}else if(response.errcode()!=0){
				error="business error: "+std::to_string(response.errcode());
			}else if(response.msg()!=node_a&&response.msg()!=node_b){
				error="unexpected server identity: "+response.msg();
			}
			{
				std::lock_guard<std::mutex> lock(stats_mutex);
				if(error.empty()){
					++phases[phase].nodes[response.msg()];
				}else{
					++phases[phase].errors[error];
				}
			}
			// Pace failed calls to avoid a busy loop; this does not retry the failed request.
			if(!error.empty()&&mode=="failover"){
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
			}
		}
	};
	// If thread creation partially fails, release the gate and join every created worker.
	try{
		for(int index=0;index<thread_count;++index){
			workers.emplace_back(worker);
		}
	}catch(...){
		stopping.store(true);
		start_gate.set_value();
		for(auto& thread:workers){thread.join();}
		throw;
	}
	phases[phase].start=TestClock::now();
	std::cout<<"PHASE "<<phase<<std::endl;
	start_gate.set_value();
	bool acknowledged=true;
	// Workers continue issuing requests while the operator stops/restarts node B.
	if(mode=="failover"){
		std::this_thread::sleep_for(std::chrono::seconds(stable_seconds));
		change_phase("down_transition");
		acknowledged=confirm("Stop ONLY node B ("+node_b+") in its own terminal using Ctrl+C.");
		if(acknowledged){
			std::this_thread::sleep_for(std::chrono::seconds(settle_seconds));
			change_phase("down_stable");
			std::this_thread::sleep_for(std::chrono::seconds(stable_seconds));
			change_phase("up_transition");
			acknowledged=confirm("Restart node B using cluster_test_server and its original configuration.");
			if(acknowledged){
				std::this_thread::sleep_for(std::chrono::seconds(settle_seconds));
				change_phase("recovered_stable");
				std::this_thread::sleep_for(std::chrono::seconds(stable_seconds));
			}
		}
		stopping.store(true);
	}
	// Drain in-flight calls before reporting; include drain time in the final phase.
	for(auto& thread:workers){thread.join();}
	phases[phase].end=TestClock::now();
	bool passed=acknowledged;
	for(const auto& name:phase_order){
		const auto& stats=phases.at(name);
		const auto success=total(stats.nodes);
		const auto failed=total(stats.errors);
		const double seconds=std::chrono::duration<double>(stats.end-stats.start).count();
		std::cout<<std::fixed<<std::setprecision(2)<<"RESULT phase="<<name
			<<" seconds="<<seconds<<" success="<<success<<" failed="<<failed
			<<" attempt_qps="<<(success+failed)/seconds<<" success_qps="<<success/seconds<<std::endl;
		// Node QPS uses the shared client observation window, not isolated server capacity.
		for(const auto& node:stats.nodes){
			std::cout<<"NODE endpoint="<<node.first<<" responses="<<node.second
				<<" success_share_pct="<<(100.0*node.second/success)<<" delivered_qps="<<node.second/seconds<<std::endl;
		}
		for(const auto& error:stats.errors){
			std::cout<<"ERROR count="<<error.second<<" reason="<<error.first<<std::endl;
		}
		// Report transition errors, but require zero failures and the expected nodes in stable windows.
		if(name=="down_transition"||name=="up_transition"){continue;}
		const bool has_a=stats.nodes.count(node_a)!=0;
		const bool has_b=stats.nodes.count(node_b)!=0;
		passed=passed&&failed==0&&has_a&&(name=="down_stable"?!has_b:has_b);
		if(mode=="distribution"){
			passed=passed&&success+failed==static_cast<std::uint64_t>(thread_count)*request_count;
		}
	}
	// Whole-run event counters are separate from phase-based completed-call statistics.
	const auto fallback_after=RpcChannel::GetSyncFailoverStats();
	passed=passed&&fallback_after.completed-fallback_before.completed<=fallback_after.connected-fallback_before.connected
		&&fallback_after.connected-fallback_before.connected<=fallback_after.attempted-fallback_before.attempted;
	std::cout<<"FAILOVER scope=whole_run attempted="<<fallback_after.attempted-fallback_before.attempted
		<<" connected="<<fallback_after.connected-fallback_before.connected
		<<" completed="<<fallback_after.completed-fallback_before.completed<<std::endl;
	for(const auto& failure:RpcConnectPool::GetInstance().GetFailureCounts()){
		const auto previous=failures_before.find(failure.first);
		const auto count=failure.second-(previous==failures_before.end()?0:previous->second);
		if(count!=0){std::cout<<"BORROW_FAILURE count="<<count<<" "<<failure.first<<std::endl;}
	}
	std::cout<<"VERDICT "<<(passed?"PASS":"FAIL")<<std::endl;
	return passed?0:1;
}

int main(int argc,char** argv){
	RpcApplication::Init(argc,argv);
	RpcLogger logger("cluster_test_client");
	try{
		return run_test();
	}catch(const std::exception& error){
		std::cerr<<"TEST ERROR: "<<error.what()<<std::endl;
		return 2;
	}
}
