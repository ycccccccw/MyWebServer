#include "http_conn.h"
#include "password_hash.h"
#include <mysql/mysql.h>
#include <openssl/rand.h>
#include <fstream>
#include <ctime>
#include <sstream>
#include <iomanip>
#include <cctype>


//定义http响应的一些状态信息：给浏览器返回的服务器状态信息
//title：状态行的响应信息
//form：响应正文的信息
const char *ok_200_title = "OK";//状态码200表示请求成功，只有这个状态码才是正常状态
const char *error_400_title = "Bad Request";
const char *error_400_form = "Your request has bad syntax or is inherently impossible to staisfy.\n";
const char *error_403_title = "Forbidden";
const char *error_403_form = "You do not have permission to get file form this server.\n";
const char *error_404_title = "Not Found";
const char *error_404_form = "The requested file was not found on this server.\n";
const char *error_500_title = "Internal Error";
const char *error_500_form = "There was an unusual problem serving the request file.\n";

locker m_lock;
map<string, string> users;//存储数据库中所有已注册用户的用户名和密码（在程序启动时就先提前全部取出）
static string to_lower_copy(const string &src)
{
    string dst = src;
    for (size_t i = 0; i < dst.size(); ++i)
    {
        dst[i] = static_cast<char>(tolower(static_cast<unsigned char>(dst[i])));
    }
    return dst;
}

static bool contains_sensitive_data(const string &text)
{
    string lower = to_lower_copy(text);

    if (lower.find("cookie:") == 0)
        return true;
    if (lower.find("set-cookie:") == 0)
        return true;
    if (lower.find("authorization:") == 0)
        return true;

    if (lower.find("password=") != string::npos)
        return true;
    if (lower.find("passwd=") != string::npos)
        return true;
    if (lower.find("token=") != string::npos)
        return true;
    if (lower.find("sid=") != string::npos)
        return true;

    return false;
}

static string sanitize_log_text(const char *text)
{
    if (text == NULL)
        return "";

    string value(text);

    if (contains_sensitive_data(value))
        return "[sensitive data masked]";

    return value;
}

static bool has_allowed_suffix(const string &filename)
{
    string lower = to_lower_copy(filename);

    return lower.size() >= 4 &&
           (lower.rfind(".jpg") == lower.size() - 4 ||
            lower.rfind(".png") == lower.size() - 4 ||
            lower.rfind(".gif") == lower.size() - 4 ||
            lower.rfind(".mp4") == lower.size() - 4 ||
            lower.rfind(".txt") == lower.size() - 4 ||
            lower.rfind(".webm") == lower.size() - 5 ||
            lower.rfind(".jpeg") == lower.size() - 5);
}

static string get_file_type(const string &filename)
{
    string lower = to_lower_copy(filename);

    if (lower.rfind(".jpg") == lower.size() - 4 ||
        lower.rfind(".png") == lower.size() - 4 ||
        lower.rfind(".gif") == lower.size() - 4 ||
        lower.rfind(".jpeg") == lower.size() - 5)
        return "image";

    if (lower.rfind(".mp4") == lower.size() - 4 ||
        lower.rfind(".webm") == lower.size() - 5)
        return "video";

    if (lower.rfind(".txt") == lower.size() - 4)
        return "text";

    return "other";
}

