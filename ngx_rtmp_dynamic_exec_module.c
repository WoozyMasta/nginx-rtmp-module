
/*
 * Copyright (C) Maxim Levchenko
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include "ngx_rtmp_cmd_module.h"
#include "ngx_rtmp_eval.h"
#include <stdlib.h>

#ifdef NGX_LINUX
#include <unistd.h>
#endif

#if (NGX_LINUX)
#include <sys/prctl.h>
#endif


#if !(NGX_WIN32)
static ngx_rtmp_publish_pt       next_publish;
static ngx_rtmp_close_stream_pt  next_close_stream;
#endif


static void    *ngx_rtmp_dynamic_exec_create_app_conf(ngx_conf_t *cf);
static char    *ngx_rtmp_dynamic_exec_merge_app_conf(ngx_conf_t *cf,
                    void *parent, void *child);
static ngx_int_t ngx_rtmp_dynamic_exec_postconfiguration(ngx_conf_t *cf);
static char    *ngx_rtmp_dynamic_exec_arg(ngx_conf_t *cf, ngx_command_t *cmd,
                    void *conf);


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
    ngx_pid_t         pid;
    int               pipefd;
    ngx_connection_t  dummy_conn;
    ngx_event_t       read_evt;
    ngx_event_t       write_evt;
    ngx_log_t        *log;
    unsigned          active:1;
} ngx_rtmp_dynamic_exec_proc_t;


typedef struct {
    ngx_array_t  procs;
    ngx_str_t    current_value;
    u_char       name[NGX_RTMP_MAX_NAME];
    u_char       args_str[NGX_RTMP_MAX_ARGS];
} ngx_rtmp_dynamic_exec_ctx_t;

#endif


static ngx_command_t  ngx_rtmp_dynamic_exec_commands[] = {

    { ngx_string("dynamic_exec_arg"),
      NGX_RTMP_APP_CONF|NGX_CONF_2MORE,
      ngx_rtmp_dynamic_exec_arg,
      NGX_RTMP_APP_CONF_OFFSET,
      0,
      NULL },

    ngx_null_command
};


static ngx_rtmp_module_t  ngx_rtmp_dynamic_exec_module_ctx = {
    NULL,                                       /* preconfiguration */
    ngx_rtmp_dynamic_exec_postconfiguration,    /* postconfiguration */
    NULL,                                       /* create main configuration */
    NULL,                                       /* init main configuration */
    NULL,                                       /* create server configuration */
    NULL,                                       /* merge server configuration */
    ngx_rtmp_dynamic_exec_create_app_conf,      /* create app configuration */
    ngx_rtmp_dynamic_exec_merge_app_conf        /* merge app configuration */
};


ngx_module_t  ngx_rtmp_dynamic_exec_module = {
    NGX_MODULE_V1,
    &ngx_rtmp_dynamic_exec_module_ctx,          /* module context */
    ngx_rtmp_dynamic_exec_commands,             /* module directives */
    NGX_RTMP_MODULE,                            /* module type */
    NULL,                                       /* init master */
    NULL,                                       /* init module */
    NULL,                                       /* init process */
    NULL,                                       /* init thread */
    NULL,                                       /* exit thread */
    NULL,                                       /* exit process */
    NULL,                                       /* exit master */
    NGX_MODULE_V1_PADDING
};


static void *
ngx_rtmp_dynamic_exec_create_app_conf(ngx_conf_t *cf)
{
    ngx_rtmp_dynamic_exec_app_conf_t  *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(ngx_rtmp_dynamic_exec_app_conf_t));
    if (conf == NULL) {
        return NULL;
    }

    if (ngx_array_init(&conf->rules, cf->pool, 4,
                       sizeof(ngx_rtmp_dynamic_exec_rule_t))
        != NGX_OK)
    {
        return NULL;
    }

    return conf;
}


static char *
ngx_rtmp_dynamic_exec_merge_app_conf(ngx_conf_t *cf, void *parent, void *child)
{
    return NGX_CONF_OK;
}


