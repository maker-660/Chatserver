#include "chatservice.hpp"
#include "public.hpp"
#include <muduo/base/Logging.h> //日志
#include <vector>
using namespace std;
using namespace muduo;
using namespace muduo::net;

// 获取单例对象接口函数
ChatService *ChatService::instance()
{
    static ChatService service;
    return &service;
}
// 注册消息以及对应的handler回调操作
ChatService::ChatService()
{
    // 用户基本业务管理相关事件处理回调注册
    _msgHandlerMap.insert({LOGIN_MSG, std::bind(&ChatService::login, this, _1, _2, _3)});
    _msgHandlerMap.insert({LOGINOUT_MSG, std::bind(&ChatService::loginout, this, _1, _2, _3)});
    _msgHandlerMap.insert({REG_MSG, std::bind(&ChatService::reg, this, _1, _2, _3)});
    _msgHandlerMap.insert({ONE_CHAT_MSG, std::bind(&ChatService::oneChat, this, _1, _2, _3)});
    _msgHandlerMap.insert({ADD_FRIEND_MSG, std::bind(&ChatService::addFriend, this, _1, _2, _3)});
    // 群组业务管理相关事件处理回调注册
    _msgHandlerMap.insert({CREATE_GROUP_MSG, std::bind(&ChatService::createGroup, this, _1, _2, _3)});
    _msgHandlerMap.insert({ADD_GROUP_MSG, std::bind(&ChatService::addGroup, this, _1, _2, _3)});
    _msgHandlerMap.insert({GROUP_CHAT_MSG, std::bind(&ChatService::groupChat, this, _1, _2, _3)});

    // 连接redis服务器
    if (_redis.connect())
    {
        // 设置上报消息的回调
        _redis.init_notify_handler(std::bind(&ChatService::handleRedisSubscribeMessage, this, _1, _2));
    }
}

// 处理注册 //test: {"msgid":1,"name":"luo jun","password":"123456"}
void ChatService::reg(const TcpConnectionPtr &conn, json &js, Timestamp)
{
    string name = js["name"];
    string pwd = js["password"];
    User user;
    user.setName(name);
    user.setPwd(pwd);
    bool state = _userModel.insert(user);
    if (state)
    {
        // 注册成功
        json response;
        response["msgid"] = REG_MSG_ACK;
        response["errno"] = 0;
        response["id"] = user.getId();
        conn->send(response.dump());
    }
    else
    {
        // 注册失败
        json response;
        response["msgid"] = REG_MSG_ACK;
        response["errno"] = 1;
        conn->send(response.dump());
    }
}

// 处理登录 ORM 业务层操作的都是对象 DAO
// 处理登录 id password password
void ChatService::login(const TcpConnectionPtr &conn, json &js, Timestamp)
{
    int id = js["id"].get<int>();
    string pwd = js["password"];
    User user = _userModel.query(id);
    // 登录 id password
    if (user.getId() == id && user.getPwd() == pwd)
    {
        // 已登录 //test: {"msgid":1,"id":13,"password":"123456"}
        if (user.getState() == "online")
        {
            json response;
            response["msgid"] = LOGIN_MSG_ACK;
            response["errno"] = 2;
            response["errmsg"] = "this account is using, input another!";
            conn->send(response.dump());
        }
        // 登录成功记录用户连接信息
        {
            lock_guard<mutex> lock(_connMutex); // 互斥锁，力度不要太大
            _userConnMap.insert({id, conn});
        }
        // id用户登录成功后，向redis订阅channel(id)
        _redis.subscribe(id);
        // 正常登录 //test: {"id":22,"password":"123456"}
        user.setState("online");
        _userModel.updateState(user);
        json response;
        response["msgid"] = LOGIN_MSG_ACK;
        response["errno"] = 0;
        response["id"] = user.getId();
        response["name"] = user.getName();

        // 查询该用户是否有离线消息
        vector<string> vec = _offlineMsgModel.query(id);
        if (!vec.empty())
        {
            response["offlinemsg"] = vec;
            // 读取离线消息后，把该用户所有离线消息删除掉
            _offlineMsgModel.remove(id);
        }

        // 查询该用户的好友消息并返回
        vector<User> userVec = _friendModel.query(id);
        if (!userVec.empty())
        {
            vector<string> vec2;
            for (User &user : userVec)
            {
                json js;
                js["id"] = user.getId();
                js["name"] = user.getName();
                js["state"] = user.getState();
                vec2.push_back(js.dump());
            }
            response["friends"] = vec2;
        }
        conn->send(response.dump());
    }
    // 登陆失败
    else
    {
        // // 用户不存在 //test: {"id":80,"password":"123456"}
        // if (user.getId() != id)
        // {
        //     json response;
        //     response["msgid"] = LOGIN_MSG_ACK;
        //     response["errno"] = 1;
        //     response["errmsg"] = "this id can not be found!";
        //     conn->send(response.dump());
        // }
        // // 密码错误
        // else // test: {"msgid":1,"id":22,"password":"1234567"}
        // {
        //     json response;
        //     response["msgid"] = LOGIN_MSG_ACK;
        //     response["errno"] = 2;
        //     response["errmsg"] = "the password is wrong!";
        //     conn->send(response.dump());
        // 该用户不存在，用户存在但是密码错误，登录失败
        json response;
        response["msgid"] = LOGIN_MSG_ACK;
        response["errno"] = 1;
        response["errmsg"] = "id or password is invalid!";
        conn->send(response.dump());
    }
}
// 服务器异常重置
void ChatService::reset()
{
    // 把online的用户状态信息，设置为offline
    _userModel.resetState();
}

