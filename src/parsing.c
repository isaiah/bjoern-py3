#define STOP_PARSER(code)   do { \
                                ((bjoern_http_parser*)parser)->exit_code = code; \
                                return PARSER_EXIT; \
                            } while(0)
#define GET_TRANSACTION ((bjoern_http_parser*)parser)->transaction

/*
    Initialize the http parser.
*/
static int
http_on_start_parsing(http_parser* parser)
{
    ((bjoern_http_parser*)parser)->header_name_start   = NULL;
    ((bjoern_http_parser*)parser)->header_name_length  = 0;
    ((bjoern_http_parser*)parser)->header_value_start  = NULL;
    ((bjoern_http_parser*)parser)->header_value_length = 0;

    return PARSER_CONTINUE;
}

/*
    Parsing is done, populate some WSGI `environ` keys and so on.
*/
static int
http_on_end_parsing(http_parser* parser)
{
    /* Set the REQUEST_METHOD: */
    PyObject* py_request_method;

    switch(parser->method) {
        case HTTP_GET:
            py_request_method = PY_STRING_GET;
            break;
        case HTTP_POST:
            py_request_method = PY_STRING_GET;
            break;
        default:
            /* Currently, only POST and GET is supported. Fail here. */
            STOP_PARSER(HTTP_NOT_IMPLEMENTED);
    }

    PyDict_SetItem(
        GET_TRANSACTION->wsgi_environ,
        PY_STRING_REQUEST_METHOD,
        py_request_method
    );

    /* Set the CONTENT_TYPE, which is the same as HTTP_CONTENT_TYPE. */
    PyObject* content_type = PyDict_GetItem(GET_TRANSACTION->wsgi_environ,
                                            PY_STRING_HTTP_CONTENT_TYPE);
    if(content_type) {
        PyDict_SetItem(GET_TRANSACTION->wsgi_environ,
                       PY_STRING_CONTENT_TYPE, content_type);
    }

    /* TODO: Set SERVER_NAME and SERVER_PORT. */
    return PARSER_CONTINUE;
}

/*
    Set SCRIPT_NAME and PATH_INFO.

    TODO: Implement when routing is implemented.
*/
static int
http_on_path(http_parser* parser, const char* path_start, size_t path_length)
{
#ifdef WANT_CACHING
    if(CACHE_HAS(path_start, path_length)) {
        ((char*)path_start)[path_length] = '\0'; /* <-- we can do this safely because we need nothing but the URL for the cache stuff */
        parser->data = (void*)path_start;
        /* Stop parsing here, we don't need any more information: */
        STOP_PARSER(USE_CACHE);
    }
#endif
    PyObject* py_path = PyStringWithLen(path_start, path_length);
    if(py_path == NULL)
        STOP_PARSER(HTTP_INTERNAL_SERVER_ERROR);

    Py_INCREF(py_path);

#ifdef WANT_ROUTING
    Route* route = get_route_for_url(py_path);
    if(route == NULL) {
        Py_DECREF(py_path);
        /* TODO: 404 fallback callback? */
        STOP_PARSER(HTTP_NOT_FOUND);
    }

    STOP_PARSER(HTTP_NOT_FOUND);
    parser->data = (void*)route;
#endif

    PyDict_SetItem(GET_TRANSACTION->wsgi_environ, PY_STRING_PATH_INFO, py_path);

    return PARSER_CONTINUE;
}

/*
    Set the QUERY_STRING.
*/
static int
http_on_query(http_parser* parser, const char* query_start, size_t query_length)
{
    PyObject* py_tmp = PyStringWithLen(query_start, query_length);
    Py_INCREF(py_tmp);

    PyDict_SetItem(GET_TRANSACTION->wsgi_environ, PY_STRING_QUERY_STRING, py_tmp);

    return PARSER_CONTINUE;
}


