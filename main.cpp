#include "config.h"

#include <cstdlib>
#include <iostream>
#include <string>

static std::string get_env_or_exit(const char *name)
{
    const char *value = std::getenv(name);
    if (value == nullptr || value[0] == '\0')
    {
        std::cerr << "Missing required environment variable: " << name << std::endl;
        std::cerr << "Please set it before starting the server." << std::endl;
        std::exit(1);
    }
    return std::string(value);
}

int main(int argc, char *argv[])
{
    // MySQL configuration is read from environment variables.
    // Do not hardcode real database usernames or passwords in source code.
    std::string user = get_env_or_exit("MYSQL_USER");
    std::string passwd = get_env_or_exit("MYSQL_PASSWORD");
    std::string databasename = get_env_or_exit("MYSQL_DATABASE");

    // 命令行解析
    Config config;
    config.parse_arg(argc, argv);

    WebServer server;

    // 初始化
    server.init(config.PORT, user, passwd, databasename, config.LOGWrite,
                config.OPT_LINGER, config.TRIGMode, config.sql_num, config.thread_num,
                config.close_log, config.actor_model);

    // 日志
    server.log_write();

    // 数据库
    server.sql_pool();

    // 线程池
    server.thread_pool();

    // 触发模式
    server.trig_mode();

    // 监听
    server.eventListen();

    // 运行
    server.eventLoop();

    return 0;
}
