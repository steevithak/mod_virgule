/* Nice hash table for Apache runtime. */

typedef struct _HashTable HashTable;
typedef struct _HashTableIter HashTableIter;

HashTable *
virgule_hash_table_new (apr_pool_t *p);

void *
virgule_hash_table_get (const HashTable *ht, const char *key);

void
virgule_hash_table_set (apr_pool_t *p, HashTable *ht, const char *key, void *val);

HashTableIter *
virgule_hash_table_iter (apr_pool_t *p, const HashTable *ht);

int
virgule_hash_table_iter_get (HashTableIter *iter, const char **pkey, void **pval);

void
virgule_hash_table_iter_next (HashTableIter *iter);
