typedef struct _Buffer Buffer;

Buffer *buffer_new (apr_pool_t *p);

void buffer_set_translations (Buffer *b, const char **translations);

void buffer_write (Buffer *b, const char *data, int size);

/* todo: gcc format pragma */
void buffer_printf (Buffer *b, const char *fmt, ...);

void buffer_puts (Buffer *b, const char *str);

void buffer_append (Buffer *b, const char *str1, ...);

int buffer_send_response (request_rec *r, Buffer *b);

char *buffer_extract (Buffer *b);
