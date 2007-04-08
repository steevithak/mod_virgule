typedef struct _Buffer Buffer;

Buffer *virgule_buffer_new (apr_pool_t *p);

void virgule_buffer_set_translations (Buffer *b, const char **translations);

void virgule_buffer_write (Buffer *b, const char *data, int size);

/* todo: gcc format pragma */
void virgule_buffer_printf (Buffer *b, const char *fmt, ...);

void virgule_buffer_puts (Buffer *b, const char *str);

void virgule_buffer_append (Buffer *b, const char *str1, ...);

int virgule_buffer_send_response (request_rec *r, Buffer *b);

char *virgule_buffer_extract (Buffer *b);

int virgule_buffer_size (Buffer *b);
