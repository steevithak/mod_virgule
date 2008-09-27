
char *
virgule_rand_cookie (apr_pool_t *p);

const char *
virgule_match_prefix (const char *url, const char *prefix);

char *
virgule_nice_text (apr_pool_t *p, const char *raw);

char *
virgule_nice_utf8 (apr_pool_t *p, const char *raw);

const NavOption *
virgule_add_nav_option (VirguleReq *vr, const char *label, const char *url);

const Topic *
virgule_add_topic (VirguleReq *vr, const char *desc, const char *url);

const AllowedTag *
virgule_add_allowed_tag (VirguleReq *vr, const char *tagname, int can_be_empty,
		char **allowed_attributes);

int
virgule_render_acceptable_html (VirguleReq *vr);

int
virgule_user_is_special (VirguleReq *vr, const char *user);

char *
virgule_add_nofollow (VirguleReq *vr, const char *raw);

char *
virgule_strip_a (VirguleReq *vr, const char *raw);

char *
virgule_nice_htext (VirguleReq *vr, const char *raw, char **p_error);

char *
virgule_force_legal_css_name (VirguleReq *vr, const char *name);

char *
virgule_iso_now (apr_pool_t *p);

time_t
virgule_iso_to_time_t (const char *iso);

char *
virgule_time_t_to_iso (VirguleReq *vr, time_t t);

time_t
virgule_rfc822_to_time_t (VirguleReq *vr, const char *time_string);

time_t
virgule_rfc3339_to_time_t (VirguleReq *vr, const char *time_string);

time_t
virgule_virgule_to_time_t (VirguleReq *vr, const char *time_string);

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

char *
virgule_sha1(apr_pool_t *p, const char *input);

char *
virgule_youtube_link (VirguleReq *vr, const char *id);
