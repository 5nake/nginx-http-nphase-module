/*
 * Copyright (C) Simon Lee@Huawei Tech.
 */

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

typedef struct {
    off_t        start;
    off_t        end;
    off_t        length;
    ngx_int_t    flag;         /* -1: no range; 0: xxx-xxx ; 1: xxx-;  2: -xxx; */
    ngx_str_t    range;
} ngx_http_nphase_range_t;

typedef struct {
    ngx_str_t   uri;
    ngx_int_t   uri_var_index;
    ngx_int_t   range_var_index;
} ngx_http_nphase_conf_t;

typedef struct {
    ngx_uint_t                pr_status;
    ngx_uint_t                sr_count;
    ngx_uint_t                sr_count_e;
    ngx_str_t                 uri_var_value;

    off_t                     wfsz;
    ngx_array_t               range_in;
    ngx_http_nphase_range_t   range_sent;
    
    ngx_str_t                 loc_body_c;
    unsigned                  sr_done:1;
    unsigned                  sr_error:1;
    unsigned                  header_sent:1;
    unsigned                  body_ready:1;
    unsigned                  loc_ready:1;
    unsigned                  loc_body:1;
} ngx_http_nphase_ctx_t;

typedef struct {
    ngx_http_nphase_range_t   range_sent;
} ngx_http_nphase_sub_ctx_t;

typedef struct {
    ngx_int_t                 index;
    ngx_http_complex_value_t  value;
    ngx_http_set_variable_pt  set_handler;
} ngx_http_nphase_variable_t;

#define NGX_HTTP_NPHASE_MAX_RETRY         3