static string get_safe_filename(const string &filename)
{
    string name = filename;

    size_t slash_pos = name.find_last_of("/\\");
    if (slash_pos != string::npos)
        name = name.substr(slash_pos + 1);

    string safe;
    for (size_t i = 0; i < name.size(); ++i)
    {
        char c = name[i];
        if ((c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') ||
            c == '.' || c == '_' || c == '-')
        {
            safe.push_back(c);
        }
    }

    if (safe.empty())
        safe = "upload.dat";

    return safe;
}

static string html_escape(const string &src)
{
    string out;

    for (size_t i = 0; i < src.size(); ++i)
    {
        char c = src[i];

        if (c == '&')
            out += "&amp;";
        else if (c == '<')
            out += "&lt;";
        else if (c == '>')
            out += "&gt;";
        else if (c == '"')
            out += "&quot;";
        else
            out.push_back(c);
    }

    return out;
}


struct session_data
{
    string username;
    time_t expire_time;
};

static locker session_lock;
static map<string, session_data> sessions;

static const int SESSION_EXPIRE_SECONDS = 30 * 60;

static string random_hex(size_t bytes)
{
    unsigned char buf[64];

    if (bytes > sizeof(buf))
        bytes = sizeof(buf);

    if (RAND_bytes(buf, bytes) != 1)
        return "";

    std::ostringstream oss;
    for (size_t i = 0; i < bytes; ++i)
    {
        oss << std::hex << std::setw(2) << std::setfill('0')
            << static_cast<int>(buf[i]);
    }

    return oss.str();
}

static string create_session(const string &username)
{
    string sid = random_hex(32);
    if (sid.empty())
        return "";

    session_data data;
    data.username = username;
    data.expire_time = time(NULL) + SESSION_EXPIRE_SECONDS;

    session_lock.lock();
    sessions[sid] = data;
    session_lock.unlock();

    return sid;
}

static string get_sid_from_cookie(const char *cookie)
{
    if (cookie == NULL)
        return "";

    string cookies(cookie);
    string key = "sid=";

    size_t pos = cookies.find(key);
    if (pos == string::npos)
        return "";

    pos += key.size();
    size_t end = cookies.find(';', pos);

    if (end == string::npos)
        return cookies.substr(pos);

    return cookies.substr(pos, end - pos);
}

static bool check_session(const char *cookie)
{
    string sid = get_sid_from_cookie(cookie);
    if (sid.empty())
        return false;

    bool ok = false;
    time_t now = time(NULL);

    session_lock.lock();

    map<string, session_data>::iterator it = sessions.find(sid);
    if (it != sessions.end())
    {
        if (it->second.expire_time > now)
        {
            ok = true;
        }
        else
        {
            sessions.erase(it);
        }
    }

    session_lock.unlock();

    return ok;
}

static string get_session_username(const char *cookie)
{
    string sid = get_sid_from_cookie(cookie);
    if (sid.empty())
        return "";

    string username;
    time_t now = time(NULL);

    session_lock.lock();

    map<string, session_data>::iterator it = sessions.find(sid);
    if (it != sessions.end())
    {
        if (it->second.expire_time > now)
        {
            username = it->second.username;
        }
        else
        {
            sessions.erase(it);
        }
    }

    session_lock.unlock();

    return username;
}



//prepared statement
// prepared statement
static bool insert_user_by_stmt(MYSQL *mysql, const string &name, const string &password)
{
    const char *sql = "INSERT INTO user(username, passwd) VALUES(?, ?)";

    MYSQL_STMT *stmt = mysql_stmt_init(mysql);
    if (stmt == NULL)
    {
        fprintf(stderr, "mysql_stmt_init error\n");
        return false;
    }

    bool success = false;

    do
    {
        if (mysql_stmt_prepare(stmt, sql, strlen(sql)) != 0)
        {
            fprintf(stderr, "mysql_stmt_prepare error:%s\n", mysql_stmt_error(stmt));
            break;
        }

        MYSQL_BIND bind[2];
        memset(bind, 0, sizeof(bind));

        unsigned long name_length = name.size();
        unsigned long password_length = password.size();

        bind[0].buffer_type = MYSQL_TYPE_STRING;
        bind[0].buffer = (void *)name.c_str();
        bind[0].buffer_length = name_length;
        bind[0].length = &name_length;

        bind[1].buffer_type = MYSQL_TYPE_STRING;
        bind[1].buffer = (void *)password.c_str();
        bind[1].buffer_length = password_length;
        bind[1].length = &password_length;

        if (mysql_stmt_bind_param(stmt, bind) != 0)
        {
            fprintf(stderr, "mysql_stmt_bind_param error:%s\n", mysql_stmt_error(stmt));
            break;
        }

        if (mysql_stmt_execute(stmt) != 0)
        {
            fprintf(stderr, "mysql_stmt_execute error:%s\n", mysql_stmt_error(stmt));
            break;
        }

        success = true;
    } while (false);

    mysql_stmt_close(stmt);
    return success;
}



/*------------------------------SQL Pool 初始化 BEGIN--------------------------------------------------------*/
//main中初始化WebServer类中的m_connPool时会同时在HTTP类中取出一个数据库连接用于提前将所有注册过的用户信息取出存在map中
void http_conn::initmysql_result(connection_pool *connPool)
{
    //先从连接池中取一个连接（RAII机制）
    MYSQL *mysql = NULL;
    connectionRAII mysqlcon(&mysql, connPool);

    //在user表中检索username，passwd数据，浏览器端输入
    if (mysql_query(mysql, "SELECT username,passwd FROM user"))
    {
        LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
    }

    //从表中检索完整的结果集
    MYSQL_RES *result = mysql_store_result(mysql);

    //返回结果集中的列数
    int num_fields = mysql_num_fields(result);

    //返回所有字段结构的数组
    MYSQL_FIELD *fields = mysql_fetch_fields(result);

    //从结果集中获取下一行，将对应的用户名和密码，存入map中
    while (MYSQL_ROW row = mysql_fetch_row(result))
    {
        string temp1(row[0]);//key：用户名
        string temp2(row[1]);//value：密码
        users[temp1] = temp2;//存入map中
    }
}
/*------------------------------SQL Pool 初始化 END--------------------------------------------------------*/

/*------------------------------Epoll socketfd处理 BEGIN---------------------------------------------------*/

//设置客户端socketfd为非阻塞，这里也跟util.cpp中的setnonblocking实现是一样的
int setnonblocking(int fd){
    //使用 fcntl 函数来设置文件描述符的属性
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

//注册事件到epool中进行监听，这里其实跟util.cpp中的addfd实现是一样的
void addfd(int epollfd, int fd, bool one_shot, int TRIGMode){
    //注册fd及其相关的events事件到epoll中

    //创建事件:注册fd文件描述符
    epoll_event event;
    event.data.fd = fd;

    //给fd注册对应的epoll监听事件
    if(TRIGMode == 1)
        //注册ET模式
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    else
        //注册LT模式
        event.events = EPOLLIN | EPOLLRDHUP;

    //注册EPOLLONESHOT事件:设置fd是否只加内特一次
    if(one_shot)
        event.events |= EPOLLONESHOT;

    //注册fd到epoll中:epoll_ctl函数增fd
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);

    //设置fd为非阻塞(ET模式下必须设置非阻塞,包括listenfd和connfd)
    setnonblocking(fd);
}

//将事件重置为EPOLLONESHOT（ONESHOT模式只监听一次事件就会从epoll中删除）
void modfd(int epollfd, int fd, int ev, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode)
        event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;//ET模式下，EPOLLONESHOT是必须的
    else
        event.events = ev | EPOLLONESHOT | EPOLLRDHUP;

    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

//从epoll中删除fd（一般是close_conn中把对应的socketfd从epoll中删除）
void removefd(int epollfd, int fd){
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);//关闭socket前先从epoll中移除
    close(fd);
}


int http_conn::m_user_count = 0;
int http_conn::m_epollfd = -1;

//关闭连接
void http_conn::close_conn(bool real_close){
    if(real_close && (m_sockfd != -1)){
        printf("close %d\n", m_sockfd);
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_count--;//关闭一个连接，将客户总量减一
    }
}

/*------------------------------Epoll socketfd处理 END---------------------------------------------------*/


/*------------------------------HTTP类初始化 BEGIN--------------------------------------------------------*/

//初始化客户端连接中http_conn的一些用户状态参数，这个函数是在主线程（epoll）中收到用户的连接处理accept时调用的
void http_conn::init(int sockfd, const sockaddr_in &addr, char *root, int TRTGMide, int close_log, string user, string passwd, string sqlname)
{
    m_sockfd = sockfd;
    m_address = addr;

    addfd(m_epollfd, sockfd, true, m_TRIGMode);//将sockfd注册到epoll中
    m_user_count++;//客户端连接数+1

    //当浏览器出现连接重置时，可能是网站根目录出错或http响应格式出错或者访问的文件中内容完全为空
    doc_root = root;
    m_TRIGMode = TRTGMide;
    m_close_log = close_log;

    //更新数据库的用户名、密码、数据库名
    strcpy(sql_user, user.c_str());
    strcpy(sql_passwd, passwd.c_str());
    strcpy(sql_name, sqlname.c_str());

    //初始化http_conn类中剩下的一些参数为默认值
    init();

}

