#ifndef RPC_CHANNEL_h_
#define RPC_CHANNEL_h_

#include <google/protobuf/service.h>
#include <unistd.h>
#include "service_discovery.h"

class RpcChannel : public google::protobuf::RpcChannel
{
public:

    RpcChannel(bool connectNow = false) {} 
    
   
    virtual ~RpcChannel() override {}

    void CallMethod(const ::google::protobuf::MethodDescriptor *method,
                    ::google::protobuf::RpcController *controller,
                    const ::google::protobuf::Message *request,
                    ::google::protobuf::Message *response,
                    ::google::protobuf::Closure *done) override;

private:
    // 内部辅助函数声明
    ssize_t recv_exact(int fd, char *buf, size_t size);
};

#endif