static void * ngx_http_nphase_create_conf(ngx_conf_t *cf);
static char * ngx_http_nphase_merge_conf(ngx_conf_t *cf, void *parent, void *child);
static ngx_int_t ngx_http_nphase_init(ngx_conf_t *cf);
static ngx_int_t ngx_http_nphase_access_handler(ngx_http_request_t *r);
static ngx_int_t ngx_http_nphase_content_handler(ngx_http_request_t *r);
static ngx_int_t ngx_http_nphase_subrequest_done(ngx_http_request_t *r, void *data, ngx_int_t rc);
ngx_int_t    ngx_http_nphase_filter_init(ngx_conf_t *cf);    
static ngx_int_t ngx_http_nphase_header_filter(ngx_http_request_t *r);
static ngx_int_t ngx_http_nphase_body_filter(ngx_http_request_t *r, ngx_chain_t *in);
void ngx_http_nphase_discard_bufs(ngx_pool_t *pool, ngx_chain_t *in);
static char *ngx_http_nphase_uri(ngx_conf_t *cf, ngx_command_t *cmd, void *conf); 
static char *ngx_http_nphase_set_uri_var(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
static char *ngx_http_nphase_set_range_var(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
ngx_int_t ngx_http_nphase_range_update(ngx_http_request_t *r, ngx_int_t index, off_t start, off_t end, ngx_int_t flag);
ngx_int_t ngx_http_nphase_range_parse(ngx_http_request_t *r, ngx_http_nphase_ctx_t *ctx);
ngx_int_t ngx_http_nphase_content_range_parse(u_char *p, ngx_http_nphase_range_t *range);
ngx_int_t ngx_http_nphase_copy_header_value(ngx_list_t *headers, ngx_str_t *k, ngx_str_t *v);
ngx_int_t ngx_http_nphase_run_subrequest(ngx_http_request_t *r, ngx_http_nphase_ctx_t *ctx,
                                                        ngx_str_t *uri, ngx_str_t *args);
ngx_int_t ngx_http_nphase_process_header(ngx_http_request_t *r, ngx_http_nphase_ctx_t *ctx);
static ngx_int_t ngx_http_nphase_add_range_singlepart_header(ngx_http_request_t *r, ngx_http_nphase_ctx_t *ctx);

static ngx_command_t  ngx_http_nphase_commands[] = {

    { ngx_string("nphase_uri"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_http_nphase_uri,
      NGX_HTTP_LOC_CONF_OFFSET,
      0,
      NULL },
      
    { ngx_string("nphase_set_uri_var"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_http_nphase_set_uri_var,
      NGX_HTTP_LOC_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("nphase_set_range_var"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_http_nphase_set_range_var,
      NGX_HTTP_LOC_CONF_OFFSET,
      0,
      NULL },

      ngx_null_command
};

static ngx_http_module_t  ngx_http_nphase_module_ctx = {
    NULL,                            /* preconfiguration */
    ngx_http_nphase_init,            /* postconfiguration */

    NULL,                                  /* create main configuration */
    NULL,                                  /* init main configuration */

    NULL,                                  /* create server configuration */
    NULL,                                  /* merge server configuration */

    ngx_http_nphase_create_conf,     /* create location configuration */
    ngx_http_nphase_merge_conf       /* merge location configuration */
};


ngx_module_t  ngx_http_nphase_module = {
    NGX_MODULE_V1,
    &ngx_http_nphase_module_ctx,     /* module context */
    ngx_http_nphase_commands,        /* module directives */
    NGX_HTTP_MODULE,                       /* module type */
    NULL,                                  /* init master */
    NULL,                                  /* init module */
    NULL,                                  /* init process */
    NULL,                                  /* init thread */
    NULL,                                  /* exit thread */
    NULL,                                  /* exit process */
    NULL,                                  /* exit master */
    NGX_MODULE_V1_PADDING
};

static ngx_http_output_header_filter_pt    ngx_http_next_header_filter;
static ngx_http_output_body_filter_pt    ngx_http_next_body_filter;

static void *
ngx_http_nphase_create_conf(ngx_conf_t *cf)
{
    ngx_http_nphase_conf_t  *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_nphase_conf_t));
    if (conf == NULL) {
        return NULL;
    }

    conf->uri_var_index = NGX_CONF_UNSET_UINT;
    conf->range_var_index = NGX_CONF_UNSET_UINT;
    return conf;
}


static char *
ngx_http_nphase_merge_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_http_nphase_conf_t *prev = parent;
    ngx_http_nphase_conf_t *conf = child;

    ngx_conf_merge_str_value(conf->uri, prev->uri, "");
    ngx_conf_merge_value(conf->uri_var_index, prev->uri_var_index, -1);
    ngx_conf_merge_value(conf->range_var_index, prev->range_var_index, -1);

    return NGX_CONF_OK;
}


static ngx_int_t
ngx_http_nphase_init(ngx_conf_t *cf)
{
    ngx_http_handler_pt        *h;
    ngx_http_core_main_conf_t  *cmcf;

    cmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_core_module);
    if (cmcf == NULL) {
        return NGX_ERROR;
    }

    h = ngx_array_push(&cmcf->phases[NGX_HTTP_ACCESS_PHASE].handlers);
    if (h == NULL) {
        return NGX_ERROR;
    }

    *h = ngx_http_nphase_access_handler;

    ngx_http_nphase_filter_init(cf);
    return NGX_OK;
}


static ngx_int_t
ngx_http_nphase_access_handler(ngx_http_request_t *r)
{
    ngx_http_nphase_ctx_t             *ctx;
    ngx_http_nphase_conf_t            *npcf;
    ngx_http_variable_value_t         *var;
    ngx_int_t                       rc;
    ngx_http_nphase_range_t         *rin;

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "nphase access handler");

    npcf = ngx_http_get_module_loc_conf(r, ngx_http_nphase_module);

    if (npcf->uri.len == 0) {
        return NGX_DECLINED;
    }

    ctx = ngx_http_get_module_ctx(r, ngx_http_nphase_module);

    if (ctx != NULL) {
        if (ctx->sr_count_e >= NGX_HTTP_NPHASE_MAX_RETRY) {
            ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                          "nphase subrequest max retry num(%d) reached",
                          NGX_HTTP_NPHASE_MAX_RETRY);
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }
        
        /* phase 2 process */
        if (ctx->body_ready == 1) {
            if (ctx->sr_done == 0) {
                return NGX_AGAIN;
            }
            
            if (ctx->range_in.nelts > 1) {
                return NGX_HTTP_RANGE_NOT_SATISFIABLE;
            }

            rin = ctx->range_in.elts;
            
            ngx_log_debug8(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "nphase rin s:%O e:%O f:%d, sent s:%O e:%O, w:%O, c:%d, ce:%d",
                   rin->start, rin->end, rin->flag, 
                   ctx->range_sent.start, ctx->range_sent.end,
                   ctx->wfsz, ctx->sr_count, ctx->sr_count_e);
            
            /* todo: compare ctx->wfsz and range_sent to find out range need send */

            if ((rin->flag == -1 && ctx->range_sent.end < ctx->wfsz) 
                || (ctx->range_sent.end < rin->end - rin->start + 1))
            {
                ctx->loc_ready = 0;
                ctx->body_ready = 0;

                if (ngx_http_nphase_range_update(r, npcf->range_var_index, 
                        rin->start + ctx->range_sent.end, 
                        rin->end ? rin->end : ctx->wfsz, 0)
                    != NGX_OK) 
                {
                    return NGX_HTTP_INTERNAL_SERVER_ERROR;
                }
                
                /* restore next phase subrequest uri to phase 1 uri */
                var = ngx_http_get_indexed_variable(r, npcf->uri_var_index);
                if (var == NULL) {
                    return NGX_HTTP_INTERNAL_SERVER_ERROR;
                }
                var->data = ctx->uri_var_value.data;
                var->len  = ctx->uri_var_value.len;
                
                if (ngx_http_nphase_run_subrequest(r, ctx, &npcf->uri, NULL) 
                        != NGX_OK) 
                {
                    return NGX_HTTP_INTERNAL_SERVER_ERROR;
                }
                return NGX_AGAIN;
            }
            return NGX_OK;
        }

        /* phase 1 process */
        if (ctx->loc_ready == 1) {
            ctx->loc_ready = 0;
            
            /* run subrequest by loc_body_c */
            if (ctx->loc_body_c.len == 0) {
                return NGX_HTTP_INTERNAL_SERVER_ERROR;
            }

            if (ctx->loc_body_c.data[0] == '/') {
                return NGX_HTTP_INTERNAL_SERVER_ERROR;
            }
            
            var = ngx_http_get_indexed_variable(r, npcf->uri_var_index);
            if (var == NULL) {
                return NGX_HTTP_INTERNAL_SERVER_ERROR;
            }
            var->len = ctx->loc_body_c.len;
            var->data = ctx->loc_body_c.data;
            
            rin = ctx->range_in.elts;
/*
if (ngx_http_nphase_range_update(r, npcf->range_var_index, 
                    rin->start, rin->end, rin->flag)
                != NGX_OK) 
                */
            if (ngx_http_nphase_range_update(r, npcf->range_var_index, 
                    rin->start + ctx->range_sent.end, 
                    rin->end ? rin->end : ctx->wfsz, 0)
                != NGX_OK) 
            {
                return NGX_HTTP_INTERNAL_SERVER_ERROR;
            }

            if (ngx_http_nphase_run_subrequest(r, ctx, &npcf->uri, NULL) 
                    != NGX_OK) 
            {
                return NGX_HTTP_INTERNAL_SERVER_ERROR;
            }

            return NGX_AGAIN;
        }
        
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "nphase location uri and body both not ready");
        return NGX_HTTP_INTERNAL_SERVER_ERROR;    
    }

    /* initial module ctx */
    ctx = ngx_pcalloc(r->pool, sizeof(ngx_http_nphase_ctx_t));
    if (ctx == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    /* parse headers_in range to ctx->range_in */
    if (r->headers_in.range != NULL) {
        if (r->headers_in.range->value.len >= 7
            && ngx_strncasecmp(r->headers_in.range->value.data,
                           (u_char *) "bytes=", 6)
               == 0) 
        {
            if (ngx_array_init(&ctx->range_in, r->pool, 1, 
                            sizeof(ngx_http_nphase_range_t))
                != NGX_OK)
            {
                return NGX_HTTP_INTERNAL_SERVER_ERROR;
            }

            rc = ngx_http_nphase_range_parse(r, ctx);
            
            if (rc == NGX_OK) {
                if (ctx->range_in.nelts > 1) {
                    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                                  "multipart range request not supported");
                    return NGX_HTTP_INTERNAL_SERVER_ERROR;
                }
                r->allow_ranges = 1;
            } else {
                return NGX_HTTP_RANGE_NOT_SATISFIABLE;
            }
        } else {
            return NGX_HTTP_RANGE_NOT_SATISFIABLE;
        }
    } else {
        if (ngx_array_init(&ctx->range_in, r->pool, 1, 
                        sizeof(ngx_http_nphase_range_t))
            != NGX_OK)
        {
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }
        
        ngx_http_nphase_range_t  *range;

        range = ngx_array_push(&ctx->range_in);
        if (range == NULL) {
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }

        range->start = 0;
        range->end   = 0;
        range->flag  = -1;
    }
    
    rin = ctx->range_in.elts;
    if (ngx_http_nphase_range_update(r, npcf->range_var_index, 
            rin->start, rin->end, rin->flag)
        != NGX_OK) 
    {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    /* run a subrequest to nphase_uri */
    if (ngx_http_nphase_run_subrequest(r, ctx, &npcf->uri, NULL) 
            != NGX_OK) 
    {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    ngx_http_set_ctx(r, ctx, ngx_http_nphase_module);

    /* save phase 1 uri to ctx->uri_var_value */
    var = ngx_http_get_indexed_variable(r, npcf->uri_var_index);
    if (var == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }
    ctx->uri_var_value.data = var->data;
    ctx->uri_var_value.len  = var->len;
    
    return NGX_AGAIN;
}

static ngx_int_t
ngx_http_nphase_content_handler(ngx_http_request_t *r)
{
    ngx_http_nphase_ctx_t   *ctx;
    
    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "nphase content handler");

    ctx = ngx_http_get_module_ctx(r, ngx_http_nphase_module);

    /* Entering content phase means valid response has been 
            received by subrequest. */
    
    if (ctx == NULL) {
        return NGX_DECLINED;
    }

    if (! ctx->header_sent) {
        if (ngx_http_send_header(r) == NGX_ERROR) {
            return NGX_ERROR;
        }
    }

    /* send out buf in case of not sending by subrequest */
    if (r->out && r->out->buf && r->out->buf->pos) {
        return ngx_http_output_filter(r, NULL);
    }
    
    return NGX_OK;
}