//初始化http_conn类中剩下的一些参数为默认值
void http_conn::init()
{
    mysql = NULL;
    bytes_to_send = 0;
    bytes_have_send = 0;
    m_check_state = CHECK_STATE_REQUESTLINE;//根据报文的结构，主状态机初始状态应该是解析请求行，也就是CHECK_STATE_REQUESTLINE
    m_linger = false;
    m_method = GET;
    m_url = 0;
    m_version = 0;
    m_content_length = 0;
    m_host = 0;
    m_cookie = 0;
    m_content_type = 0;
    m_login_user.clear();
    m_set_cookie_sid.clear();
    m_start_line = 0;
    m_body_start = 0;
    m_checked_idx = 0;
    m_read_idx = 0;
    m_write_idx = 0;
    cgi = 0;                                //cgi=1 标志是POST请求
    m_state = 0;
    timer_flag = 0;
    improv = 0;

    //初始化清空缓冲区
    memset(m_read_buf, '\0', READ_BUFFER_SIZE);
    memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);
    memset(m_real_file, '\0', FILENAME_LEN);
}

/*------------------------------HTTP类初始化 END--------------------------------------------------------*/

/*------------------------------HTTP请求报文解析：主从状态机 BEGIN----------------------------------------*/

//处理主状态机状态1：解析请求行，获得GET/POST方法、url、http版本号
http_conn::HTTP_CODE http_conn::parse_request_line(char *text)
{
    /*请求行格式举例：GET / HTTP/1.1
      请求行的格式：| 请求方法 | \t | URL | \t | HTTP版本号 | \r | \n |
      经过parse_line()函数处理后\r\n被替换成\0\0，所以这里可以直接用字符串函数来处理
    */

    //1. 获取URL：资源在服务端中的路径
    m_url = strpbrk(text, " \t");//m_url:指向请求报文中的URL的index
    if (!m_url)
    {
        return BAD_REQUEST;
    }
    *m_url++ = '\0';

    //2. 获取method：请求方法，本项目中只支持GET和POST
    char *method = text;
    if (strcasecmp(method, "GET") == 0)
        m_method = GET;
    else if (strcasecmp(method, "POST") == 0)
    {
        m_method = POST;
        cgi = 1;
    }
    else
        return BAD_REQUEST;

    //3. 获取http版本号：http版本号只支持HTTP/1.1
    m_url += strspn(m_url, " \t");
    m_version = strpbrk(m_url, " \t");
    if (!m_version)
        return BAD_REQUEST;
    *m_version++ = '\0';
    m_version += strspn(m_version, " \t");
    if (strcasecmp(m_version, "HTTP/1.1") != 0)
        return BAD_REQUEST;
    if (strncasecmp(m_url, "http://", 7) == 0)
    {
        m_url += 7;
        m_url = strchr(m_url, '/');
    }

    if (strncasecmp(m_url, "https://", 8) == 0)
    {
        m_url += 8;
        m_url = strchr(m_url, '/');
    }

    //4. 解析URL资源
    // 当URL为/时，显示初始欢迎界面"judge.html"
    // 剩下的其它URL资源的解析在do_request()函数中进行同一实现
    if (!m_url || m_url[0] != '/')
        return BAD_REQUEST;
    if (strlen(m_url) == 1)
        strcat(m_url, "judge.html");//将url追加到字符串中

    //5. 请求行解析完毕，主状态机由CHECK_STATE_REQUESTLINE转移到CHECK_STATE_HEADER，解析请求头
    m_check_state = CHECK_STATE_HEADER;
    return NO_REQUEST;//当前只解析完了请求行，还没解析完完整HTTP报文，所以返回NO_REQUEST
}

//处理主状态机状态2：解析请求头，获取Connection字段、Content-Length字段、Host字段
http_conn::HTTP_CODE http_conn::parse_headers(char *text)
{
    /*请求行格式举例：Connection:keep-alive
      请求行的格式：| 头部字段名 | : |   | \t | \r | \n |
      经过parse_line()函数处理后\r\n被替换成\0\0，所以这里可以直接用字符串函数来处理
    */

    //1. 遇到空行| \r | \n |，表示头部字段解析完毕
    if (text[0] == '\0')
{
    if (m_content_length != 0)
    {
        m_check_state = CHECK_STATE_CONTENT;
        m_body_start = m_checked_idx;
        return NO_REQUEST;
    }

    return GET_REQUEST;
}

    //2. 解析Connection字段，判断是keep-alive还是close
    //  HTTP/1.1默认是持久连接(keep-alive)
    else if (strncasecmp(text, "Connection:", 11) == 0)
    {
        text += 11;
        text += strspn(text, " \t");
        if (strcasecmp(text, "keep-alive") == 0)
        {
            m_linger = true;//用于返回响应报文时添加对应的Connection字段的值
        }
    }
    //3. 解析Content-Length字段，获取消息体的长度（主要是用于判断主状态机是否需要转为CHECK_STATE_CONTENT状态）
    else if (strncasecmp(text, "Content-length:", 15) == 0)
    {
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text);
    }
    //4. 解析Host字段，获取请求的主机名
    else if (strncasecmp(text, "Host:", 5) == 0)
    {
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    }
    else if (strncasecmp(text, "Cookie:", 7) == 0)
    {
        text += 7;
        text += strspn(text, " \t");
        m_cookie = text;
    }
    else if (strncasecmp(text, "Content-Type:", 13) == 0)
    {
        text += 13;
        text += strspn(text, " \t");
        m_content_type = text;
    }


    else
    {
        //其它字段本项目不解析，直接跳过
        string safe_text = sanitize_log_text(text);
        LOG_INFO("oop!unknow header: %s", safe_text.c_str());

    }
    return NO_REQUEST;
}

//处理主状态机状态3：解析请求内容，获取POST请求中的消息体
http_conn::HTTP_CODE http_conn::parse_content(char *text)
{
    if (m_read_idx >= m_body_start + m_content_length)
    {
        m_string = m_read_buf + m_body_start;
        return GET_REQUEST;
    }

    LOG_INFO("upload body not complete, read_idx=%d, need=%d, body_start=%d",
             m_read_idx, m_body_start + m_content_length, m_body_start);

    return NO_REQUEST;
}



