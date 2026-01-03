#define _POSIX_C_SOURCE 200112L
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/inotify.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <signal.h>
#include <regex.h>
#include <stdbool.h>

#include "../libmslog/inc/mslog.h"

#define AGENT_VERSION               ( "v1.0.0" )
#define MAX_EPOLL_EVENTS            ( 64 )
#define BUF_SIZE                    ( 4096 )
#define BATCH_SEND_THRESHOLD        ( 1024 )
#define MONITOR_DIR                 ( "/var/log" )
#define FILTER_KEYWORD              ( "ERROR" )
#define DAEMON_RUN                  ( 1 )

static int g_epoll_fd = -1;
static int g_inotify_fd = -1;
static pthread_mutex_t g_batch_mutex = PTHREAD_MUTEX_INITIALIZER;
static char *g_batch_buf = NULL;
static int g_batch_line_cnt = 0;
static bool g_running = true;

static int agent_init(void){
    g_epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if ( g_epoll_fd < 0 ) 
    { 
        MSLOG_ERROR("AGENT", "epoll create failed: %s", strerror(errno)); 
        return -1;
    }

    g_inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if ( g_inotify_fd < 0 )
    { 
        MSLOG_ERROR("AGENT", "inotify init failed: %s", strerror(errno)); 
        return -1;
    }

    struct epoll_event ev = {
        .events = EPOLLIN, 
        .data.fd = g_inotify_fd
    };
    if ( epoll_ctl(g_epoll_fd, EPOLL_CTL_ADD, g_inotify_fd, &ev) < 0 )
    {
        MSLOG_ERROR("AGENT", "epoll ctl failed: %s", strerror(errno));
        return -1;
    }
    return 0;
}

static int agent_monitor_dir(const char *dir){
    if ( !dir || strlen(dir) == 0 )
    {
        return -1;
    }
    int wd = inotify_add_watch(g_inotify_fd, dir, IN_CREATE | IN_MODIFY | IN_DELETE );
    if ( wd < 0 )
    {
        MSLOG_ERROR("AGENT", "add watch %s failed: %s", dir, strerror(errno));
        return -1;
    }
    return 0;
}

static ssize_t agent_read_log(const char *file, char *buf, size_t len){
    if ( !M_MEM_IS_VALID(buf) || !file || len == 0 )
    {
        return -1;
    }

    int fd = open(file, O_RDONLY);
    if ( fd < 0)
    {
        return -1;
    }
    
    struct stat st;
    fstat(fd, &st);
    if ( st.st_size == 0 )
    {
        close(fd);
        return 0;
    }
    
    char *mmap_buf = (char *)mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if ( mmap_buf == MAP_FAILED )
    {
        close(fd);
        return -1;
    }
    
    ssize_t copy_len = ( st.st_size < len ) ? st.st_size : len;
    memcpy(buf, mmap_buf, copy_len);
    munmap(mmap_buf, st.st_size);
    close(fd);
    return copy_len;
}

static int agent_filter_log(const char *log_line){
    if ( !log_line )
    {
        return 0;
    }
    regex_t reg;
    int ret = regcomp(&reg, FILTER_KEYWORD, REG_EXTENDED | REG_ICASE);
    if ( ret != 0 )
    {
        return 1;
    }
    ret = regexec(&reg, log_line, 0, NULL, 0);
    regfree(&reg);
    return ( ( ret == 0 ) ? 1 : 0 );
}

static void agent_batch_send(const char *log_line){
    if ( !M_MEM_IS_VALID(log_line) || strlen(log_line) == 0 )
    {
        return;
    }
    pthread_mutex_lock(&g_batch_mutex);

    size_t free_size = ( BUF_SIZE * BATCH_SEND_THRESHOLD ) - strlen(g_batch_buf) - 1;
    strncat(g_batch_buf, log_line, free_size);
    strncat(g_batch_buf, "\n", 1);
    g_batch_line_cnt++;

    if ( g_batch_line_cnt >= BATCH_SEND_THRESHOLD )
    {
        MSLOG_INFO("AGENT", "batch send %d lines log", g_batch_line_cnt);
        memset(g_batch_buf, 0, BUF_SIZE * BATCH_SEND_THRESHOLD);
        g_batch_line_cnt = 0;
    }
    pthread_mutex_unlock(&g_batch_mutex);
}

