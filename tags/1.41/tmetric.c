/* This is glue code to run the trust metric as HTML. */

#include "httpd.h"
#include "http_protocol.h"
#include <glib.h>

#include <tree.h>
#include <xmlmemory.h>

#include "buffer.h"
#include "db.h"
#include "req.h"
#include "db_xml.h"
#include "acct_maint.h"
#include "xml_util.h"
#include "certs.h"
#include "style.h"

#include "net_flow.h"
#include "tmetric.h"

typedef struct _NodeInfo NodeInfo;

struct _NodeInfo {
  const char *name;
  const char *givenname;
  const char *surname;
  CertLevel level;
};

static int cert_level_n;

static int
tmetric_find_node (array_header *info, NetFlow *flows[], const char *u)
{
  int idx;

  idx = net_flow_find_node (flows[1], u);
  if (idx >= info->nelts)
    {
      NodeInfo *ni = (NodeInfo *)ap_push_array (info);
      int i;

      ni->name = u;
      ni->surname = NULL;
      ni->level = CERT_LEVEL_NONE;
      for (i = 2; i < cert_level_n; i++)
	if (idx != net_flow_find_node (flows[i], u))
	  /* todo: warning */;
    }
  return idx;
}

static void
tmetric_set_name (array_header *info, NetFlow *flows[], const char *u,
		  const char *givenname, const char *surname)
{
  int idx;

  idx = tmetric_find_node (info, flows, u);
  ((NodeInfo *)info->elts)[idx].givenname = givenname;
  ((NodeInfo *)info->elts)[idx].surname = surname;
}

/**
 * tmetric_run: Run trust metric.
 * @vr: The request context.
 * @seeds: An array of usernames for the seed.
 * @n_seeds: Size of @seeds.
 * @caps: Capacity array.
 * @n_caps: Size of @caps.
 *
 * Return value: NodeInfo array.
 **/
static array_header *
tmetric_run (VirguleReq *vr,
	     const char *seeds[], int n_seeds,
	     const int *caps, int n_caps)
{
  pool *p = vr->r->pool;
  Db *db = vr->db;
  NetFlow *flows[cert_level_n];
  array_header *result;
  int i, j;
  int seed;
  int idx;
  DbCursor *dbc;
  char *issuer;

  
  result = ap_make_array (vr->r->pool, 16, sizeof(NodeInfo));

  for (i = 1; i < cert_level_n; i++)
    flows[i] = net_flow_new ();

  seed = tmetric_find_node (result, flows, "-");

  for (j = 0; j < n_seeds; j++)
    {
      (void) tmetric_find_node (result, flows, seeds[j]);
      for (i = 1; i < cert_level_n; i++)
	net_flow_add_edge (flows[i], "-", seeds[j]);
    }

  dbc = db_open_dir (db, "acct");
  while ((issuer = db_read_dir_raw (dbc)) != NULL)
    {
      char *db_key;
      xmlDoc *profile;
      xmlNode *tree;
      xmlNode *cert;
      const char *givenname, *surname;

#if 0
      issuer = ((NodeInfo *)(result->elts))[idx].name;
#endif
#if 0
      buffer_printf (vr->b, "issuer = %s\n", issuer);
#endif

      db_key = acct_dbkey (p, issuer);
      profile = db_xml_get (p, db, db_key);
      tree = xml_find_child (profile->root, "info");
      if (tree != NULL)
	{
	  tmetric_find_node (result, flows, issuer);
	  givenname = xml_get_prop (p, tree, "givenname");
	  surname = xml_get_prop (p, tree, "surname");
	  tmetric_set_name (result, flows, issuer, givenname, surname);
	  tree = xml_find_child (profile->root, "certs");
	  if (tree == NULL)
	    continue;
	  for (cert = tree->childs; cert != NULL; cert = cert->next)
	    {
	      if (cert->type == XML_ELEMENT_NODE &&
		  !strcmp (cert->name, "cert"))
		{
		  char *cert_subj;
		  
		  cert_subj = xml_get_prop (p, cert, "subj");
		  if (cert_subj)
		    {
		      char *cert_level;
		      CertLevel level;
		      
		      (void) tmetric_find_node (result, flows, cert_subj);
		      cert_level = xmlGetProp (cert, "level");
		      level = cert_level_from_name (vr, cert_level);
		      xmlFree (cert_level);
#if 0
		      buffer_printf (vr->b, "cert_subj = %s, level %d\n", cert_subj, level);
#endif
		      for (i = 1; i <= level; i++)
			net_flow_add_edge (flows[i], issuer, cert_subj);
		    }
		}
	    }
	}
      db_xml_free (p, db, profile);
    }
  db_close_dir (dbc);

  if (vr->lock)
    db_unlock (vr->lock);
  vr->lock = NULL;

  for (i = 1; i < cert_level_n; i++)
    {
      int *flow;
      net_flow_max_flow (flows[i], seed, caps, n_caps);
      flow = net_flow_extract (flows[i]);
      net_flow_free (flows[i]);
      for (idx = 1; idx < result->nelts; idx++)
	if (flow[idx])
	  ((NodeInfo *)(result->elts))[idx].level = i;
      g_free (flow);
    }

  return result;
}