bool http_conn::save_post_to_db(const string &username, const string &content_text,
                                const string &file_path, const string &file_type)
{
    const char *sql = "INSERT INTO user_posts(username, content_text, file_path, file_type) VALUES(?, ?, ?, ?)";

    MYSQL_STMT *stmt = mysql_stmt_init(mysql);
    if (stmt == NULL)
    {
        fprintf(stderr, "mysql_stmt_init user_posts error\n");
        return false;
    }

    bool success = false;

    do
    {
        if (mysql_stmt_prepare(stmt, sql, strlen(sql)) != 0)
        {
            fprintf(stderr, "mysql_stmt_prepare user_posts error:%s\n", mysql_stmt_error(stmt));
            break;
        }

        MYSQL_BIND bind[4];
        memset(bind, 0, sizeof(bind));

        unsigned long username_len = username.size();
        unsigned long content_len = content_text.size();
        unsigned long path_len = file_path.size();
        unsigned long type_len = file_type.size();

        bind[0].buffer_type = MYSQL_TYPE_STRING;
        bind[0].buffer = (void *)username.c_str();
        bind[0].buffer_length = username_len;
        bind[0].length = &username_len;

        bind[1].buffer_type = MYSQL_TYPE_STRING;
        bind[1].buffer = (void *)content_text.c_str();
        bind[1].buffer_length = content_len;
        bind[1].length = &content_len;

        bind[2].buffer_type = MYSQL_TYPE_STRING;
        bind[2].buffer = (void *)file_path.c_str();
        bind[2].buffer_length = path_len;
        bind[2].length = &path_len;

        bind[3].buffer_type = MYSQL_TYPE_STRING;
        bind[3].buffer = (void *)file_type.c_str();
        bind[3].buffer_length = type_len;
        bind[3].length = &type_len;

        if (mysql_stmt_bind_param(stmt, bind) != 0)
        {
            fprintf(stderr, "mysql_stmt_bind_param user_posts error:%s\n", mysql_stmt_error(stmt));
            break;
        }

        if (mysql_stmt_execute(stmt) != 0)
        {
            fprintf(stderr, "mysql_stmt_execute user_posts error:%s\n", mysql_stmt_error(stmt));
            break;
        }

        success = true;
    } while (false);

    mysql_stmt_close(stmt);
    return success;
}

bool http_conn::rebuild_community_page()
{
    if (mysql_query(mysql, "SELECT username, content_text, file_path, file_type, created_at FROM user_posts ORDER BY id DESC"))
    {
        fprintf(stderr, "SELECT user_posts error:%s\n", mysql_error(mysql));
        return false;
    }

    MYSQL_RES *result = mysql_store_result(mysql);
    if (result == NULL)
    {
        fprintf(stderr, "mysql_store_result user_posts error:%s\n", mysql_error(mysql));
        return false;
    }

    string html;
    html += "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><title>大家发布的内容</title></head><body>";
    html += "<h2>大家发布的内容</h2>";
    html += "<p><a href=\"/upload.html\">我要发布</a></p>";
    html += "<p><a href=\"/welcome.html\">返回首页</a></p>";
    html += "<hr>";

    MYSQL_ROW row;
    while ((row = mysql_fetch_row(result)))
    {
        string username = row[0] ? row[0] : "";
        string content_text = row[1] ? row[1] : "";
        string file_path = row[2] ? row[2] : "";
        string file_type = row[3] ? row[3] : "";
        string created_at = row[4] ? row[4] : "";

        html += "<div style=\"margin-bottom:24px;border-bottom:1px solid #ccc;padding-bottom:16px;\">";
        html += "<p><b>用户：</b>" + html_escape(username) + "</p>";
        html += "<p><b>时间：</b>" + html_escape(created_at) + "</p>";

        if (!content_text.empty())
        {
            html += "<p>" + html_escape(content_text) + "</p>";
        }

        if (!file_path.empty())
        {
            string safe_path = html_escape(file_path);

            if (file_type == "image")
            {
                html += "<p><img src=\"" + safe_path + "\" style=\"max-width:500px;\"></p>";
            }
            else if (file_type == "video")
            {
                html += "<p><video src=\"" + safe_path + "\" controls style=\"max-width:600px;\"></video></p>";
            }
            else
            {
                html += "<p><a href=\"" + safe_path + "\">查看文件</a></p>";
            }
        }

        html += "</div>";
    }

    html += "</body></html>";

    mysql_free_result(result);

    string path = string(doc_root) + "/community.html";
    ofstream out(path.c_str());
    if (!out)
        return false;

    out << html;
    out.close();

    return true;
}