static void agent_handle_file_event(int fd){
    char buf[BUF_SIZE] = {0};
    ssize_t len = read(fd, buf, ( sizeof(buf) - 1 ));
    if ( len <= 0 )
    {
        return;
    }
    
    char log_buf[BUF_SIZE] = {0};
    struct inotify_event *event = (struct inotify_event *)buf;
    char file_path[256] = {0};
    if ( event->mask & IN_MODIFY && event->len > 0 )
    {
        size_t base_len = strlen(MONITOR_DIR);
        size_t name_max_len = sizeof(file_path) - base_len - 2;
        if ( event->len > name_max_len )
        {
            MSLOG_WARN("AGENT", "filename too long, truncate: %s", event->name);
        }
        snprintf(file_path, sizeof(file_path), "%s/%.246s", MONITOR_DIR, event->name);
        
        ssize_t read_len = agent_read_log(file_path, log_buf, ( sizeof(log_buf) - 1 ));
        if ( read_len > 0 && agent_filter_log(log_buf) )
        {
            agent_batch_send(log_buf);
        }
    }
}

static void agent_daemonize(void){
    pid_t pid = fork();
    if ( pid < 0 )
    {
        exit(-1);
    }
    if ( pid > 0 )
    {
        exit(0);
    }
    setsid();
    pid = fork();
    if ( pid > 0 )
    {
        exit(0);
    }
    
    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);
    open("/dev/null", O_RDONLY);
    open("/dev/null", O_WRONLY);
    open("/dev/null", O_WRONLY);
}

static void agent_signal_handler(int sig){
    switch ( sig )
    {
    case SIGINT:
    case SIGTERM:
        MSLOG_INFO("AGENT", "recv stop signal, exit...");
        g_running = false;
        break;
    case SIGHUP:
        MSLOG_INFO("AGENT", "recv reload signal, reload config...");
        break;
    default:
        break;
    }
}

static void agent_exit(void){
    if ( M_MEM_IS_VALID(g_batch_buf) )
    {
        M_MEM_FREE(g_batch_buf);
        g_batch_buf = NULL;
    }
    if ( g_epoll_fd >= 0 )
    {
        close(g_epoll_fd);
    }
    if ( g_inotify_fd >= 0 )
    {
        close(g_inotify_fd);
    }
    pthread_mutex_destroy(&g_batch_mutex);
    mslog_deinit();
}

int main(int argc, char* argv[]){
    if ( mslog_init_default("./log_agent.log", MSLOG_INFO, 1024* 1024 * 50, 5, MSLOG_FLUSH_BATCH) < 0 )
    {
        fprintf(stderr, "mslog init failed!\n");
        return -1;
    }
    MSLOG_INFO("AGENT", "Log Agent start, version: %s", AGENT_VERSION);

    signal(SIGINT, agent_signal_handler);
    signal(SIGTERM, agent_signal_handler);
    signal(SIGHUP, agent_signal_handler);

    if ( DAEMON_RUN )
    {
        agent_daemonize();
        MSLOG_INFO("AGENT", "Agent run as daemon mode");
    }

    if (agent_init() < 0)
    {
        MSLOG_FATAL("AGENT", "Agent init failed!");
        agent_exit();
        return -1;
    }

    g_batch_buf = (char*)M_MEM_ALLOC(BUF_SIZE * BATCH_SEND_THRESHOLD);
    if ( !M_MEM_IS_VALID(g_batch_buf) )
    {
        MSLOG_ERROR("AGENT", "batch buf alloc failed");
        agent_exit();
        return -1;
    }
    memset(g_batch_buf, 0, BUF_SIZE * BATCH_SEND_THRESHOLD);

    if ( agent_monitor_dir(MONITOR_DIR) < 0 )
    {
        MSLOG_FATAL("AGENT", "monitor dir %s failed", MONITOR_DIR);
        agent_exit();
        return -1;
    }
    MSLOG_INFO("AGENT", "start monitor dir: %s, filter keyword: %s", MONITOR_DIR, FILTER_KEYWORD);

    struct epoll_event events[MAX_EPOLL_EVENTS];
    while ( g_running )
    {
        int nfds = epoll_wait(g_epoll_fd, events, MAX_EPOLL_EVENTS, 1000);
        if ( nfds < 0 )
        {
            if ( errno == EINTR )
            {
                continue;
            }
            MSLOG_ERROR("AGENT", "epoll wait failed: %s", strerror(errno));
            break;
        }
        for ( int i = 0; i < nfds; i++ )
        {
            if ( events[i].data.fd == g_inotify_fd )
            {
                agent_handle_file_event(g_inotify_fd);
            }
        }
    }

    agent_exit();
    MSLOG_INFO("AGENT", "Log Agent exit success");
    return 0;
}