// 获取消息对应的处理器
MsgHandler ChatService::getHandler(int msgid)
{
    // 记录错误日志
    auto it = _msgHandlerMap.find(msgid);
    if (it == _msgHandlerMap.end())
    {
        // 返回一个默认处理器，空操作
        return [=](const TcpConnectionPtr &conn, json &js, Timestamp)
        {
            LOG_ERROR << "msgid" << msgid << "can not find handler!";
        };
    }
    else
    {
        return _msgHandlerMap[msgid];
    }
}

// 处理注销业务
void ChatService::loginout(const TcpConnectionPtr &conn, json &js, Timestamp time)
{
    int userid = js["id"].get<int>();
    {
        lock_guard<mutex> lock(_connMutex);
        auto it = _userConnMap.find(userid);
        if (it != _userConnMap.end())
        {
            _userConnMap.erase(it);
        }
    }

    // 用户注销，相当于下线，在redis中取消订阅通道

    // 更新用户的状态信息
    User user(userid, "", "", "offline");
    _userModel.updateState(user);
}
// 处理客户端异常退出
void ChatService::clientCloseException(const TcpConnectionPtr &conn)
{
    User user;
    {
        lock_guard<mutex> lock(_connMutex);
        for (auto it = _userConnMap.begin(); it != _userConnMap.end(); ++it)
        {
            if (it->second == conn)
            {
                // 从map表删除用户的链接信息
                user.setId(it->first);
                _userConnMap.erase(it);
                break;
            }
        }
    }

    // 用户注销，相当于就是下线，在redis中取消订阅通道
    // _redis.unsubscribe(user.getId());

    // 更新用户的状态信息
    if (user.getId() != -1)
    {
        user.setState("offline");
        _userModel.updateState(user);
    }
}

// 1对1聊天业务
void ChatService::oneChat(const TcpConnectionPtr &conn, json &js, Timestamp time)
{
    int toid = js["to"].get<int>();
    // 标识用户是否在线
    bool userState = false;
    {
        lock_guard<mutex> lock(_connMutex);
        auto it = _userConnMap.find(toid);
        if (it != _userConnMap.end())
        {
            // 登录1 {"msgid":1,"id":13,"password":"123456"}
            // 登录2 {"msgid":1,"id":15,"password":"666666"}
            // 登录3 {"msgid":1,"id":22,"password":"123456"}
            // 1对1聊天{"msgid":5,"id":13,"from":"zhang san","to":15,"msg":"hello!"}
            // 添加好友{"msgid":6,"id":13,"friendid":22} {"msgid":6,"id":22,"friendid":13}
            // toid在线，转发消息 服务器主动推送消息给toid用户
            it->second->send(js.dump());
            return;
        }
        // toid不在线，存储离线消息
        _offlineMsgModel.insert(toid, js.dump());
    }
}

// 添加好友 msgid id fromid
void ChatService::addFriend(const TcpConnectionPtr &conn, json &js, Timestamp time)
{
    int userid = js["id"].get<int>();
    int friendid = js["friendid"].get<int>();

    // 存储好友信息
    _friendModel.insert(userid, friendid);
}

// 创建群业务
void ChatService::createGroup(const TcpConnectionPtr &conn, json &js, Timestamp time)
{
    int userid = js["id"].get<int>();
    string name = js["groupname"];
    string desc = js["groupdesc"];
    // 存储新创建的群组消息
    Group group(-1, name, desc);
    if (_groupModel.createGroup(group))
    {
        // 存储群组创建人信息
        _groupModel.addGroup(userid, group.getId(), "creator");
    }
}

// 加入群组业务
void ChatService::addGroup(const TcpConnectionPtr &conn, json &js, Timestamp time)
{
    int userid = js["jd"].get<int>();
    int groupid = js["groupid"].get<int>();
    _groupModel.addGroup(userid, groupid, "normal");
}

// 群组聊天业务
void ChatService::groupChat(const TcpConnectionPtr &conn, json &js, Timestamp time)
{
    int userid = js["id"].get<int>();
    int groupid = js["groupid"].get<int>();
    vector<int> useridVec = _groupModel.queryGroupUsers(userid, groupid);
    lock_guard<mutex> lock(_connMutex);
    for (auto id : useridVec)
    {
        auto it = _userConnMap.find(id);
        if (it != _userConnMap.end())
        {
            // 转发群消息
            it->second->send(js.dump());
        }
        else
        {
            // //查询toid是否在线
            // User user = _userModel.query(id);
            // if(user.getState()=="online")
            // {

            // }
            // else
            // {
            _offlineMsgModel.insert(id, js.dump());
            // }
        }
    }
}

// 从redis消息队列中获取订阅的消息
void ChatService::handleRedisSubscribeMessage(int userid, string msg)
{
    lock_guard<mutex> lock(_connMutex);
    auto it = _userConnMap.find(userid);
    if (it != _userConnMap.end())
    {
        it->second->send(msg);
        return;
    }

    // 存储该用户的离线消息
    _offlineMsgModel.insert(userid, msg);
}