/*
   MOD AUTH GSSAPI

   Copyright (C) 2014 Simo Sorce <simo@redhat.com>

   Permission is hereby granted, free of charge, to any person obtaining a
   copy of this software and associated documentation files (the "Software"),
   to deal in the Software without restriction, including without limitation
   the rights to use, copy, modify, merge, publish, distribute, sublicense,
   and/or sell copies of the Software, and to permit persons to whom the
   Software is furnished to do so, subject to the following conditions:

   The above copyright notice and this permission notice shall be included in
   all copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
   THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
   FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
   DEALINGS IN THE SOFTWARE.
*/

#include "mod_auth_gssapi.h"

module AP_MODULE_DECLARE_DATA auth_gssapi_module;

APR_DECLARE_OPTIONAL_FN(int, ssl_is_https, (conn_rec *));

static char *mag_status(request_rec *req, int type, uint32_t err)
{
    uint32_t maj_ret, min_ret;
    gss_buffer_desc text;
    uint32_t msg_ctx;
    char *msg_ret;
    int len;

    msg_ret = NULL;
    msg_ctx = 0;
    do {
        maj_ret = gss_display_status(&min_ret, err, type,
                                     GSS_C_NO_OID, &msg_ctx, &text);
        if (maj_ret != GSS_S_COMPLETE) {
            return msg_ret;
        }

        len = text.length;
        if (msg_ret) {
            msg_ret = apr_psprintf(req->pool, "%s, %*s",
                                   msg_ret, len, (char *)text.value);
        } else {
            msg_ret = apr_psprintf(req->pool, "%*s", len, (char *)text.value);
        }
        gss_release_buffer(&min_ret, &text);
    } while (msg_ctx != 0);

    return msg_ret;
}

static char *mag_error(request_rec *req, const char *msg,
                       uint32_t maj, uint32_t min)
{
    char *msg_maj;
    char *msg_min;

    msg_maj = mag_status(req, GSS_C_GSS_CODE, maj);
    msg_min = mag_status(req, GSS_C_MECH_CODE, min);
    return apr_psprintf(req->pool, "%s: [%s (%s)]", msg, msg_maj, msg_min);
}

static APR_OPTIONAL_FN_TYPE(ssl_is_https) *mag_is_https = NULL;

static int mag_post_config(apr_pool_t *cfg, apr_pool_t *log,
                           apr_pool_t *temp, server_rec *s)
{
    /* FIXME: create mutex to deal with connections and contexts ? */
    mag_is_https = APR_RETRIEVE_OPTIONAL_FN(ssl_is_https);

    return OK;
}


struct mag_conn {
    apr_pool_t *parent;
    gss_ctx_id_t ctx;
    bool established;
    char *user_name;
    char *gss_name;
};

static int mag_pre_connection(conn_rec *c, void *csd)
{
    struct mag_conn *mc;

    mc = apr_pcalloc(c->pool, sizeof(struct mag_conn));
    if (!mc) return DECLINED;

    mc->parent = c->pool;
    ap_set_module_config(c->conn_config, &auth_gssapi_module, (void*)mc);
    return OK;
}

static apr_status_t mag_conn_destroy(void *ptr)
{
    struct mag_conn *mc = (struct mag_conn *)ptr;
    uint32_t min;

    if (mc->ctx) {
        (void)gss_delete_sec_context(&min, &mc->ctx, GSS_C_NO_BUFFER);
        mc->established = false;
    }
    return APR_SUCCESS;
}

static bool mag_conn_is_https(conn_rec *c)
{
    if (mag_is_https) {
        if (mag_is_https(c)) return true;
    }

    return false;
}