bool http_conn::handle_upload()
{
    LOG_INFO("handle_upload start");

    string username = get_session_username(m_cookie);
    if (username.empty())
    {
        LOG_INFO("upload failed: no valid session");
        strcpy(m_url, "/log.html");
        return true;
    }

    LOG_INFO("upload username=%s", username.c_str());

    if (m_content_type == NULL)
    {
        LOG_INFO("upload failed: content-type is null");
        strcpy(m_url, "/upload.html");
        return true;
    }

    string content_type(m_content_type);
    LOG_INFO("upload content_type=%s", content_type.c_str());

    string boundary_key = "boundary=";
    size_t boundary_pos = content_type.find(boundary_key);

    if (boundary_pos == string::npos)
    {
        LOG_INFO("upload failed: boundary not found");
        strcpy(m_url, "/upload.html");
        return true;
    }

    string boundary = "--" + content_type.substr(boundary_pos + boundary_key.size());
    LOG_INFO("upload boundary=%s", boundary.c_str());

    string body(m_string, m_content_length);
    LOG_INFO("upload body size=%d", (int)body.size());

    string content_text;
    string file_path;
    string file_type;

    size_t part_pos = 0;

    while (true)
    {
        size_t start = body.find(boundary, part_pos);
        if (start == string::npos)
        {
            LOG_INFO("upload parse: no more boundary");
            break;
        }

        start += boundary.size();

        if (start + 2 < body.size() && body.substr(start, 2) == "--")
        {
            LOG_INFO("upload parse: reach final boundary");
            break;
        }

        if (start + 2 < body.size() && body.substr(start, 2) == "\r\n")
            start += 2;

        size_t header_end = body.find("\r\n\r\n", start);
        if (header_end == string::npos)
        {
            LOG_INFO("upload failed: part header end not found");
            break;
        }

        string header = body.substr(start, header_end - start);
        LOG_INFO("upload part header=%s", header.c_str());

        size_t data_start = header_end + 4;

        size_t next_part = body.find(boundary, data_start);
        if (next_part == string::npos)
        {
            LOG_INFO("upload failed: next boundary not found");
            break;
        }

        size_t data_end = next_part;
        if (data_end >= 2 && body.substr(data_end - 2, 2) == "\r\n")
            data_end -= 2;

        string data = body.substr(data_start, data_end - data_start);
        LOG_INFO("upload part data size=%d", (int)data.size());

        if (header.find("name=\"content\"") != string::npos)
        {
            content_text = data;
            LOG_INFO("upload content text size=%d", (int)content_text.size());
        }
        else if (header.find("name=\"file\"") != string::npos)
        {
            size_t filename_pos = header.find("filename=\"");
            if (filename_pos != string::npos)
            {
                filename_pos += strlen("filename=\"");
                size_t filename_end = header.find("\"", filename_pos);

                if (filename_end != string::npos)
                {
                    string origin_name = header.substr(filename_pos, filename_end - filename_pos);
                    LOG_INFO("upload origin filename=%s", origin_name.c_str());

                    if (!origin_name.empty())
                    {
                        string safe_name = get_safe_filename(origin_name);
                        LOG_INFO("upload safe filename=%s", safe_name.c_str());

                        if (!has_allowed_suffix(safe_name))
                        {
                            LOG_INFO("upload failed: suffix not allowed");
                            strcpy(m_url, "/upload.html");
                            return true;
                        }

                        string rand_name = random_hex(8) + "_" + safe_name;
                        string disk_path = string(doc_root) + "/uploads/" + rand_name;
                        file_path = "/uploads/" + rand_name;
                        file_type = get_file_type(safe_name);

                        LOG_INFO("upload disk path=%s", disk_path.c_str());
                        LOG_INFO("upload url path=%s", file_path.c_str());
                        LOG_INFO("upload file type=%s", file_type.c_str());

                        ofstream out(disk_path.c_str(), ios::binary);
                        if (!out)
                        {
                            LOG_INFO("upload failed: open disk file failed");
                            strcpy(m_url, "/upload.html");
                            return true;
                        }

                        out.write(data.data(), data.size());
                        out.close();

                        LOG_INFO("upload file saved, size=%d", (int)data.size());
                    }
                }
            }
        }

        part_pos = next_part;
    }

    LOG_INFO("upload final content_text size=%d, file_path=%s",
             (int)content_text.size(), file_path.c_str());

    if (content_text.empty() && file_path.empty())
    {
        LOG_INFO("upload failed: empty content and empty file");
        strcpy(m_url, "/upload.html");
        return true;
    }

    if (!save_post_to_db(username, content_text, file_path, file_type))
    {
        LOG_INFO("upload failed: save post to db failed");
        strcpy(m_url, "/upload.html");
        return true;
    }

    LOG_INFO("upload db saved");

    if (!rebuild_community_page())
    {
        LOG_INFO("upload warning: rebuild community page failed");
    }
    else
    {
        LOG_INFO("upload community page rebuilt");
    }

    strcpy(m_url, "/community.html");
    LOG_INFO("upload success, redirect to community.html");

    return true;
}



//解析完整的HTTP请求后，解析请求的URL进行处理并返回响应报文
//m_real_file:完成处理后拼接的响应资源在服务端中的完整路径
//m_string   :POST请求中在parse_content()中解析出的消息体（包含用户名和密码）
http_conn::HTTP_CODE http_conn::do_request()
{
    //1. 将m_real_file初始化为项目的根目录（WebServer类中初始化过的root）
    strcpy(m_real_file, doc_root);
    int len = strlen(doc_root);
    //printf("m_url:%s\n", m_url);
    const char *p = strrchr(m_url, '/');
    if (cgi == 1 && strcmp(m_url, "/upload") == 0)
    {
        handle_upload();

        strcpy(m_real_file, doc_root);
        int upload_len = strlen(doc_root);
        strncpy(m_real_file + upload_len, m_url, FILENAME_LEN - upload_len - 1);

        if (stat(m_real_file, &m_file_stat) < 0)
            return NO_RESOURCE;

        if (!(m_file_stat.st_mode & S_IROTH))
            return FORBIDDEN_REQUEST;

        if (S_ISDIR(m_file_stat.st_mode))
            return BAD_REQUEST;

        int fd = open(m_real_file, O_RDONLY);
        m_file_address = (char *)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
        close(fd);

        return FILE_REQUEST;
    }


    //2. 处理登录/注册请求（消息体中都会有用户名和密码）
    //处理cgi：POST请求会将cgi置为1
    if (cgi == 1 && (*(p + 1) == '2' || *(p + 1) == '3'))
    {

        //根据标志判断是登录检测还是注册检测（flag为"2"是登录，为"3"是注册）
        char flag = m_url[1];

        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/");
        strcat(m_url_real, m_url + 2);
        strncpy(m_real_file + len, m_url_real, FILENAME_LEN - len - 1);
        free(m_url_real);

        //2.1 将用户名和密码提取出来
        //存于报文的消息体中：user=akira&password=akira
        char name[100], password[100];
        //a. 通过识别连接符 & 确定用户名
        int i;
        for (i = 5; m_string[i] != '&'; ++i)
            name[i - 5] = m_string[i];
        name[i - 5] = '\0';
        //b. 确定密码
        int j = 0;
        for (i = i + 10; m_string[i] != '\0'; ++i, ++j)
            password[j] = m_string[i];
        password[j] = '\0';

        //2.2 处理注册请求
    if (*(p + 1) == '3')
{
        string user_name(name);
        string hashed_password = password_hash::make_password_hash(password);

        if (hashed_password.empty())
        {
            strcpy(m_url, "/registerError.html");
        }
        else if (users.find(user_name) == users.end())
        {
            m_lock.lock();

            bool insert_ok = insert_user_by_stmt(mysql, user_name, hashed_password);
            if (insert_ok)
            {
                users.insert(pair<string, string>(user_name, hashed_password));
            }

            m_lock.unlock();

            if (insert_ok)
                strcpy(m_url, "/log.html");
            else
                strcpy(m_url, "/registerError.html");
        }
        else
        {
            strcpy(m_url, "/registerError.html");
        }
    }

    else if (*(p + 1) == '2')
{
    if (users.find(name) != users.end() &&
        password_hash::verify_password(password, users[name]))
    {
        m_login_user = name;
        m_set_cookie_sid = create_session(m_login_user);

        if (!m_set_cookie_sid.empty())
            strcpy(m_url, "/welcome.html");
        else
            strcpy(m_url, "/logError.html");
    }
    else
    {
        strcpy(m_url, "/logError.html");
    }
}


    }
bool need_login = false;

if (strcmp(m_url, "/welcome.html") == 0 ||
    strcmp(m_url, "/upload.html") == 0 ||
    strcmp(m_url, "/community.html") == 0 ||
    strcmp(m_url, "/upload") == 0 ||
    strncmp(m_url, "/uploads/", 9) == 0 ||
    *(p + 1) == '5' ||
    *(p + 1) == '6' ||
    *(p + 1) == '7')
{
    need_login = true;
}



    // 如果本次请求刚刚登录成功，m_set_cookie_sid 不为空，说明服务端已经创建了新 session。
// 这时候浏览器还没来得及在当前请求里携带 Cookie，所以不能再用 m_cookie 拦截本次请求。
    if (need_login && m_set_cookie_sid.empty() && !check_session(m_cookie))
    {
        strcpy(m_url, "/log.html");
        p = strrchr(m_url, '/');
    }


    //3. 处理跳转到注册界面的请求
    if (*(p + 1) == '0')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/register.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }

    //4. 处理跳转到登录界面的请求
    else if (*(p + 1) == '1')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/log.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }

    //5. 处理图片资源请求
    else if (*(p + 1) == '5')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/picture.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }

    //6. 处理视频资源请求
    else if (*(p + 1) == '6')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/video.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }

    //7. 处理关注界面的请求
    else if (*(p + 1) == '7')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/fans.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else
        strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);

    //判断该路径的文件是否存在
    if (stat(m_real_file, &m_file_stat) < 0)
        return NO_RESOURCE;

    //判断文件的权限是否可读
    if (!(m_file_stat.st_mode & S_IROTH))
        return FORBIDDEN_REQUEST;

    //判断请求的资源是文件夹还是文件（文件夹返回BAD_REQUEST，不可响应）
    if (S_ISDIR(m_file_stat.st_mode))
        return BAD_REQUEST;

    //通过mmap将资源文件映射到内存中，提高文件的访问速度
    int fd = open(m_real_file, O_RDONLY);
    m_file_address = (char *)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    return FILE_REQUEST;
}

