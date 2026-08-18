#include <iostream>
#include "rpc_application.h"

int main(int argc , char** argv){
    RpcApplication::Init(argc ,argv);
    //得到全局配置
    RpcConfig &config = RpcApplication::GetConfig();
    std::cout << "rpcserverip = " << config.Load("rpcserverip") << std::endl;
    std::cout << "rpcserverport = " << config.Load("rpcserverport") << std::endl;
    std::cout << "zookeeperip = " << config.Load("zookeeperip") << std::endl;
    std::cout << "zookeeperport = " << config.Load("zookeeperport") << std::endl;
    return 0;
}