static ngx_int_t
ngx_http_nphase_subrequest_done(ngx_http_request_t *r, void *data, ngx_int_t rc)
{
    
    off_t         size = 0;
    ngx_chain_t   *ln;
    ngx_http_nphase_ctx_t       *ctx = data;   /* parent ctx */
    ngx_http_nphase_sub_ctx_t   *sr_ctx;

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "nphase subrequest done s:%d", r->headers_out.status);

    sr_ctx = ngx_http_get_module_ctx(r, ngx_http_nphase_module);
    if (!sr_ctx) {
        return rc;
    }

    /* todo: update ctx->range_sent */
    for (ln = r->parent->out; ln; ln = ln->next) {
        size += ngx_buf_size(ln->buf);
    }
    
    ctx->range_sent.end = 
        r->parent->connection->sent + size - r->parent->header_size;
        
    if (ctx->range_sent.end < 0) {
        ctx->range_sent.end = 0;
    }

    ngx_log_debug6(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "nphase range sent offset:%O", ctx->range_sent.end);

    if (r->headers_out.status >= NGX_HTTP_SPECIAL_RESPONSE 
        && r->headers_out.status != NGX_HTTP_MOVED_TEMPORARILY ) 
    {
        ctx->sr_error = 1;
        ctx->sr_count_e++;
    }

    // todo: .....
    if ((r->headers_out.status == NGX_HTTP_OK
        || r->headers_out.status == NGX_HTTP_PARTIAL_CONTENT)
        && ctx->range_sent.end == 0) 
    {
        /* backend return 200 or 206 without body */
        ctx->sr_error = 1;
        ctx->sr_count_e++;
    }
    
    ctx->sr_done = 1;
    return rc;
}

