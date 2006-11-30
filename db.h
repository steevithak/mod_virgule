typedef struct _Db Db;
typedef struct _DbCursor DbCursor;
typedef struct _DbLock DbLock;

Db *
db_new_filesystem (pool *p, const char *base_pathname);

/* This one is only public for testing purposes. */
char *
db_mk_filename (pool *p, Db *db, const char *key);

char *
db_get_p (pool *p, Db *db, const char *key, int *p_size);

char *
db_get (Db *db, const char *key, int *p_size);

int
db_put_p (pool *p, Db *db, const char *key, const char *val, int size);

int
db_put (Db *db, const char *key, const char *val, int size);

int
db_is_dir (Db *db, const char *key);

DbCursor *
db_open_dir (Db *db, const char *key);

char *
db_read_dir (DbCursor *dbc);

char *
db_read_dir_raw (DbCursor *dbc);

int
db_close_dir (DbCursor *dbc);

int
db_dir_max (Db *db, const char *key);

DbLock *
db_lock_key (Db *db, const char *key, int cmd);

DbLock *
db_lock (Db *db);

int
db_lock_upgrade (DbLock *dbl);

int
db_unlock (DbLock *dbl);
