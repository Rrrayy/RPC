#include "rpc_controller.h" 

#include <iostream>
#include <string> 

int main(){
	RpcController controller; 

	if(controller.Failed()){ // 检查控制器初始状态是否错误。
		std::cerr<<"initial controller state is failed"<<std::endl; // 输出初始状态错误信息。
		return 1; 
	}

	controller.SetFailed("test failure"); // 设置一次模拟 RPC 失败。

	if(!controller.Failed()){ // 检查失败状态是否被正确记录。
		std::cerr<<"SetFailed did not mark controller as failed"<<std::endl; // 输出失败状态记录错误。
		return 1; // 
	}

	if(controller.ErrorText()!="test failure"){ // 检查失败原因是否被正确保存。
		std::cerr<<"ErrorText did not return the recorded reason"<<std::endl; // 输出错误原因读取错误。
		return 1; 
	}

	controller.Reset(); // 清除本次 RPC 调用的状态。

	if(controller.Failed()){ // 检查 Reset 是否清除了失败状态。
		std::cerr<<"Reset did not clear failed state"<<std::endl; // 输出状态清理错误。
		return 1; 
	}

	if(!controller.ErrorText().empty()){ // 检查 Reset 是否清除了错误信息。
		std::cerr<<"Reset did not clear error text"<<std::endl; // 输出错误信息清理错误。
		return 1; 
	}

	std::cout<<"RpcController test passed"<<std::endl; 
	return 0; 
}