ngx_int_t
ngx_http_nphase_filter_init(ngx_conf_t *cf)
{
    ngx_http_next_header_filter = ngx_http_top_header_filter;
    ngx_http_top_header_filter = ngx_http_nphase_header_filter;

    ngx_http_next_body_filter = ngx_http_top_body_filter;
    ngx_http_top_body_filter = ngx_http_nphase_body_filter;

    return NGX_OK;
}


static ngx_int_t
ngx_http_nphase_header_filter(ngx_http_request_t *r)
{
    ngx_http_nphase_ctx_t                   *pr_ctx;
    ngx_http_nphase_sub_ctx_t               *sr_ctx;

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
               "nphase header filter status:%d", 
                   r->headers_out.status);
                   
    if (r == r->main) {
        /* parent request */
        pr_ctx = ngx_http_get_module_ctx(r, ngx_http_nphase_module);

        if (! pr_ctx) {
            return ngx_http_next_header_filter(r);
        }
        
        if (pr_ctx->header_sent) {
            return NGX_OK;
        }
        
        pr_ctx->header_sent = 1;

        if (pr_ctx->pr_status != 0) {
            r->headers_out.status = pr_ctx->pr_status;
        }
        
        if (r->headers_out.status == NGX_HTTP_OK) {
            r->headers_out.content_length_n = pr_ctx->wfsz;
        }
        
        if (r->headers_out.status == NGX_HTTP_PARTIAL_CONTENT) {
            r->headers_out.content_length_n = pr_ctx->wfsz;

            ngx_http_nphase_add_range_singlepart_header(r, pr_ctx);
        }
        
        return ngx_http_next_header_filter(r);
    }else{
        /* sub request */
        sr_ctx = ngx_http_get_module_ctx(r, ngx_http_nphase_module);
        
        if (! sr_ctx) {
            return ngx_http_next_header_filter(r);
        }

        if (! r->parent) {
            return ngx_http_next_header_filter(r);
        }
        
        pr_ctx = ngx_http_get_module_ctx(r->parent, ngx_http_nphase_module);
        
        if (! pr_ctx) {
            return ngx_http_next_header_filter(r);
        }

        if (pr_ctx->body_ready) {
            return NGX_OK;
        }

        if (r->headers_out.status >= NGX_HTTP_SPECIAL_RESPONSE 
            && r->headers_out.status != NGX_HTTP_MOVED_TEMPORARILY ) {
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }

        if (r->headers_out.status == NGX_HTTP_MOVED_TEMPORARILY ) {
            if (ngx_http_nphase_process_header(r, pr_ctx) == NGX_ERROR
                    && pr_ctx->wfsz == 0) {
                return NGX_HTTP_INTERNAL_SERVER_ERROR;
            }
            
            if (! r->headers_out.location) {
                u_char              *p;
                size_t              len = 0;
                
                ngx_str_t           val;
                ngx_str_t           key = ngx_string("Location");

                if (ngx_http_nphase_copy_header_value(
                        &r->headers_out.headers, &key, &val) == NGX_OK) 
                {
                    if ( val.data[0] == '/' ) {
                        len += r->upstream->schema.len;
                        len += r->upstream->resolved->host.len;
                        if (len == 0) {
                            return NGX_HTTP_INTERNAL_SERVER_ERROR;
                        }
                    }

                    len += val.len;
                    p = ngx_palloc(r->pool, len);
                    if (p == NULL) {
                        return NGX_ERROR;
                    }
                    pr_ctx->loc_body_c.data = p;
                    pr_ctx->loc_body_c.len = len;
                                    
                    if ( val.data[0] == '/' ) {
                        p = ngx_copy(p, r->upstream->schema.data, 
                                        r->upstream->schema.len);
                        p = ngx_copy(p, r->upstream->resolved->host.data, 
                                        r->upstream->resolved->host.len);
                    }
                    
                    p = ngx_copy(p, val.data, val.len);
                    
                    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                                    "nphase get next phase loc: %V", 
                                    &pr_ctx->loc_body_c);
                    
                    pr_ctx->loc_ready = 1;
                    return NGX_OK;                
                }

                ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "nphase get 302 from upstream without location string");
                return NGX_ERROR;
            }
            
            pr_ctx->loc_body_c.data = ngx_palloc(r->pool, 
                                        r->headers_out.location->value.len);
            if (pr_ctx->loc_body_c.data == NULL) {
                return NGX_ERROR;
            }
                            
            ngx_memcpy(pr_ctx->loc_body_c.data, 
                        r->headers_out.location->value.data,
                        r->headers_out.location->value.len);
            pr_ctx->loc_body_c.len = r->headers_out.location->value.len;
            
            ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                            "nphase get next phase loc: %V", 
                            &pr_ctx->loc_body_c);
            
            pr_ctx->loc_ready = 1;
            return NGX_OK;
        }

        /* upstream return 20x */
        pr_ctx->body_ready = 1;
        return NGX_OK;
    }
    
    return ngx_http_next_header_filter(r);
}