static char *
ngx_rtmp_dynamic_exec_arg(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_rtmp_dynamic_exec_app_conf_t  *dacf = conf;
    ngx_rtmp_dynamic_exec_rule_t      *rule;
    ngx_str_t                         *value, *arg;
    ngx_uint_t                         i;

    value = cf->args->elts;

    rule = ngx_array_push(&dacf->rules);
    if (rule == NULL) {
        return NGX_CONF_ERROR;
    }

    ngx_memzero(rule, sizeof(*rule));

    rule->arg_name = value[1];
    rule->cmd      = value[2];

    if (ngx_array_init(&rule->args, cf->pool,
                       cf->args->nelts > 3 ? cf->args->nelts - 3 : 1,
                       sizeof(ngx_str_t))
        != NGX_OK)
    {
        return NGX_CONF_ERROR;
    }

    for (i = 3; i < cf->args->nelts; i++) {
        arg = ngx_array_push(&rule->args);
        if (arg == NULL) {
            return NGX_CONF_ERROR;
        }
        *arg = value[i];
    }

    return NGX_CONF_OK;
}


#if !(NGX_WIN32)


static u_char *
ngx_rtmp_dynamic_exec_find_arg(u_char *args, ngx_str_t *name,
    ngx_str_t *value)
{
    u_char  *p, *end, *eq, *amp;
    size_t   args_len;

    if (args == NULL) {
        return NULL;
    }

    args_len = ngx_strlen(args);
    if (args_len == 0) {
        return NULL;
    }

    p   = args;
    end = args + args_len;

    while (p < end) {
        eq = ngx_strlchr(p, end, '=');
        if (eq == NULL) {
            break;
        }

        if ((size_t)(eq - p) == name->len &&
            ngx_strncmp(p, name->data, name->len) == 0)
        {
            amp         = ngx_strlchr(eq + 1, end, '&');
            value->data = eq + 1;
            value->len  = (amp ? amp : end) - (eq + 1);
            return value->data;
        }

        amp = ngx_strlchr(p, end, '&');
        if (amp == NULL) {
            break;
        }
        p = amp + 1;
    }

    return NULL;
}


static void
ngx_rtmp_dynamic_exec_eval_cstr(void *sctx, ngx_rtmp_eval_t *e, ngx_str_t *ret)
{
    ngx_rtmp_session_t           *s = sctx;
    ngx_rtmp_dynamic_exec_ctx_t  *ctx;

    ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_dynamic_exec_module);
    if (ctx == NULL) {
        ret->len = 0;
        return;
    }

    ret->data = (u_char *) ctx + e->offset;
    ret->len  = ngx_strlen(ret->data);
}


static void
ngx_rtmp_dynamic_exec_eval_value(void *sctx, ngx_rtmp_eval_t *e,
    ngx_str_t *ret)
{
    ngx_rtmp_session_t           *s = sctx;
    ngx_rtmp_dynamic_exec_ctx_t  *ctx;

    ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_dynamic_exec_module);
    if (ctx == NULL) {
        ret->len = 0;
        return;
    }

    *ret = ctx->current_value;
}


static ngx_rtmp_eval_t  ngx_rtmp_dynamic_exec_specific_eval[] = {

    { ngx_string("name"),
      ngx_rtmp_dynamic_exec_eval_cstr,
      offsetof(ngx_rtmp_dynamic_exec_ctx_t, name) },

    { ngx_string("args"),
      ngx_rtmp_dynamic_exec_eval_cstr,
      offsetof(ngx_rtmp_dynamic_exec_ctx_t, args_str) },

    { ngx_string("value"),
      ngx_rtmp_dynamic_exec_eval_value,
      0 },

    ngx_rtmp_null_eval
};


static ngx_rtmp_eval_t *ngx_rtmp_dynamic_exec_eval[] = {
    ngx_rtmp_eval_session,
    ngx_rtmp_dynamic_exec_specific_eval,
    NULL
};


static void
ngx_rtmp_dynamic_exec_kill(ngx_rtmp_dynamic_exec_proc_t *proc,
    ngx_int_t signal)
{
    if (proc->read_evt.active) {
        ngx_del_event(&proc->read_evt, NGX_READ_EVENT, 0);
    }

    if (!proc->active) {
        return;
    }

    ngx_log_error(NGX_LOG_INFO, proc->log, 0,
                  "dynamic_exec: terminating child %ui", (ngx_int_t) proc->pid);

    proc->active = 0;

    close(proc->pipefd);
    proc->pipefd = -1;

    if (signal == 0) {
        return;
    }

    if (kill(proc->pid, signal) == -1) {
        ngx_log_error(NGX_LOG_INFO, proc->log, ngx_errno,
                      "dynamic_exec: kill failed pid=%i", (ngx_int_t) proc->pid);
    }
}


