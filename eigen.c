/* Eigenvector-based generic metadata engine. */

#include <stdlib.h>
#include <math.h>

#include "httpd.h"

#include <tree.h>

#include "db.h"
#include "buffer.h"
#include "req.h"
#include "hashtable.h"
#include "db_xml.h"
#include "xml_util.h"
#include "util.h"
#include "acct_maint.h"
#include "certs.h"
#include "eigen.h"

/* On-disk format is one entry per line, of format:
   timestamp rating subj
*/
typedef struct {
  double rating;
  const char *timestamp;
} EigenLocal;

static int
parse3 (char *val, int val_size, int i, char **p0, char **p1, char **p2)
{
  /* todo: better sanitychecking */
  *p0 = val + i;
  while (i < val_size && val[i] != ' ')
    i++;
  val[i++] = 0;

  *p1 = val + i;
  while (i < val_size && val[i] != ' ')
    i++;
  val[i++] = 0;

  *p2 = val + i;
  while (i < val_size && val[i] != '\n')
    i++;
  val[i++] = 0;

  return i;
}

static int
parse4 (char *val, int val_size, int i,
	char **p0, char **p1, char **p2, char **p3)
{
  /* todo: better sanitychecking */
  *p0 = val + i;
  while (i < val_size && val[i] != ' ')
    i++;
  val[i++] = 0;

  *p1 = val + i;
  while (i < val_size && val[i] != ' ')
    i++;
  val[i++] = 0;

  *p2 = val + i;
  while (i < val_size && val[i] != ' ')
    i++;
  val[i++] = 0;

  *p3 = val + i;
  while (i < val_size && val[i] != '\n')
    i++;
  val[i++] = 0;

  return i;
}

HashTable *
eigen_local_load (pool *p, VirguleReq *vr, const char *dbkey)
{
  HashTable *result;
  int val_size;
  char *val = db_get_p (p, vr->db, dbkey, &val_size);
  int i;

  if (val == NULL)
    return NULL;

  result = hash_table_new (p);
  for (i = 0; i < val_size;)
    {
      EigenLocal *el = (EigenLocal *)ap_palloc (p, sizeof(EigenLocal));
      char *ts;
      char *rt;
      char *subj;

      i = parse3 (val, val_size, i, &ts, &rt, &subj);
      el->rating = atof (rt);
      el->timestamp = ts;
      hash_table_set (p, result, subj, (void *)el);
    }
  return result;
}

void
eigen_local_store (VirguleReq *vr, HashTable *ht, const char *dbkey)
{
  pool *p = vr->r->pool;
  HashTableIter *iter;
  const char *key;
  void *val;
  char *bigbuf;
  Buffer *b = buffer_new (p);

  /* todo: we probably want a subpool */
  for (iter = hash_table_iter (p, ht);
       hash_table_iter_get (iter, &key, &val); hash_table_iter_next (iter))
    {
      EigenLocal *el = (EigenLocal *)val;
      buffer_printf (b, "%s %.2g %s\n", el->timestamp, el->rating, key);
    }
  bigbuf = buffer_extract (b);
  db_put (vr->db, dbkey, bigbuf, strlen(bigbuf));
}

int
eigen_set_local (VirguleReq *vr, const char *subj, double rating)
{
  pool *p = vr->r->pool;
  char *dbkey;
  EigenLocal *el = (EigenLocal *)ap_palloc (p, sizeof(EigenLocal));
  HashTable *elt;

  dbkey = ap_pstrcat (p, "eigen/local/", vr->u, NULL);
  elt = eigen_local_load (p, vr, dbkey);

  if (elt == NULL)
    elt = hash_table_new (p);

  el->rating = rating;
  el->timestamp = ap_ht_time (p, time (NULL), "%Y%m%dT%H%M%SZ", 1);
  hash_table_set (p, elt, subj, (void *)el);

  eigen_local_store (vr, elt, dbkey);
  return 0;
}

HashTable *
eigen_vec_load (pool *p, VirguleReq *vr, const char *dbkey)
{
  HashTable *result;
  int val_size;
  char *val = db_get_p (p, vr->db, dbkey, &val_size);
  int i;

  result = hash_table_new (p);

  if (val == NULL)
    return result;

  for (i = 0; i < val_size;)
    {
      EigenVecEl *eve = (EigenVecEl *)ap_palloc (p, sizeof(EigenVecEl));
      char *conf;
      char *rt;
      char *rt_sq;
      char *subj;

      i = parse4 (val, val_size, i, &conf, &rt, &rt_sq, &subj);
      eve->confidence = atof (conf);
      eve->rating = atof (rt);
      eve->rating_sq = atof (rt_sq); 
      hash_table_set (p, result, subj, (void *)eve);
    }
  return result;
}

