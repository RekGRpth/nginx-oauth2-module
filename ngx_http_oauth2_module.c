#include <nginx.h>
#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
#include <string.h>
#include <curl/curl.h>
#include <uuid/uuid.h>
#include <arpa/inet.h>
#include "jsmn.h"

ngx_module_t ngx_http_oauth2_module;

typedef struct {
    ngx_str_t oauth2_authorization_endpoint; 
    ngx_str_t oauth2_token_endpoint;
    ngx_str_t oauth2_response_type;          
    ngx_str_t oauth2_client_id;              
    ngx_str_t oauth2_client_secret;          
    ngx_str_t oauth2_redirect_uri;           
    ngx_str_t oauth2_scope;                  

    ngx_str_t oauth2_authorization_endpoint_request_url;
    ngx_str_t oauth2_token_endpoint_endpoint_post;
} ngx_http_oauth2_main_conf_t;

typedef struct {
    ngx_flag_t enabled;
} ngx_http_oauth2_srv_conf_t;

typedef struct {
  ngx_str_t string;
  ngx_http_request_t *r;
} string_request_wrapper_t;

static ngx_str_t sendRequestToOauth2TokenEndpoint(ngx_http_request_t *r, ngx_http_oauth2_main_conf_t *cf);
static size_t writefunc(void *ptr, size_t size, size_t nmemb, string_request_wrapper_t *wrapper);
static ngx_str_t getVariableFromQueryString(ngx_str_t *var, ngx_http_request_t *r);
static void sendCurlPostRequest(u_char *dst, u_char *post_data, string_request_wrapper_t *wrapper);
static ngx_str_t *getJsonProperty(ngx_str_t *json, const char *property, ngx_http_request_t *r);
static ngx_str_t *getRandomUuidString(ngx_http_request_t *r);
static int isRedirect(ngx_str_t *configRedirectString, ngx_http_request_t *r);
static u_char *ngx_memcpy_e(u_char *dst, u_char *src, size_t len);

static ngx_int_t setHeader(ngx_str_t *headerName, ngx_str_t *headerValue, ngx_http_request_t *r);
static ngx_int_t setCookie(ngx_str_t *cookieName, ngx_str_t *cookieValue, ngx_http_request_t *r);
static ngx_int_t getCookie(ngx_str_t *cookieName, ngx_str_t *cookieValue, ngx_http_request_t *r);

static ngx_str_t *getServerAddr(ngx_http_request_t *r);

static int jsoneq(const char *json, jsmntok_t *tok, const char *s) {
    if (tok->type == JSMN_STRING && (int) strlen(s) == tok->end - tok->start &&
            strncmp(json + tok->start, s, tok->end - tok->start) == 0) {
        return 0;
    }
    return -1;
}