static int
node_info_compare (const void *ni1, const void *ni2)
{
  const char *name1 = ((NodeInfo *)ni1)->surname;
  const char *name2 = ((NodeInfo *)ni2)->surname;
  int i;

  if (name1 == NULL || name1[0] == 0) name1 = ((NodeInfo *)ni1)->name;
  if (name2 == NULL || name2[0] == 0) name2 = ((NodeInfo *)ni2)->name;
  for (i = 0; name2[i]; i++)
    {
      int c1, c2;
      c1 = tolower (name1[i]);
      c2 = tolower (name2[i]);
      if (c1 != c2) return c1 - c2;
    }
  return name1[i];
}

static int
tmetric_index_serve (VirguleReq *vr)
{
  pool *p = vr->r->pool;
  Buffer *b = vr->b;
  Db *db = vr->db;
  array_header *nodeinfo;
  int n_seeds, n_caps;
  int i;
  NodeInfo *ni;
  DbLock *lock;
  int status;
  Buffer *cb;
  char *cache_str;

  lock = db_lock_key (db, "tmetric/.lock", F_SETLK);
  if (lock == NULL)
    return send_error_page (vr, "Lock taken", "The tmetric lock is taken by another process.");

  for (n_seeds = 0;; n_seeds++)
    if (!vr->seeds[n_seeds])
      break;
  for (n_caps = 0;; n_caps++)
    if (!vr->caps[n_caps])
      break;

  render_header (vr, "Trust Metric");
  nodeinfo = tmetric_run (vr, vr->seeds, n_seeds, vr->caps, n_caps);
  buffer_puts (b, "<table>\n");

  qsort (nodeinfo->elts, nodeinfo->nelts, sizeof(NodeInfo),
	 node_info_compare);

  cb = buffer_new (p);
  for (i = 0; i < nodeinfo->nelts; i++)
    {
      ni = &((NodeInfo *)(nodeinfo->elts))[i];
      if (strcmp(((NodeInfo *)ni)->name, "-") == 0) {
	/* Skip the root node */
	continue;
      }
      buffer_printf (b, "<tr><td><a href=\"../person/%s/\">%s</a></td> <td>%s %s</td> <td class=\"level%d\">%s</td></tr>\n",
		     ni->name,
		     ni->name,
		     ni->givenname ? nice_text (p, ni->givenname) : "",
		     ni->surname ? nice_text (p, ni->surname) : "",
		     ni->level,
		     cert_level_to_name (vr, ni->level));
      buffer_printf (cb, "%s %s\n", ni->name, cert_level_to_name (vr, ni->level));
    }
  buffer_puts (b, "</table>\n");

  cache_str = buffer_extract (cb);
  vr->lock = db_lock (db);
  db_lock_upgrade (vr->lock);
  status = db_put (db, "tmetric/default", cache_str, strlen (cache_str));
  if (status)
    buffer_puts (b, "<p> Error writing tmetric cache. </p>\n");
  else
    buffer_puts (b, "<p> Wrote tmetric cache. </p>\n");

  db_unlock (lock);
  return render_footer_send (vr);
}

static int
tmetric_test_serve (VirguleReq *vr)
{
  request_rec *r = vr->r;
  Db *db = vr->db;
  DbLock *lock;

  if (vr->lock)
    db_unlock (vr->lock);
  vr->lock = NULL;

  r->content_type = "text/html; charset=ISO-8859-1";
  ap_send_http_header (r);
  ap_rprintf (r, "<!DOCTYPE HTML PUBLIC \"-//W3C//DTD HTML 4.01 Transitional//EN\"><html><head><title>Test</title></head><body bgcolor=white><h1>Test</h1> <p>Testing lock...</p>\n");

  ap_rflush (r);
  lock = db_lock_key (db, "tmetric/.lock", F_SETLK);
  if (lock == NULL)
    {
      ap_rprintf (r, "<p> Lock is taken by someone else. </p>\n");
    }
  else
    {
      ap_rprintf (r, "<p> Lock acquired. </p>\n");
      ap_rflush (r);
      sleep (10);
      db_unlock (lock);
    }
  ap_rprintf (r, "<p> Done. </p>\n</body></html>\n");
  
  return OK;
}

int
tmetric_serve (VirguleReq *vr)
{
  char *uri = vr->uri;

  cert_level_n = cert_num_levels (vr);

  if (!strcmp (uri, "/tmetric/"))
    return tmetric_index_serve (vr);
  if (!strcmp (uri, "/tmetric/test.html"))
    return tmetric_test_serve (vr);
  return DECLINED;
}

/**
 * tmetric_get: Retrieve trust metric info.
 * @vr: The #VirguleReq context.
 *
 * Retrieves trust metric information from the cache in the database.
 * Thus, to insure that the results of this function are fresh, you'll
 * need to make sure the cache gets updated regularly (for example,
 * by retrieving /tmetric/ from a cron job).
 *
 * Return value: the trust metric info.
 **/
char *
tmetric_get (VirguleReq *vr)
{
  char *result;
  int size;
  FILE *null;

  null = fopen ("/dev/null", "w");

  fputs ("about to get tmetric", null);
  fflush (null);

  result = db_get (vr->db, "tmetric/default", &size);

  fputs ("tmetric gotten", null);
  fflush (null);

  fclose (null);

  return result;
}
