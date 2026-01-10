CC				:= gcc
CFLAGS			:= -g -Wall -O2 -std=c99 -D_GNU_SOURCE -fPIC
LDFLAGS			:= -lpthread -lm
LIBMSLOG_DIR	:= ./libmslog
APP_DIR			:= ./app
LIB_OUT_DIR		:= ./lib
BIN_OUT_DIR		:= ./bin

LIB_NAME		:= mslog
STATIC_LIB		:= $(LIB_OUT_DIR)/lib$(LIB_NAME).a
MSLOG_INC_DIR	:= $(LIBMSLOG_DIR)/inc
MSLOG_SRC_DIR	:= $(LIBMSLOG_DIR)/src

MSLOG_SRCS		:= $(wildcard $(MSLOG_SRC_DIR)/*.c)
MSLOG_OBJS		:= $(patsubst $(MSLOG_SRC_DIR)/%.c, $(MSLOG_SRC_DIR)/%.o, $(MSLOG_SRCS))

APP_SRC			:= $(APP_DIR)/log_agent.c
APP_TARGET		:= $(BIN_OUT_DIR)/log_agent

DAEMON_DEFINE	:= -DDAEMON_RUN=1

all: dirs static_lib app
	@echo -e "\033[32m 全部编译完成！静态库：$(STATIC_LIB) | 可执行程序：$(APP_TARGET)\033[0m"
	@echo -e "\n log_agent（前台模式）全部编译完成！"

daemon_all: dirs static_lib_daemon app
	@echo -e "\033[32m 全部编译完成！静态库：$(STATIC_LIB) | 可执行程序：$(APP_TARGET)\033[0m"
	@echo -e "\n log_agent（后台守护模式）全部编译完成！"

dirs:
	@mkdir -p $(LIB_OUT_DIR)
	@mkdir -p $(BIN_OUT_DIR)

static_lib: $(MSLOG_OBJS)
	@echo -e "\033[33m 正在编译mslog所有模块（utils+thread+mem_pool+core）\033[0m"
	@ar rcs $(STATIC_LIB) $(MSLOG_OBJS)
	@rm -f $(MSLOG_OBJS)  # 清理临时.o文件
	@echo -e "\033[32m mslog静态库编译完成：$(STATIC_LIB)\033[0m"

$(MSLOG_SRC_DIR)/%.o: $(MSLOG_SRC_DIR)/%.c
	@$(CC) $(CFLAGS) -c $< -o $@ -I$(MSLOG_INC_DIR)

static_lib_daemon: $(MSLOG_OBJS)
	@echo -e "\033[33m 正在编译mslog所有模块（utils+thread+mem_pool+core）\033[0m"
	@ar rcs $(STATIC_LIB) $(MSLOG_OBJS)
	@rm -f $(MSLOG_OBJS)  # 清理临时.o文件
	@echo -e "\033[32m mslog静态库编译完成：$(STATIC_LIB)\033[0m"

$(MSLOG_SRC_DIR)/%.o: $(MSLOG_SRC_DIR)/%.c
	@$(CC) $(CFLAGS) $(DAEMON_DEFINE) -c $< -o $@ -I$(MSLOG_INC_DIR)

app:
	@echo -e "\033[33m 正在编译log_agent并静态链接libmslog.a\033[0m"
	@$(CC) $(CFLAGS) $(APP_SRC) -o $(APP_TARGET) \
	-I$(MSLOG_INC_DIR) \
	-L$(LIB_OUT_DIR) \
	-l$(LIB_NAME) \
	$(LDFLAGS)

run: all
	@echo -e "\n 开始运行 log_agent（前台模式）..."
	@if [ -f $(APP_TARGET) ]; then \
		cd $(BIN_OUT_DIR) && ./log_agent; \
	else \
		echo " 运行失败：可执行程序 $(APP_TARGET) 不存在！"; \
	fi

run-daemon: daemon_all
	@echo -e "\n 开始运行 log_agent（后台守护模式）..."
	@if [ -f $(APP_TARGET) ]; then \
		mkdir -p $(BIN_OUT_DIR); \
		cd $(BIN_OUT_DIR) && nohup ./log_agent >> log_agent.log 2>&1 & \
		sleep 1; \
		REAL_PID=`ps -ef | grep $(notdir $(APP_TARGET)) | grep -v grep | awk '{print $$2}'`; \
		echo " 运行成功！进程号：$$REAL_PID"; \
		echo " 日志查看指令：tail -f -n 100 $(BIN_OUT_DIR)/log_agent.log"; \
		echo " 停止进程指令：kill -9 $$REAL_PID"; \
	else \
		echo " 运行失败：可执行程序 $(APP_TARGET) 不存在！"; \
	fi

stop:
	@echo -e "\n 停止 log_agent 进程..."
	@REAL_PID=`ps -ef | grep $(notdir $(APP_TARGET)) | grep -v grep | awk '{print $$2}'`; \
	if [ -n "$$REAL_PID" ]; then \
		kill -9 $$REAL_PID && echo " 停止成功！已杀死进程号：$$REAL_PID"; \
	else \
		echo " 无运行中的 log_agent 进程"; \
	fi

log:
	@echo -e "\n 实时查看 log_agent 日志（Ctrl+C退出）..."
	@tail -f $(BIN_OUT_DIR)/log_agent.log

clean:
	@echo -e "\033[33m 正在清理所有编译产物\033[0m"
	@rm -rf $(LIB_OUT_DIR) $(BIN_OUT_DIR)
	@rm -rf *.log log.* core.*
	@rm -f $(MSLOG_SRC_DIR)/*.o
	@echo -e "\033[32m 清理完成！无残留文件\033[0m"

.PHONY: all dirs static_lib app clean