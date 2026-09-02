#include "rpc_provider.h"
#include "rpc_application.h"
#include "rpc_header.pb.h"
#include "rpc_logger.h"
#include <iostream>
#include <cstring>
#include <arpa/inet.h>
#include <cstdint>
#include <utility>

class RpcClosure : public google::protobuf::Closure{
public:
    explicit RpcClosure(std::function<void()> cb) : cb_(std::move(cb)){}
    void Run() override{
        auto callback=std::move(cb_);
		delete this;
		callback();
    }
private:
    std::function<void()> cb_;
};

void RpcProvider::NotifyService(google::protobuf::Service* service){
    if(service==nullptr){
		LOG(ERROR)<<"cannot register null service: ";
		return ;
	}
	ServiceInfo service_info;
    const google::protobuf::ServiceDescriptor *psd = service->GetDescriptor();
    std::string service_name = psd->name();
    int method_count = psd->method_count();
    std::cout << "service_name=" << service_name << std::endl;
    for(int i = 0 ; i<method_count ; ++i){
        const google::protobuf::MethodDescriptor* pmd = psd->method(i);
        std::string method_name = pmd->name();
        std::cout<< "method_name= "<<method_name<<std::endl;
        service_info.method_map.emplace(method_name,pmd); 
    }
    service_info.service = service;
    auto result=service_map.emplace(service_name , std::move(service_info));
	if(!result.second){
		LOG(ERROR)<<"duplicate service"<<service_name;
		delete service;
		return ;
	}
}

void RpcProvider::Run(){
    std::string ip = RpcApplication::GetInstance().GetConfig().Load("rpcserverip");
    int port = 0;
	try{
		port=std::stoi(RpcApplication::GetConfig().Load("rpcserverport"));
	}catch(const std::exception& error){
		LOG(ERROR)<<"Invalid rpcserverport: "<<error.what();
		return ;
	}
	if(port<=0||port>65535){
		LOG(ERROR)<<"rpcserverport out of range";
		return;
	}
	if(ip.empty()){
		LOG(ERROR)<<"missing rpcserverip";
		return;
	}
    muduo::net::InetAddress address(ip,port);
    std::shared_ptr<muduo::net::TcpServer> server = std::make_shared<muduo::net::TcpServer>(&event_loop , address , "RpcProvider");
    server->setConnectionCallback(std::bind(&RpcProvider::OnConnection,this,std::placeholders::_1));
    server->setMessageCallback(std::bind(&RpcProvider::OnMessage,this,std::placeholders::_1,std::placeholders::_2, std::placeholders::_3));
    server->setThreadNum(4);
    m_thread_pool.start(100);
    ZkClient zkclient;
    zkclient.Start();
    for(auto &sp : service_map){
        std::string service_path = "/"+sp.first;
        zkclient.Create(service_path.c_str(),nullptr,0);
        std::string ip_port = ip+":"+ std::to_string(port);
        std::string instance_path = service_path + "/" + ip_port;
        zkclient.Create(instance_path.c_str(),ip_port.c_str(),ip_port.length(),ZOO_EPHEMERAL);
    }
    std::cout<<"RpcProvider start service at ip: "<<ip<<" port: "<<port<<std::endl;
    server->start();
    event_loop.loop();
}

void RpcProvider::OnConnection(const muduo::net::TcpConnectionPtr &conn){
    if(!conn->connected())
        conn->shutdown();
}

void RpcProvider::OnMessage(const muduo::net::TcpConnectionPtr &conn, muduo::net::Buffer *buffer ,muduo::Timestamp receive_time){
    while(buffer->readableBytes()>=4){
        uint32_t total_len = 0 ;
        std::memcpy(&total_len, buffer->peek(),4);
        total_len =ntohl(total_len);
		constexpr uint32_t max_frame_length=4*1024*1024;

		if(total_len<4||total_len>max_frame_length){
			LOG(ERROR)<<"invalid rpc frame length: "<<total_len;
			conn->shutdown();
			return;
		}

		if(buffer->readableBytes()<static_cast<std::size_t>(total_len)+4){
			break;
		}
        buffer->retrieve(4);
        uint32_t header_len = 0;
        const char* data_ptr = buffer->peek();
        std::memcpy(&header_len,data_ptr,4);
        header_len = ntohl(header_len);

		if(header_len>total_len-4){
			LOG(ERROR)<<"invalid header length";
			conn->shutdown();
			return;
		}
		buffer->retrieve(4);
        std::string rpc_header_str(buffer->peek(),header_len);
        rpc::RpcHeader rpcHeader;
		if(!rpcHeader.ParseFromString(rpc_header_str)){
			LOG(ERROR)<<"header parse error ";
			conn->shutdown();
			return ;
		}
        buffer->retrieve(header_len);

        uint32_t args_size = total_len-4-header_len;

		if(rpcHeader.args_size()!=args_size){
			LOG(ERROR)<<"args size mismatch";
			conn->shutdown();
			return;
		}

        std::string args_str(buffer->peek(),args_size);
        buffer->retrieve(args_size);

        std::string service_name = rpcHeader.service_name();
        std::string method_name = rpcHeader.method_name();

        auto it =service_map.find(service_name);
        if(it==service_map.end()){
            std::cout<<service_name<<"is not exist" <<std::endl;
            return ;
        }

        auto mit = it->second.method_map.find(method_name);
        if(mit == it->second.method_map.end()){
            std::cout<<service_name<<"."<<method_name<<" is not exist!"<<std::endl;
            return ;
        }

        google::protobuf::Service *service = it->second.service;
        const google::protobuf::MethodDescriptor *method = mit->second;

        google::protobuf::Message *request = service->GetRequestPrototype(method).New();
        if(!request->ParseFromString(args_str)){
            delete request;
            return ;
        }

        google::protobuf::Message *response = service->GetResponsePrototype(method).New();
        google::protobuf::Closure *done = new RpcClosure(
            [this ,conn , response ,request](){ this->SendRpcResponse(conn,response,request);});

        m_thread_pool.run([service,method,request,response,done](){
            service->CallMethod(method,nullptr,request,response,done);
        });
    }
}

void RpcProvider::SendRpcResponse(const muduo::net::TcpConnectionPtr& conn, google::protobuf::Message* response, google::protobuf::Message* request){
    if(response==nullptr||request==nullptr){
		LOG(ERROR)<<"invalid response or request ";
		delete response;
		delete request;
		return ;
	}
	if(!conn->connected()){
		LOG(ERROR)<<"connection already closed ";
		delete response;
		delete request;
		return ;
	}

	std::string response_str;
    if(response->SerializeToString(&response_str)){
        uint32_t len = response_str.size();
        uint32_t net_len  = htonl(len);
        std::string send_buf;
        send_buf.resize(4+len);
        std::memcpy(&send_buf[0],&net_len,4);
        std::memcpy(&send_buf[4],response_str.data(),len);
		conn->send(send_buf);        
    }
    else{
        LOG(ERROR)<<"serialize response error!";
    }
    delete response;
    delete request;
}

RpcProvider::~RpcProvider(){
    std::cout<<"~RpcProvider()"<<std::endl;
    for(auto &sp : service_map){
        if(sp.second.service!=nullptr){
            delete sp.second.service;
            sp.second.service = nullptr;
        }
    }
    event_loop.quit();
}