static ngx_command_t ngx_http_oauth2_commands[] = {
    {
        ngx_string("oauth2"),
        NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_CONF_TAKE1,
        ngx_conf_set_flag_slot,
        NGX_HTTP_SRV_CONF_OFFSET,
        offsetof(ngx_http_oauth2_srv_conf_t, enabled),
        NULL
    },
    {
        ngx_string("oauth2_authorization_endpoint"),
        NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE1,
        ngx_conf_set_str_slot,
        NGX_HTTP_MAIN_CONF_OFFSET,
        offsetof(ngx_http_oauth2_main_conf_t, oauth2_authorization_endpoint),
        NULL
    },
    {
        ngx_string("oauth2_token_endpoint"),
        NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE1,
        ngx_conf_set_str_slot,
        NGX_HTTP_MAIN_CONF_OFFSET,
        offsetof(ngx_http_oauth2_main_conf_t, oauth2_token_endpoint),
        NULL
    },
    {
        ngx_string("oauth2_response_type"),
        NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE1,
        ngx_conf_set_str_slot,
        NGX_HTTP_MAIN_CONF_OFFSET,
        offsetof(ngx_http_oauth2_main_conf_t, oauth2_response_type),
        NULL
    },
    {
        ngx_string("oauth2_client_id"),
        NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE1,
        ngx_conf_set_str_slot,
        NGX_HTTP_MAIN_CONF_OFFSET,
        offsetof(ngx_http_oauth2_main_conf_t, oauth2_client_id),
        NULL
    },
    {
        ngx_string("oauth2_client_secret"),
        NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE1,
        ngx_conf_set_str_slot,
        NGX_HTTP_MAIN_CONF_OFFSET,
        offsetof(ngx_http_oauth2_main_conf_t, oauth2_client_secret),
        NULL
    },
    {
        ngx_string("oauth2_redirect_uri"),
        NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE1,
        ngx_conf_set_str_slot,
        NGX_HTTP_MAIN_CONF_OFFSET,
        offsetof(ngx_http_oauth2_main_conf_t, oauth2_redirect_uri),
        NULL
    },
    {
        ngx_string("oauth2_scope"),
        NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE1,
        ngx_conf_set_str_slot,
        NGX_HTTP_MAIN_CONF_OFFSET,
        offsetof(ngx_http_oauth2_main_conf_t, oauth2_scope),
        NULL
    },
    ngx_null_command
};

static void* ngx_http_oauth2_create_main_conf(ngx_conf_t *cf) {
    ngx_http_oauth2_main_conf_t *main_conf;

    main_conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_oauth2_main_conf_t));
    if (main_conf == NULL) {
        return NULL;
    }

    return main_conf;
}

static char* ngx_http_oauth2_init_main_conf(ngx_conf_t *cf, void *conf) {
    ngx_http_oauth2_main_conf_t *main_conf = (ngx_http_oauth2_main_conf_t *) conf;

    ngx_str_t response_type = ngx_string("response_type=");
    ngx_str_t client_id = ngx_string("client_id=");
    ngx_str_t redirect_uri = ngx_string("redirect_uri=");
    ngx_str_t scope = ngx_string("scope=");

    int url_len = main_conf->oauth2_authorization_endpoint.len
                + main_conf->oauth2_response_type.len + response_type.len
                + main_conf->oauth2_client_id.len + client_id.len
                + main_conf->oauth2_redirect_uri.len + redirect_uri.len
                + main_conf->oauth2_scope.len + scope.len
                + 1    // 1 * ?
                + 3    // 3 * &
                + 1;   // '\0'
    
    main_conf->oauth2_authorization_endpoint_request_url.data = ngx_pnalloc(cf->pool, url_len);
    if (main_conf->oauth2_authorization_endpoint_request_url.data == NULL) {
        return NGX_CONF_ERROR;
    }

    strncpy((char *) main_conf->oauth2_authorization_endpoint_request_url.data, (char *) main_conf->oauth2_authorization_endpoint.data, main_conf->oauth2_authorization_endpoint.len);
    strcat((char *) main_conf->oauth2_authorization_endpoint_request_url.data, "?");
    strcat((char *) main_conf->oauth2_authorization_endpoint_request_url.data, (char *) client_id.data);
    strcat((char *) main_conf->oauth2_authorization_endpoint_request_url.data, (char *) main_conf->oauth2_client_id.data);
    strcat((char *) main_conf->oauth2_authorization_endpoint_request_url.data, "&");
    strcat((char *) main_conf->oauth2_authorization_endpoint_request_url.data, (char *) response_type.data);
    strcat((char *) main_conf->oauth2_authorization_endpoint_request_url.data, (char *) main_conf->oauth2_response_type.data);
    strcat((char *) main_conf->oauth2_authorization_endpoint_request_url.data, "&");
    strcat((char *) main_conf->oauth2_authorization_endpoint_request_url.data, (char *) redirect_uri.data);
    strcat((char *) main_conf->oauth2_authorization_endpoint_request_url.data, (char *) main_conf->oauth2_redirect_uri.data);
    strcat((char *) main_conf->oauth2_authorization_endpoint_request_url.data, "&");
    strcat((char *) main_conf->oauth2_authorization_endpoint_request_url.data, (char *) scope.data);
    strcat((char *) main_conf->oauth2_authorization_endpoint_request_url.data, (char *) main_conf->oauth2_scope.data);
    main_conf->oauth2_authorization_endpoint_request_url.len = url_len - 1;

    ngx_str_t client_secret = ngx_string("client_secret=");
    
    int post_len = + main_conf->oauth2_client_id.len + client_id.len
                + main_conf->oauth2_client_secret.len + client_secret.len 
                + main_conf->oauth2_redirect_uri.len + redirect_uri.len
                + 3  // 3 * &
                + strlen("grant_type=authorization_code");
    
    main_conf->oauth2_token_endpoint_endpoint_post.data = ngx_pnalloc(cf->pool, post_len + 1);
    if (main_conf->oauth2_token_endpoint_endpoint_post.data == NULL) {
        return NGX_CONF_ERROR;
    } 
    u_char *p = ngx_memcpy_e(main_conf->oauth2_token_endpoint_endpoint_post.data, client_id.data, client_id.len);
    p = ngx_memcpy_e(p, main_conf->oauth2_client_id.data, main_conf->oauth2_client_id.len);
    p = ngx_memcpy_e(p, (u_char *) "&", sizeof("&") - 1);
    p = ngx_memcpy_e(p, client_secret.data, client_secret.len);
    p = ngx_memcpy_e(p, main_conf->oauth2_client_secret.data, main_conf->oauth2_client_secret.len);
    p = ngx_memcpy_e(p, (u_char *) "&", sizeof("&") - 1);
    p = ngx_memcpy_e(p, redirect_uri.data, redirect_uri.len);
    p = ngx_memcpy_e(p, main_conf->oauth2_redirect_uri.data, main_conf->oauth2_redirect_uri.len);
    p = ngx_memcpy_e(p, (u_char *) "&grant_type=authorization_code", sizeof("&grant_type=authorization_code") - 1);
    main_conf->oauth2_token_endpoint_endpoint_post.data[post_len] = '\0';
    main_conf->oauth2_token_endpoint_endpoint_post.len = post_len;
    
    return NGX_CONF_OK;
}

