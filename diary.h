void
virgule_diary_entry_render (VirguleReq *vr, const char *u, int n, EigenVecEl *ev, int hcert);

int
virgule_diary_exists (VirguleReq *vr, const char *u);

void
virgule_diary_render (VirguleReq *vr, const char *u, int max_num, int start);

//void
//virgule_diary_latest_render (VirguleReq *vr, const char *u, int n);

char *
virgule_diary_get_backup(VirguleReq *vr);

int
virgule_diary_serve (VirguleReq *vr);

/*
int
virgule_diary_export (VirguleReq *vr, xmlNode *root, char *u);
*/

int
virgule_diary_rss_export (VirguleReq *vr, xmlNode *root, char *u);

int
virgule_diary_store_feed_item (VirguleReq *vr, xmlChar *user, FeedItem *item);

int
virgule_diary_update_feed_item (VirguleReq *vr, xmlChar *user, FeedItem *item, int e);

int
virgule_diary_store_entry (VirguleReq *vr, const char *key, const char *entry);

time_t
virgule_diary_latest_feed_entry (VirguleReq *vr, xmlChar *u);

int
virgule_diary_entry_id_exists(VirguleReq *vr, xmlChar *u, char *id);
