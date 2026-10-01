#ifndef RPC_CHANNEL_H
#define RPC_CHANNEL_H

#include <cstddef>
#include <string>
#include <sys/types.h>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <atomic>
#include <thread>	
#include <utility>
#include <google/protobuf/service.h>

#include "service_discovery.h"

class RpcChannel:public google::protobuf::RpcChannel{
public:
	RpcChannel(bool connect_now=false);

	~RpcChannel()override;

	void CallMethod(
		const google::protobuf::MethodDescriptor* method,
		google::protobuf::RpcController* controller,
		const google::protobuf::Message* request,
		google::protobuf::Message* response,
		google::protobuf::Closure* done
	)override;

private:
	struct PendingRequest {
		std::uint64_t request_id;
		std::string endpoint;
		google::protobuf::RpcController* controller;
		google::protobuf::Message* response;
		google::protobuf::Closure* done;
	};

		struct ConnectionState {
			std::string endpoint;
			std::string target_ip;
			std::uint16_t target_port;
			int file_descriptor;
			std::mutex write_mutex;
			std::atomic<bool> stopped;
			std::atomic<bool> returned_to_pool;
			std::thread reader_thread;

			ConnectionState(
				std::string connection_endpoint,
				std::string connection_ip,
				std::uint16_t connection_port,
				int connection_file_descriptor
			):
				endpoint(std::move(connection_endpoint)),
				target_ip(std::move(connection_ip)),
				target_port(connection_port),
				file_descriptor(connection_file_descriptor),
				stopped(false),
				returned_to_pool(false)
			{
			}
	};

	void CallMethodSync(
		const google::protobuf::MethodDescriptor* method,
		google::protobuf::RpcController* controller,
		const google::protobuf::Message* request,
		google::protobuf::Message* response,
		google::protobuf::Closure* done
	);
	void CallMethodAsync(
		const google::protobuf::MethodDescriptor* method,
		google::protobuf::RpcController* controller,
		const google::protobuf::Message* request,
		google::protobuf::Message* response,
		google::protobuf::Closure* done
	);
	std::shared_ptr<ConnectionState> GetOrCreateConnection(const std::string& endpoint);

	void ReaderLoop(const std::shared_ptr<ConnectionState>& connection);
	
	ssize_t send_exact(
		int file_descriptor,
		const char* buffer,
		std::size_t size
	);

	ssize_t recv_exact(
		int file_descriptor,
		char* buffer,
		std::size_t size
	);

	std::mutex pending_mutex;
	std::unordered_map<std::uint64_t,std::shared_ptr<PendingRequest>> pending_requests;
	std::mutex connections_mutex;

	std::unordered_map<std::string,std::shared_ptr<ConnectionState>> connections;
	bool RegisterPendingRequest(const std::shared_ptr<PendingRequest>& pending_request);

	std::shared_ptr<PendingRequest> TakePendingRequest(std::uint64_t request_id);

	void FailPendingRequests(
		const std::string& endpoint,
		const std::string& reason
	);

	void FailAllPendingRequests(const std::string& reason);

	void ReleaseConnection(
		const std::shared_ptr<ConnectionState>& connection
	);
};

#endif
