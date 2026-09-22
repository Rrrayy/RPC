#include "rpc_channel.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <exception>
#include <mutex>
#include <string>
#include <sys/socket.h>
#include <utility>
#include <vector>
#include <atomic>
#include "rpc_application.h"
#include "rpc_connect_pool.h"
#include "rpc_header.pb.h"
#include "rpc_logger.h"

namespace{
	std::atomic<std::uint64_t> request_id{0};

	constexpr std::uint32_t max_message_length=4*1024*1024;
}

RpcChannel::RpcChannel(bool connect_now){
	(void)connect_now;
}

RpcChannel::~RpcChannel()=default;

ssize_t RpcChannel::send_exact(
	int file_descriptor,
	const char* buffer,
	std::size_t size)
{
	std::size_t total_sent=0;

	while(total_sent<size){
		ssize_t sent=::send(
			file_descriptor,
			buffer+total_sent,
			size-total_sent,
			MSG_NOSIGNAL
		);

		if(sent<0){
			if(errno==EINTR){
				continue;
			}

			return -1;
		}

		if(sent==0){
			return -1;
		}

		total_sent+=static_cast<std::size_t>(sent);
	}

	return static_cast<ssize_t>(total_sent);
}

ssize_t RpcChannel::recv_exact(
	int file_descriptor,
	char* buffer,
	std::size_t size)
{
	std::size_t total_read=0;

	while(total_read<size){
		ssize_t received=::recv(
			file_descriptor,
			buffer+total_read,
			size-total_read,
			0
		);

		if(received==0){
			return 0;
		}

		if(received<0){
			if(errno==EINTR){
				continue;
			}

			return -1;
		}

		total_read+=static_cast<std::size_t>(received);
	}

	return static_cast<ssize_t>(total_read);
}

