
/*
 * Copyright (C) Maxim Levchenko
 */

#include <ngx_config.h>
#include <ngx_core.h>
#include "ngx_rtmp.h"
#include "ngx_rtmp_cmd_module.h"
#include "ngx_rtmp_relay_module.h"


#define NGX_RTMP_DYNAMIC_PUSH_KEY_MAX_LEN  512


typedef struct {
    ngx_str_t                arg_name;
    ngx_flag_t               nokey;
    ngx_rtmp_relay_target_t  target;
} ngx_rtmp_dynamic_push_rule_t;


typedef struct {
    ngx_array_t              rules;       /* ngx_rtmp_dynamic_push_rule_t */
    ngx_flag_t               required;
    ngx_flag_t               strict;
    size_t                   key_max_len;
#if (NGX_SSL)
    ngx_ssl_t               *ssl;        /* shared SSL ctx for rtmps:// rules */
#endif
} ngx_rtmp_dynamic_push_app_conf_t;


static ngx_rtmp_publish_pt  next_publish;

static void *ngx_rtmp_dynamic_push_create_app_conf(ngx_conf_t *cf);
static char *ngx_rtmp_dynamic_push_merge_app_conf(ngx_conf_t *cf,
    void *parent, void *child);
static ngx_int_t ngx_rtmp_dynamic_push_postconfiguration(ngx_conf_t *cf);
static char *ngx_rtmp_dynamic_push_arg(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);


static ngx_command_t  ngx_rtmp_dynamic_push_commands[] = {

    { ngx_string("dynamic_push_arg"),
      NGX_RTMP_APP_CONF|NGX_CONF_TAKE23,
      ngx_rtmp_dynamic_push_arg,
      NGX_RTMP_APP_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("dynamic_push_required"),
      NGX_RTMP_APP_CONF|NGX_CONF_FLAG,
      ngx_conf_set_flag_slot,
      NGX_RTMP_APP_CONF_OFFSET,
      offsetof(ngx_rtmp_dynamic_push_app_conf_t, required),
      NULL },

    { ngx_string("dynamic_push_strict"),
      NGX_RTMP_APP_CONF|NGX_CONF_FLAG,
      ngx_conf_set_flag_slot,
      NGX_RTMP_APP_CONF_OFFSET,
      offsetof(ngx_rtmp_dynamic_push_app_conf_t, strict),
      NULL },

    { ngx_string("dynamic_push_key_max_len"),
      NGX_RTMP_APP_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_size_slot,
      NGX_RTMP_APP_CONF_OFFSET,
      offsetof(ngx_rtmp_dynamic_push_app_conf_t, key_max_len),
      NULL },

    ngx_null_command
};


static ngx_rtmp_module_t  ngx_rtmp_dynamic_push_module_ctx = {
    NULL,                                       /* preconfiguration */
    ngx_rtmp_dynamic_push_postconfiguration,    /* postconfiguration */
    NULL,                                       /* create main configuration */
    NULL,                                       /* init main configuration */
    NULL,                                       /* create server configuration */
    NULL,                                       /* merge server configuration */
    ngx_rtmp_dynamic_push_create_app_conf,      /* create app configuration */
    ngx_rtmp_dynamic_push_merge_app_conf        /* merge app configuration */
};


ngx_module_t  ngx_rtmp_dynamic_push_module = {
    NGX_MODULE_V1,
    &ngx_rtmp_dynamic_push_module_ctx,          /* module context */
    ngx_rtmp_dynamic_push_commands,             /* module directives */
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
ngx_rtmp_dynamic_push_create_app_conf(ngx_conf_t *cf)
{
    ngx_rtmp_dynamic_push_app_conf_t  *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(ngx_rtmp_dynamic_push_app_conf_t));
    if (conf == NULL) {
        return NULL;
    }

    if (ngx_array_init(&conf->rules, cf->pool, 4,
                       sizeof(ngx_rtmp_dynamic_push_rule_t))
        != NGX_OK)
    {
        return NULL;
    }

    conf->required    = NGX_CONF_UNSET;
    conf->strict      = NGX_CONF_UNSET;
    conf->key_max_len = NGX_CONF_UNSET_SIZE;

    return conf;
}


