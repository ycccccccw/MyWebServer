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

### 2026-06-02：增加基于 Cookie 的服务端 Session 登录态

修改前：

- 登录成功后只是把 URL 改成 `/welcome.html`。
- 用户可以直接访问 `/welcome.html`。
- 没有 session、cookie、token、过期时间校验。

修改后：

- 登录成功后生成随机 `sid`。
- 服务端维护 `sid -> username / expire_time` 映射。
- 响应头返回：
http
Set-Cookie: sid=...; Max-Age=1800; HttpOnly; SameSite=Lax


### 2026-06-02：日志敏感信息脱敏

修改前：

- `process_read()` 会把 HTTP 请求行、请求头、POST body 原样写入日志。
- 登录/注册时，POST body 中可能包含 `password=...`。
- 增加 session 后，请求头中可能包含 `Cookie: sid=...`。
- 响应头中可能包含 `Set-Cookie: sid=...`。
- 如果这些内容进入日志，日志泄露时会暴露密码或登录凭证。

修改后：

- 新增 `sanitize_log_text()` 脱敏函数。
- 对以下内容进行脱敏：
  - `Cookie`
  - `Set-Cookie`
  - `Authorization`
  - `password`
  - `passwd`
  - `token`
  - `sid`
- 敏感内容统一替换为：[sensitive data masked]

## 2026-06-02 用户内容发布功能

本次新增了登录用户发布内容功能。

功能说明

- 登录用户可以在 `welcome.html` 点击“发布内容”进入 `upload.html`。
- 用户可以发布文字内容。
- 用户可以上传图片或视频。
- 上传成功后会跳转到 `community.html`。
- 其他登录用户可以在 `community.html` 查看所有用户发布的内容。
- 未登录用户不能直接访问 `upload.html`、`community.html` 和 `/uploads/` 下的上传资源。

支持的上传类型

图片：jpg、jpeg、png、gif
视频：mp4、webm
文本：txt

### 2026-06-08：上传接口增加 CSRF Token 校验

修改原因：

- 项目已经使用 Cookie + Session 实现登录态。
- Cookie 会被浏览器自动携带。
- 如果用户登录后访问恶意网站，恶意网站可能伪造表单请求 `/upload`。
- 因此对发布内容这种敏感 POST 操作增加 CSRF Token 校验。

实现方式：

- 登录成功时，服务端为 session 生成随机 `csrf_token`。
- 服务端保存 `sid -> username / csrf_token / expire_time`。
- 响应中返回两个 Cookie：
  - `sid`：HttpOnly，用于登录态。
  - `csrf_token`：供前端读取并写入隐藏表单字段。
- `upload.html` 提交时携带隐藏字段 `csrf_token`。
- `/upload` 处理时，服务端校验表单中的 token 是否和 session 中保存的一致。
- 校验失败则拒绝上传。

### 2026-06-23：增加单机流量控制

本次新增单机版流量控制功能，暂不依赖 Nginx 和 Redis。

实现内容：

- 使用令牌桶算法限制请求频率。
- 根据不同接口设置不同限流策略。
- `/upload` 上传接口限制为同一 IP 短时间最多突发 5 次，之后约每 10 秒恢复 1 次。
- `/community.html`、`/uploads/`、登录、注册接口分别设置独立限流 key。
- 上传接口增加 `Content-Length` 大小限制，避免大文件占用过多内存。
- 登录失败增加限制：同一 IP 或同一用户名 5 分钟内失败 5 次后冻结 5 分钟。
- 新增 HTTP 429 Too Many Requests 响应。

当前是单机内存版限流，服务器重启后限流记录会清空。后续如果部署多实例，可以将限流状态迁移到 Redis，并在 Nginx 层增加粗粒度 IP 限流。

### 2026-07-15：连接管理与内存池优化

本次对 WebServer 的连接资源管理做了工程化改造，目标是降低 fd 编号和内存分配之间的耦合，并减少高并发短连接场景下的频繁动态分配。

主要优化：

- 将 `users` 从 `http_conn[MAX_FD]` 数组改为 `fd -> http_conn*` 映射。
- 将 `users_timer` 从 `client_data[MAX_FD]` 数组改为 `fd -> client_data*` 映射。
- 增加 `http_conn` 对象池，连接关闭后对象归还池中复用。
- 增加 `client_data` 和 `util_timer` 对象池，避免每个连接反复 `new/delete` 定时器相关对象。
- 定时器链表只负责排序和摘链，`util_timer` 的内存生命周期交给对象池管理。
- 修复空定时器链表插入首个节点时可能形成异常链表的问题。
- 将原来的 `MAX_FD` 语义改为 `DEFAULT_MAX_CONNECTIONS`，明确表示最大并发连接数，而不是系统 fd 最大编号。
- 新增启动参数 `-n` 配置最大并发连接数，例如 `./start_server.sh -n 4096`。

优化后的含义：

- `ulimit -n` 控制系统允许进程打开的 fd 上限。
- `max_connections` 控制服务器愿意同时维护的连接对象数量。
- fd 编号可以很大，但服务器只按真实最大并发连接数预分配连接对象、定时器数据和定时器节点。

上传请求也做了缓冲区调整：

- 普通请求默认使用较小的读缓冲区。
- 上传请求先解析请求头，根据 `Content-Length` 判断 body 大小。
- 大 body 使用额外动态缓冲区保存，避免每个普通连接都长期携带大上传缓冲区。
- 上传内容超过限制时返回自定义 `413 Payload Too Large` 页面。
