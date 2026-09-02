#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include "../user.pb.h"
#include "rpc_application.h"
#include "rpc_provider.h"
/*
  UserService 原本是一个本地服务，提供了两个本地方法：Login 和 GetFriendLists。
  现在通过 RPC 框架，这些方法可以被远程调用。
*/
class UserService : public fixbug::UserServiceRpc
{
public:
    bool Login(std::string username, std::string password)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        return !username.empty()&&!password.empty();
    }

    void login(::google::protobuf::RpcController *controller,
                const ::fixbug::LoginRequst *request,
                ::fixbug::LoginResponse *response,
                ::google::protobuf::Closure *done)
    {
    	if(request==nullptr	|| response==nullptr){
			if(controller!=nullptr)
				controller->SetFailed("Invalid rpc argument");
			return;
		}
		std::string username = request->username();
        std::string password = request->password();

        bool login_result = Login(username, password);

		if(login_result){
			response->set_errcode(0);
			response->set_msg("login success");
		}
		else{
			response->set_errcode(1);
			response->set_msg("login failed");
		}
		if(done!=nullptr)
        	done->Run();
    }
};

int main(int argc, char **argv)
{
    RpcApplication::Init(argc, argv);

    RpcProvider provider;

    provider.NotifyService(new UserService());

    provider.Run();

    return 0;
}