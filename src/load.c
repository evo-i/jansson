#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <assert.h>

#include <jansson.h>
#include "jansson_private.h"
#include "strbuffer.h"
#include "utf.h"

#define TOKEN_INVALID         -1
#define TOKEN_EOF              0
#define TOKEN_STRING         256
#define TOKEN_INTEGER        257
#define TOKEN_REAL           258
#define TOKEN_TRUE           259
#define TOKEN_FALSE          260
#define TOKEN_NULL           261

/* read one byte from stream, return EOF on end of file */
typedef int (*get_func)(void *data);

/* return non-zero if end of file has been reached */
typedef int (*eof_func)(void *data);

typedef struct {
    get_func get;
    eof_func eof;
    void *data;
    char buffer[5];
    int buffer_pos;
} stream_t;


typedef struct {
    stream_t stream;
    strbuffer_t saved_text;
    int token;
    int line, column;
    union {
        char *string;
        int integer;
        double real;
    } value;
} lex_t;


/*** error reporting ***/

static void error_set(json_error_t *error, const lex_t *lex,
                      const char *msg, ...)
{
    va_list ap;
    char text[JSON_ERROR_TEXT_LENGTH];

    if(!error)
        return;

    va_start(ap, msg);
    vsnprintf(text, JSON_ERROR_TEXT_LENGTH, msg, ap);
    va_end(ap);

    if(lex)
    {
        const char *saved_text = strbuffer_value(&lex->saved_text);
        error->line = lex->line;
        if(saved_text && saved_text[0])
        {
            snprintf(error->text, JSON_ERROR_TEXT_LENGTH,
                     "%s near '%s'", text, saved_text);
        }
        else
        {
            snprintf(error->text, JSON_ERROR_TEXT_LENGTH,
                     "%s near end of file", text);
        }
    }
    else
    {
        error->line = -1;
        snprintf(error->text, JSON_ERROR_TEXT_LENGTH, "%s", text);
    }
}


/*** lexical analyzer ***/

void stream_init(stream_t *stream, get_func get, eof_func eof, void *data)
{
    stream->get = get;
    stream->eof = eof;
    stream->data = data;
    stream->buffer[0] = '\0';
    stream->buffer_pos = 0;
}

static char stream_get(stream_t *stream)
{
    if(!stream->buffer[stream->buffer_pos])
    {
        char c;

        stream->buffer[0] = stream->get(stream->data);
        stream->buffer_pos = 0;

        c = stream->buffer[0];

        if(c == EOF && stream->eof(stream->data))
            return EOF;

        if(c < 0)
        {
            /* multi-byte UTF-8 sequence */
            int i, count;

            count = utf8_check_first(c);
            if(!count)
                return 0;

            assert(count >= 2);

            for(i = 1; i < count; i++)
                stream->buffer[i] = stream->get(stream->data);

            if(!utf8_check_full(stream->buffer, count))
                return 0;

            stream->buffer[count] = '\0';
        }
        else
            stream->buffer[1] = '\0';
    }

    return (char)stream->buffer[stream->buffer_pos++];
}

static void stream_unget(stream_t *stream, char c)
{
    assert(stream->buffer_pos > 0);
    stream->buffer_pos--;
    assert(stream->buffer[stream->buffer_pos] == (unsigned char)c);
}


static int lex_get(lex_t *lex)
{
    return stream_get(&lex->stream);
}

static int lex_eof(lex_t *lex)
{
    return lex->stream.eof(lex->stream.data);
}

static void lex_save(lex_t *lex, char c)
{
    strbuffer_append_byte(&lex->saved_text, c);
}

static int lex_get_save(lex_t *lex)
{
    char c = stream_get(&lex->stream);
    lex_save(lex, c);
    return c;
}

static void lex_unget_unsave(lex_t *lex, char c)
{
    char d;
    stream_unget(&lex->stream, c);
    d = strbuffer_pop(&lex->saved_text);
    assert(c == d);
}

