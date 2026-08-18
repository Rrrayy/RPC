#include"rpc_config.h"
#include<memory>
//加载配置文件
void RpcConfig::LoadConfigFile(const char* config_file){
    std::unique_ptr<FILE,decltype(&fclose)> pf(fopen(config_file,"r"),&fclose);
    if(pf == nullptr)
        exit(EXIT_FAILURE);
    char buf[1024];
    while(fgets(buf,1024,pf.get()) != nullptr){
        std::string read_buf(buf);
        Trim(read_buf);
        //跳过注释和空行
        if(read_buf[0] == '#'  || read_buf.empty())
            continue;
        int index = read_buf.find('=');
        if(index == -1)
            continue;
        std::string key = read_buf.substr(0,index);
        Trim(key);
        std::string value = read_buf.substr(index+1);
        Trim(value);

        config_map.insert({key,value});
    }
}

std::string RpcConfig::Load(const std::string &key){
    auto it = config_map.find(key);
    return it == config_map.end()? "" : it->second; 
}

void RpcConfig::Trim(std::string &read_buf){
    int index = read_buf.find_first_not_of(' ');
    if(index != -1){
        read_buf = read_buf.substr(index ,read_buf.size() - index);
    }
    index = read_buf.find_last_not_of(' ');
    if(index != -1){
        read_buf = read_buf.substr(0,index+1);
    }
}