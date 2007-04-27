/* Eigenvector-based generic metadata engine. */

#include <stdlib.h>
#include <time.h>
#include <math.h>

#include <apr.h>
#include <apr_strings.h>
#include <httpd.h>

#include <libxml/tree.h>

#include "private.h"
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
virgule_eigen_local_load (apr_pool_t *p, VirguleReq *vr, const char *dbkey)
{
  HashTable *result;
  int val_size;
  char *val = virgule_db_get_p (p, vr->db, dbkey, &val_size);
  int i;

  if (val == NULL)
    return NULL;

  result = virgule_hash_table_new (p);
  for (i = 0; i < val_size;)
    {
      EigenLocal *el = (EigenLocal *)apr_palloc (p, sizeof(EigenLocal));
      char *ts;
      char *rt;
      char *subj;

      i = parse3 (val, val_size, i, &ts, &rt, &subj);
      el->rating = atof (rt);
      el->timestamp = ts;
      virgule_hash_table_set (p, result, subj, (void *)el);
    }
  return result;
}

void
virgule_eigen_local_store (VirguleReq *vr, HashTable *ht, const char *dbkey)
{
  apr_pool_t *p = vr->r->pool;
  HashTableIter *iter;
  const char *key;
  void *val;
  char *bigbuf;
  Buffer *b = virgule_buffer_new (p);

  /* todo: we probably want a subpool */
  for (iter = virgule_hash_table_iter (p, ht);
       virgule_hash_table_iter_get (iter, &key, &val);
       virgule_hash_table_iter_next (iter))
    {
      EigenLocal *el = (EigenLocal *)val;
      virgule_buffer_printf (b, "%s %.2g %s\n", el->timestamp, el->rating, key);
    }
  bigbuf = virgule_buffer_extract (b);
  virgule_db_put (vr->db, dbkey, bigbuf, strlen(bigbuf));
}

int
virgule_eigen_set_local (VirguleReq *vr, const char *subj, double rating)
{
  apr_pool_t *p = vr->r->pool;
  char *dbkey;
  EigenLocal *el = (EigenLocal *)apr_palloc (p, sizeof(EigenLocal));
  HashTable *elt;

  dbkey = apr_pstrcat (p, "eigen/local/", vr->u, NULL);
  elt = virgule_eigen_local_load (p, vr, dbkey);

  if (elt == NULL)
    elt = virgule_hash_table_new (p);

  el->rating = rating;
  el->timestamp = ap_ht_time (p, (apr_time_t) (time (NULL)) * 1000000,
                             "%Y%m%dT%H%M%SZ", 1);
  virgule_hash_table_set (p, elt, subj, (void *)el);

  virgule_eigen_local_store (vr, elt, dbkey);
  return 0;
}

HashTable *
virgule_eigen_vec_load (apr_pool_t *p, VirguleReq *vr, const char *dbkey)
{
  HashTable *result;
  int val_size;
  char *val = virgule_db_get_p (p, vr->db, dbkey, &val_size);
  int i;

  result = virgule_hash_table_new (p);

  if (val == NULL)
    return result;

  for (i = 0; i < val_size;)
    {
      EigenVecEl *eve = (EigenVecEl *)apr_palloc (p, sizeof(EigenVecEl));
      char *conf;
      char *rt;
      char *rt_sq;
      char *subj;

      i = parse4 (val, val_size, i, &conf, &rt, &rt_sq, &subj);
      eve->confidence = atof (conf);
      eve->rating = atof (rt);
      eve->rating_sq = atof (rt_sq); 
      virgule_hash_table_set (p, result, subj, (void *)eve);
    }
  return result;
}


/**
 * rsr notes: Couldn't we use an APR sprintf string allocation function
 * instead of all the overhead of creating a temporary virgule buffer here?
 **/
static void
eigen_vec_store (apr_pool_t *p, VirguleReq *vr, HashTable *ht, const char *dbkey)
{
  HashTableIter *iter;
  const char *key;
  void *val;
  char *bigbuf;
  Buffer *b = virgule_buffer_new (p);

  /* todo: we probably want a subpool */
  for (iter = virgule_hash_table_iter (p, ht);
       virgule_hash_table_iter_get (iter, &key, &val);
       virgule_hash_table_iter_next (iter))
    {
      EigenVecEl *eve = (EigenVecEl *)val;
      virgule_buffer_printf (b, "%.3g %.5g %.5g %s\n",
		     eve->confidence, eve->rating, eve->rating_sq, key);
    }
  bigbuf = virgule_buffer_extract (b);
  virgule_db_put_p (p, vr->db, dbkey, bigbuf, strlen(bigbuf));
}

