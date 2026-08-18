#ifndef _RpcConfig_h
#define _RpcConfig_h
#include<unordered_map>
#include<string>

class RpcConfig{
public:
    void LoadConfigFile(const char* config_file);
    std::string Load(const std::string &key);
private:
    std::unordered_map<std::string,std::string> config_map;
    void Trim(std::string &read_buf);
};

#endif