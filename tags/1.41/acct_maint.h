int
acct_set_lastread(VirguleReq *vr, const char *section, const char *location, int last_read);

int
acct_get_lastread(VirguleReq *vr, const char *section, const char *location);

int
acct_get_num_old(VirguleReq *vr);

char *
acct_get_lastread_date(VirguleReq *vr, const char *section, const char *location);

char *
validate_username (const char *u);

char *
acct_dbkey (pool *p, const char *u);

int
acct_login (VirguleReq *vr, const char *u, const char *pass,
	    const char **ret1, const char **ret2);

int
acct_maint_serve (VirguleReq *vr);