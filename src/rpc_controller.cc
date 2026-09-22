#include "rpc_controller.h"

RpcController::RpcController()
	:m_failed(false), // 初始化调用失败状态为未失败。
	m_errText() // 初始化错误信息为空字符串。
{
}

void RpcController::Reset(){
	m_failed=false; // 清除上一轮调用留下的失败状态。
	m_errText.clear(); // 清除上一轮调用留下的错误信息。
}

bool RpcController::Failed() const{
	return m_failed; // 返回当前调用是否已被标记为失败。
}

std::string RpcController::ErrorText() const{
	return m_errText; // 返回当前调用失败时记录的错误原因。
}

void RpcController::SetFailed(const std::string& reason){
	m_failed=true; // 标记当前调用失败。
	m_errText=reason; // 保存本次调用失败的具体原因。
}

void RpcController::StartCancel(){
	// 当前同步调用模型尚未实现请求取消；后续异步请求状态机完成后在这里触发取消。
}

bool RpcController::IsCanceled() const{
	return false; // 当前模型不支持取消，因此请求始终未取消。
}

void RpcController::NotifyOnCancel(google::protobuf::Closure* callback){
	(void)callback; // 当前模型不会触发取消回调，调用方仍保有 callback 的所有权。
}