static void
eigen_vec_store (pool *p, VirguleReq *vr, HashTable *ht, const char *dbkey)
{
  HashTableIter *iter;
  const char *key;
  void *val;
  char *bigbuf;
  Buffer *b = buffer_new (p);

  /* todo: we probably want a subpool */
  for (iter = hash_table_iter (p, ht);
       hash_table_iter_get (iter, &key, &val); hash_table_iter_next (iter))
    {
      EigenVecEl *eve = (EigenVecEl *)val;
      buffer_printf (b, "%.3g %.5g %.5g %s\n",
		     eve->confidence, eve->rating, eve->rating_sq, key);
    }
  bigbuf = buffer_extract (b);
  db_put_p (p, vr->db, dbkey, bigbuf, strlen(bigbuf));
}

/* Add in a vector from another user. */
static void
eigen_add_in (pool *p, VirguleReq *vr, HashTable *ev, const char *u)
{
  char *dbkey;
  HashTable *succ_ev;
  HashTableIter *iter;
  const char *key;
  EigenVecEl *val;

  dbkey = ap_pstrcat (p, "eigen/vec/", u, NULL);
  succ_ev = eigen_vec_load (p, vr, dbkey);
  for (iter = hash_table_iter (p, succ_ev);
       hash_table_iter_get (iter, &key, (void **)&val);
       hash_table_iter_next (iter))
    {
      EigenVecEl *eve = hash_table_get (ev, key);
      if (eve)
	{
	  eve->confidence += val->confidence;
	  eve->rating += val->confidence * val->rating;
	  eve->rating_sq += val->confidence * val->rating_sq;
	}
      else
	{
	  val->rating *= val->confidence;
	  val->rating_sq *= val->confidence;
	  hash_table_set (p, ev, key, (void *)val);
	}
    }
}

/* One iteration, for one particular user. */
int
eigen_crank (pool *p, VirguleReq *vr, const char *u)
{
  double damping = 0.95;
  char *dbkey;
  xmlDoc *profile;
  xmlNode *tree;
  int n_succ = 0;
  HashTable *ev = hash_table_new (p);
  HashTable *el;
  HashTableIter *iter;
  const char *key;
  EigenLocal *val;
  EigenVecEl *eve;

  dbkey = acct_dbkey (p, u);
  profile = db_xml_get (p, vr->db, dbkey);
  if (profile == NULL)
    return -1;

  tree = xml_find_child (profile->root, "certs");
  if (tree)
    {
      xmlNode *cert;

      for (cert = tree->childs; cert != NULL; cert = cert->next)
	if (cert->type == XML_ELEMENT_NODE &&
	    !strcmp (cert->name, "cert"))
	  {
	    char *subject;
	    char *level;

	    subject = xmlGetProp (cert, "subj");
	    level = xmlGetProp (cert, "level");
	    if (strcmp (level, cert_level_to_name (vr, 0)) &&
		strcmp (u, subject))
	      {
		eigen_add_in (p, vr, ev, subject);
		n_succ++;
	      }
	  }
    }

  if (n_succ)
    {
      double scale = damping / n_succ;

      for (iter = hash_table_iter (p, ev);
	   hash_table_iter_get (iter, &key, (void **)&eve);
	   hash_table_iter_next (iter))
	{
	  eve->rating /= eve->confidence;
	  eve->rating_sq /= eve->confidence;
	  eve->confidence *= scale;
	}
    }

  dbkey = ap_pstrcat (p, "eigen/local/", u, NULL);
  el = eigen_local_load (p, vr, dbkey);
  if (el)
    {
      for (iter = hash_table_iter (p, el);
	   hash_table_iter_get (iter, &key, (void **)&val);
	   hash_table_iter_next (iter))
	{
	  eve = hash_table_get (ev, key);
	  if (eve == NULL)
	    {
	      eve = (EigenVecEl *)ap_palloc (p, sizeof(EigenVecEl));
	      hash_table_set (p, ev, key, (void *)eve);
	    }
	  eve->confidence = 1.0;
	  eve->rating = val->rating;
	  eve->rating_sq = val->rating * val->rating;
	}
    }

  dbkey = ap_pstrcat (p, "eigen/vec/", u, NULL);
  eigen_vec_store (p, vr, ev, dbkey);
  return 0;
}

/* Report results, for debugging purposes. */
int
eigen_report (VirguleReq *vr, const char *u)
{
  pool *p = vr->r->pool;
  char *dbkey;
  HashTable *ev;
  HashTableIter *iter;
  const char *key;
  EigenVecEl *val;

  dbkey = ap_pstrcat (p, "eigen/vec/", u, NULL);
  ev = eigen_vec_load (p, vr, dbkey);
  for (iter = hash_table_iter (p, ev);
       hash_table_iter_get (iter, &key, (void **)&val);
       hash_table_iter_next (iter))
    {
      /* The fabs here is a hack to avoid NaN's when roundoff errors
	 push the difference negative. */
      buffer_printf (vr->b, "%s: %.2g &plusmn;%.1f (confidence %.2g)<br>\n",
		     nice_text (p, key), val->rating,
		     sqrt (fabs (val->rating_sq - val->rating * val->rating)),
		     val->confidence);
    }
  return 0;
}
