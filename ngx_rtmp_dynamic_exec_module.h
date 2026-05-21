
/*
 * Copyright (C) Maxim Levchenko
 */

#ifndef _NGX_RTMP_DYNAMIC_EXEC_MODULE_H_INCLUDED_
#define _NGX_RTMP_DYNAMIC_EXEC_MODULE_H_INCLUDED_


#include <ngx_config.h>
#include <ngx_core.h>
#include "ngx_rtmp.h"


typedef struct {
    ngx_str_t    arg_name;
    ngx_str_t    cmd;
    ngx_array_t  args;
} ngx_rtmp_dynamic_exec_rule_t;


typedef struct {
    ngx_array_t  rules;
} ngx_rtmp_dynamic_exec_app_conf_t;


#if !(NGX_WIN32)

typedef struct {
    ngx_pid_t                      pid;
    int                            pipefd;
    ngx_connection_t               dummy_conn;
    ngx_event_t                    read_evt;
    ngx_event_t                    write_evt;
    ngx_log_t                     *log;
    ngx_rtmp_dynamic_exec_rule_t  *rule;
    unsigned                       active:1;
} ngx_rtmp_dynamic_exec_proc_t;


typedef struct {
    ngx_array_t  procs;
    ngx_str_t    current_value;
    u_char       name[NGX_RTMP_MAX_NAME];
    u_char       args_str[NGX_RTMP_MAX_ARGS];
} ngx_rtmp_dynamic_exec_ctx_t;

#endif


extern ngx_module_t  ngx_rtmp_dynamic_exec_module;


#endif /* _NGX_RTMP_DYNAMIC_EXEC_MODULE_H_INCLUDED_ */