//主状态机，用于处理解析读取到的报文
//状态1：CHECK_STATE_REQUESTLINE（进行请求行的解析--从状态机中获取数据位置）
//状态2：CHECK_STATE_HEADER（进行请求头的解析--从状态机中获取数据位置）
//状态3：CHECK_STATE_CONTENT（进行请求内容的解析--主状态机中读取buffer剩下的所有数据）
http_conn::HTTP_CODE http_conn::process_read()
{
    LINE_STATUS line_status = LINE_OK;  //初始化当前从状态机的行处理状态
    HTTP_CODE ret = NO_REQUEST;         //初始化当前HTTP请求的处理结果
    char *text = 0;                     //存储主状态机当前正在解析的行数据（字符串）

    //主状态机解析状态通过从状态机来驱动：LINE_OK说明主状态机可以开始解析了
    //1. 如果是GET请求，那么其实只需要parse_line()函数就能保证解析完整个请求报文
    //2. 但是由于POST请求的content没有固定的行结束标志，所以content的解析不在从状态机中进行，而是在主状态机中进行
    //   当主状态机由CHECK_STATE_HEADER转移到CHECK_STATE_CONTENT时，我们将主状态机继续循环的判断改为m_check_state == CHECK_STATE_CONTENT，表示content部分不进入从状态机解析
    //   同时为了保证解析完content后能退出循环，我们在解析完content后将line_status = LINE_OPEN
    //   这里由于进入content解析状态前，line_status还会保持上一个状态的LINE_OK，所以不会影响主状态机进入content的解析
    while((m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK) || ((line_status = parse_line()) == LINE_OK)){
        text = get_line();
        m_start_line = m_checked_idx;//更新为下一行的起始位置，方便下次调用get_line获取当前行的字符串

        if (m_check_state == CHECK_STATE_CONTENT)
        {
            LOG_INFO("request body masked, content_length=%d", m_content_length);
        }
        else
        {
            string safe_text = sanitize_log_text(text);
            LOG_INFO("%s", safe_text.c_str());
        }



        //主状态机根据当前状态机状态进行报文解析
        switch(m_check_state){
        //1. 解析请求行
        case CHECK_STATE_REQUESTLINE:
        {
            ret = parse_request_line(text);
            if(ret == BAD_REQUEST){
                return BAD_REQUEST;
            }
            break;
        }
        //2. 解析请求头
        case CHECK_STATE_HEADER:
        {
            ret = parse_headers(text);
            if(ret == BAD_REQUEST){
                return BAD_REQUEST;
            }
            //------------------------------
            else if(ret == GET_REQUEST){
                return do_request();
            }
            break;
        }
        //3. 解析请求内容
        case CHECK_STATE_CONTENT:
{
    ret = parse_content(text);

    if (ret == GET_REQUEST)
    {
        return do_request();
    }

    // POST 请求体，尤其是 multipart/form-data 图片/视频上传体，
    // 不能继续交给 parse_line() 按行解析，否则 \r\n 会被改成 \0\0，导致二进制请求体被破坏。
    return NO_REQUEST;
}

        default:
            return INTERNAL_ERROR;
        }
    }

    return NO_REQUEST;//表示socket还需要继续读取数据
}