static ngx_int_t
ngx_http_nphase_body_filter(ngx_http_request_t *r, ngx_chain_t *in)
{
    ngx_http_nphase_sub_ctx_t       *sr_ctx;
    ngx_http_nphase_ctx_t           *pr_ctx;
    
    ngx_log_debug2(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "nphase body filter status:%d", 
                   r->headers_out.status);
    
    if (r == r->main) {
        /* parent request */
        pr_ctx = ngx_http_get_module_ctx(r, ngx_http_nphase_module);

        if (! pr_ctx) {
            return ngx_http_next_body_filter(r, in);
        }
        
        if (pr_ctx->sr_error 
            && r->headers_out.status < NGX_HTTP_SPECIAL_RESPONSE) 
        {
            return ngx_http_next_body_filter(r, NULL);
        }
        
        return ngx_http_next_body_filter(r, in);
    }else{
        sr_ctx = ngx_http_get_module_ctx(r, ngx_http_nphase_module);
        
        if (!sr_ctx) {
            return ngx_http_next_body_filter(r, in);
        }
    
        if (! r->parent) {
            return ngx_http_next_body_filter(r, in);
        }
        
        pr_ctx = ngx_http_get_module_ctx(r->parent, ngx_http_nphase_module);
        
        if (! pr_ctx) {
            return ngx_http_next_body_filter(r, in);
        }

        if (! pr_ctx->body_ready) {
            return NGX_OK;
        }
        
        if (! pr_ctx->header_sent){
            pr_ctx->pr_status = r->headers_out.status;

            if (r->parent->headers_out.content_length_n == -1) {
                r->parent->headers_out.content_length_n = 
                    r->headers_out.content_length_n;
            }
            
            if (ngx_http_send_header(r->parent) == NGX_ERROR) {
                return NGX_ERROR;
            }
        }

        return ngx_http_output_filter(r->parent, in);        
    }
}

