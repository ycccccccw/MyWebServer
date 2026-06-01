# MyWebServer

2026-06-01：去掉数据库账号密码硬编码
修改内容：

main.cpp 不再写死 MySQL 用户名、密码和数据库名。
新增环境变量读取逻辑：
MYSQL_USER
MYSQL_PASSWORD
MYSQL_DATABASE
新增 .env.example，只提供配置示例，不保存真实密码。





