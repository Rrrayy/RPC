#ifndef _RpcController_H
#define _RpcController_H
#include<google/protobuf/service.h>
#include<string>  
//描述RPC调用的控制器 跟踪RPC方法调用的状态，错误信息并提供功能控制

class RpcController:public google::protobuf::RpcController{
public:
    RpcController();
    void Reset();
    bool Failed() const;
    std::string ErrorText() const;
    void SetFailed(const std::string &reason);

    //待实现具体的功能
    void StartCancel();
    bool IsCanceled() const;
    void NotifyOnCancel(google::protobuf::Closure* callback);

private:
    bool m_failed;          //RPC方法执行过程中的状态
    std::string m_errText;  //RPC方法执行过程中的错误信息
};

#endif