void
ngx_http_nphase_discard_bufs(ngx_pool_t *pool, ngx_chain_t *in)
{
    ngx_chain_t         *cl;

    for (cl = in; cl; cl = cl->next) {
        cl->buf->pos = cl->buf->last;
        cl->buf->file_pos = cl->buf->file_last;
    }
}

static char *
ngx_http_nphase_uri(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_nphase_conf_t      *npcf = conf;
    ngx_http_core_loc_conf_t    *clcf;
    ngx_str_t        *value;

    if (npcf->uri.data != NULL) {
        return "is duplicate";
    }

    value = cf->args->elts;

    if (ngx_strcmp(value[1].data, "off") == 0) {
        npcf->uri.len = 0;
        npcf->uri.data = (u_char *) "";

        return NGX_CONF_OK;
    }

    npcf->uri = value[1];

    /* register content phase handler */
    clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);
    if (clcf == NULL) {
        return NGX_CONF_ERROR;
    }
    
    clcf->handler = ngx_http_nphase_content_handler;

    return NGX_CONF_OK;
}

static char *
ngx_http_nphase_set_uri_var(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_nphase_conf_t      *npcf = conf;
    ngx_str_t                   *value;
    
    value = cf->args->elts;

    if (value[1].data[0] != '$') {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "invalid variable name \"%V\"", &value[1]);
        return NGX_CONF_ERROR;
    }

    value[1].len--;
    value[1].data++;

    npcf->uri_var_index = ngx_http_get_variable_index(cf, &value[1]);
    if (npcf->uri_var_index == NGX_ERROR) {
        return NGX_CONF_ERROR;
    }

    return NGX_CONF_OK;
}


static char *
ngx_http_nphase_set_range_var(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_nphase_conf_t      *npcf = conf;
    ngx_str_t                   *value;
    
    value = cf->args->elts;

    if (value[1].data[0] != '$') {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "invalid variable name \"%V\"", &value[1]);
        return NGX_CONF_ERROR;
    }

    value[1].len--;
    value[1].data++;

    npcf->range_var_index = ngx_http_get_variable_index(cf, &value[1]);
    if (npcf->range_var_index == NGX_ERROR) {
        return NGX_CONF_ERROR;
    }

    return NGX_CONF_OK;
}

ngx_int_t
ngx_http_nphase_range_update(ngx_http_request_t *r, ngx_int_t index, 
                                            off_t start, off_t end, ngx_int_t flag)
{
    ngx_http_variable_value_t         *var;
    
    var = ngx_http_get_indexed_variable(r, index);
    if (var == NULL) {
        return NGX_ERROR;
    }
                    
    var->data = ngx_pnalloc(r->pool, 
                    sizeof("bytes=-") + 2 * NGX_OFF_T_LEN);
    if (var->data == NULL) {
        return NGX_ERROR;
    }

    if (flag == 0) {
        var->len = ngx_sprintf(var->data, "bytes=%O-%O", start, end)
                        - var->data;
    } else if (flag == 1) {
        var->len = ngx_sprintf(var->data, "bytes=%O-", start)
                        - var->data;
    } else if (flag == 2) {
        var->len = ngx_sprintf(var->data, "bytes=-%O", end)
                        - var->data;
    } else if (flag == -1) {
        var->len = ngx_sprintf(var->data, "bytes=0-")
                        - var->data;
    } else {
        return NGX_ERROR;
    }
    
    return NGX_OK;
}

