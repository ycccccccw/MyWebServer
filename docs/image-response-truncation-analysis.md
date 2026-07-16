# 社区图片响应截断问题：排查、根因与修复

## 1. 文档目的

本文记录社区页面图片出现以下异常的完整排查过程：

- 页面打开后浏览器标签持续转圈。
- 图片开始时能够显示，经过一段时间后变成破图。
- 所有图片变成破图后，浏览器停止转圈。
- 文字内容始终能够正常显示。
- 单次请求有时正常，并发请求时更容易出现。

问题最终被确认是非阻塞文件响应发生部分写入后，部分连接没有可靠地继续发送剩余数据。服务端响应头中的 `Content-Length` 是完整文件大小，但连接关闭时客户端只收到了文件的一部分，所以浏览器最终判定图片损坏。

本文不仅描述最终修改，也保留中间的误判、证据和验证方法，便于以后排查类似的 Reactor、epoll、HTTP 长连接和大文件发送问题。

## 2. 相关架构

当前服务器采用 Main-Sub Reactor 加 Worker 线程池：

1. Main Reactor 监听服务端口并执行 `accept()`。
2. Main Reactor 将新连接轮询分配给某个 Sub Reactor。
3. Sub Reactor 负责客户端 socket 的非阻塞读写、连接超时和完成通知。
4. Worker 从任务队列中取出请求，解析 HTTP 报文并执行业务逻辑。
5. Worker 生成响应后，通过完成队列通知该连接所属的 Sub Reactor。
6. Sub Reactor 注册 `EPOLLOUT` 并发送响应。

静态文件响应由两部分组成：

- HTTP 响应头，保存在 `m_write_buf`。
- 文件内容，通过 `mmap()` 映射到 `m_file_address`。

图片文件往往大于 socket 发送缓冲区，因此一次 `send()` 或 `writev()` 不保证发送完整。

## 3. 为什么文字正常但图片失败

文字页面通常只有几百字节或几 KB，一次系统调用就能写入 socket 发送缓冲区。即使代码没有正确处理后续可写事件，小响应也不容易触发问题。

图片响应明显更大。例如排查时使用的测试图片为：

```text
root/xxx.jpg
文件大小：6,007,312 字节
```

非阻塞 socket 第一次只能接收其中一部分数据。当发送缓冲区写满后，系统调用返回 `EAGAIN`，服务器必须保存当前偏移并在 socket 再次可写时继续发送。

因此，这个问题具有以下特点：

- 小 HTML 响应正常。
- 大图片更容易失败。
- 并发越高，发送缓冲区越容易写满。
- 图片可以先渐进显示，因为浏览器已经收到了有效的文件头和前半部分。
- 浏览器直到连接关闭才确认实际长度小于 `Content-Length`，随后将图片标记为损坏。

## 4. 最初观察到的干扰因素

### 4.1 文本 `123` 与 `888`

最初观察到发布 `123` 时图片失败，而发布 `888` 时图片正常。这并不是文字内容影响了图片。

检查结果显示：

- 两次上传文件均存在。
- 文件大小相同。
- 文件格式均为有效 PNG。
- 两个文件的 SHA-256 相同。
- 数据库和 `community.html` 中保存的路径正确。

实际原因是不同请求碰巧经历了不同的 socket 写入时序。`123` 只是与一次失败请求同时出现，文字本身没有参与文件发送逻辑。

### 4.2 上传资源限流

旧代码对每一个 `/uploads/...` 请求执行令牌桶限流：

```cpp
if (url.find("/uploads/") == 0)
    return allow_by_token_bucket("uploads_ip:" + client_ip, 60, 6.0);
```

社区页面包含很多图片时，一次页面访问会快速消耗同一 IP 的令牌。令牌耗尽后，图片 URL 返回 HTTP 429 和 HTML 错误正文。浏览器将 HTML 当成图片解码，自然会显示破图。

这是一个真实问题，但不是所有图片在发送一段时间后统一损坏的唯一原因。它已被单独修复：

