#ifndef _zookeeperutil_h_
#define _zookeeperutil_h_

#include<semaphore.h>
#include<zookeeper/zookeeper.h>
#include<vector>
#include<string>

class ZkClient{
public:
    ZkClient();
    ~ZkClient();
    //启动连接zkserver
    void Start();
    //在zkserver中根据指定的path创建新节点 
    void Create(const char* path , const char* data , int datalen, int state=0);
    //根据参数指定的znode结点路径或节点值
    std::string GetData(const char* path);
    std::vector<std::string> GetChildren(const char* path, watcher_fn in, void* cbContext);
private:
    zhandle_t* m_zhandle;
};
#endif