u_char *ngx_memcpy_e(u_char *dst, u_char *src, size_t len) {
    ngx_memcpy(dst, src, len);
    return dst + len;
}

static void *ngx_http_oauth2_create_srv_conf(ngx_conf_t *cf) {
    ngx_http_oauth2_srv_conf_t  *srv_conf;
    srv_conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_oauth2_srv_conf_t));
    if (srv_conf == NULL) {
        return NGX_CONF_ERROR;
    }
    
    srv_conf->enabled = NGX_CONF_UNSET;

    return srv_conf;
}

static char *ngx_http_oauth2_merge_srv_conf(ngx_conf_t *cf, void *parent, void *child) {
    ngx_http_oauth2_srv_conf_t *prev = parent;
    ngx_http_oauth2_srv_conf_t *conf = child;

    ngx_conf_merge_off_value(conf->enabled, prev->enabled, 0);
    return NGX_CONF_OK;
}

static ngx_int_t ngx_http_oauth2_handler(ngx_http_request_t *r) {
    if (r->main->internal) {
        return NGX_DECLINED; 
    }

    r->main->internal = 1;

    ngx_http_oauth2_srv_conf_t  *srv_conf = ngx_http_get_module_srv_conf(r, ngx_http_oauth2_module);
    
    if (!srv_conf->enabled || srv_conf->enabled == NGX_CONF_UNSET) {
        return NGX_DECLINED;
    }

    ngx_str_t *serverAddr = getServerAddr(r);

    ngx_http_oauth2_main_conf_t *config = ngx_http_get_module_main_conf(r, ngx_http_oauth2_module);
    
    if(isRedirect(&config->oauth2_redirect_uri, r)) {
        ngx_str_t orgUriCookieValue;
        ngx_str_t orgUriCookieName = ngx_string("org_uri");
        ngx_int_t orgUriCookieLocation = getCookie(&orgUriCookieName, &orgUriCookieValue, r);

        if (orgUriCookieLocation != NGX_OK) {
            ngx_str_set(&orgUriCookieValue, "/");
        }
        

        ngx_str_t response = sendRequestToOauth2TokenEndpoint(r, config);    
        ngx_str_t *accessToken = getJsonProperty(&response, "access_token", r);

        // START: set access_token cookie
        ngx_str_t accessTokenCookieName = ngx_string("access_token");
        if(setCookie(&accessTokenCookieName, accessToken, r) != NGX_OK) {
            return NGX_ERROR;
        }
        // END: set access_token cookie

        // START: set Location header
        int orgUrlLen = serverAddr->len + orgUriCookieValue.len;
        u_char *orgUrl = ngx_pnalloc(r->pool, orgUrlLen + 1);
        u_char *orgUrlLenTp = ngx_memcpy_e(orgUrl, serverAddr->data, serverAddr->len);
        orgUrlLenTp = ngx_memcpy_e(orgUrlLenTp, orgUriCookieValue.data, orgUriCookieValue.len);
        orgUrl[orgUrlLen] = '\0';
            
        ngx_str_t locationHeaderName = ngx_string("Location");
        ngx_str_t locationHeaderValue = {orgUrlLen, orgUrl};
        if(setHeader(&locationHeaderName, &locationHeaderValue, r) != NGX_OK) {
            return NGX_ERROR;
        }
        // END: set Location header

        return NGX_HTTP_MOVED_TEMPORARILY;

    } else {
        
        ngx_str_t cookie_value;
        ngx_str_t cookie_name = ngx_string("access_token");
        ngx_int_t cookie_location = getCookie(&cookie_name, &cookie_value, r);
        
        if (cookie_location == NGX_DECLINED) {
            
            ngx_str_t *uuidString = getRandomUuidString(r);
            
            // START: set Location header
            int authorizationEndpointUrlLen = config->oauth2_authorization_endpoint_request_url.len + sizeof("&state=") - 1 + 36;
            u_char *authorizationEndpointUrl = ngx_pnalloc(r->pool, authorizationEndpointUrlLen + 1);
            u_char *authorizationEndpointUrlTp = ngx_memcpy_e(authorizationEndpointUrl, 
                config->oauth2_authorization_endpoint_request_url.data, config->oauth2_authorization_endpoint_request_url.len);
            authorizationEndpointUrlTp = ngx_memcpy_e(authorizationEndpointUrlTp, (u_char *) "&state=", sizeof("&state=") - 1);
            authorizationEndpointUrlTp = ngx_memcpy_e(authorizationEndpointUrlTp, uuidString->data, uuidString->len);
            authorizationEndpointUrl[authorizationEndpointUrlLen] = '\0';
            
            ngx_str_t locationHeaderName = ngx_string("Location");
            ngx_str_t locationHeaderValue = {authorizationEndpointUrlLen, authorizationEndpointUrl};
            if(setHeader(&locationHeaderName, &locationHeaderValue, r) != NGX_OK) {
                return NGX_ERROR;
            }
            // END: set Location header

            // START: set org_uri cookie
            ngx_str_t orgUriCookieName = ngx_string("org_uri");
            if(setCookie(&orgUriCookieName, &r->uri, r) != NGX_OK) {
                return NGX_ERROR;
            }
            // END: set org_uri cookie
            
            return NGX_HTTP_MOVED_TEMPORARILY;
        }
    }
    return NGX_DECLINED;
}

