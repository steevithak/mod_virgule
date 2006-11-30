/* This file contains a number of useful operations based on the db
   layer below, such as keeping a list of recent updates, and so
   on. As I develop advogato, it's likely that I'll put in schemas for
   relations, ontology, indexing, and some other things. */


int
virgule_add_recent (apr_pool_t *p, Db *db, const char *key, const char *val, int n_max, int dup);


/* Relations */

typedef struct _DbRelation DbRelation;
typedef struct _DbField DbField;
typedef struct _DbSelect DbSelect;

typedef enum {
  DB_FIELD_INDEX = 1,
  DB_FIELD_UNIQUE = 2
} DbFieldFlags;


typedef enum {
  DB_REL_DUMMY
  /* I'm sure there is stuff in here relevant to date handling */
} DbRelFlags;

struct _DbField {
  char *name;
  char *prefix;
  DbFieldFlags flags;
};

struct _DbRelation {
  char *name;
  int n_fields;
  DbField *fields;
  DbRelFlags flags;
};

int
virgule_db_relation_put (apr_pool_t *p, Db *db, const DbRelation *rel, const char **values);

void
virgule_remove_recent (VirguleReq *vr, const char *key, const char *val);
