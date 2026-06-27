#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>


typedef struct {
    ngx_array_t  *links;          /* array of ngx_http_complex_value_t */
} ngx_http_early_hints_loc_conf_t;


typedef struct {
    unsigned      sent:1;
} ngx_http_early_hints_ctx_t;


static ngx_int_t ngx_http_early_hints_handler(ngx_http_request_t *r);
static ngx_int_t ngx_http_early_hints_is_document(ngx_http_request_t *r);
static ngx_http_early_hints_ctx_t *ngx_http_early_hints_get_ctx(
    ngx_http_request_t *r);
static void ngx_http_early_hints_cleanup(void *data);
static void *ngx_http_early_hints_create_loc_conf(ngx_conf_t *cf);
static char *ngx_http_early_hints_merge_loc_conf(ngx_conf_t *cf, void *parent,
    void *child);
static char *ngx_http_early_hints_link(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static ngx_int_t ngx_http_early_hints_init(ngx_conf_t *cf);


static ngx_command_t  ngx_http_early_hints_commands[] = {

    { ngx_string("early_hints_link"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_http_early_hints_link,
      NGX_HTTP_LOC_CONF_OFFSET,
      0,
      NULL },

      ngx_null_command
};


static ngx_http_module_t  ngx_http_early_hints_module_ctx = {
    NULL,                                  /* preconfiguration */
    ngx_http_early_hints_init,             /* postconfiguration */

    NULL,                                  /* create main configuration */
    NULL,                                  /* init main configuration */

    NULL,                                  /* create server configuration */
    NULL,                                  /* merge server configuration */

    ngx_http_early_hints_create_loc_conf,  /* create location configuration */
    ngx_http_early_hints_merge_loc_conf    /* merge location configuration */
};


ngx_module_t  ngx_http_early_hints_module = {
    NGX_MODULE_V1,
    &ngx_http_early_hints_module_ctx,      /* module context */
    ngx_http_early_hints_commands,         /* module directives */
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


static ngx_int_t
ngx_http_early_hints_handler(ngx_http_request_t *r)
{
    ngx_str_t                         val;
    ngx_uint_t                        i, nelts;
    ngx_table_elt_t                  *h;
    ngx_list_part_t                  *part;
    ngx_http_complex_value_t         *cv;
    ngx_http_early_hints_ctx_t       *ctx;
    ngx_http_early_hints_loc_conf_t  *ehcf;

    if (r != r->main) {
        return NGX_DECLINED;
    }

    ehcf = ngx_http_get_module_loc_conf(r, ngx_http_early_hints_module);

    if (ehcf->links == NULL) {
        return NGX_DECLINED;
    }

    if (r->http_version < NGX_HTTP_VERSION_11) {
        return NGX_DECLINED;
    }

    if (r->header_sent) {
        return NGX_DECLINED;
    }

    // Deduplicate when doing  internal redirects, try_files, etc.
    ctx = ngx_http_early_hints_get_ctx(r);

    if (ctx && ctx->sent) {
        return NGX_DECLINED;
    }

    // Browsers killed HTTP/2 Server Push beacuse webservers were misconfigured
    // and sent it wastefully so much. Lets try not to wind up there again.
    if (!ngx_http_early_hints_is_document(r)) {
        return NGX_DECLINED;
    }

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "early hints handler");

    /*
     * Remember the tail of the output headers list so the hint headers
     * can be removed after the 103 response is serialized.  This keeps
     * the generated "Link" headers in the early hints response only,
     * matching the behavior of upstream-originated early hints.
     */

    part = r->headers_out.headers.last;
    nelts = part->nelts;

    cv = ehcf->links->elts;

    for (i = 0; i < ehcf->links->nelts; i++) {

        if (ngx_http_complex_value(r, &cv[i], &val) != NGX_OK) {
            return NGX_ERROR;
        }

        if (val.len == 0) {
            continue;
        }

        h = ngx_list_push(&r->headers_out.headers);
        if (h == NULL) {
            return NGX_ERROR;
        }

        h->hash = 1;
        h->next = NULL;
        ngx_str_set(&h->key, "Link");
        h->value = val;
    }

    if (part->next == NULL && part->nelts == nelts) {
        /* nothing was added */
        return NGX_DECLINED;
    }

    if (ngx_http_send_early_hints(r) == NGX_ERROR) {
        return NGX_ERROR;
    }

    /* drop the hint headers, leaving them in the 103 response only */
    part->nelts = nelts;
    part->next = NULL;
    r->headers_out.headers.last = part;

    if (ctx == NULL) {
        ctx = ngx_http_early_hints_get_ctx(r);
    }

    if (ctx) {
        ctx->sent = 1;
    }

    return NGX_DECLINED;
}


static ngx_int_t
ngx_http_early_hints_is_document(ngx_http_request_t *r)
{
    ngx_uint_t        i;
    ngx_list_part_t  *part;
    ngx_table_elt_t  *header;

    static ngx_str_t  name = ngx_string("Sec-Fetch-Dest");

    part = &r->headers_in.headers.part;
    header = part->elts;

    for (i = 0; /* void */; i++) {

        if (i >= part->nelts) {
            if (part->next == NULL) {
                break;
            }

            part = part->next;
            header = part->elts;
            i = 0;
        }

        if (header[i].hash == 0) {
            continue;
        }

        if (header[i].key.len != name.len
            || ngx_strncasecmp(header[i].key.data, name.data, name.len) != 0)
        {
            continue;
        }

        return header[i].value.len == sizeof("document") - 1
               && ngx_strncmp(header[i].value.data, "document",
                              sizeof("document") - 1) == 0;
    }

    return 0;
}


// Copied from realip module for tracking state accross internal redirects
static ngx_http_early_hints_ctx_t *
ngx_http_early_hints_get_ctx(ngx_http_request_t *r)
{
    ngx_pool_cleanup_t          *cln;
    ngx_http_early_hints_ctx_t  *ctx;

    ctx = ngx_http_get_module_ctx(r, ngx_http_early_hints_module);

    if (ctx == NULL && r->internal) {

        for (cln = r->pool->cleanup; cln; cln = cln->next) {
            if (cln->handler == ngx_http_early_hints_cleanup) {
                ctx = cln->data;
                ngx_http_set_ctx(r, ctx, ngx_http_early_hints_module);
                break;
            }
        }
    }

    if (ctx == NULL) {

        cln = ngx_pool_cleanup_add(r->pool,
                                   sizeof(ngx_http_early_hints_ctx_t));
        if (cln == NULL) {
            return NULL;
        }

        cln->handler = ngx_http_early_hints_cleanup;

        ctx = cln->data;
        ctx->sent = 0;

        ngx_http_set_ctx(r, ctx, ngx_http_early_hints_module);
    }

    return ctx;
}


static void
ngx_http_early_hints_cleanup(void *data)
{
    /* the context is only used as a marker, nothing to clean up */
}


static void *
ngx_http_early_hints_create_loc_conf(ngx_conf_t *cf)
{
    ngx_http_early_hints_loc_conf_t  *ehcf;

    ehcf = ngx_pcalloc(cf->pool, sizeof(ngx_http_early_hints_loc_conf_t));
    if (ehcf == NULL) {
        return NULL;
    }

    ehcf->links = NGX_CONF_UNSET_PTR;

    return ehcf;
}


static char *
ngx_http_early_hints_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_http_early_hints_loc_conf_t *prev = parent;
    ngx_http_early_hints_loc_conf_t *conf = child;

    ngx_conf_merge_ptr_value(conf->links, prev->links, NULL);

    return NGX_CONF_OK;
}


static char *
ngx_http_early_hints_link(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_early_hints_loc_conf_t *ehcf = conf;

    ngx_str_t                         *value;
    ngx_http_complex_value_t          *cv;
    ngx_http_compile_complex_value_t   ccv;

    value = cf->args->elts;

    if (ehcf->links == NGX_CONF_UNSET_PTR) {
        ehcf->links = ngx_array_create(cf->pool, 2,
                                       sizeof(ngx_http_complex_value_t));
        if (ehcf->links == NULL) {
            return NGX_CONF_ERROR;
        }
    }

    cv = ngx_array_push(ehcf->links);
    if (cv == NULL) {
        return NGX_CONF_ERROR;
    }

    ngx_memzero(&ccv, sizeof(ngx_http_compile_complex_value_t));

    ccv.cf = cf;
    ccv.value = &value[1];
    ccv.complex_value = cv;

    if (ngx_http_compile_complex_value(&ccv) != NGX_OK) {
        return NGX_CONF_ERROR;
    }

    return NGX_CONF_OK;
}


static ngx_int_t
ngx_http_early_hints_init(ngx_conf_t *cf)
{
    ngx_http_handler_pt        *h;
    ngx_http_core_main_conf_t  *cmcf;

    cmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_core_module);

    // As early as possible, but after NGX_HTTP_ACCESS_PHASE to avoid leaks
    h = ngx_array_push(&cmcf->phases[NGX_HTTP_PRECONTENT_PHASE].handlers);
    if (h == NULL) {
        return NGX_ERROR;
    }

    *h = ngx_http_early_hints_handler;

    return NGX_OK;
}
