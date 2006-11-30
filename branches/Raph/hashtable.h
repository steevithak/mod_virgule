/* Nice hash table for Apache runtime. */

typedef struct _HashTable HashTable;
typedef struct _HashTableIter HashTableIter;

HashTable *
hash_table_new (pool *p);

void *
hash_table_get (const HashTable *ht, const char *key);

void
hash_table_set (pool *p, HashTable *ht, const char *key, void *val);

HashTableIter *
hash_table_iter (pool *p, const HashTable *ht);

int
hash_table_iter_get (HashTableIter *iter, const char **pkey, void **pval);

void
hash_table_iter_next (HashTableIter *iter);