static char *
ngx_rtmp_dynamic_push_merge_app_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_rtmp_dynamic_push_app_conf_t *prev = parent;
    ngx_rtmp_dynamic_push_app_conf_t *conf = child;

    ngx_conf_merge_value(conf->required, prev->required, 0);
    ngx_conf_merge_value(conf->strict, prev->strict, 0);
    ngx_conf_merge_size_value(conf->key_max_len, prev->key_max_len,
                              NGX_RTMP_DYNAMIC_PUSH_KEY_MAX_LEN);

    return NGX_CONF_OK;
}


static ngx_int_t
ngx_rtmp_dynamic_push_validate_key(u_char *p, size_t len, size_t max_len)
{
    size_t  i;
    u_char  c;

    if (len == 0 || len > max_len) {
        return NGX_ERROR;
    }

    for (i = 0; i < len; i++) {
        c = p[i];

        if ((c >= 'A' && c <= 'Z') ||
            (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') ||
            c == '_' || c == '-' || c == '/')
        {
            continue;
        }

        return NGX_ERROR;
    }

    /* reject "://" to block SSRF attempts even through allowed chars */
    if (ngx_strnstr(p, "://", len) != NULL) {
        return NGX_ERROR;
    }

    return NGX_OK;
}


static u_char *
ngx_rtmp_dynamic_push_find_arg(u_char *args, ngx_str_t *name,
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

        if ((size_t) (eq - p) == name->len &&
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


static ngx_int_t
ngx_rtmp_dynamic_push_publish(ngx_rtmp_session_t *s, ngx_rtmp_publish_t *v)
{
    ngx_rtmp_dynamic_push_app_conf_t  *conf;
    ngx_rtmp_dynamic_push_rule_t      *rule;
    ngx_rtmp_relay_target_t            target;
    ngx_str_t                          name, key;
    ngx_uint_t                         i, created;
    ngx_int_t                          rc;

    if (s->auto_pushed || s->relay) {
        goto next;
    }

    conf = ngx_rtmp_get_module_app_conf(s, ngx_rtmp_dynamic_push_module);
    if (conf == NULL || conf->rules.nelts == 0) {
        goto next;
    }

    name.data = v->name;
    name.len  = ngx_strlen(v->name);

    created = 0;
    rule    = conf->rules.elts;

    for (i = 0; i < conf->rules.nelts; i++) {
        if (ngx_rtmp_dynamic_push_find_arg(v->args, &rule[i].arg_name, &key)
            == NULL)
        {
            continue;
        }

        target = rule[i].target;  /* stack copy */

        if (rule[i].nokey) {
            target.play_path = name;

        } else {
            if (ngx_rtmp_dynamic_push_validate_key(key.data, key.len,
                                                   conf->key_max_len)
                != NGX_OK)
            {
                ngx_log_error(NGX_LOG_ERR, s->connection->log, 0,
                              "dynamic_push: invalid key for arg \"%V\"",
                              &rule[i].arg_name);
                return NGX_ERROR;
            }

            target.play_path = key;
        }

        rc = ngx_rtmp_relay_push(s, &name, &target);
        if (rc != NGX_OK) {
            ngx_log_error(NGX_LOG_ERR, s->connection->log, 0,
                          "dynamic_push: push failed for arg \"%V\" key \"%V\"",
                          &rule[i].arg_name, &key);
            if (conf->strict) {
                return NGX_ERROR;
            }
            continue;
        }

        created++;
    }

    if (created == 0 && conf->required) {
        ngx_log_error(NGX_LOG_ERR, s->connection->log, 0,
                      "dynamic_push: no targets created, publish rejected "
                      "(dynamic_push_required on)");
        return NGX_ERROR;
    }

next:
    return next_publish(s, v);
}


static char *
ngx_rtmp_dynamic_push_arg(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_rtmp_dynamic_push_app_conf_t  *dacf = conf;
    ngx_rtmp_dynamic_push_rule_t      *rule;
    ngx_str_t                         *value;
    ngx_url_t                          u;
    u_char                            *uri_start, *uri_end, *slash;

    /* value[0]=directive  value[1]=arg_name  value[2]=rtmp_url  [value[3]=nokey] */
    value = cf->args->elts;

    rule = ngx_array_push(&dacf->rules);
    if (rule == NULL) {
        return NGX_CONF_ERROR;
    }
    ngx_memzero(rule, sizeof(*rule));

    rule->arg_name = value[1];

    ngx_memzero(&u, sizeof(u));
    u.uri_part = 1;
    u.url      = value[2];

    if (u.url.len >= 8 &&
        ngx_strncasecmp(u.url.data, (u_char *) "rtmps://", 8) == 0)
    {
#if (NGX_SSL)
        u.default_port = 443;
        u.url.data += 8;
        u.url.len  -= 8;

        if (dacf->ssl == NULL) {
            dacf->ssl = ngx_pcalloc(cf->pool, sizeof(ngx_ssl_t));
            if (dacf->ssl == NULL) {
                return NGX_CONF_ERROR;
            }
            dacf->ssl->log = cf->log;
            if (ngx_ssl_create(dacf->ssl,
                               NGX_SSL_TLSv1_2|NGX_SSL_TLSv1_3, NULL)
                != NGX_OK)
            {
                return NGX_CONF_ERROR;
            }
            SSL_CTX_set_verify(dacf->ssl->ctx, SSL_VERIFY_NONE, NULL);
        }

        rule->target.ssl = dacf->ssl;
#else
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "dynamic_push: rtmps:// requires nginx built "
                           "with SSL support: \"%V\"", &value[2]);
        return NGX_CONF_ERROR;
#endif

    } else if (u.url.len >= 7 &&
               ngx_strncasecmp(u.url.data, (u_char *) "rtmp://", 7) == 0)
    {
        u.default_port = 1935;
        u.url.data += 7;
        u.url.len  -= 7;

    } else {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "dynamic_push: URL must start with "
                           "rtmp:// or rtmps://: \"%V\"", &value[2]);
        return NGX_CONF_ERROR;
    }

    if (ngx_parse_url(cf->pool, &u) != NGX_OK) {
        if (u.err) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "dynamic_push: %s in URL \"%V\"",
                               u.err, &value[2]);
        }
        return NGX_CONF_ERROR;
    }

    /* extract app from URI path; reject if playpath already present */
    uri_start = u.uri.data;
    uri_end   = u.uri.data + u.uri.len;

    if (uri_start != uri_end && *uri_start == '/') {
        uri_start++;
    }

    if (uri_start == uri_end) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "dynamic_push: application name missing in "
                           "URL \"%V\"", &value[2]);
        return NGX_CONF_ERROR;
    }

    slash = ngx_strlchr(uri_start, uri_end, '/');
    if (slash != NULL && slash + 1 < uri_end) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "dynamic_push: base URL must not contain a stream "
                           "name (use \"rtmp://host/app\"): \"%V\"",
                           &value[2]);
        return NGX_CONF_ERROR;
    }

    rule->target.url      = u;
    rule->target.app.data = uri_start;
    rule->target.app.len  = (slash ? slash : uri_end) - uri_start;
    rule->target.tag      = &ngx_rtmp_dynamic_push_module;
    rule->target.data     = &rule->target;
    rule->target.live     = 1;

    if (cf->args->nelts == 4) {
        if (ngx_strncmp(value[3].data, "nokey", 5) != 0 || value[3].len != 5) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "dynamic_push: unknown option \"%V\" "
                               "(expected \"nokey\")", &value[3]);
            return NGX_CONF_ERROR;
        }
        rule->nokey = 1;
    }

    return NGX_CONF_OK;
}


static ngx_int_t
ngx_rtmp_dynamic_push_postconfiguration(ngx_conf_t *cf)
{
    next_publish     = ngx_rtmp_publish;
    ngx_rtmp_publish = ngx_rtmp_dynamic_push_publish;

    return NGX_OK;
}
