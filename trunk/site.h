int
virgule_site_serve (VirguleReq *vr);

void
virgule_site_render_banner_ad (VirguleReq *vr);

int
virgule_site_send_banner_ad (VirguleReq *vr);

int
virgule_site_render_page (VirguleReq *vr, xmlNode *node, char *itag, char *istr, char *title);

int
virgule_conf_to_gray (double confidence);

void
virgule_site_render_person_link (VirguleReq *vr, const char *name, CertLevel cl);
