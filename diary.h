void
virgule_diary_render (VirguleReq *vr, const char *u, int max_num, int start);

void
virgule_diary_latest_render (VirguleReq *vr, const char *u, int n);

char *
virgule_diary_get_backup(VirguleReq *vr);

int
virgule_diary_serve (VirguleReq *vr);

int
virgule_diary_export (VirguleReq *vr, xmlNode *root, char *u);

int
virgule_diary_rss_export (VirguleReq *vr, xmlNode *root, char *u);

int
virgule_diary_store_entry (VirguleReq *vr, const char *key, const char *entry);
