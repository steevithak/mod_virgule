void
render_header_raw (VirguleReq *vr, const char *title);

void
render_header (VirguleReq *vr, const char *title);

void
render_sitemap (VirguleReq *vr, int enclose);

void
render_footer (VirguleReq *vr);

int
render_footer_send (VirguleReq *vr);

int
render_table_open (VirguleReq *vr);

int
render_table_close (VirguleReq *vr);

int
send_error_page (VirguleReq *vr, const char *error_short,
		 const char *fmt, ...);

char *
render_date (VirguleReq *vr, const char *iso);
