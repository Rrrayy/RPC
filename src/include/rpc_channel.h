#ifndef RPC_CHANNEL_H
#define RPC_CHANNEL_H

#include <cstddef>
#include <string>
#include <sys/types.h>

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
};

#endif