// epoll监测到客户端sockfd有读事件时，调用read_once循环读取数据到buffer中，直到无数据可读或者对方关闭连接
// 在reactor模式下，该函数是在工作线程中调用的，在proactor模式下，该函数是在主线程中调用的
// 非阻塞ET工作模式下，需要一次性将数据读完
bool http_conn::read_once()
{
    if (m_read_idx >= READ_BUFFER_SIZE)
    {
        return false;
    }
    int bytes_read = 0;

    //将数据读到m_read_buf + m_read_idx位置开始的内存中（存在读缓冲区m_read_buf中）
    //LT方式读取数据：epoll_wait会多次通知读数据，直到读完，所以这里不用while循环
    if (m_TRIGMode == 0)
{
    while (true)
    {
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx,
                          READ_BUFFER_SIZE - m_read_idx, 0);

        if (bytes_read == -1)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;

            return false;
        }
        else if (bytes_read == 0)
        {
            return false;
        }

        m_read_idx += bytes_read;

        if (m_read_idx >= READ_BUFFER_SIZE)
            break;
    }

    return true;
}

    //ET方式读取数据：epoll_wait只通知一次读数据，所以这里要用while循环读完
    else{
        while(true){
            bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
            if(bytes_read == -1){//接收失败
                if(errno == EAGAIN || errno == EWOULDBLOCK)
                    break;
                return false;//接收结束
            }else if(bytes_read == 0){//对方关闭连接
                return false;
            }
            m_read_idx += bytes_read;
        }
        return true;//ET读完所有数据返回
    }
}

//从状态机，用于一行一行解析出客户端发送请求的报文，并将解读行的状态作为返回值
//主状态机负责对该行数据进行解析，主状态机内部调用从状态机，从状态机驱动主状态机。
//注意，由于报文中的content没有固定的行结束标志，所以content的解析不在从状态机中进行，而是在主状态机中进行
//状态1：LINE_OK表示读完了完整的一行（读到了行结束符\r\n）
//状态2：LINE_BAD表示读取的行格式有误（结束符只读到了\r或\n，而不是\r + \n）
//状态3：LINE_OPEN表示LT模式下还没接收完完整的buffer，还需等待继续recv到buffer后再次触发解析数据包
http_conn::LINE_STATUS http_conn::parse_line()
{
    char temp;
    //循环当前buffer中已读取到的数据
    //如果是ET模式，则客户端发送的数据包是已经全部读完了的，buffer是完整的
    //如果是LT模式，则客户端发送的数据包是分批次读取的，buffer是不完整的，所以需要LINE_OPEN状态来等待下一次读取
    for(;m_checked_idx < m_read_idx; ++m_checked_idx){

        /*m_checked_idx:    当前已确认（取出）的字符位置
          temp:             当前读取到的m_checked_idx处的字符
          m_read_idx:       读缓冲区中的数据长度（已经接收的socket的数据总长度）
        */
        temp = m_read_buf[m_checked_idx];

        //1. 读到一个完整行的倒数第二个字符\r
        if(temp == '\r'){
            //如果已经把buffer中已经接收的数据读完了，但是此时buffer中的数据还不完整，那么就返回LINE_OPEN状态，等待下一次读取
            if((m_checked_idx + 1) == m_read_idx){//m_read_idx是个数，所以这里index得+1
                return LINE_OPEN;
            }

            //如果读到了完整的行，也几乎是判断出了下一个字符为'\n'就返回LINE_OK
            //LINE_OK状态在主状态机中是可以进行行解析的状态
            else if(m_read_buf[m_checked_idx + 1] == '\n'){
                m_read_buf[m_checked_idx++] = '\0';//'\r'换成'\0'
                m_read_buf[m_checked_idx++] = '\0';//'\n'换成'\0'，m_checked_idx更新为下一行的起始位置
                return LINE_OK;
            }

            //如果读到的行格式有误，即buffer明明还没结束，但是读不到'\n'了，则返回LINE_BAD状态
            return LINE_BAD;
        }

        //2. 读到一个完整行的最后一个字符\n
        //情况1：正常来说对于完整的数据而言，'\n'应该已经被上面的if语句处理了，但是还存在第一种情况是LT下数据是还没读完整的
        //      也就是对于上面的if中，已经读到了m_read_idx了，返回LINE_OPEN，等接着继续读到socket数据再触发当前函数时，就会从'\n'开始判断
        //情况2：当前数据是坏数据，没有配套的'\r'+ '\n'，所以返回LINE_BAD
        else if(temp == '\n'){
            if(m_checked_idx > 1 && m_read_buf[m_checked_idx - 1] == '\r'){
                m_read_buf[m_checked_idx - 1] = '\0';//'\r'换成'\0'
                m_read_buf[m_checked_idx++] = '\0';//'\n'换成'\0'，m_checked_idx更新为下一行的起始位置
                return LINE_OK;
            }

            //如果上一个字符不是'\r'，则说明数据包格式有误，返回LINE_BAD
            return LINE_BAD;
        }
    }
    return LINE_OPEN;//读完了buffer中的数据，但是数据包可能还没读完，需要等待下一次读取
}


/*------------------------------HTTP请求报文解析：主从状态机 END----------------------------------------*/

/*------------------------------HTTP响应报文打包 BEGIN-------------------------------------------------*/

//更新m_write_idx指针和缓冲区m_write_buf中的内容：将数据写入缓冲区
//采用可变参函数，向缓冲区写入格式化字符串
//用va_list va_start va_end来实现变参的列表处理
//用vsprintf将格式化的字符串写入缓冲区
bool http_conn::add_response(const char *format, ...)
{
    //已些入的数据m_write_idx指针越界，缓冲区m_write_buf不允许再写入了
    if (m_write_idx >= WRITE_BUFFER_SIZE)
        return false;

    //可变参数列表接收，通过vsnprintf函数格式化写入缓冲区
    va_list arg_list;
    va_start(arg_list, format);
    int len = vsnprintf(m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list);
    //格式化的字符串长度超过缓冲区剩余长度，写入失败
    if (len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx))
    {
        va_end(arg_list);
        return false;
    }

    //格式化字符串写入缓冲区成功，更新m_write_idx指针
    m_write_idx += len;
    va_end(arg_list);

    LOG_INFO("response buffer updated, write_idx=%d", m_write_idx);


    return true;
}

//1. 添加状态行：HTTP/1.1 200 OK
bool http_conn::add_status_line(int status, const char *title)
{
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}
//2. 添加消息报头和空行
// Content-Length字段：Content-Length: 78443
// Connection字段：Connection: keep-alive
bool http_conn::add_headers(int content_len)
{
    return add_content_length(content_len) && add_linger() &&
           add_session_cookie() && add_blank_line();
}

