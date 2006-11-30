char *
b64enc (pool *p, const char *data, int size);

char *
rand_cookie (pool *p);

const char *
match_prefix (const char *url, const char *prefix);

char *
nice_text (pool *p, const char *raw);

const AllowedTag *
add_allowed_tag (VirguleReq *vr, const char *tag, int can_be_empty);

int
render_acceptable_html (VirguleReq *vr);

char *
nice_htext (VirguleReq *vr, const char *raw, char **p_error);

char *
iso_now (pool *p);

time_t
iso_to_time_t (const char *iso);

char *
str_subst (pool *p, const char *str, const char *pattern, const char *repl);

char *
escape_uri_arg (pool *p, const char *str);

char *
render_url (pool *p, const char *prefix, const char *url);

char *
escape_html_attr (pool *p, const char *raw);
