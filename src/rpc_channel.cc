#include "rpc_channel.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <exception>
#include <mutex>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
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

RpcChannel::~RpcChannel()
{
	std::vector<std::shared_ptr<ConnectionState>> active_connections;

	{
		std::lock_guard<std::mutex> lock(connections_mutex);

		for(auto& entry:connections){
			std::shared_ptr<ConnectionState> connection=entry.second;

			connection->stopped.store(true);

			::shutdown(
				connection->file_descriptor,
				SHUT_RDWR
			);

			active_connections.push_back(connection);
		}

		connections.clear();
	}

	for(
		const std::shared_ptr<ConnectionState>& connection:
		active_connections
	){
		if(connection->reader_thread.joinable()){
			connection->reader_thread.join();
		}

		ReleaseConnection(connection);
	}

	FailAllPendingRequests("rpc channel closed");
}

bool RpcChannel::RegisterPendingRequest(const std::shared_ptr<PendingRequest>& pending_request)
{
	if(pending_request==nullptr){
		return false;
	}

	std::lock_guard<std::mutex> lock(pending_mutex);

	auto result=pending_requests.emplace(
		pending_request->request_id,
		pending_request
	);

	return result.second;
}

std::shared_ptr<RpcChannel::PendingRequest> RpcChannel::TakePendingRequest(std::uint64_t request_id)
{
	std::lock_guard<std::mutex> lock(pending_mutex);

	auto iter=pending_requests.find(request_id);

	if(iter==pending_requests.end()){
		return nullptr;
	}

	std::shared_ptr<PendingRequest> pending_request=iter->second;
	pending_requests.erase(iter);

	return pending_request;
}

void RpcChannel::FailPendingRequests(
	const std::string& endpoint,
	const std::string& reason
)
{
	std::vector<std::shared_ptr<PendingRequest>> failed_requests;

	{
		std::lock_guard<std::mutex> lock(pending_mutex);

		for(auto iter=pending_requests.begin();
			iter!=pending_requests.end();){
			if(iter->second->endpoint==endpoint){
				failed_requests.push_back(iter->second);
				iter=pending_requests.erase(iter);
			}
			else{
				++iter;
			}
		}
	}

	for(const std::shared_ptr<PendingRequest>& pending_request:
		failed_requests){
		if(pending_request->controller!=nullptr){
			pending_request->controller->SetFailed(reason);
		}

		if(pending_request->done!=nullptr){
			pending_request->done->Run();
		}
	}
}

void RpcChannel::FailAllPendingRequests(const std::string& reason)
{
	std::vector<std::shared_ptr<PendingRequest>> failed_requests;

	{
		std::lock_guard<std::mutex> lock(pending_mutex);

		for(auto& entry:pending_requests){
			failed_requests.push_back(entry.second);
		}

		pending_requests.clear();
	}

	for(const std::shared_ptr<PendingRequest>& pending_request:
		failed_requests){
		if(pending_request->controller!=nullptr){
			pending_request->controller->SetFailed(reason);
		}

		if(pending_request->done!=nullptr){
			pending_request->done->Run();
		}
	}
}

std::shared_ptr<RpcChannel::ConnectionState> RpcChannel::GetOrCreateConnection(const std::string& endpoint)
{
	{
		std::lock_guard<std::mutex> lock(connections_mutex);
		auto iter=connections.find(endpoint);

		if(iter!=connections.end()){
			if(!iter->second->stopped.load()){
				return iter->second;
			}

			connections.erase(iter);
		}
	}

	std::size_t separator_position=endpoint.rfind(':');

	if(separator_position==std::string::npos||separator_position==0||separator_position+1>=endpoint.size())
		return nullptr;

	std::string target_ip=endpoint.substr(0,separator_position);

	int port=0;

	try{
		port=std::stoi(
			endpoint.substr(separator_position+1)
		);
	}catch(const std::exception& error){
		(void)error;
		return nullptr;
	}

	if(port<=0||port>65535){
		return nullptr;
	}

	std::uint16_t target_port=
		static_cast<std::uint16_t>(port);

	int file_descriptor=
		RpcConnectPool::GetInstance().BorrowConnection(
			target_ip,
			target_port
		);

	if(file_descriptor==-1){
		return nullptr;
	}

	auto new_connection=
		std::make_shared<ConnectionState>(
			endpoint,
			target_ip,
			target_port,
			file_descriptor
		);

	{
		std::lock_guard<std::mutex> lock(connections_mutex);

		auto result=connections.emplace(
			endpoint,
			new_connection
		);

		if(!result.second){
			RpcConnectPool::GetInstance().ReturnConnection(
				target_ip,
				target_port,
				file_descriptor
			);

			return result.first->second;
		}
	}
		try{
		new_connection->reader_thread=std::thread(&RpcChannel::ReaderLoop,this,new_connection);
	}catch(const std::exception& error){
		(void)error;

		new_connection->stopped.store(true);

		{
			std::lock_guard<std::mutex> lock(
				connections_mutex
			);

			connections.erase(endpoint);
		}

		ReleaseConnection(new_connection);

		return nullptr;
	}

	return new_connection;
}