/*
    Transform the current header name to something WSGI (CGI) compatible, e.g.
        User-Agent => HTTP_USER_AGENT
    and store it in the `wsgi_environ` dictionary.
*/
static inline void store_current_header(bjoern_http_parser* parser)
{
    /* Allocate an empty Python string with size 'header-length + 5': */
    PyObject* py_header_name = PyString_FromStringAndSize(NULL,
                                  parser->header_name_length + strlen("HTTP_"));
    Py_INCREF(py_header_name);
    /* Get the internal buffer of `py_header_name`: */
    char* header_name = PyString_AS_STRING(py_header_name);

    /* Copy the new header name: */
    header_name[0] = 'H';
    header_name[1] = 'T';
    header_name[2] = 'T';
    header_name[3] = 'P';
    header_name[4] = '_';

    bjoern_http_to_wsgi_header(&header_name[5], parser->header_name_start,
                                                parser->header_name_length);

    PyObject* py_header_value = PyStringWithLen(parser->header_value_start,
                                                parser->header_value_length);
    Py_INCREF(py_header_value);


    PyDict_SetItem(GET_TRANSACTION->wsgi_environ, py_header_name, py_header_value);
}


static int
http_on_header_name(http_parser* parser, const char* header_start, size_t header_length)
{
    bjoern_http_parser* bj_parser = (bjoern_http_parser*)parser;
    if(bj_parser->header_value_start) {
        /* We have a name/value pair to store, so do so. */
        store_current_header((bjoern_http_parser*)parser);
        goto start_new_header;
    }
    if(bj_parser->header_name_start) {
        /*  We already have a pointer to the header, so update the length. */
        /* TODO: Documentation */
        bj_parser->header_name_length = \
            (header_start - bj_parser->header_name_start) + header_length;
        return PARSER_CONTINUE;
    }
    else {
        goto start_new_header;
    }

/* Start a new header. */
start_new_header:
    ((bjoern_http_parser*)parser)->header_name_start   = header_start;
    ((bjoern_http_parser*)parser)->header_name_length  = header_length;
    ((bjoern_http_parser*)parser)->header_value_start  = NULL;
    ((bjoern_http_parser*)parser)->header_value_length = 0;

    return PARSER_CONTINUE;
}

static int
http_on_header_value(http_parser* parser, const char* value_start, size_t value_length)
{
    bjoern_http_parser* bj_parser = (bjoern_http_parser*)parser;
    if(bj_parser->header_value_start) {
        /* We already have a value pointer, so update the length. */
        bj_parser->header_value_length =
            (value_start - bj_parser->header_value_start) + value_length;
    }
    else {
        /* Start new value. */
        bj_parser->header_value_start = value_start;
        bj_parser->header_value_length = value_length;
    }

    return PARSER_CONTINUE;
}



/* TODO: Implement with StringIO or something like that. */
static int
http_on_body(http_parser* parser, const char* body, size_t body_length)
{
    return PARSER_CONTINUE;
}

/* TODO: Decide what to do with this one. */
static int
http_on_fragment(http_parser* parser, const char* fragment_start, size_t fragment_length)
{
    return PARSER_CONTINUE;
}

static int
http_on_url(http_parser* parser, const char* url_start, size_t url_length)
{
    return PARSER_CONTINUE;
}

static int
http_on_headers_complete(http_parser* parser)
{
    return PARSER_CONTINUE;
}


static struct http_parser_settings
  parser_settings = {
    http_on_start_parsing,      /* http_cb      on_message_begin; */
    http_on_path,               /* http_data_cb on_path; */
    http_on_query,              /* http_data_cb on_query_string; */
    http_on_url,                /* http_data_cb on_url; */
    http_on_fragment,           /* http_data_cb on_fragment; */
    http_on_header_name,        /* http_data_cb on_header_field; */
    http_on_header_value,       /* http_data_cb on_header_value; */
    http_on_headers_complete,   /* http_cb      on_headers_complete; */
    http_on_body,               /* http_data_cb on_body; */
    http_on_end_parsing,        /* http_cb      on_message_complete; */
};

#undef GET_TRANSACTION