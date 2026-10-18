#include "consistent_hash.h"
#include <iostream>
#include <stdexcept>

void require(bool condition,const char* message){
	if(!condition){throw std::runtime_error(message);}
}

int main(){
	try{
		ConsistentHash ring;
		require(ring.GetTargetNode("request").empty(),"empty ring must not select a node");
		ring.UpdateNodes({"127.0.0.1:8000"});
		require(ring.GetTargetNode("request")=="127.0.0.1:8000","single node routing failed");
		require(ring.GetTargetNode("request","127.0.0.1:8000").empty(),"must not select excluded virtual nodes");
		ring.UpdateNodes({"127.0.0.1:8000","127.0.0.1:8001"});
		for(int index=0;index<1000;++index){
			const auto key=std::to_string(index);
			const auto primary=ring.GetTargetNode(key);
			const auto alternative=ring.GetTargetNode(key,primary);
			require(alternative=="127.0.0.1:8000"||alternative=="127.0.0.1:8001","alternative missing");
			require(alternative!=primary,"alternative must be a different physical node");
			require(ring.GetTargetNode(key)==primary,"exclusion must not mutate the shared ring");
		}
		std::cout<<"Hash alternative test passed"<<std::endl;
		return 0;
	}catch(const std::exception& error){
		std::cerr<<error.what()<<std::endl;
		return 1;
	}
}