static ngx_int_t ngx_http_oauth2_init(ngx_conf_t *cf) {
    ngx_http_handler_pt *h;
    ngx_http_core_main_conf_t *cmcf;

    cmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_core_module);

    h = ngx_array_push(&cmcf->phases[NGX_HTTP_ACCESS_PHASE].handlers);
    
    if (h == NULL) {
        return NGX_ERROR;
    }

    *h = ngx_http_oauth2_handler;
    
    return NGX_OK;
}

static ngx_int_t ngx_http_oauth2_init_module(ngx_cycle_t *cycle) {
    curl_global_init(CURL_GLOBAL_ALL);
    return NGX_OK;
}

static void ngx_http_oauth2_exit_master(ngx_cycle_t *cycle) {
    curl_global_cleanup();
}

static ngx_http_module_t ngx_http_oauth2_module_ctx = { 
    NULL,                               /* preconfiguration */ 
    ngx_http_oauth2_init,               /* postconfiguration */ 
    ngx_http_oauth2_create_main_conf,   /* create main configuration */ 
    ngx_http_oauth2_init_main_conf,     /* init main configuration */ 
    ngx_http_oauth2_create_srv_conf,    /* create server configuration */ 
    ngx_http_oauth2_merge_srv_conf,     /* merge server configuration */ 
    NULL,                               /* create location configuration */ 
    NULL                                /* merge location configuration */ 
}; 

