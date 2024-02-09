实现了集群式聊天服务器，基于muduo网络库，通过nginx tcp负载均衡算法，利用redis发布订阅模型作为中间件，实现跨服务器通信。
文件说明:
/bin 产生的可执行文件
/include 文件编译所依赖的库文件
/src C++源文件
/build 编译产生的过程文件
/thirdparty 依赖的第三方库，序列化方法json.hpp
