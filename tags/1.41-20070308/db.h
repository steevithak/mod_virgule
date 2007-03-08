typedef struct _Db Db;
typedef struct _DbCursor DbCursor;
typedef struct _DbLock DbLock;

Db *
virgule_db_new_filesystem (apr_pool_t *p, const char *base_pathname);

/* This one is only public for testing purposes. */
char *
virgule_db_mk_filename (apr_pool_t *p, Db *db, const char *key);

char *
virgule_db_get_p (apr_pool_t *p, Db *db, const char *key, int *p_size);

char *
virgule_db_get (Db *db, const char *key, int *p_size);

int
virgule_db_put_p (apr_pool_t *p, Db *db, const char *key, const char *val, int size);

int
virgule_db_put (Db *db, const char *key, const char *val, int size);

int
virgule_db_del (Db *db, const char *key);

int
virgule_db_is_dir (Db *db, const char *key);

DbCursor *
virgule_db_open_dir (Db *db, const char *key);

char *
virgule_db_read_dir (DbCursor *dbc);

char *
virgule_db_read_dir_raw (DbCursor *dbc);

int
virgule_db_close_dir (DbCursor *dbc);

int
virgule_db_dir_max (Db *db, const char *key);

DbLock *
virgule_db_lock_key (Db *db, const char *key, int cmd);

DbLock *
virgule_db_lock (Db *db);

int
virgule_db_lock_upgrade (DbLock *dbl);

int
virgule_db_lock_downgrade (DbLock *dbl);

int
virgule_db_unlock (DbLock *dbl);
