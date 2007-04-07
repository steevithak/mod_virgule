#define STYLE

void
virgule_render_header_raw (VirguleReq *vr, const char *title, const char *head_content);

void
virgule_render_header (VirguleReq *vr, const char *title, const char *head_content);

void
virgule_render_userstats (VirguleReq *vr);

void
virgule_render_sitemap (VirguleReq *vr, int enclose);

int
virgule_render_footer_send (VirguleReq *vr);

int
virgule_send_error_page (VirguleReq *vr, const char *error_short,
		 const char *fmt, ...);

char *
virgule_render_date (VirguleReq *vr, const char *iso, int dateonly);

int 
virgule_set_temp_buffer (VirguleReq *vr);

void
virgule_set_main_buffer (VirguleReq *vr);

int
virgule_render_in_template (VirguleReq *vr, char *tpath, char *tagname, char *title);
