
char *
b64enc (apr_pool_t *p, const char *data, int size);

char *
rand_cookie (apr_pool_t *p);

const char *
match_prefix (const char *url, const char *prefix);

char *
nice_text (apr_pool_t *p, const char *raw);

const NavOption *
add_nav_option (VirguleReq *vr, const char *label, const char *url);

const Topic *
add_topic (VirguleReq *vr, const char *desc, const char *url);

const AllowedTag *
add_allowed_tag (VirguleReq *vr, const char *tag, int can_be_empty);

int
render_acceptable_html (VirguleReq *vr);

char *
nice_htext (VirguleReq *vr, const char *raw, char **p_error);

char *
iso_now (apr_pool_t *p);

time_t
iso_to_time_t (const char *iso);

char *
str_subst (apr_pool_t *p, const char *str, const char *pattern, const char *repl);

char *
escape_uri_arg (apr_pool_t *p, const char *str);

char *
render_url (apr_pool_t *p, const char *prefix, const char *url);

char *
escape_html_attr (apr_pool_t *p, const char *raw);

int
is_input_valid(const char *val);

int
is_legal_XML(unsigned char *c);

int
is_legal_UTF8(unsigned char *source, char length);
