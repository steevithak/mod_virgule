void
diary_render (VirguleReq *vr, const char *u, int max_num, int start);

void
diary_latest_render (VirguleReq *vr, const char *u, int n);

char *
diary_get_backup(VirguleReq *vr);

int
diary_serve (VirguleReq *vr);

int
diary_export (VirguleReq *vr, xmlNode *root, char *u);

int
diary_rss_export (VirguleReq *vr, xmlNode *root, char *u);

int
diary_store_entry (VirguleReq *vr, const char *key, const char *entry);