static int mag_auth(request_rec *req)
{
    const char *type;
    struct mag_config *cfg;
    const char *auth_header;
    char *auth_header_type;
    char *auth_header_value;
    int ret = HTTP_UNAUTHORIZED;
    gss_ctx_id_t ctx = GSS_C_NO_CONTEXT;
    gss_ctx_id_t *pctx;
    gss_buffer_desc input = GSS_C_EMPTY_BUFFER;
    gss_buffer_desc output = GSS_C_EMPTY_BUFFER;
    gss_buffer_desc name = GSS_C_EMPTY_BUFFER;
    gss_name_t client = GSS_C_NO_NAME;
    gss_cred_id_t delegated_cred = GSS_C_NO_CREDENTIAL;
    uint32_t flags;
    uint32_t maj, min;
    char *reply;
    size_t replen;
    char *clientname;
    gss_OID mech_type = GSS_C_NO_OID;
    gss_buffer_desc lname = GSS_C_EMPTY_BUFFER;
    struct mag_conn *mc = NULL;

    type = ap_auth_type(req);
    if ((type == NULL) || (strcasecmp(type, "GSSAPI") != 0)) {
        return DECLINED;
    }

    cfg = ap_get_module_config(req->per_dir_config, &auth_gssapi_module);

    if (cfg->ssl_only) {
        if (!mag_conn_is_https(req->connection)) {
            ap_log_rerror(APLOG_MARK, APLOG_ERR|APLOG_NOERRNO, 0, req,
                          "Not a TLS connection, refusing to authenticate!");
            goto done;
        }
    }

    if (cfg->gss_conn_ctx) {
        mc = (struct mag_conn *)ap_get_module_config(
                                                req->connection->conn_config,
                                                &auth_gssapi_module);
        if (!mc) {
            return DECLINED;
        }
        if (mc->established) {
            ap_log_rerror(APLOG_MARK, APLOG_DEBUG|APLOG_NOERRNO, 0, req,
                          "Connection bound pre-authentication found.");
            apr_table_set(req->subprocess_env, "GSS_NAME", mc->gss_name);
            req->ap_auth_type = apr_pstrdup(req->pool, "Negotiate");
            req->user = apr_pstrdup(req->pool, mc->user_name);
            ret = OK;
            goto done;
        }
        pctx = &mc->ctx;
    } else {
        pctx = &ctx;
    }

    auth_header = apr_table_get(req->headers_in, "Authorization");
    if (!auth_header) goto done;

    auth_header_type = ap_getword_white(req->pool, &auth_header);
    if (!auth_header_type) goto done;

    if (strcasecmp(auth_header_type, "Negotiate") != 0) goto done;

    auth_header_value = ap_getword_white(req->pool, &auth_header);
    if (!auth_header_value) goto done;
    input.length = apr_base64_decode_len(auth_header_value) + 1;
    input.value = apr_pcalloc(req->pool, input.length);
    if (!input.value) goto done;
    input.length = apr_base64_decode(input.value, auth_header_value);

    maj = gss_accept_sec_context(&min, pctx, GSS_C_NO_CREDENTIAL,
                                 &input, GSS_C_NO_CHANNEL_BINDINGS,
                                 &client, &mech_type, &output, &flags, NULL,
                                 &delegated_cred);
    if (GSS_ERROR(maj)) {
        ap_log_rerror(APLOG_MARK, APLOG_ERR|APLOG_NOERRNO, 0, req,
                      mag_error(req, "gss_accept_sec_context() failed",
                                maj, min));
        goto done;
    }

    /* register the context in the connection pool, so it can be freed
     * when the connection is terminated */
    apr_pool_userdata_set(mc, "mag_conn_ptr", mag_conn_destroy, mc->parent);

    if (maj == GSS_S_CONTINUE_NEEDED) {
        if (!cfg->gss_conn_ctx) {
            ap_log_rerror(APLOG_MARK, APLOG_ERR|APLOG_NOERRNO, 0, req,
                          "Mechanism needs continuation but "
                          "GssapiConnectionBound is off.");
            gss_delete_sec_context(&min, pctx, GSS_C_NO_BUFFER);
            gss_release_buffer(&min, &output);
            output.length = 0;
        }
        goto done;
    }

#ifdef HAVE_GSS_STORE_CRED_INTO
    if (cfg->cred_store.count != 0 && delegated_cred != GSS_C_NO_CREDENTIAL) {
        gss_key_value_set_desc store = {0, NULL};
        /* FIXME: run substitutions */

        maj = gss_store_cred_into(&min, delegated_cred, GSS_C_INITIATE,
                                  GSS_C_NULL_OID, 1, 1, &store, NULL, NULL);
    }
#endif

    req->ap_auth_type = apr_pstrdup(req->pool, "Negotiate");

    /* Always set the GSS name in an env var */
    maj = gss_display_name(&min, client, &name, NULL);
    if (GSS_ERROR(maj)) {
        ap_log_rerror(APLOG_MARK, APLOG_ERR|APLOG_NOERRNO, 0, req,
                      mag_error(req, "gss_accept_sec_context() failed",
                                maj, min));
        goto done;
    }
    clientname = apr_pstrndup(req->pool, name.value, name.length);
    apr_table_set(req->subprocess_env, "GSS_NAME", clientname);

    if (cfg->map_to_local) {
        maj = gss_localname(&min, client, mech_type, &lname);
        if (maj != GSS_S_COMPLETE) {
            ap_log_rerror(APLOG_MARK, APLOG_ERR|APLOG_NOERRNO, 0, req,
                          mag_error(req, "gss_localname() failed", maj, min));
            goto done;
        }
        req->user = apr_pstrndup(req->pool, lname.value, lname.length);
    } else {
        req->user = clientname;
    }

    if (mc) {
        mc->user_name = apr_pstrdup(mc->parent, req->user);
        mc->gss_name = apr_pstrdup(mc->parent, clientname);
        mc->established = true;
    }

    ret = OK;

done:
    if (ret == HTTP_UNAUTHORIZED) {
        if (output.length != 0) {
            replen = apr_base64_encode_len(output.length) + 1;
            reply = apr_pcalloc(req->pool, 10 + replen);
            if (reply) {
                memcpy(reply, "Negotiate ", 10);
                apr_base64_encode(&reply[10], output.value, output.length);
                apr_table_add(req->err_headers_out,
                              "WWW-Authenticate", reply);
            }
        } else {
            apr_table_add(req->err_headers_out,
                          "WWW-Authenticate", "Negotiate");
        }
    }
    gss_release_cred(&min, &delegated_cred);
    gss_release_buffer(&min, &output);
    gss_release_name(&min, &client);
    gss_release_buffer(&min, &name);
    gss_release_buffer(&min, &lname);
    return ret;
}