/* Add in a vector from another user. */
static void
eigen_add_in (apr_pool_t *p, VirguleReq *vr, HashTable *ev, const char *u)
{
  char *dbkey;
  HashTable *succ_ev;
  HashTableIter *iter;
  const char *key;
  EigenVecEl *val;

  dbkey = apr_pstrcat (p, "eigen/vec/", u, NULL);
  succ_ev = virgule_eigen_vec_load (p, vr, dbkey);
  for (iter = virgule_hash_table_iter (p, succ_ev);
       virgule_hash_table_iter_get (iter, &key, (void **)&val);
       virgule_hash_table_iter_next (iter))
    {
      EigenVecEl *eve = virgule_hash_table_get (ev, key);
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
	  virgule_hash_table_set (p, ev, key, (void *)val);
	}
    }
}

/* One iteration, for one particular user. */
int
virgule_eigen_crank (apr_pool_t *p, VirguleReq *vr, const char *u)
{
  double damping = 0.95;
  char *dbkey;
  xmlDoc *profile;
  xmlNode *tree;
  int n_succ = 0;
  HashTable *ev = virgule_hash_table_new (p);
  HashTable *el;
  HashTableIter *iter;
  const char *key;
  EigenLocal *val;
  EigenVecEl *eve;

  dbkey = virgule_acct_dbkey (vr, u);
  profile = virgule_db_xml_get (p, vr->db, dbkey);
  if (profile == NULL)
    return -1;

  tree = virgule_xml_find_child (profile->xmlRootNode, "certs");
  if (tree)
    {
      xmlNode *cert;

      for (cert = tree->children; cert != NULL; cert = cert->next)
	if (cert->type == XML_ELEMENT_NODE &&
	    !strcmp ((char *)cert->name, "cert"))
	  {
	    char *subject;
	    char *level;

	    subject = (char *)xmlGetProp (cert, (xmlChar *)"subj");
	    level = (char *)xmlGetProp (cert, (xmlChar *)"level");
	    if (strcmp (level, virgule_cert_level_to_name (vr, 0)) &&
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

      for (iter = virgule_hash_table_iter (p, ev);
	   virgule_hash_table_iter_get (iter, &key, (void **)&eve);
	   virgule_hash_table_iter_next (iter))
	{
	  eve->rating /= eve->confidence;
	  eve->rating_sq /= eve->confidence;
	  eve->confidence *= scale;
	}
    }

  dbkey = apr_pstrcat (p, "eigen/local/", u, NULL);
  el = virgule_eigen_local_load (p, vr, dbkey);
  if (el)
    {
      for (iter = virgule_hash_table_iter (p, el);
	   virgule_hash_table_iter_get (iter, &key, (void **)&val);
	   virgule_hash_table_iter_next (iter))
	{
	  eve = virgule_hash_table_get (ev, key);
	  if (eve == NULL)
	    {
	      eve = (EigenVecEl *)apr_palloc (p, sizeof(EigenVecEl));
	      virgule_hash_table_set (p, ev, key, (void *)eve);
	    }
	  eve->confidence = 1.0;
	  eve->rating = val->rating;
	  eve->rating_sq = val->rating * val->rating;
	}
    }

  dbkey = apr_pstrcat (p, "eigen/vec/", u, NULL);
  eigen_vec_store (p, vr, ev, dbkey);
  return 0;
}

/* Report results, for debugging purposes. */
int
virgule_eigen_report (VirguleReq *vr, const char *u)
{
  apr_pool_t *p = vr->r->pool;
  char *dbkey;
  HashTable *ev;
  HashTableIter *iter;
  const char *key;
  EigenVecEl *val;

  dbkey = apr_pstrcat (p, "eigen/vec/", u, NULL);
  ev = virgule_eigen_vec_load (p, vr, dbkey);
  for (iter = virgule_hash_table_iter (p, ev);
       virgule_hash_table_iter_get (iter, &key, (void **)&val);
       virgule_hash_table_iter_next (iter))
    {
      /* The fabs here is a hack to avoid NaN's when roundoff errors
	 push the difference negative. */
      virgule_buffer_printf (vr->b, "%s: %.2g &plusmn;%.1f (confidence %.2g)<br>\n",
		     virgule_nice_text (p, key), val->rating,
		     sqrt (fabs (val->rating_sq - val->rating * val->rating)),
		     val->confidence);
    }
  return 0;
}


/**
 * virgule_eigen_cleanup: Remove any eigen data associated with the passed
 * user account. Called by the acct_kill function.
 **/
void
virgule_eigen_cleanup (VirguleReq *vr, const char *u)
{
  char *dbkey;
  dbkey = apr_pstrcat (vr->r->pool, "eigen/vec/", u, NULL);
  virgule_db_del (vr->db, dbkey);
  dbkey = apr_pstrcat (vr->r->pool, "eigen/local/", u, NULL);
  virgule_db_del (vr->db, dbkey);
}