- `/community.html`、上传、登录和注册仍然限流。
- `/uploads/` 不再按每张图片扣除同一个 IP 的令牌。
- 上传资源仍然执行 Session 权限校验。
- 新生成的社区页面给图片增加 `loading="lazy"` 和 `decoding="async"`。

### 4.3 Session 重启失效

Session 保存在进程内存中。服务器重启后，浏览器中的旧 `sid` Cookie 仍然存在，但服务端 Session 映射已经清空。

这时受保护的图片请求可能被映射到登录页面，表现为图片 URL 收到 `200 text/html`。因此每次重启服务器后都需要重新登录。

这同样会产生破图，但与文件响应被截断是不同问题。

## 5. 第一个代码问题：部分写入偏移

旧的 `writev()` 逻辑会修改 `m_iv[0].iov_len`，随后又使用这个已经改变的长度和累计发送字节数比较。

简化后的风险逻辑如下：

```cpp
if (bytes_have_send >= m_iv[0].iov_len) {
    // 判断响应头已发送完成
}
```

但 `m_iv[0].iov_len` 在前一次部分写入后可能已经缩短。累计发送量与一个不断变化的长度比较，可能过早判断响应头已经发送完成，并计算错误的文件偏移。

第一阶段修复将判断基准改为原始响应头总长度 `m_write_idx`：

```cpp
if (bytes_have_send < m_write_idx) {
    // 继续发送剩余响应头
} else {
    int file_bytes_sent = bytes_have_send - m_write_idx;
    // 从正确文件偏移继续发送
}
```

这个修改解决了 iovec 偏移计算风险，但后续并发验证证明，服务器仍然存在 `EAGAIN` 后没有继续发送的问题。

## 6. 第二个问题：错误地把发送连接当成空闲连接

Sub Reactor 最初对所有连接使用同一个 15 秒超时：

```text
3 * TIMESLOT = 15 秒
```

这没有区分：

- 已经发送完响应、正在等待下一个请求的空闲 keep-alive 连接。
- 仍有响应数据等待写入的连接。

如果图片尚未发送完整，却长时间没有收到下一次可写事件，连接会被当作空闲连接关闭。

修改后使用两种期限：

- keep-alive 空闲期限：15 秒。
- 响应发送期限：60 秒。

Worker 生成响应后进入发送期限；每次发送取得进展时刷新发送期限；完整发送后才恢复为空闲期限。

这个修改延缓了错误关闭，并明确了连接状态，但它仍然不能解决“可写通知永远没有再次到达”的根本问题。

## 7. 建立可重复验证方法

仅靠浏览器观察不能判断问题发生在哪一层，因此使用 `curl`、文件大小和 SHA-256 建立验证闭环。

### 7.1 单连接下载

请求运行中的 9006 服务：

```bash
curl -v --max-time 90 \
  http://127.0.0.1:9006/xxx.jpg \
  -o /tmp/server-xxx.jpg
```

失败时服务器返回：

```text
HTTP/1.1 200 OK
Content-Length:6007312
Content-Type:image/jpeg
Connection:keep-alive
```

但客户端实际只收到：

```text
5,274,955 字节
```

`curl` 报告：

```text
transfer closed with 732357 bytes remaining to read
```

这证明了以下事实：

- HTTP 状态和 MIME 类型正确。
- 服务端文件大小声明正确。
- 文件传输没有完成。
- 浏览器破图是响应体截断，不是 HTML 或 CSS 显示问题。

### 7.2 并发下载与哈希校验

使用 10 路并发连续下载 30 次：

```bash
seq 1 30 | xargs -P 10 -I{} sh -c '
  curl -sS --max-time 30 \
    http://127.0.0.1:9006/xxx.jpg \
    -o /tmp/xxx-{}.jpg
  sha256sum /tmp/xxx-{}.jpg
'
```

源文件 SHA-256：

```text
5b2eab8b319301688260a52f04ff1a606033e6597c07cb36e464097bbf385c7e
```

修复前的结果不是偶发单例，而是能稳定出现多次截断。失败连接的缺失量经常相同，例如：

```text
transfer closed with 3393323 bytes remaining to read
```