static void *mag_create_dir_config(apr_pool_t *p, char *dir)
{
    struct mag_config *cfg;

    cfg = (struct mag_config *)apr_pcalloc(p, sizeof(struct mag_config));
    if (!cfg) return NULL;

    return cfg;
}

static const char *mag_ssl_only(cmd_parms *parms, void *mconfig, int on)
{
    struct mag_config *cfg = (struct mag_config *)mconfig;
    cfg->ssl_only = on ? true : false;
    return NULL;
}

static const char *mag_map_to_local(cmd_parms *parms, void *mconfig, int on)
{
    struct mag_config *cfg = (struct mag_config *)mconfig;
    cfg->map_to_local = on ? true : false;
    return NULL;
}

static const char *mag_conn_ctx(cmd_parms *parms, void *mconfig, int on)
{
    struct mag_config *cfg = (struct mag_config *)mconfig;
    cfg->gss_conn_ctx = on ? true : false;
    return NULL;
}

static const char *mag_cred_store(cmd_parms *parms, void *mconfig,
                                  const char *w)
{
    struct mag_config *cfg = (struct mag_config *)mconfig;
    gss_key_value_element_desc *elements;
    uint32_t count;
    size_t size;
    const char *p;
    char *value;
    char *key;

    p = strchr(w, ':');
    if (!p) {
        ap_log_error(APLOG_MARK, APLOG_ERR|APLOG_NOERRNO, 0, parms->server,
                     "%s [%s]", "Invalid syntax for GssapiCredStore option", w);
        return NULL;
    }

    key = apr_pstrndup(parms->pool, w, (p-w));
    value = apr_pstrdup(parms->pool, p + 1);
    if (!key || !value) {
        ap_log_error(APLOG_MARK, APLOG_ERR|APLOG_NOERRNO, 0, parms->server,
                     "%s", "OOM handling GssapiCredStore option");
        return NULL;
    }

    size = sizeof(gss_key_value_element_desc) * cfg->cred_store.count + 1;
    elements = apr_palloc(parms->pool, size);
    if (!elements) {
        ap_log_error(APLOG_MARK, APLOG_ERR|APLOG_NOERRNO, 0, parms->server,
                     "%s", "OOM handling GssapiCredStore option");
        return NULL;
    }

    for (count = 0; count < cfg->cred_store.count; count++) {
        elements[count] = cfg->cred_store.elements[count];
    }
    elements[count].key = key;
    elements[count].value = value;

    cfg->cred_store.elements = elements;
    cfg->cred_store.count = count;

    return NULL;
}

static const command_rec mag_commands[] = {
    AP_INIT_FLAG("GssapiSSLonly", mag_ssl_only, NULL, OR_AUTHCFG,
                  "Work only if connection is SSL Secured"),
    AP_INIT_FLAG("GssapiLocalName", mag_map_to_local, NULL, OR_AUTHCFG,
                  "Work only if connection is SSL Secured"),
    AP_INIT_FLAG("GssapiConnectionBound", mag_conn_ctx, NULL, OR_AUTHCFG,
                  "Authentication is bound to the TCP connection"),
    AP_INIT_ITERATE("GssapiCredStore", mag_cred_store, NULL, OR_AUTHCFG,
                    "Credential Store"),
    { NULL }
};

static void
mag_register_hooks(apr_pool_t *p)
{
    ap_hook_check_user_id(mag_auth, NULL, NULL, APR_HOOK_MIDDLE);
    ap_hook_post_config(mag_post_config, NULL, NULL, APR_HOOK_MIDDLE);
    ap_hook_pre_connection(mag_pre_connection, NULL, NULL, APR_HOOK_MIDDLE);
}

module AP_MODULE_DECLARE_DATA auth_gssapi_module =
{
    STANDARD20_MODULE_STUFF,
    mag_create_dir_config,
    NULL,
    NULL,
    NULL,
    mag_commands,
    mag_register_hooks
};