ngx_module_t ngx_http_oauth2_module = { 
    NGX_MODULE_V1, 
    &ngx_http_oauth2_module_ctx,   /* module context */ 
    ngx_http_oauth2_commands,      /* module directives */ 
    NGX_HTTP_MODULE,               /* module type */ 
    NULL,                          /* init master */ 
    ngx_http_oauth2_init_module,   /* init module */ 
    NULL,                          /* init process */ 
    NULL,                          /* init thread */ 
    NULL,                          /* exit thread */ 
    NULL,                          /* exit process */ 
    &ngx_http_oauth2_exit_master,  /* exit master */ 
    NGX_MODULE_V1_PADDING 
};

static ngx_str_t sendRequestToOauth2TokenEndpoint(ngx_http_request_t *r, ngx_http_oauth2_main_conf_t *cf) {
    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "IN: sendRequestToOauth2TokenEndpoint");
    
    ngx_str_t code_param_key = ngx_string("code");
    ngx_str_t code_param_value = getVariableFromQueryString(&code_param_key, r);

    int post_len = cf->oauth2_token_endpoint_endpoint_post.len + strlen("&code=") + code_param_value.len;
       
    u_char *post = ngx_pnalloc(r->pool, post_len + 1);
    u_char *p = ngx_memcpy_e(post, cf->oauth2_token_endpoint_endpoint_post.data, cf->oauth2_token_endpoint_endpoint_post.len);
    p = ngx_memcpy_e(p, (u_char *) "&code=", sizeof("&code=") - 1);
    p = ngx_memcpy_e(p, code_param_value.data, code_param_value.len);
    post[post_len] = '\0';
    
    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "sendRequestToOauth2TokenEndpoint, POST_VALUE: %s", post);
    
    ngx_str_t str = ngx_string("");
    string_request_wrapper_t wrapper = {str, r};

    sendCurlPostRequest(cf->oauth2_token_endpoint.data, post, &wrapper); 

    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "OUT: sendRequestToOauth2TokenEndpoint, VALUE: %s", wrapper.string.data);
    return wrapper.string;
}