static void lex_scan_string(lex_t *lex)
{
    char c;
    const char *p;
    char *t;

    lex->token = TOKEN_INVALID;

    /* skip the " */
    c = lex_get_save(lex);

    while(c != '"') {
        if(c == EOF && lex_eof(lex))
            goto out;

        else if(0 <= c && c <= 0x1F) {
            /* control character */
            lex_unget_unsave(lex, c);
            goto out;
        }

        else if(c == '\\') {
            c = lex_get_save(lex);
            if(c == 'u') {
                c = lex_get_save(lex);
                for(int i = 0; i < 4; i++) {
                    if(!isxdigit(c)) {
                        lex_unget_unsave(lex, c);
                        goto out;
                    }
                    c = lex_get_save(lex);
                }
            }
            else if(c == '"' || c == '\\' || c == '/' || c == 'b' ||
                    c == 'f' || c == 'n' || c == 'r' || c == 't')
                c = lex_get_save(lex);
            else {
                lex_unget_unsave(lex, c);
                goto out;
            }
        }
        else
            c = lex_get_save(lex);
    }

    /* the actual value is at most of the same length as the source
       string, because:
         - shortcut escapes (e.g. "\t") (length 2) are converted to 1 byte
         - a single \uXXXX escape (length 6) is converted to at most 3 bytes
         - two \uXXXX escapes (length 12) forming an UTF-16 surrogate pair
           are converted to 4 bytes
    */
    lex->value.string = malloc(lex->saved_text.length + 1);
    if(!lex->value.string) {
        /* this is not very nice, since TOKEN_INVALID is returned */
        goto out;
    }

    /* the target */
    t = lex->value.string;

    /* + 1 to skip the " */
    p = strbuffer_value(&lex->saved_text) + 1;

    while(*p != '"') {
        if(*p == '\\') {
            p++;
            if(*p == 'u') {
                /* TODO: \uXXXX not supported yet */
                free(lex->value.string);
                lex->value.string = NULL;
                goto out;
            } else {
                switch(*p) {
                    case '"': case '\\': case '/':
                        *t = *p; break;
                    case 'b': *t = '\b'; break;
                    case 'f': *t = '\f'; break;
                    case 'n': *t = '\n'; break;
                    case 'r': *t = '\r'; break;
                    case 't': *t = '\t'; break;
                    default: assert(0);
                }
            }
        }
        else
            *t = *p;

        t++;
        p++;
    }
    *t = '\0';
    lex->token = TOKEN_STRING;

out:
    return;
}

static void lex_scan_number(lex_t *lex, char c)
{
    const char *saved_text;
    char *end;

    lex->token = TOKEN_INVALID;

    if(c == '-')
        c = lex_get_save(lex);

    if(c == '0') {
        c = lex_get_save(lex);
        if(isdigit(c)) {
            lex_unget_unsave(lex, c);
            goto out;
        }
    }
    else /* c != '0' */ {
        c = lex_get_save(lex);
        while(isdigit(c))
            c = lex_get_save(lex);
    }

    if(c != '.' && c != 'E' && c != 'e') {
        lex_unget_unsave(lex, c);
        lex->token = TOKEN_INTEGER;

        saved_text = strbuffer_value(&lex->saved_text);
        lex->value.integer = strtol(saved_text, &end, 10);
        assert(end == saved_text + lex->saved_text.length);

        return;
    }

    if(c == '.') {
        c = lex_get(lex);
        if(!isdigit(c))
            goto out;
        lex_save(lex, c);

        c = lex_get_save(lex);
        while(isdigit(c))
            c = lex_get_save(lex);
    }

    if(c == 'E' || c == 'e') {
        c = lex_get_save(lex);
        if(c == '+' || c == '-')
            c = lex_get_save(lex);

        if(!isdigit(c)) {
            lex_unget_unsave(lex, c);
            goto out;
        }

        c = lex_get_save(lex);
        while(isdigit(c))
            c = lex_get_save(lex);
    }

    lex_unget_unsave(lex, c);
    lex->token = TOKEN_REAL;

    saved_text = strbuffer_value(&lex->saved_text);
    lex->value.real = strtod(saved_text, &end);
    assert(end == saved_text + lex->saved_text.length);

out:
    return;
}

static int lex_scan(lex_t *lex)
{
    char c;

    strbuffer_clear(&lex->saved_text);

    if(lex->token == TOKEN_STRING) {
      free(lex->value.string);
      lex->value.string = NULL;
    }

    c = lex_get(lex);
    while(c == ' ' || c == '\t' || c == '\n' || c == '\r')
    {
        if(c == '\n')
            lex->line++;

        c = lex_get(lex);
    }

    if(c == EOF && lex_eof(lex)) {
        lex->token = TOKEN_EOF;
        goto out;
    }

    lex_save(lex, c);

    if(c == '{' || c == '}' || c == '[' || c == ']' || c == ':' || c == ',')
        lex->token = c;

    else if(c == '"')
        lex_scan_string(lex);

    else if(isdigit(c) || c == '-')
        lex_scan_number(lex, c);

    else if(isupper(c) || islower(c)) {
        /* eat up the whole identifier for clearer error messages */
        const char *saved_text;

        c = lex_get_save(lex);
        while(isupper(c) || islower(c))
            c = lex_get_save(lex);
        lex_unget_unsave(lex, c);

        saved_text = strbuffer_value(&lex->saved_text);

        if(strcmp(saved_text, "true") == 0)
            lex->token = TOKEN_TRUE;
        else if(strcmp(saved_text, "false") == 0)
            lex->token = TOKEN_FALSE;
        else if(strcmp(saved_text, "null") == 0)
            lex->token = TOKEN_NULL;
        else
            lex->token = TOKEN_INVALID;
    }

    else
        lex->token = TOKEN_INVALID;

out:
    return lex->token;
}

static int lex_init(lex_t *lex, get_func get, eof_func eof, void *data)
{
    stream_init(&lex->stream, get, eof, data);
    if(strbuffer_init(&lex->saved_text))
        return -1;

    lex->token = TOKEN_INVALID;
    lex->line = 1;

    return 0;
}

