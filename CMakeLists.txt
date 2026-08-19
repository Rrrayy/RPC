cmake_minimum_required(VERSION 3.0)
project(rpc)

# C++17 标准
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# 查找 protobuf
find_package(Protobuf REQUIRED)
include_directories(${Protobuf_INCLUDE_DIRS})

# 全局链接库（目前只用 protobuf
set(LIBS
    protobuf
    pthread
    muduo_net
    muduo_base
    glog
    zookeeper_mt
)

# 框架核心库：GLOB 收集 src 下所有 .cc 和 .pb.cc
file(GLOB RPC_SRC ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cc)
file(GLOB PROTO_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.pb.cc)
add_library(rpc_core STATIC ${RPC_SRC} ${PROTO_SRCS})
 target_compile_definitions(rpc_core PUBLIC THREADED)
# 头文件路径：src/include（自己的头文件）+ src（pb 生成物）
target_include_directories(rpc_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(rpc_core PUBLIC ${LIBS})

# 验证程序
add_executable(config_test example/config_test.cpp)
target_link_libraries(config_test rpc_core)
# RPC 服务端
add_executable(server example/callee/server.cc example/user.pb.cc)
target_link_libraries(server rpc_core)