static ngx_str_t getVariableFromQueryString(ngx_str_t *variable, ngx_http_request_t *r) {
    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "IN: getVariableFromQueryString");
    
    int varLen = variable->len + sizeof("arg_") - 1;
    u_char varData[varLen + 1];
    ngx_memcpy(varData, (u_char *) "arg_", sizeof("arg_") - 1);
    ngx_memcpy(varData + strlen("arg_"), variable->data, variable->len);
    varData[varLen] = '\0';
    
    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "getVariableFromQueryString:varData: [%s]", varData);
    
    ngx_str_t argument = ngx_string(varData);
    ngx_uint_t id_key = ngx_hash_key(argument.data, argument.len);

    ngx_http_variable_value_t *variableValue = ngx_http_get_variable(r, &argument, id_key);

    ngx_str_t resultString = {0, NULL};
    if (variableValue != NULL && variableValue->not_found != 1) {
        u_char *buffer = ngx_pnalloc(r->pool, variableValue->len + 1);
        ngx_memcpy(buffer, variableValue->data, variableValue->len);
        buffer[variableValue->len] = '\0';
        resultString.len = variableValue->len;
        resultString.data = (u_char *) buffer;
    }

    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "getVariableFromQueryString, VALUE: %s", resultString.data);
    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "OUT: getVariableFromQueryString");
    return resultString;
}

static size_t writefunc(void *ptr, size_t size, size_t nmemb, string_request_wrapper_t *wrapper) {
    size_t new_len = wrapper->string.len + size*nmemb;
    u_char *d = ngx_pnalloc(wrapper->r->pool, new_len + 1);
    if (d == NULL) {
        ngx_log_error(NGX_LOG_ERR, wrapper->r->connection->log, 0, "wrapper->string.data is NULL");
        return 0;
    }
    ngx_memcpy(d, wrapper->string.data, wrapper->string.len);
    ngx_memcpy(d + wrapper->string.len, ptr, size * nmemb);
    d[new_len] = '\0';
    wrapper->string.data = d;
    wrapper->string.len = new_len;

    return size*nmemb;
}

static void sendCurlPostRequest(u_char *dst, u_char *post_data, string_request_wrapper_t *wrapper) {
    CURL *curl;
    CURLcode res;
         
    curl = curl_easy_init();
    if(curl) {
        
        curl_easy_setopt(curl, CURLOPT_URL, dst);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_data);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writefunc);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, wrapper);
                
        res = curl_easy_perform(curl);
        if(res != CURLE_OK) {
            ngx_log_error(NGX_LOG_ERR, wrapper->r->connection->log, 0, "===> CURL NOT OK!");

        }   
        ngx_log_error(NGX_LOG_ERR, wrapper->r->connection->log, 0, "DATA: %s, LEN: %d", wrapper->string.data, wrapper->string.len); 
        curl_easy_cleanup(curl);
    }
}

static ngx_str_t *getJsonProperty(ngx_str_t *json, const char *property, ngx_http_request_t *request) {
    int i;
    int r;
    jsmn_parser p;
    jsmntok_t t[10]; 

    ngx_str_t *result = ngx_pnalloc(request->pool, sizeof(ngx_str_t));
    result->data = NULL;
    result->len = 0;

    jsmn_init(&p);
    r = jsmn_parse(&p, (char *) json->data, json->len, t, sizeof(t)/sizeof(t[0]));
    if (r < 0) {
        printf("Failed to parse JSON: %d\n", r);
        return result;
    }

    /* Assume the top-level element is an object */
    if (r < 1 || t[0].type != JSMN_OBJECT) {
        printf("Object expected\n");
        return result;
    }

    /* Loop over all keys of the root object */
    for (i = 1; i < r; i++) {
        if (jsoneq((char *) json->data, &t[i], property) == 0) {
            int len = t[i+1].end-t[i+1].start;
            result->data = ngx_pnalloc(request->pool, len + 1);
            ngx_memcpy(result->data, json->data + t[i+1].start, len);
            result->data[len] = '\0';
            result->len = len;
            i++;
        }
    }

    return result;
}

static ngx_str_t *getRandomUuidString(ngx_http_request_t *r) {
    uuid_t uuid;
    uuid_generate_random(uuid);

    ngx_str_t *uuidString = ngx_pnalloc(r->pool, sizeof(ngx_str_t));
    uuidString->data = ngx_pnalloc(r->pool, 36 + 1);
    uuid_unparse_lower(uuid, (char *) uuidString->data);
    uuidString->len = 36;
    return uuidString;
}

