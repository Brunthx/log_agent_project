#define _POSIX_C_SOURCE 200112L
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/inotify.h>
#include <sys/mman.h>
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