ngx_int_t
ngx_http_nphase_range_parse(ngx_http_request_t *r, ngx_http_nphase_ctx_t *ctx)
{
    u_char            *p;
    off_t              start, end;
    ngx_int_t          flag;
    ngx_uint_t         suffix;
    ngx_http_nphase_range_t  *range;

    p = r->headers_in.range->value.data + 6;

    for ( ;; ) {
        start = 0;
        end = 0;
        suffix = 0;
        flag = 0;

        while (*p == ' ') { p++; }

        if (*p != '-') {
            if (*p < '0' || *p > '9') {
                return NGX_HTTP_RANGE_NOT_SATISFIABLE;
            }

            while (*p >= '0' && *p <= '9') {
                start = start * 10 + *p++ - '0';
            }

            while (*p == ' ') { p++; }

            if (*p++ != '-') {
                return NGX_HTTP_RANGE_NOT_SATISFIABLE;
            }

            while (*p == ' ') { p++; }

            if (*p == ',' || *p == '\0') {
                range = ngx_array_push(&ctx->range_in);
                if (range == NULL) {
                    return NGX_ERROR;
                }

                range->start = start;
                range->end = end;
                range->flag = 1;

                if (*p++ != ',') {
                    return NGX_OK;
                }

                continue;
            }

        } else {
            suffix = 1;
            p++;
        }

        if (*p < '0' || *p > '9') {
            return NGX_HTTP_RANGE_NOT_SATISFIABLE;
        }

        while (*p >= '0' && *p <= '9') {
            end = end * 10 + *p++ - '0';
        }

        while (*p == ' ') { p++; }

        if (*p != ',' && *p != '\0') {
            return NGX_HTTP_RANGE_NOT_SATISFIABLE;
        }

        if (suffix) {
            flag = 2;
        }

        if (start > end && flag != 1) {
            return NGX_HTTP_RANGE_NOT_SATISFIABLE;
        }

        range = ngx_array_push(&ctx->range_in);
        if (range == NULL) {
            return NGX_ERROR;
        }

        range->start = start;
        range->end = end;
        range->flag = flag;

        if (*p++ != ',') {
            return NGX_OK;
        }
    }
}


ngx_int_t
ngx_http_nphase_content_range_parse(u_char *p, ngx_http_nphase_range_t *range)
{
    off_t              start, end, length;
    ngx_uint_t         suffix;

    start = 0;
    end = 0;
    length = 0;
    suffix = 0;

    while (*p == ' ') { p++; }

    if (*p != '-') {
        if (*p < '0' || *p > '9') {
            return NGX_HTTP_RANGE_NOT_SATISFIABLE;
        }

        while (*p >= '0' && *p <= '9') {
            start = start * 10 + *p++ - '0';
        }

        while (*p == ' ') { p++; }

        if (*p++ != '-') {
            return NGX_HTTP_RANGE_NOT_SATISFIABLE;
        }

        while (*p == ' ') { p++; }

        if (*p == ',' || *p == '\0') {
            return NGX_HTTP_RANGE_NOT_SATISFIABLE;
        }

    } else {
        suffix = 1;
        p++;
    }

    if (*p < '0' || *p > '9') {
        return NGX_HTTP_RANGE_NOT_SATISFIABLE;
    }

    while (*p >= '0' && *p <= '9') {
        end = end * 10 + *p++ - '0';
    }
    if (start > end) {
        return NGX_HTTP_RANGE_NOT_SATISFIABLE;
    }
    if (!suffix) {
       range->start = start;
    }
    range->end = end;

    while (*p == ' ') { p++; }

    if (*p != '/') {
        return NGX_HTTP_RANGE_NOT_SATISFIABLE;
    }
    p++;
    
    if (*p < '0' || *p > '9') {
        return NGX_HTTP_RANGE_NOT_SATISFIABLE;
    }
    while (*p >= '0' && *p <= '9') {
        length = length * 10 + *p++ - '0';
    }
    range->length = length;
    if (*p++ != ',') {
        return NGX_OK;
    }
    return NGX_HTTP_RANGE_NOT_SATISFIABLE;
}


ngx_int_t
ngx_http_nphase_copy_header_value(ngx_list_t *headers, ngx_str_t *k, ngx_str_t *v)
{
    ngx_uint_t          n;
    ngx_table_elt_t     *ho;
    u_char              *p;
    size_t              len = 0;

    for (n = 0; n < headers->part.nelts; n++) {
        ho = &((ngx_table_elt_t *)headers->part.elts)[n];
        if (ngx_strncmp(ho->key.data, k->data, k->len) == 0) {

            len += ho->value.len;
            p = ngx_palloc(headers->pool, len);
            if (p == NULL) {
                return NGX_ERROR;
            }
            
            v->data = p;
            v->len = len;
            p = ngx_copy(p, ho->value.data, ho->value.len);
            p = 0;
            
            return NGX_OK;
        }
    }

    return NGX_ERROR;
}