static int isRedirect(ngx_str_t *configRedirectString, ngx_http_request_t *r) {
    ngx_str_t *serverAddr = getServerAddr(r);
    
    size_t urlLen = serverAddr->len + r->uri.len;
    u_char httpUrl[urlLen + 1];
    ngx_memcpy(httpUrl, serverAddr->data, serverAddr->len);
    ngx_memcpy(httpUrl + serverAddr->len, r->uri.data, r->uri.len);
    httpUrl[urlLen] = '\0';
    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "IN: isRedirect:httpUrl: [%s]", httpUrl);
    
    return !ngx_strncmp(configRedirectString->data, httpUrl, configRedirectString->len);
}

static ngx_str_t *getServerAddr(ngx_http_request_t *r) {
    struct sockaddr_in *addr = (struct sockaddr_in *) r->connection->listening->sockaddr;
    int port = ntohs(addr->sin_port);

    ngx_str_t scheme;
    if (port == 443) {
        ngx_str_set(&scheme, "https://");
    } else {
        ngx_str_set(&scheme, "http://");
    }

    size_t serverAddrLen = scheme.len + r->headers_in.host->value.len;
    
    ngx_str_t *serverAddr = ngx_pnalloc(r->pool, sizeof(ngx_str_t));
    serverAddr->data = ngx_pnalloc(r->pool, serverAddrLen + 1);
    ngx_memcpy(serverAddr->data, scheme.data, scheme.len);
    ngx_memcpy(serverAddr->data + scheme.len, r->headers_in.host->value.data, r->headers_in.host->value.len);
    serverAddr->data[serverAddrLen] = '\0';
    serverAddr->len = serverAddrLen;

    return serverAddr;
}

static ngx_int_t setHeader(ngx_str_t *headerName, ngx_str_t *headerValue, ngx_http_request_t *r) {
    ngx_table_elt_t *h = ngx_list_push(&r->headers_out.headers);
    if (h == NULL) {
        return NGX_ERROR;
    }

    h->hash = ngx_hash_key(headerName->data, headerName->len);
    h->key.data = headerName->data;
    h->key.len = headerName->len;

    h->value.data = headerValue->data;
    h->value.len = headerValue->len;

    return NGX_OK;
}

static ngx_int_t getCookie(ngx_str_t *cookieName, ngx_str_t *cookieValue, ngx_http_request_t *r) {
    return ngx_http_parse_multi_header_lines(&r->headers_in.cookies, cookieName, cookieValue);
}

static ngx_int_t setCookie(ngx_str_t *cookieName, ngx_str_t *cookieValue, ngx_http_request_t *r) {
    u_char *cookie_value = ngx_pnalloc(r->pool, cookieName->len + sizeof("=") - 1 + cookieValue->len + 1);
    u_char *cookie_value_tp = ngx_memcpy_e(cookie_value, cookieName->data, cookieName->len);
    cookie_value_tp = ngx_memcpy_e(cookie_value_tp, (u_char *) "=", sizeof("=") - 1);
    cookie_value_tp = ngx_memcpy_e(cookie_value_tp, cookieValue->data, cookieValue->len);
    cookie_value[cookieName->len + sizeof("=") - 1 + cookieValue->len] = '\0';
    
    ngx_str_t headerName = ngx_string("Set-Cookie");
    ngx_str_t headerValue = {cookieName->len + sizeof("=") - 1 + cookieValue->len, cookie_value};
    
    return setHeader(&headerName, &headerValue, r);
}

// https://www.googleapis.com/oauth2/v3/userinfo?alt=json&access_token=ya29.UQFpwFrksQY-evJ9O9ZXLEuW_TJq4O1zAVMKzNV8K-x6F2_05pbit3gHjjBr-7OLX7jQL6VD6cU1NQ
