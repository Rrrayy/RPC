#ifndef _RpcApplication_H
#define _RpcApplication_H
#include "rpc_config.h"
#include <mutex>

class RpcApplication{
public:
    static void Init(int argc , char **argv);               //解析 -i 配置
    static RpcApplication &GetInstance();                   //拿单例
    static void deleteInstance();                           //程序退出销毁单例
    static RpcConfig & GetConfig();                         //拿全局配置
private:
    static RpcConfig m_config;                              //全局配置对象
    static RpcApplication *m_application;                   //单例对象指针
    static std::mutex m_mutex;
    RpcApplication(){}                                      //私有构造
    ~RpcApplication(){}
    RpcApplication(const RpcApplication&) = delete;         //禁止拷贝
    RpcApplication(RpcApplication&&) = delete;        //禁止移动
};

#endif