void RpcChannel::ReleaseConnection(
	const std::shared_ptr<ConnectionState>& connection
)
{
	if(connection==nullptr){
		return;
	}

	bool expected=false;
	if(!connection->returned_to_pool.compare_exchange_strong(
		expected,
		true
	)){
		return;
	}

	RpcConnectPool::GetInstance().ReturnConnection(
		connection->target_ip,
		connection->target_port,
		connection->file_descriptor,
		true
	);
}

void RpcChannel::ReaderLoop(	const std::shared_ptr<ConnectionState>& connection)
{
	if(connection==nullptr){
		return;
	}

	while(!connection->stopped.load()){
		std::uint32_t network_response_length=0;

		ssize_t length_result=recv_exact(
			connection->file_descriptor,
			reinterpret_cast<char*>(
				&network_response_length
			),
			sizeof(network_response_length)
		);

		if(length_result!=static_cast<ssize_t>(
			sizeof(network_response_length)
		)){
			if(connection->stopped.load()){
				break;
			}

			LOG(ERROR)
				<<"connection reader failed to receive response length: "
				<<connection->endpoint
				<<" received_bytes="
				<<length_result;

			connection->stopped.store(true);
			break;
		}

		std::uint32_t response_length=
			ntohl(network_response_length);

		if(
			response_length==0||
			response_length>max_message_length
		){
			LOG(ERROR)
				<<"invalid response length from "
				<<connection->endpoint
				<<": "
				<<response_length;
			break;
		}

		std::string response_buffer(
			response_length,
			'\0'
		);

		ssize_t body_result=recv_exact(
			connection->file_descriptor,
			response_buffer.data(),
			response_buffer.size()
		);

		if(body_result!=static_cast<ssize_t>(
			response_buffer.size()
		)){
			LOG(ERROR)
				<<"connection reader failed to receive response body: "
				<<connection->endpoint;
			break;
		}

		rpc::RpcResponse rpc_response;

		if(!rpc_response.ParseFromString(response_buffer)){
			LOG(ERROR)
				<<"connection reader failed to parse response: "
				<<connection->endpoint;
			break;
		}

		std::shared_ptr<PendingRequest> pending_request=
			TakePendingRequest(
				rpc_response.request_id()
			);

		if(pending_request==nullptr){
			LOG(ERROR)
				<<"received response for unknown request id: "
				<<rpc_response.request_id();
			continue;
		}

		if(
			rpc_response.error_code()!=0&&
			pending_request->controller!=nullptr
		){
			pending_request->controller->SetFailed(
				rpc_response.error_message()
			);
		}else if(
			pending_request->response==nullptr||
			!pending_request->response->ParseFromString(
				rpc_response.payload()
			)
		){
			if(pending_request->controller!=nullptr){
				pending_request->controller->SetFailed(
					"parse business response failed"
				);
			}
		}

		if(pending_request->done!=nullptr){
			pending_request->done->Run();
		}
	}

	connection->stopped.store(true);

	FailPendingRequests(
		connection->endpoint,
		"rpc connection closed"
	);

	{
		std::lock_guard<std::mutex> lock(connections_mutex);
		auto iter=connections.find(connection->endpoint);
		if(iter!=connections.end()&&iter->second==connection){
			connections.erase(iter);
		}
	}

	ReleaseConnection(connection);
}
void RpcChannel::CallMethod(
	const google::protobuf::MethodDescriptor* method,
	google::protobuf::RpcController* controller,
	const google::protobuf::Message* request,
	google::protobuf::Message* response,
	google::protobuf::Closure* done)
{
	if(done!=nullptr){
		CallMethodAsync(
			method,
			controller,
			request,
			response,
			done
		);

		return;
	}

	CallMethodSync(
		method,
		controller,
		request,
		response,
		nullptr
	);
}
void RpcChannel::CallMethodAsync(
	const google::protobuf::MethodDescriptor* method,
	google::protobuf::RpcController* controller,
	const google::protobuf::Message* request,
	google::protobuf::Message* response,
	google::protobuf::Closure* done)
{
	auto finish_failed=[
		controller,
		done
	](const std::string& reason){
		if(controller!=nullptr){
			controller->SetFailed(reason);
		}

		if(done!=nullptr){
			done->Run();
		}
	};

	if(
		method==nullptr||
		request==nullptr||
		response==nullptr
	){
		finish_failed("invalid rpc argument");
		return;
	}

	const google::protobuf::ServiceDescriptor* service_descriptor=
		method->service();

	if(service_descriptor==nullptr){
		finish_failed("invalid service descriptor");
		return;
	}

	std::string service_name=service_descriptor->name();
	std::string method_name=method->name();

	ServiceDiscovery::GetInstance().Init();

	std::uint64_t current_request_id=
		request_id.fetch_add(
			1,
			std::memory_order_relaxed
		)+1;

	std::string endpoint=
		ServiceDiscovery::GetInstance().GetTargetNode(
			service_name,
			std::to_string(current_request_id)
		);

	if(endpoint.empty()){
		finish_failed("hash ring returned empty node");
		return;
	}

	std::string argument_string;

	if(!request->SerializeToString(&argument_string)){
		finish_failed("serialize request failed");
		return;
	}

	if(argument_string.size()>max_message_length){
		finish_failed("request body too large");
		return;
	}

	rpc::RpcHeader rpc_header;

	rpc_header.set_service_name(service_name);
	rpc_header.set_method_name(method_name);
	rpc_header.set_args_size(
		static_cast<std::uint32_t>(argument_string.size())
	);
	rpc_header.set_request_id(current_request_id);

	std::string header_string;

	if(!rpc_header.SerializeToString(&header_string)){
		finish_failed("serialize rpc header failed");
		return;
	}

	if(header_string.size()>max_message_length){
		finish_failed("rpc header too large");
		return;
	}

	std::size_t total_length=
		sizeof(std::uint32_t)+
		header_string.size()+
		argument_string.size();

	if(
		total_length>max_message_length||
		total_length>UINT32_MAX
	){
		finish_failed("rpc request too large");
		return;
	}

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

	std::string send_buffer;

	send_buffer.reserve(
		sizeof(network_total_length)+
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

	std::shared_ptr<ConnectionState> connection=
		GetOrCreateConnection(endpoint);

	if(connection==nullptr){
		finish_failed("get persistent connection failed");
		return;
	}

	auto pending_request=
		std::make_shared<PendingRequest>();

	pending_request->request_id=current_request_id;
	pending_request->endpoint=endpoint;
	pending_request->controller=controller;
	pending_request->response=response;
	pending_request->done=done;

	if(!RegisterPendingRequest(pending_request)){
		finish_failed("duplicate rpc request id");
		return;
	}

	bool send_failed=false;

	{
		std::lock_guard<std::mutex> lock(
			connection->write_mutex
		);

		if(
			connection->stopped.load()||
			send_exact(
				connection->file_descriptor,
				send_buffer.data(),
				send_buffer.size()
			)<0
		){
			send_failed=true;
		}
	}

	if(!send_failed){
		return;
	}

	std::shared_ptr<PendingRequest> failed_request=
		TakePendingRequest(current_request_id);

	connection->stopped.store(true);

	::shutdown(
		connection->file_descriptor,
		SHUT_RDWR
	);

	if(failed_request!=nullptr){
		if(failed_request->controller!=nullptr){
			failed_request->controller->SetFailed(
				"send rpc request failed"
			);
		}

		if(failed_request->done!=nullptr){
			failed_request->done->Run();
		}
	}
}
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

void RpcChannel::CallMethodSync(
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

	ServiceDiscovery::GetInstance().Init();

	std::uint64_t current_request_id =
    request_id.fetch_add(1, std::memory_order_relaxed) + 1;

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
		rpc_header.set_request_id(current_request_id);
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
					}else if(rpc_response.request_id()!=current_request_id){
						set_failed("rpc response request id mismatch");
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