static void
ngx_rtmp_dynamic_exec_child_dead(ngx_event_t *ev)
{
    ngx_connection_t              *c = ev->data;
    ngx_rtmp_dynamic_exec_proc_t  *proc;

    proc = c->data;

    ngx_log_error(NGX_LOG_INFO, proc->log, 0,
                  "dynamic_exec: child %ui exited", (ngx_int_t) proc->pid);

    ngx_rtmp_dynamic_exec_kill(proc, 0);
}


static ngx_int_t
ngx_rtmp_dynamic_exec_spawn(ngx_rtmp_session_t *s,
    ngx_rtmp_dynamic_exec_rule_t *rule, ngx_rtmp_dynamic_exec_proc_t *proc)
{
    int         fd, ret, maxfd, pipefd[2];
    char      **args, **arg_out;
    ngx_pid_t   pid;
    ngx_str_t  *arg_in, a;
    ngx_uint_t  n;
    ngx_log_t  *log;

    log = s->connection->log;

    ngx_log_error(NGX_LOG_INFO, log, 0,
                  "dynamic_exec: starting '%V' for arg '%V'",
                  &rule->cmd, &rule->arg_name);

    if (pipe(pipefd) == -1) {
        ngx_log_error(NGX_LOG_ERR, log, ngx_errno,
                      "dynamic_exec: pipe failed");
        return NGX_ERROR;
    }

    /* make write end survive execvp so parent detects child exit via read EOF */
    ret = fcntl(pipefd[1], F_GETFD);

    if (ret != -1) {
        ret &= ~FD_CLOEXEC;
        ret  = fcntl(pipefd[1], F_SETFD, ret);
    }

    if (ret == -1) {
        close(pipefd[0]);
        close(pipefd[1]);
        ngx_log_error(NGX_LOG_ERR, log, ngx_errno,
                      "dynamic_exec: fcntl failed");
        return NGX_ERROR;
    }

    pid = fork();

    switch (pid) {

    case -1:

        close(pipefd[0]);
        close(pipefd[1]);
        ngx_log_error(NGX_LOG_ERR, log, ngx_errno,
                      "dynamic_exec: fork failed");
        return NGX_ERROR;

    case 0:

        /* child */

#if (NGX_LINUX)
        prctl(PR_SET_PDEATHSIG, SIGKILL, 0, 0, 0);
#endif

        maxfd = (int) sysconf(_SC_OPEN_MAX);
        for (fd = 0; fd < maxfd; ++fd) {
            if (fd == pipefd[1]) {
                continue;
            }
            close(fd);
        }

        fd = open("/dev/null", O_RDWR);
        dup2(fd, STDIN_FILENO);
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);

        args = ngx_alloc((rule->args.nelts + 2) * sizeof(char *), log);
        if (args == NULL) {
            exit(1);
        }

        arg_in  = rule->args.elts;
        arg_out = args;
        *arg_out++ = (char *) rule->cmd.data;

        for (n = 0; n < rule->args.nelts; n++, ++arg_in) {
            ngx_rtmp_eval(s, arg_in, ngx_rtmp_dynamic_exec_eval, &a, log);

            if (ngx_rtmp_eval_streams(&a) != NGX_DONE) {
                continue;
            }

            *arg_out++ = (char *) a.data;
        }

        *arg_out = NULL;

        if (execvp((char *) rule->cmd.data, args) == -1) {
            exit(1);
        }

        break;

    default:

        /* parent */

        close(pipefd[1]);

        proc->pid    = pid;
        proc->pipefd = pipefd[0];
        proc->active = 1;
        proc->log    = log;

        proc->dummy_conn.fd    = pipefd[0];
        proc->dummy_conn.data  = proc;
        proc->dummy_conn.log   = log;
        proc->dummy_conn.read  = &proc->read_evt;
        proc->dummy_conn.write = &proc->write_evt;

        proc->read_evt.data    = &proc->dummy_conn;
        proc->read_evt.log     = log;
        proc->read_evt.handler = ngx_rtmp_dynamic_exec_child_dead;

        proc->write_evt.data   = &proc->dummy_conn;

        if (ngx_add_event(&proc->read_evt, NGX_READ_EVENT, 0) != NGX_OK) {
            ngx_log_error(NGX_LOG_ERR, log, ngx_errno,
                          "dynamic_exec: failed to add child event");
        }

        ngx_log_debug2(NGX_LOG_DEBUG_RTMP, log, 0,
                       "dynamic_exec: child '%V' started pid=%i",
                       &rule->cmd, (ngx_int_t) pid);
        break;
    }

    return NGX_OK;
}