这意味着客户端只收到第一次写入 socket 缓冲区的数据，剩余部分没有继续发送。

## 8. 使用系统调用跟踪确认发送过程

使用 `strace` 跟踪：

```bash
strace -ff -tt \
  -e trace=writev,epoll_ctl,close \
  -p <server-pid>
```

一次成功发送的系统调用如下：

```text
writev(... header=91 bytes, file=6007312 bytes ...) = 2614080
writev(... remaining file=3393323 bytes ...)       = 3393323
```

两次返回值相加：

```text
2,614,080 + 3,393,323 = 6,007,403
```

其中包含：

- 91 字节 HTTP 响应头。
- 6,007,312 字节文件正文。

成功连接会发生第二次写入，失败连接通常停留在第一次写入后。

## 9. 直接记录关闭原因

为了避免继续猜测，在 Sub Reactor 的所有关闭路径临时加入原因记录，包括：

- `write()` 返回失败。
- Worker 请求关闭。
- 处理期间超时。
- `EPOLLHUP` 或 `EPOLLERR`。
- 截止时间到期。

最终失败连接的诊断结果为：

```text
close_reason=deadline fd=11 pending=1
close_reason=deadline fd=18 pending=1
close_reason=deadline fd=20 pending=1
```

`pending=1` 表示连接关闭时 `bytes_to_send > 0`，仍然存在未发送文件数据。

这排除了以下可能：

- 服务器错误地认为文件已经全部发送。
- `Content-Length` 计算错误。
- Worker 主动关闭连接。
- 图片文件本身不完整。

最终结论是：socket 第一次写满并返回 `EAGAIN` 后，部分连接没有再次得到有效的 `EPOLLOUT` 续写机会，直到发送截止时间到期才被关闭。

## 10. 最终发送状态机

为了降低可变 iovec 状态的复杂度，最终实现改为显式的两阶段发送。

### 10.1 发送响应头

当累计发送量小于响应头长度时：

```cpp
data = m_write_buf + bytes_have_send;
remaining = m_write_idx - bytes_have_send;
```

### 10.2 发送文件正文

响应头发送完成后：

```cpp
int file_bytes_sent = bytes_have_send - m_write_idx;
data = m_file_address + file_bytes_sent;
remaining = m_file_stat.st_size - file_bytes_sent;
```

### 10.3 只按真实返回值推进

```cpp
ssize_t sent = send(m_sockfd, data, remaining, MSG_NOSIGNAL);

bytes_have_send += sent;
bytes_to_send -= sent;
```

不会根据预期长度假设数据已经发送，也不会通过被修改过的 iovec 长度推断偏移。

### 10.4 错误处理

```cpp
if (errno == EINTR)
    continue;

if (errno == EAGAIN || errno == EWOULDBLOCK) {
    modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);
    return true;
}
```

- `EINTR`：系统调用被信号中断，保持偏移并立即重试。
- `EAGAIN`：发送缓冲区暂时不可写，保留状态并等待续写。
- 其他错误：解除文件映射并关闭连接。
- 使用 `MSG_NOSIGNAL` 防止对端关闭时触发 `SIGPIPE`。

## 11. 调整 epoll 写事件策略

读取仍然使用 ET 和 ONESHOT：

```text
EPOLLIN | EPOLLET | EPOLLONESHOT | EPOLLRDHUP
```

读取函数会一直读取到 `EAGAIN`，适合边缘触发。

写事件改用 LT 和 ONESHOT：

```text
EPOLLOUT | EPOLLONESHOT | EPOLLRDHUP
```

这样只要 socket 处于可写状态，Sub Reactor 就有机会继续处理，而不依赖一次容易遗漏的可写边沿。

## 12. 调整 Sub Reactor 事件优先级

旧逻辑优先处理：

```cpp
EPOLLRDHUP | EPOLLHUP | EPOLLERR
```

这可能在仍有响应数据时过早关闭连接。

新逻辑为：