void RpcChannel::CallMethod(
	const google::protobuf::MethodDescriptor* method,
	google::protobuf::RpcController* controller,
	const google::protobuf::Message* request,
	google::protobuf::Message* response,
	google::protobuf::Closure* done)
{
	auto set_failed=[controller](const std::string& reason){
		if(controller!=nullptr){
			controller->SetFailed(reason);
		}
	};

	if(method==nullptr||request==nullptr||response==nullptr){
		set_failed("invalid rpc argument");

		if(done!=nullptr){
			done->Run();
		}

		return;
	}

	const google::protobuf::ServiceDescriptor* service_descriptor=
		method->service();

	if(service_descriptor==nullptr){
		set_failed("invalid service descriptor");

		if(done!=nullptr){
			done->Run();
		}

		return;
	}

	std::string service_name=service_descriptor->name();
	std::string method_name=method->name();

	static std::once_flag init_flag;

	std::call_once(
		init_flag,
		[](){
			ServiceDiscovery::GetInstance().Init();
		}
	);

	std::uint64_t current_request_id=
		request_id.fetch_add(
			1,
			std::memory_order_relaxed
		);

	std::string route_key=std::to_string(current_request_id);

	std::string ip_port=
		ServiceDiscovery::GetInstance().GetTargetNode(
			service_name,
			route_key
		);

	if(ip_port.empty()){
		set_failed("hash ring returned empty node");

		if(done!=nullptr){
			done->Run();
		}

		return;
	}

	std::size_t separator_position=ip_port.rfind(':');

	if(
		separator_position==std::string::npos||
		separator_position==0||
		separator_position+1>=ip_port.size()
	){
		set_failed("invalid service address");

		if(done!=nullptr){
			done->Run();
		}

		return;
	}

	int port=0;

	try{
		port=std::stoi(
			ip_port.substr(separator_position+1)
		);
	}catch(const std::exception& error){
		(void)error;
		set_failed("invalid service port");

		if(done!=nullptr){
			done->Run();
		}

		return;
	}

	if(port<=0||port>65535){
		set_failed("service port out of range");

		if(done!=nullptr){
			done->Run();
		}

		return;
	}

	std::string target_ip=
		ip_port.substr(0,separator_position);

	std::uint16_t target_port=
		static_cast<std::uint16_t>(port);

	int client_fd=
		RpcConnectPool::GetInstance().BorrowConnection(
			target_ip,
			target_port
		);

	if(client_fd==-1){
		set_failed("borrow connection from pool failed");

		if(done!=nullptr){
			done->Run();
		}

		return;
	}

	bool is_bad=false;
	std::string argument_string;
	std::string send_buffer;

	if(!request->SerializeToString(&argument_string)){
		set_failed("serialize request failed");
		is_bad=true;
	}else if(argument_string.size()>max_message_length){
		set_failed("request body too large");
		is_bad=true;
	}else{
		rpc::RpcHeader rpc_header;

		rpc_header.set_service_name(service_name);
		rpc_header.set_method_name(method_name);
		rpc_header.set_args_size(
			static_cast<std::uint32_t>(argument_string.size())
		);

		std::string header_string;

		if(!rpc_header.SerializeToString(&header_string)){
			set_failed("serialize rpc header failed");
			is_bad=true;
		}else if(header_string.size()>max_message_length){
			set_failed("rpc header too large");
			is_bad=true;
		}else{
			std::size_t total_length=
				sizeof(std::uint32_t)+
				header_string.size()+
				argument_string.size();

			if(total_length>max_message_length||
				total_length>UINT32_MAX){
				set_failed("rpc request too large");
				is_bad=true;
			}else{
				std::uint32_t network_total_length=
					htonl(
						static_cast<std::uint32_t>(
							total_length
						)
					);

				std::uint32_t network_header_length=
					htonl(
						static_cast<std::uint32_t>(
							header_string.size()
						)
					);

				send_buffer.reserve(
					sizeof(std::uint32_t)+
					total_length
				);

				send_buffer.append(
					reinterpret_cast<const char*>(
						&network_total_length
					),
					sizeof(network_total_length)
				);

				send_buffer.append(
					reinterpret_cast<const char*>(
						&network_header_length
					),
					sizeof(network_header_length)
				);

				send_buffer.append(header_string);
				send_buffer.append(argument_string);

				if(
					send_exact(
						client_fd,
						send_buffer.data(),
						send_buffer.size()
					)<0
				){
					set_failed("send rpc request failed");
					is_bad=true;
				}
			}
		}
	}

	if(!is_bad){
		std::uint32_t network_response_length=0;

		if(
			recv_exact(
				client_fd,
				reinterpret_cast<char*>(
					&network_response_length
				),
				sizeof(network_response_length)
			)!=static_cast<ssize_t>(
				sizeof(network_response_length)
			)
		){
			set_failed("receive response header failed");
			is_bad=true;
		}else{
			std::uint32_t response_length=
				ntohl(network_response_length);

			if(response_length>max_message_length){
				set_failed("response body too large");
				is_bad=true;
			}else{
				std::vector<char> response_buffer(response_length);
				if(
					recv_exact(
						client_fd,
						response_buffer.data(),
						response_length
					)!=static_cast<ssize_t>(
						response_length
					)
				){
					set_failed("receive response body failed");
					is_bad=true;
				}else{
					rpc::RpcResponse rpc_response;

					if(!rpc_response.ParseFromArray(
						response_buffer.data(),
						static_cast<int>(response_length)
					)){
						set_failed("parse rpc response failed");
						is_bad=true;
					}else if(rpc_response.error_code()!=0){
						set_failed(rpc_response.error_message());
						is_bad=true;
					}else if(!response->ParseFromString(
						rpc_response.payload()
					)){
						set_failed("parse business response failed");
						is_bad=true;
					}
				}
			}
		}
	}

	RpcConnectPool::GetInstance().ReturnConnection(
		target_ip,
		target_port,
		client_fd,
		is_bad
	);

	if(done!=nullptr){
		done->Run();
	}
}