static ngx_int_t
ngx_rtmp_dynamic_exec_publish(ngx_rtmp_session_t *s, ngx_rtmp_publish_t *v)
{
    ngx_rtmp_dynamic_exec_app_conf_t  *conf;
    ngx_rtmp_dynamic_exec_ctx_t       *ctx;
    ngx_rtmp_dynamic_exec_rule_t      *rule;
    ngx_rtmp_dynamic_exec_proc_t      *proc;
    ngx_str_t                          value;
    ngx_uint_t                         i;

    if (s->auto_pushed || s->relay) {
        goto next;
    }

    conf = ngx_rtmp_get_module_app_conf(s, ngx_rtmp_dynamic_exec_module);
    if (conf == NULL || conf->rules.nelts == 0) {
        goto next;
    }

    ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_dynamic_exec_module);

    if (ctx != NULL) {
        proc = ctx->procs.elts;
        for (i = 0; i < ctx->procs.nelts; i++) {
            ngx_rtmp_dynamic_exec_kill(&proc[i], SIGKILL);
        }

        /*
         * Allocate fresh proc storage so the new proc addresses differ from
         * the killed ones; prevents stale event callbacks from touching new
         * proc state should a child-dead event arrive after ngx_del_event.
         */
        if (ngx_array_init(&ctx->procs, s->connection->pool,
                           conf->rules.nelts,
                           sizeof(ngx_rtmp_dynamic_exec_proc_t))
            != NGX_OK)
        {
            goto next;
        }

    } else {
        ctx = ngx_pcalloc(s->connection->pool,
                          sizeof(ngx_rtmp_dynamic_exec_ctx_t));
        if (ctx == NULL) {
            goto next;
        }

        if (ngx_array_init(&ctx->procs, s->connection->pool,
                           conf->rules.nelts,
                           sizeof(ngx_rtmp_dynamic_exec_proc_t))
            != NGX_OK)
        {
            goto next;
        }

        ngx_rtmp_set_ctx(s, ctx, ngx_rtmp_dynamic_exec_module);
    }

    ngx_memcpy(ctx->name,     v->name, NGX_RTMP_MAX_NAME);
    ngx_memcpy(ctx->args_str, v->args, NGX_RTMP_MAX_ARGS);

    rule = conf->rules.elts;

    for (i = 0; i < conf->rules.nelts; i++) {

        if (ngx_rtmp_dynamic_exec_find_arg(ctx->args_str, &rule[i].arg_name,
                                           &value)
            == NULL)
        {
            continue;
        }

        ctx->current_value = value;

        proc = ngx_array_push(&ctx->procs);
        if (proc == NULL) {
            continue;
        }

        ngx_memzero(proc, sizeof(*proc));
        proc->pipefd = -1;

        ngx_rtmp_dynamic_exec_spawn(s, &rule[i], proc);
    }

next:
    return next_publish(s, v);
}


static ngx_int_t
ngx_rtmp_dynamic_exec_close_stream(ngx_rtmp_session_t *s,
    ngx_rtmp_close_stream_t *v)
{
    ngx_rtmp_dynamic_exec_ctx_t   *ctx;
    ngx_rtmp_dynamic_exec_proc_t  *proc;
    ngx_uint_t                     i;

    ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_dynamic_exec_module);

    if (ctx != NULL) {
        proc = ctx->procs.elts;
        for (i = 0; i < ctx->procs.nelts; i++) {
            ngx_rtmp_dynamic_exec_kill(&proc[i], SIGKILL);
        }
    }

    return next_close_stream(s, v);
}


static ngx_int_t
ngx_rtmp_dynamic_exec_postconfiguration(ngx_conf_t *cf)
{
    next_publish          = ngx_rtmp_publish;
    ngx_rtmp_publish      = ngx_rtmp_dynamic_exec_publish;

    next_close_stream     = ngx_rtmp_close_stream;
    ngx_rtmp_close_stream = ngx_rtmp_dynamic_exec_close_stream;

    return NGX_OK;
}


#else   /* NGX_WIN32 */


static ngx_int_t
ngx_rtmp_dynamic_exec_postconfiguration(ngx_conf_t *cf)
{
    return NGX_OK;
}


#endif  /* !(NGX_WIN32) */
