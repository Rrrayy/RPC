#include "rpc_application.h"
#include <iostream>
#include <cstdlib>
#include <unistd.h>

RpcConfig RpcApplication::m_config;
std::mutex RpcApplication::m_mutex;
RpcApplication *RpcApplication::m_application = nullptr;

//解析命令行并取到配置文件 移交给config解析 
void RpcApplication::Init(int argc , char** argv){
    if(argc < 2){
        std::cout<< "格式: command -i <配置文件路径> "<<std::endl;
        exit(EXIT_FAILURE);
    }
    int o;
    std::string config_file;
    while(-1 != (o = getopt(argc , argv , "i:"))){
        switch(o){
            case 'i':
                config_file = optarg;
                break;
            case '?':
            case ':':
                std::cout<<"格式: command -i <配置文件路径>  "<<std::endl;
                exit(EXIT_FAILURE);
                break;
            default:
                break;
        }
    }

    m_config.LoadConfigFile(config_file.c_str());
}

RpcApplication &RpcApplication::GetInstance(){
    std::lock_guard<std::mutex> lock(m_mutex);
    if(m_application == nullptr){
        m_application = new RpcApplication();
        //程序退出自动销毁  raii
        atexit(deleteInstance);
    }
    return *m_application;
}

void RpcApplication::deleteInstance(){
    if(m_application){
        delete m_application;
        m_application = nullptr;
    }
}

RpcConfig &RpcApplication::GetConfig(){
    return m_config;
}