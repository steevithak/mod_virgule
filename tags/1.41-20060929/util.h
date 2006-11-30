
char *
virgule_rand_cookie (apr_pool_t *p);

const char *
virgule_match_prefix (const char *url, const char *prefix);

char *
virgule_nice_text (apr_pool_t *p, const char *raw);

const NavOption *
virgule_add_nav_option (VirguleReq *vr, const char *label, const char *url);

const Topic *
virgule_add_topic (VirguleReq *vr, const char *desc, const char *url);

const AllowedTag *
virgule_add_allowed_tag (VirguleReq *vr, const char *tag, int can_be_empty);

int
virgule_render_acceptable_html (VirguleReq *vr);

char *
virgule_strip_a (VirguleReq *vr, const char *raw);

char *
virgule_nice_htext (VirguleReq *vr, const char *raw, char **p_error);

char *
virgule_iso_now (apr_pool_t *p);

time_t
virgule_iso_to_time_t (const char *iso);

char *
virgule_str_subst (apr_pool_t *p, const char *str, const char *pattern, const char *repl);

char *
virgule_escape_uri_arg (apr_pool_t *p, const char *str);

char *
virgule_render_url (apr_pool_t *p, const char *prefix, const char *url);

char *
virgule_escape_html_attr (apr_pool_t *p, const char *raw);

int
virgule_is_input_valid(const char *val);

//int
//virgule_is_legal_XML(unsigned char *c);

//int
//is_legal_UTF8(unsigned char *source, char length);
