void
virgule_acct_person_index_serve (VirguleReq *vr, int max);

int
virgule_acct_set_lastread(VirguleReq *vr, const char *section, const char *location, int last_read);

int
virgule_acct_get_lastread(VirguleReq *vr, const char *section, const char *location);

int
virgule_acct_get_num_old(VirguleReq *vr);

char *
virgule_acct_get_lastread_date(VirguleReq *vr, const char *section, const char *location);

char *
virgule_validate_username (VirguleReq *vr, const char *u);

char *
virgule_acct_dbkey (VirguleReq *vr, const char *u);

int
virgule_acct_login (VirguleReq *vr, const char *u, const char *pass,
	    const char **ret1, const char **ret2);

int
virgule_acct_maint_serve (VirguleReq *vr);

void
virgule_acct_touch(VirguleReq *vr, const char *u);

void
virgule_acct_update_art_index(VirguleReq *vr, int art);
