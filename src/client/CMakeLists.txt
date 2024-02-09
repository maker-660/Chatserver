#客户端开发一首页面功能开发以及测试_ev
# 定义一个SRC_LIST变量，包含该目录下所有源文件
aux_source_directory(. SRC_LIST)

#指定生成可执行文件
add_executable(ChatClient ${SRC_LIST})

#指定可执行文件链接时需要的库文件
target_link_libraries(ChatClient pthread)