bool http_conn::add_content_length(int content_len)
{
    return add_response("Content-Length:%d\r\n", content_len);
}
// bool http_conn::add_content_type()
// {
//     return add_response("Content-Type:%s\r\n", "text/html");
// }
bool http_conn::add_linger()
{
    return add_response("Connection:%s\r\n", (m_linger == true) ? "keep-alive" : "close");
}
bool http_conn::add_session_cookie()
{
    if (m_set_cookie_sid.empty())
        return true;

    return add_response("Set-Cookie:sid=%s; Max-Age=%d; HttpOnly; SameSite=Lax\r\n",
                        m_set_cookie_sid.c_str(),
                        SESSION_EXPIRE_SECONDS);
}

bool http_conn::add_blank_line()
{
    return add_response("%s", "\r\n");
}
//3. 添加响应体：文件资源无法访问的才需要调用这个函数，其他情况都是通过mmap映射到内存中的
bool http_conn::add_content(const char *content)
{
    return add_response("%s", content);
}

//报文打包状态机：根据服务器处理HTTP请求的结果和状态ret，打包相应的HTTP响应报文
bool http_conn::process_write(HTTP_CODE ret)
{
    switch (ret)
    {
    //1. 服务器内部错误：500
    //在主状态机switch-case出现的错误，一般不会触发
    case INTERNAL_ERROR:
    {
        add_status_line(500, error_500_title);
        add_headers(strlen(error_500_form));
        if (!add_content(error_500_form))
            return false;
        break;
    }
    //2. 请求报文语法有错/请求的资源不是文件，是文件夹：404
    case BAD_REQUEST:
    {
        add_status_line(404, error_404_title);
        add_headers(strlen(error_404_form));
        if (!add_content(error_404_form))
            return false;
        break;
    }
    //3. 请求资源没有访问权限：403
    case FORBIDDEN_REQUEST:
    {
        add_status_line(403, error_403_title);
        add_headers(strlen(error_403_form));
        if (!add_content(error_403_form))
            return false;
        break;
    }
    //4. 请求资源可以正常访问：200
    case FILE_REQUEST:
    {
        add_status_line(200, ok_200_title);
        if (m_file_stat.st_size != 0)
        {
            add_headers(m_file_stat.st_size);//文件字节数，用于Content-Length字段
            // iovec 结构体将多个非连续的内存区域组合在一起，进行一次性的 I/O 操作
            //FILE_REQUEST状态代表请求的文件资源是可以正常访问的，所以需要多申请一个文件资源的iovec
            m_iv[0].iov_base = m_write_buf;
            m_iv[0].iov_len = m_write_idx;
            m_iv[1].iov_base = m_file_address;
            m_iv[1].iov_len = m_file_stat.st_size;
            m_iv_count = 2;
            bytes_to_send = m_write_idx + m_file_stat.st_size;
            return true;
        }
        else
        {
            //请求的文件资源是空的，生成一个空的html文件（ok_string）返回
            const char *ok_string = "<html><body></body></html>";
            add_headers(strlen(ok_string));
            if (!add_content(ok_string))
                return false;
        }
    }
    default:
        return false;
    }

    //请求资源异常的，只申请一个buff的iovec
    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    bytes_to_send = m_write_idx;
    return true;
}

void http_conn::unmap()
{
    if (m_file_address)
    {
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }
}

//向socketfd写数据：
// Reactor模式下，工作线程调用users[sockfd].write函数向客户端发送响应报文
// Proactor模式下，主线程调用users[sockfd].write函数向客户端发送响应报文，不经过工作线程处理
bool http_conn::write()
{
    int temp = 0;

    //没有数据需要发送，将sockfd从epoll中注册写事件（EPOLLOUT）改为读事件（EPOLLIN）继续监听
    if (bytes_to_send == 0)
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
        init();
        return true;
    }

    //将响应报文发送给客户端
    while (1)
    {
        temp = writev(m_sockfd, m_iv, m_iv_count);//将多个缓冲区iovec的数据一次性写入（发送）I/O描述符（m_sockfd）

        //发送失败：eagain满了暂时不可用 or 其他情况（取消映射）
        if (temp < 0)
        {
            //I/O缓冲区暂时满了，将sockfd再次注册写事件（EPOLLOUT）继续等待下一次写事件继续发送
            if (errno == EAGAIN)
            {
                modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);
                return true;
            }

            //未知原因发送失败，取消响应资源文件的映射
            unmap();
            return false;
        }

        //writev负责将缓冲区iovec数据写入I/O描述符，但是不会对已发送的数据进行删除，所以需要更新缓冲区iovec已发送的数据长度
        bytes_have_send += temp;
        bytes_to_send -= temp;

        //第一个缓冲区m_write_buf已全部发送完
        if (bytes_have_send >= m_iv[0].iov_len)
        {
            m_iv[0].iov_len = 0;
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
            m_iv[1].iov_len = bytes_to_send;
        }
        //第一个缓冲区m_write_buf还没发送完，更新m_iv[0]后继续发送
        else
        {
            m_iv[0].iov_base = m_write_buf + bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len - bytes_have_send;
        }

        //缓冲区全部发送完毕，取消响应资源文件的映射并重新将sockfd注册为读事件（EPOLLIN）
        if (bytes_to_send <= 0)
        {
            unmap();
            modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);

            //保持长连接，重新初始化http_conn类中的一些参数
            if (m_linger)
            {
                init();
                return true;
            }
            //短连接return false，在webserver类或者工作线程中结束write后会调用deal_timer中timer的cb_func函数关闭连接
            else
            {
                return false;
            }
        }
    }
    return false;
}

/*------------------------------HTTP响应报文打包 END--------------------------------------------------*/

//进行报文解析处理
void http_conn::process()
{
    //解析请求报文
    HTTP_CODE read_ret = process_read();//http客户端刚进来肯定是先读取解析请求报文
    if (read_ret == NO_REQUEST)
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);//NO_REQUEST是数据没读完，还需要继续读取，重新注册读事件（EPOLLIN）
        return;
    }

    //生成响应报文
    bool write_ret = process_write(read_ret);
    if (!write_ret)
    {
        close_conn();//报文生成失败，关闭连接
    }
    modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);//报文生成成功，注册写事件（EPOLLOUT），发送响应报文
}