static void lex_close(lex_t *lex)
{
    if(lex->token == TOKEN_STRING)
        free(lex->value.string);
}


/*** parser ***/

static json_t *parse_value(lex_t *lex, json_error_t *error);

static json_t *parse_object(lex_t *lex, json_error_t *error)
{
    json_t *object = json_object();
    if(!object)
        return NULL;

    lex_scan(lex);
    if(lex->token == '}')
        return object;

    while(1) {
        char *key;
        json_t *value;

        if(lex->token != TOKEN_STRING) {
            error_set(error, lex, "string or '}' expected");
            goto error;
        }

        key = strdup(lex->value.string);
        if(!key)
            return NULL;

        lex_scan(lex);
        if(lex->token != ':') {
            free(key);
            error_set(error, lex, "':' expected");
            goto error;
        }

        lex_scan(lex);
        value = parse_value(lex, error);
        if(!value) {
            free(key);
            goto error;
        }

        if(json_object_set_nocheck(object, key, value)) {
            free(key);
            json_decref(value);
            goto error;
        }

        json_decref(value);
        free(key);

        lex_scan(lex);
        if(lex->token != ',')
            break;

        lex_scan(lex);
    }

    if(lex->token != '}') {
        error_set(error, lex, "'}' expected");
        goto error;
    }

    return object;

error:
    json_decref(object);
    return NULL;
}

static json_t *parse_array(lex_t *lex, json_error_t *error)
{
    json_t *array = json_array();
    if(!array)
        return NULL;

    lex_scan(lex);
    if(lex->token == ']')
        return array;

    while(lex->token) {
        json_t *elem = parse_value(lex, error);
        if(!elem)
            goto error;

        if(json_array_append(array, elem)) {
            json_decref(elem);
            goto error;
        }
        json_decref(elem);

        lex_scan(lex);
        if(lex->token != ',')
            break;

        lex_scan(lex);
    }

    if(lex->token != ']') {
        error_set(error, lex, "']' expected");
        goto error;
    }

    return array;

error:
    json_decref(array);
    return NULL;
}

static json_t *parse_value(lex_t *lex, json_error_t *error)
{
    json_t *json;

    switch(lex->token) {
        case TOKEN_STRING: {
            json = json_string_nocheck(lex->value.string);
            break;
        }

        case TOKEN_INTEGER: {
            json = json_integer(lex->value.integer);
            break;
        }

        case TOKEN_REAL: {
            json = json_real(lex->value.real);
            break;
        }

        case TOKEN_TRUE:
            json = json_true();
            break;

        case TOKEN_FALSE:
            json = json_false();
            break;

        case TOKEN_NULL:
            json = json_null();
            break;

        case '{':
          json = parse_object(lex, error);
            break;

        case '[':
            json = parse_array(lex, error);
            break;

        case TOKEN_INVALID:
            error_set(error, lex, "invalid token");
            return NULL;

        default:
            error_set(error, lex, "unexpected token");
            return NULL;
    }

    if(!json)
        return NULL;

    return json;
}

json_t *parse_json(lex_t *lex, json_error_t *error)
{
    lex_scan(lex);

    if(lex->token != '[' && lex->token != '{') {
        error_set(error, lex, "'[' or '{' expected");
        return NULL;
    }

    return parse_value(lex, error);
}

json_t *json_load(const char *path, json_error_t *error)
{
    json_t *result;
    FILE *fp;

    fp = fopen(path, "r");
    if(!fp)
    {
        error_set(error, NULL, "unable to open %s: %s",
                  path, strerror(errno));
        return NULL;
    }

    result = json_loadf(fp, error);

    fclose(fp);
    return result;
}

typedef struct
{
    const char *data;
    int pos;
} string_data_t;

static int string_get(void *data)
{
    char c;
    string_data_t *stream = (string_data_t *)data;
    c = stream->data[stream->pos++];
    if(c == '\0')
        return EOF;
    else
        return c;
}

static int string_eof(void *data)
{
    string_data_t *stream = (string_data_t *)data;
    return (stream->data[stream->pos] == '\0');
}

json_t *json_loads(const char *string, json_error_t *error)
{
    lex_t lex;
    json_t *result;

    string_data_t stream_data = {
        .data = string,
        .pos = 0
    };

    if(lex_init(&lex, string_get, string_eof, (void *)&stream_data))
        return NULL;

    result = parse_json(&lex, error);
    if(!result)
        goto out;

    lex_scan(&lex);
    if(lex.token != TOKEN_EOF) {
        error_set(error, &lex, "end of file expected");
        json_decref(result);
        result = NULL;
    }

out:
    lex_close(&lex);
    return result;
}

json_t *json_loadf(FILE *input, json_error_t *error)
{
    lex_t lex;
    json_t *result;

    if(lex_init(&lex, (get_func)fgetc, (eof_func)feof, input))
        return NULL;

    result = parse_json(&lex, error);

    lex_close(&lex);
    return result;
}
