# MyWebServer

### 2026-06-01：去掉数据库硬编码
修改内容：

main.cpp 不再写死 MySQL 用户名、密码和数据库名。
新增环境变量读取逻辑：
MYSQL_USER
MYSQL_PASSWORD
MYSQL_DATABASE
新增 .env.example，只提供配置示例，不保存真实密码。


### 2026-06-01：用户密码改为带盐哈希存储

修改前：

- 注册时直接把用户输入的原始密码写入数据库。
- 登录时从数据库读取 `username, passwd`，然后用明文密码直接比较。
- 程序运行时会把所有用户的明文密码放进 `map<string, string>`。

存在风险：

- 数据库泄露后，用户密码直接暴露。
- 程序内存或日志泄露时，也可能暴露用户密码。
- 相同密码在数据库中表现完全相同，容易被识别。
- 不符合真实 Web 项目的密码安全要求。

修改后：

- 新增 `http/password_hash.h`。
- 使用 OpenSSL 提供的 `PBKDF2-HMAC-SHA256` 计算密码哈希。
- 每次注册时随机生成 salt。
- 数据库 `passwd` 字段保存的不再是原始密码，而是：pbkdf2_sha256$迭代次数$salt$hash


### 2026-06-01：注册 SQL 改为 prepared statement

修改前：

- 注册逻辑使用 `strcpy`、`strcat` 手动拼接 SQL。
- 用户名和密码直接进入 SQL 字符串。
- 如果用户输入特殊字符，可能造成 SQL 注入。
- 固定长度 SQL 缓冲区也有溢出风险。

修改后：

- 新增 `insert_user_by_stmt()` 函数。
- 使用 MySQL prepared statement：
  - SQL 模板：`INSERT INTO user(username, passwd) VALUES(?, ?)`
  - `?` 作为参数占位符。
  - 用户名和密码通过 `mysql_stmt_bind_param()` 绑定。
- SQL 结构和用户输入分离，用户输入不会被当成 SQL 代码执行。