ngx_int_t
ngx_http_nphase_run_subrequest(ngx_http_request_t *r, 
                                            ngx_http_nphase_ctx_t *ctx,
                                            ngx_str_t *uri,
                                            ngx_str_t *args)
{
    ngx_http_post_subrequest_t      *ps;
    ngx_http_nphase_sub_ctx_t       *sr_ctx;
    ngx_http_request_t              *sr;

    ps = ngx_palloc(r->pool, sizeof(ngx_http_post_subrequest_t));
    if (ps == NULL) {
        return NGX_ERROR;
    }

    ps->handler = ngx_http_nphase_subrequest_done;
    ps->data = ctx;
    ctx->sr_done = 0;

    if (ngx_http_subrequest(r, uri, args, &sr, ps,
                            NGX_HTTP_SUBREQUEST_WAITED)
        != NGX_OK)
    {
        return NGX_ERROR;
    }

    sr_ctx = ngx_pcalloc(r->pool, sizeof(ngx_http_nphase_sub_ctx_t));
    if (sr_ctx == NULL) {
        return NGX_ERROR;
    }
    ngx_http_set_ctx(sr, sr_ctx, ngx_http_nphase_module);

    ctx->sr_count++;
    return NGX_OK;
}

ngx_int_t
ngx_http_nphase_process_header(ngx_http_request_t *r, 
                                            ngx_http_nphase_ctx_t *ctx)
{
    off_t               fsz;
    u_char              *p;
    size_t              len;
    ngx_str_t           val;
    ngx_str_t           key = ngx_string("X-NP-File-Size");
    
    if (ngx_http_nphase_copy_header_value(
            &r->headers_out.headers, &key, &val) == NGX_OK) 
    {
        fsz = 0;
        len = val.len;
        p = val.data;
        for ( ;; ) {

            while (*p == ' ') { 
                if (fsz != 0) {
                    return NGX_ERROR;
                }
                p++; 
                len--; 
            }

            if (*p < '0' || *p > '9') {
                return NGX_ERROR;
            }
            
            fsz = fsz * 10 + *p++ - '0';
            len--;
            if (len == 0) {
                ctx->wfsz = fsz;
                break;
            }
        }
    }
    return NGX_OK;
}


static ngx_int_t
ngx_http_nphase_add_range_singlepart_header(ngx_http_request_t *r,
    ngx_http_nphase_ctx_t *ctx)
{
    ngx_table_elt_t   *content_range;
    ngx_http_nphase_range_t         *rin;
    ngx_http_nphase_range_t         range;

    rin = ctx->range_in.elts;
    if (rin->flag == -1) {
        return NGX_OK;
    }

    content_range = ngx_list_push(&r->headers_out.headers);
    if (content_range == NULL) {
        return NGX_ERROR;
    }

    r->headers_out.content_range = content_range;

    content_range->hash = 1;
    ngx_str_set(&content_range->key, "Content-Range");

    content_range->value.data = ngx_pnalloc(r->pool,
                                    sizeof("bytes -/") - 1 + 3 * NGX_OFF_T_LEN);
    if (content_range->value.data == NULL) {
        return NGX_ERROR;
    }

    /* "Content-Range: bytes SSSS-EEEE/TTTT" header */

    if (rin->flag == 0) {
        range.start = rin->start;
        range.end = rin->end;
    } else if (rin->flag == 1) {
        range.start = rin->start;
        range.end = r->headers_out.content_length_n - 1;
    } else if (rin->flag == 2) {
        range.start = r->headers_out.content_length_n - rin->end;
        range.end = r->headers_out.content_length_n - 1;
    } else {
        return NGX_ERROR;
    }

    content_range->value.len = ngx_sprintf(content_range->value.data,
                                           "bytes %O-%O/%O",
                                           range.start, range.end,
                                           r->headers_out.content_length_n)
                               - content_range->value.data;
    
    r->headers_out.content_length_n = range.end - range.start + 1;

    if (r->headers_out.content_length) {
        r->headers_out.content_length->hash = 0;
        r->headers_out.content_length = NULL;
    }

    return NGX_OK;
}