1. `EPOLLHUP` 或 `EPOLLERR`：关闭异常连接。
2. 如果连接仍有待发送数据且出现 `EPOLLOUT`：优先续写。
3. `EPOLLIN`：读取请求。
4. 普通 `EPOLLOUT`：处理写入。
5. 只有没有更优先事件时，才根据 `EPOLLRDHUP` 关闭连接。

其目标是允许服务器在客户端半关闭写方向后，仍然完成已经生成的 HTTP 响应。

## 13. 待续写集合

仅依赖 epoll 的写事件重新通知，在实际环境中仍然出现少量遗漏。为此，每个 Sub Reactor 增加：

```cpp
std::unordered_set<int> m_pending_writes;
```

### 13.1 加入集合

Worker 生成可写响应时：

```cpp
m_pending_writes.insert(sockfd);
```

`send()` 返回 `EAGAIN` 后，连接仍然留在集合中。

### 13.2 移出集合

只有满足以下条件之一时才移除：

- `bytes_to_send` 变为 0，响应完整发送。
- 连接发生不可恢复错误并关闭。
- 连接被回收。

### 13.3 补偿重试

Sub Reactor 仍然通过 `epoll_wait()` 阻塞，不进行无限循环轮询。每秒最多对待续写集合做一次补偿发送：

```cpp
if (now != last_write_retry) {
    last_write_retry = now;
    // retry pending writes
}
```

这个机制用于补偿遗漏的 `EPOLLOUT` 通知。它不是忙等待，因为：

- 线程绝大部分时间阻塞在 `epoll_wait()`。
- 补偿有一秒时间间隔。
- 只检查确实存在待发送数据的 fd。
- 完整发送后立即从集合移除。

## 14. 最终验证结果

最终版本在独立端口启动：

```bash
./server -p 19006 -r 1 -t 2 -s 2 -n 256 -m 3 -c 1
```

使用同一张 6,007,312 字节图片执行 30 次、10 路并发下载，并对每个结果计算 SHA-256。

最终结果：

```text
valid=30
failures=0
```

随后在正式 9006 实例上执行 10 路并发复测：

```text
valid=10
```

所有下载文件的 SHA-256 均与源文件一致。这同时验证了：

- 响应头完整。
- 文件正文没有缺字节。
- 部分写入后可以正确续传。
- 并发连接不会相互覆盖发送状态。
- 连接不会在仍有待发送数据时按空闲连接回收。

## 15. 相关提交

排查过程中相关提交如下：

```text
de34343 Optimize static responses and document multi-reactor server
5418ce4 Avoid throttling authenticated community media
eafec06 Separate response-write and idle timeouts
b454894 Guarantee partial response write completion
```

最终解决文件截断的核心提交是：

```text
b454894 Guarantee partial response write completion
```

## 16. 经验总结

### 16.1 `send()` 和 `writev()` 成功不等于发送完整

返回正数只代表本次接受了部分字节。必须维护累计偏移，直到待发送长度归零。

### 16.2 `EAGAIN` 不是连接错误

它只表示当前发送缓冲区已满。关闭连接会直接造成 HTTP 响应截断。

### 16.3 ET 必须配合彻底排空

读事件比较容易通过循环读取到 `EAGAIN` 实现。写事件涉及大文件、发送窗口和对端读取速度，状态更复杂。写入使用 LT 可以降低边沿重挂风险。

### 16.4 超时必须感知连接状态

等待下一个请求的空闲连接和正在发送响应的连接不能使用完全相同的超时语义。

### 16.5 浏览器显示不是完整性证明

图片能够暂时显示，只能说明浏览器收到了可解码的前缀。必须使用 `Content-Length`、实际下载字节数和哈希校验确认响应完整。

### 16.6 必须用并发测试验证时序问题

单次请求可能连续成功。只有重复并发请求才能稳定触发发送缓冲区写满、`EAGAIN` 和事件重挂路径。

### 16.7 诊断要记录关闭原因

只记录“连接关闭”不足以定位问题。至少应区分：

- 对端关闭。
- epoll 错误。
- 业务处理失败。
- 发送失败。
- 空闲超时。
- 仍有待发送数据时的发送超时。

本次问题最终依靠 `deadline + pending=1` 这一证据完成定性。
