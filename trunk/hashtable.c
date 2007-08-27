/* Nice hash table for Apache runtime. */
/* rsr note: shouldn't this whole pile of code be replaced with the
   hash implementation that is part of the Apache runtime? Does this
   code provide features not present in the normal Apache APR hash
   routines? */

#include <apr.h>
#include <httpd.h>

#include "hashtable.h"

typedef struct {
  const char *key;
  void *val;
} HashBucket;

struct _HashTable {
  int n;
  int n_max;
  HashBucket **buckets;
};

struct _HashTableIter {
  const HashTable *ht;
  int index;
};

/* This hash function, the choice of 2^k for the size, and the linear
   search are all relatively unsophisticated. It should be good enough
   for government use, though. */
static unsigned int
hash_func (const char *string)
{
  unsigned int result;
  int c;
  int i;

  result = 0;
  for (i = 0; (c = ((unsigned char *)string)[i]) != '\0'; i++)
    result += (result << 3) + c;

  return result;
}

HashTable *
virgule_hash_table_new (apr_pool_t *p)
{
  HashTable *result;
  result = (HashTable *)apr_palloc (p, sizeof(HashTable));

  result->n = 0;
  result->n_max = 4;
  result->buckets = (HashBucket **)apr_palloc (p, sizeof(HashBucket *) * result->n_max);

  memset (result->buckets, 0, sizeof(HashBucket *) * result->n_max);

  return result;
}

void *
virgule_hash_table_get (const HashTable *ht, const char *key)
{
  unsigned int hash;

  hash = hash_func (key) % ht->n_max;

  while (ht->buckets[hash] != NULL)
    {
      if (!strcmp (ht->buckets[hash]->key, key))
	return ht->buckets[hash]->val;
      hash = (hash + 1) % ht->n_max;
    }
  return NULL;
}

static void
hash_table_insert_bucket (apr_pool_t *p, HashBucket **buckets, int n_max,
		     HashBucket *bucket)
{
  unsigned int hash;

  hash = hash_func (bucket->key) % n_max;

  while (buckets[hash] != NULL)
    hash = (hash + 1) % n_max;

  buckets[hash] = bucket;
}

/* Internal insert function. Assumes that key is not already present. */
static void
hash_table_insert (apr_pool_t *p, HashBucket **buckets, int n_max,
	     const char *key, void *val)
{
  HashBucket *bucket = (HashBucket *)apr_palloc (p, sizeof(HashBucket));

  bucket->key = key;
  bucket->val = val;
  hash_table_insert_bucket (p, buckets, n_max, bucket);
}

void
virgule_hash_table_set (apr_pool_t *p, HashTable *ht, const char *key, void *val)
{
  unsigned int hash;
  int n_max;

  hash = hash_func (key) % ht->n_max;

  while (ht->buckets[hash] != NULL)
    {
      if (!strcmp (ht->buckets[hash]->key, key))
	{
	  ht->buckets[hash]->val = val;
	  return;
	}
      hash = (hash + 1) % ht->n_max;
    }
  ht->n++;
  n_max = ht->n_max;
  if (ht->n > (n_max >> 1))
    {
      /* rehash */

      int new_n_max = n_max << 1;
      HashBucket **old_buckets = ht->buckets;
      HashBucket **new_buckets = (HashBucket **)apr_palloc (p, sizeof(HashBucket *) * new_n_max);
      int i;

      memset (new_buckets, 0, sizeof(HashBucket *) * new_n_max); 

      for (i = 0; i < n_max; i++)
	{
	  if (old_buckets[i] != NULL)
	    hash_table_insert_bucket (p, new_buckets, new_n_max,
				      old_buckets[i]);
	}
      hash_table_insert (p, new_buckets, new_n_max, key, val);
      ht->n_max = new_n_max;
      ht->buckets = new_buckets;
    }
  else
    {
      HashBucket *bucket = (HashBucket *)apr_palloc (p, sizeof(HashBucket));
      ht->buckets[hash] = bucket;
      bucket->key = key;
      bucket->val = val;
    }
}

HashTableIter *
virgule_hash_table_iter (apr_pool_t *p, const HashTable *ht)
{
  HashTableIter *result;
  result = (HashTableIter *)apr_palloc (p, sizeof(HashTableIter));

  result->ht = ht;
  result->index = 0;

  return result;
}

int
virgule_hash_table_iter_get (HashTableIter *iter, const char **pkey, void **pval)
{
  const HashTable *ht = iter->ht;

  for (; iter->index < ht->n_max; iter->index++)
    {
      if (ht->buckets[iter->index] != NULL)
	{
	  *pkey = ht->buckets[iter->index]->key;
	  *pval = ht->buckets[iter->index]->val;
	  return 1;
	}
    }

  return 0;
}

void
virgule_hash_table_iter_next (HashTableIter *iter)
{
  iter->index++;
}
