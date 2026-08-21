#ifndef _RpcLogger_h
#define _RpcLogger_h
#include <glog/logging.h>
#include <string>

class RpcLogger{
public:
    explicit RpcLogger(const char* argv0){
        google::InitGoogleLogging(argv0);
        //启用彩色日志
        FLAGS_colorlogtostderr = true;
        //默认输出标准错误
        FLAGS_logtostderr = true;
    }
    ~RpcLogger(){
        google::ShutdownGoogleLogging();
    }

    static void Info(const std::string &message){
        LOG(INFO)<<message;
    }

    static void Warning(const std::string &message){
        LOG(WARNING)<<message;
    }
    static void ERROR(const std::string &message){
        LOG(ERROR)<<message;
    }
    static void Fatal(const std::string &message){
        LOG(FATAL)<<message;
    }
private:
    RpcLogger(const RpcLogger&) = delete;
    RpcLogger& operator=(const RpcLogger&) = delete;
};

#endif