/*
 * mod_virgule.c -- Apache module for building community sites
 *
 * Copyright 1999 Raph Levien <raph@acm.org>
 *
 * Released under GPL v2.
 */ 

#define VIRGULE_VERSION "mod_virgule-rsr/1.41-20050727"

#include <string.h>

/* gnome-xml includes */
#include <libxml/parser.h>
#include <libxml/tree.h>

/* Apache includes */
#include <apr.h>
#include <apr_strings.h>
#include <httpd.h>
#include <http_log.h>
#include <http_core.h>
#include <http_config.h>
#include <http_request.h>
#include <http_protocol.h>
#include <ap_config.h>

/* mod_virgule includes */
#include "private.h"
#include "buffer.h"
#include "db.h"
#include "req.h"
#include "site.h"
#include "apache_util.h"
#include "acct_maint.h"
#include "diary.h"
#include "article.h"
#include "proj.h"
#include "rss_export.h"
#include "style.h"
#include "tmetric.h"
#include "auth.h"
#include "db_ops.h"
#include "util.h"
#include "xmlrpc.h"
#include "db_xml.h"
#include "xml_util.h"
#include "rating.h"
#include "hashtable.h" /* for unit testing */

/* Process specific pool */
static apr_pool_t *ppool;

/* Thread private key */
static apr_threadkey_t *tkey;

/* Per-directory configuration record structure. */
typedef struct {
  char *dir;
  char *db;
  apr_array_header_t *topics;
  apr_array_header_t *pass_dirs;
} virgule_dir_conf;

/* Declare the module name so configuration routines can find it.
   Module structure is filled in at the end of this file */
module AP_MODULE_DECLARE_DATA virgule_module;

/**
 * Per-directory initialization function. This creates the per-directory
 * configuration structure and initializes the contents. The structure
 * will be filled in by the per-directory command handlers
 **/
static void *
create_virgule_dir_conf (apr_pool_t *p, char *dir)
{
  virgule_dir_conf *result = (virgule_dir_conf *)apr_palloc (p, sizeof (virgule_dir_conf));

  result->dir = apr_pstrdup (p, dir);
  result->db = NULL;
  result->topics = apr_array_make(p, 16, sizeof(ArticleTopic));
  result->pass_dirs = apr_array_make(p, 16, sizeof(char *));

  return result;
}


/**
 * Apache config file option handler (called by Apache).
 * Sets the Virgule database from the httpd.conf option
 **/
static const char *
set_virgule_db (cmd_parms *parms, void *mconfig, char *db)
{
  virgule_dir_conf *cfg = (virgule_dir_conf *)mconfig;

  /* temp!!! used by ad function in site.c */
  srand((unsigned int) time(NULL));

  cfg->db = (char *)apr_pstrdup (parms->pool, db);
  return NULL;
}

/**
 * Apache config file option handler (called by Apache once for each seed)
 * Adds the supplied topic name and icon URL topic list
 **/
static const char *
set_virgule_topic (cmd_parms *parms, void *mconfig, const char *name, const char *url)
{
  ArticleTopic *topic;
  
  virgule_dir_conf *cfg = (virgule_dir_conf *)mconfig;

  topic = (ArticleTopic *)apr_array_push(cfg->topics);
  topic->name = (char *)apr_pstrdup(parms->pool, name);
  topic->iconURL = (char *)apr_pstrdup(parms->pool, url);
  return NULL;
}

/**
 * set_virgule_pass: An Apache config file option handler (called by Apache
 * once for each directory name supplied after the VirgulePass directive in
 * the httpd.conf file). The directories are added to a passthrough list
 * used by xlat_handler to accept or decline requests.
 **/
static const char *
set_virgule_pass (cmd_parms *parms, void *mconfig, const char *dir)
{
  virgule_dir_conf *cfg = (virgule_dir_conf *)mconfig;
  
  *(char **)apr_array_push(cfg->pass_dirs) = (char *)apr_pstrdup(parms->pool, dir);
  return NULL;
}

/* Prints header data. Used only in test_page() */
static int header_trace (void *data, const char *key, const char *val)
{
  Buffer *b = (Buffer *)data;
  buffer_printf (b, "<dd>%s: <tt>%s</tt>\n", key, val);
  return 1;
}

/* Prints contents of test.xml. Used only in test_page(). Note that the xml
   file is expected to be in the base rather than site directory */
static void
render_doc (Buffer *b, Db *db, const char *key)
{
  char *buf;
  int buf_size;
  xmlDocPtr doc;
  xmlNodePtr root;

  buf = db_get (db, key, &buf_size);
  if (buf == NULL)
    {
      buffer_append (b, "Error reading key ", key, "\n", NULL);
    }
  else
    {
      doc = xmlParseMemory (buf, buf_size);
      if (doc != NULL)
	{
	  root = doc->xmlRootNode;
	  buffer_printf (b, "&lt;%s&gt;\n", root->name);
	  xmlFreeDoc (doc);
	}
      else
	buffer_append (b, "Error parsing key ", key, "\n", NULL);
    }
}

/* Counts the number of times a db key has been accessed. Appears to be
   used only in test_page(). */
static void
count (apr_pool_t *p, Buffer *b, Db *db, const char *key)
{
  char *buf;
  int buf_size;
  int count;

  buf = db_get (db, key, &buf_size);
  if (buf == NULL)
    count = 0;
  else
    count = atoi (buf);

  count++;
  buffer_printf (b, "<p> This page has been accessed %d time%s. </p>\n",
		 count, count == 1 ? "" : "s");
  buf = apr_psprintf (p, "%d\n", count);
#if 0
  /* useful for testing locking */
  sleep (5);
#endif
  if (db_put (db, key, buf, strlen (buf)))
    buffer_puts (b, "Error updating count\n");
}

/* Displays a useful diagnostic page if /foo.html is requested */
static int
test_page (VirguleReq *vr)
{
  request_rec *r = vr->r;
  virgule_dir_conf *cfg = (virgule_dir_conf *)ap_get_module_config (r->per_dir_config, &virgule_module);
  Buffer *b = vr->b;
  Db *db = vr->db;
  apr_table_t *args_table;
  int i;
  char tm[APR_CTIME_LEN];
  char *args;

  r->content_type = "text/html; charset=UTF-8";

  buffer_puts(b, "<!DOCTYPE HTML PUBLIC \"-//W3C//DTD HTML 4.01 Transitional//EN\">\n"
	      "<html>\n<head><title>\n"
	      "Virgule\n"
	      "</title></head>\n"
	      "<body bgcolor=white>");
  buffer_puts (b, "<h1>Virgule test</h1>\n");
  apr_ctime(tm,vr->priv->mtime);
  buffer_printf (b, "<p> Timestamp of loaded configuration: <tt>%s</tt> </p>\n", tm);
  buffer_printf (b, "<p> The unparsed uri is: <tt>%s</tt> </p>\n", r->unparsed_uri);
  buffer_printf (b, "<p> The uri (r->uri): <tt>%s</tt> </p>\n", r->uri);
  buffer_printf (b, "<p> The adjusted uri (vr->uri) is: <tt>%s</tt></p>\n",vr->uri);
  buffer_printf (b, "<p> The filename is: <tt>%s</tt> </p>\n", r->filename);
  buffer_printf (b, "<p> The path_info is: <tt>%s</tt> </p>\n", r->path_info);
  buffer_printf (b, "<p> The document root is: <tt>%s</tt> </p>\n", ap_document_root (r));
  buffer_printf (b, "<p> Browser requested protocol: <tt>%s</tt></p>\n", r->protocol);
  buffer_printf (b, "<p> Browser requested host: <tt>%s</tt></p>\n", r->hostname);
  buffer_printf (b, "<p> Requested Apache handler is: <tt>%s</tt></p>\n", r->handler);
  buffer_printf (b, "<p> Apache thread/process ID: <tt>%lu</tt></p>\n", apr_os_thread_current());
  if (cfg)
    buffer_printf (b, "<p> cfg->db=\"%s\", cfg->dir=\"%s\"\n",
		   cfg->db, cfg->dir);

  /* Dump pass-through directory names read from httpd.conf */
  if (cfg->pass_dirs)
    {
      buffer_puts (b, "<p>Pass-through Directory Names:\n<ul>\n");
      for ( i = 0; i < cfg->pass_dirs->nelts; i++ )
        buffer_printf (b, "<li>%s", ((char **)cfg->pass_dirs->elts)[i]);
      buffer_puts (b, "</ul></p>");
    }
  else 
    {
      buffer_puts (b, "<p> No pass-through directories found.</p>");
    }

  args = vr->args;
  if (args)
    {
      const char *key;

      buffer_printf (b, "<p> The args are: <tt>%s</tt> </p>\n", args);
      args_table = get_args_table (vr);
      key = apr_table_get (args_table, "key");
      if (key)
	{
	  add_recent (r->pool, db, "test/recent.xml", key, 5, 0);
	  buffer_printf (b, "<p> The translation of key %s is %s\n",
			 key, db_mk_filename (r->pool, vr->db, key));
	}
    }

  auth_user (vr);
  if (vr->u)
    buffer_printf (b, "<p> The authenticated user is: <tt>%s</tt> </p>\n",
		   vr->u);
  
  buffer_puts (b, "<dl><dt>Headers in:\n");
  apr_table_do (header_trace, b, r->headers_in, NULL);
  buffer_puts (b, "</dl>\n");
  
  buffer_puts (b, "<dl><dt>Headers out:\n");
  apr_table_do (header_trace, b, r->headers_out, NULL);
  buffer_puts (b, "</dl>\n");
  
  render_doc (b, db, "test.xml");

//  if(db_lock_upgrade (vr->lock) == -1) return SERVER_ERROR;
  db_lock_upgrade (vr->lock);
	   
  buffer_printf (b, "<p> The site name is <tt>%s</tt> \n", vr->priv->site_name);
  buffer_printf (b, "and it lives at <tt>%s</tt> </p> \n", vr->priv->base_uri);

  if (*vr->priv->cert_level_names)
    {
      const char **l;

      buffer_puts (b, "<p> The certification levels are: </p>\n<ol>");
      for (l = vr->priv->cert_level_names; *l; l++)
	buffer_printf (b, "<li> %s </li>\n", *l);
      buffer_puts (b, "</ol>\n");
    }

  if (*vr->priv->seeds)
    {
      const char **s;

      buffer_puts (b, "<p> The seeds are: </p>\n<ul>");
      for (s = vr->priv->seeds; *s; s++)
	buffer_printf (b, "<li> %s </li>\n", *s);
      buffer_puts (b, "</ul>\n");
    }

  if (*vr->priv->caps)
    {
      const int *c;

      buffer_puts (b, "<p> The capacities are: </p>\n<ol>");
      for (c = vr->priv->caps; *c; c++)
	buffer_printf (b, "<li> %d </li>\n", *c);
      buffer_puts (b, "</ol>\n");
    }

  if (*vr->priv->special_users)
    {
      const char **u;

      buffer_puts (b, "<p> The following users are special: </p>\n<ul>");
      for (u = vr->priv->special_users; *u; u++)
	buffer_printf (b, "<li> %s </li>\n", *u);
      buffer_puts (b, "</ul>\n");
    }

  if (vr->priv->render_diaryratings)
    buffer_puts (b, "<p>Diary rating system is active</p>\n");
  else
    buffer_puts (b, "<p>Diary rating system is inactive</p>\n");

  buffer_printf (b, "<p>Recentlog style is %s</p>\n",
		 vr->priv->recentlog_as_posted ? "As Posted" : "Unique");

  buffer_printf (b, "<p>Account creation is %s</p>\n",
		 vr->priv->allow_account_creation ? "allowed" : "not allowed");

  count (r->pool, b, db, "misc/admin/counter/count");
  
  buffer_puts (b, "</body></html>\n");

  return send_response (vr);
}

#if 0
/* Generates a test entry in the test/inc directory under site. May not
   be needed anymore? */
static int
test_serve (VirguleReq *vr)
{
  int max;
  char *new_key;

  if (strcmp (vr->r->uri, "/test.html"))
    return DECLINED;
  db_lock_upgrade(vr->lock);
  max = db_dir_max (vr->db, "test/inc");
  new_key = apr_psprintf (vr->r->pool, "test/inc/_%d", max + 1);
  db_put (vr->db, new_key, new_key, strlen (new_key));
  return send_error_page (vr, "Test page",
			  "This is a test, max = %d, new_key = %s.",
			  max, new_key);
}

static void
test_hashtable_set (VirguleReq *vr, HashTable *ht,
		    const char *key, const char *val)
{
  buffer_printf (vr->b, "Setting key %s to \"%s\".<br>\n",
		 key, val);
  hash_table_set (vr->r->pool, ht, key, (void *)val);
}

static void
test_hashtable_get (VirguleReq *vr, HashTable *ht,
		    const char *key)
{
  char *val = (char *)hash_table_get (ht, key);

  if (val == NULL)
    buffer_printf (vr->b, "Key %s has no value.<br>\n", key);
  else
    buffer_printf (vr->b, "Key %s has value \"%s\".<br>\n",
		 key, val);
}

static int
test_hashtable_serve (VirguleReq *vr)
{
  HashTable *ht = hash_table_new (vr->r->pool);
  HashTableIter *iter;
  const char *key, *val;

  render_header (vr, "Hash table test", NULL);
  test_hashtable_get (vr, ht, "foo");
  test_hashtable_set (vr, ht, "foo", "bar");
  test_hashtable_get (vr, ht, "foo");
  test_hashtable_set (vr, ht, "first", "one");
  test_hashtable_set (vr, ht, "second", "two");
  test_hashtable_set (vr, ht, "third", "three");
  test_hashtable_set (vr, ht, "fourth", "four");
  test_hashtable_get (vr, ht, "foo");
  test_hashtable_get (vr, ht, "first");
  test_hashtable_get (vr, ht, "second");
  test_hashtable_get (vr, ht, "third");
  test_hashtable_get (vr, ht, "fourth");
  test_hashtable_set (vr, ht, "foo", "baz");
  test_hashtable_get (vr, ht, "foo");
  for (iter = hash_table_iter (vr->r->pool, ht);
       hash_table_iter_get (iter, &key, (void **)&val);
       hash_table_iter_next (iter))
    buffer_printf (vr->b, "(%s, %s)<br>\n", key, val);
  return render_footer_send (vr);
}
#endif


/**
 * private_destroy: Destroys the virgule thread specific pool
 **/
static void private_destroy(void *data)
{
  apr_pool_destroy(((virgule_private_t *)data)->pool);
}


/**
 * virgule_child_init: Called immediately have child initialization is
 * started during the process start up. 
 **/
static void virgule_child_init(apr_pool_t *p, server_rec *s)
{
  ppool = s->process->pool;
  apr_status_t status;
  
  /* Create a thread private key for later use */
  if((status = apr_threadkey_private_create(&tkey, private_destroy, ppool))
     != APR_SUCCESS)
    ap_log_error(APLOG_MARK,APLOG_CRIT,status,s,"mod_virgule: Unable to create thread private key");
}



/**
 * virgule_init_handler: Module Initialization Handler. This function is
 * called once during server initialization. The module version number and
 * name is passed back to Apache at this point.
 **/
static int virgule_init_handler(apr_pool_t *pconf, apr_pool_t *plog,
                                 apr_pool_t *ptemp, server_rec *s)
{
  ap_add_version_component(pconf, VIRGULE_VERSION);
  return OK;
}

/* make sure this doesn't clash with any HTTP status codes */
#define CONFIG_READ 1000

/**
 * read_site_config - Reads the config.xml file containing the site
 * configuration data. The temporary request pool is used during the read
 * but the actual config data is moved to the thread private pool so it
 * will be available for later requests.
 **/
static int
read_site_config (VirguleReq *vr)
{
  xmlDoc *doc;
  xmlNode *node;
  apr_array_header_t *stack;
  xmlNode *child;
  char *uri;
  const char *text, *url;
  const char **c_item;
  int *i_item;
  const AllowedTag **t_item;
  const NavOption **n_item;
  apr_pool_t *privpool;

  /* Allocate thread private data struct and memory pool */
  if (apr_pool_create(&privpool,ppool) != APR_SUCCESS)
    return send_error_page (vr, "Config error",
                            "Unable to create thread private memory pool");
  vr->priv = apr_pcalloc(privpool,sizeof(virgule_private_t));
  vr->priv->pool = privpool;

  /* Load and parse the site configuration */
  doc = db_xml_get (vr->r->pool, vr->db, "config.xml");
  if (doc == NULL)
    return send_error_page (vr, "Config error",
			    "Unable to read the site config.");

  /* read the site name */
  vr->priv->site_name = apr_pstrdup(vr->priv->pool, xml_find_child_string (doc->xmlRootNode, "name", ""));
  if (!strlen (vr->priv->site_name))
    return send_error_page (vr, "Config error",
			    "No name found in site config.");

  /* read the site's base uri, and trim any trailing slashes */
  uri = xml_find_child_string (doc->xmlRootNode, "baseuri", "");
  do
    {
      int len = strlen (uri);
      if (!len)
	return send_error_page (vr, "Config error",
				"No base URI found in site config.");
      if (uri[len - 1] != '/')
	break;
      uri[len - 1] = 0;
    }
  while (1);
  vr->priv->base_uri = apr_pstrdup(vr->priv->pool, uri);

  /* read the project style */
  text = xml_find_child_string (doc->xmlRootNode, "projstyle", "raph");
  if (!strcasecmp (text, "Raph"))
    vr->priv->projstyle = PROJSTYLE_RAPH;
  else if (!strcasecmp (text, "Nick"))
    vr->priv->projstyle = PROJSTYLE_NICK;
  else
    return send_error_page (vr, "Config error",
			    "Unknown project style found in site config.");

  /* read the recentlog style */
  text = xml_find_child_string (doc->xmlRootNode, "recentlogstyle", "Unique");
  if (!strcasecmp (text, "Unique"))
    vr->priv->recentlog_as_posted = 0;
  else if (!strcasecmp (text, "As posted"))
    vr->priv->recentlog_as_posted = 1;
  else
    return send_error_page (vr, "Config error",
			    "Unknown recentlog style found in site config.");

  /* read the cert levels */
  stack = apr_array_make (vr->priv->pool, 10, sizeof (char *));
  node = xml_find_child (doc->xmlRootNode, "levels");
  if (node == NULL)
    return send_error_page (vr, "Config error",
			    "No cert levels found in site config.");

  for (child = node->children; child; child = child->next)
    {
      if (child->type != XML_ELEMENT_NODE) {
        continue;
      }
 
      if (strcmp (child->name, "level"))
	return send_error_page (vr, "Config error",
				"Unknown element <tt>%s</tt> in cert levels.",
				child->name);

      text = xml_get_string_contents (child);
      if (!text)
	return send_error_page (vr, "Config error",
				"Empty element in cert levels.");

      c_item = (const char **)apr_array_push (stack);
      *c_item = apr_pstrdup(vr->priv->pool, text);
    }

  if (stack->nelts < 2)
    return send_error_page (vr, "Config error",
			    "There must be at least two cert levels.");

  c_item = (const char **)apr_array_push (stack);
  *c_item = NULL;
  vr->priv->cert_level_names = (const char **)stack->elts;

  /* read the seeds */
  stack = apr_array_make (vr->priv->pool, 10, sizeof (char *));
  node = xml_find_child (doc->xmlRootNode, "seeds");
  if (node == NULL)
    return send_error_page (vr, "Config error",
			    "No seeds found in site config.");

  for (child = node->children; child; child = child->next)
    {
      if (child->type != XML_ELEMENT_NODE) {
        continue;
      }
 
      if (strcmp (child->name, "seed"))
	return send_error_page (vr, "Config error",
				"Unknown element <tt>%s</tt> in seeds.",
				child->name);

      text = xml_get_string_contents (child);
      if (!text)
	return send_error_page (vr, "Config error",
				"Empty element in seeds.");

      c_item = (const char **)apr_array_push (stack);
      *c_item = apr_pstrdup(vr->priv->pool, text);
    }

  if (stack->nelts < 1)
    return send_error_page (vr, "Config error",
			    "There must be at least one seed.");

  c_item = (const char **)apr_array_push (stack);
  *c_item = NULL;
  vr->priv->seeds = (const char **)stack->elts;

  /* read the capacities */
  stack = apr_array_make (vr->priv->pool, 10, sizeof (int));
  node = xml_find_child (doc->xmlRootNode, "caps");
  if (node == NULL)
    return send_error_page (vr, "Config error",
			    "No capacities found in site config.");

  for (child = node->children; child; child = child->next)
    {
      if (child->type != XML_ELEMENT_NODE) {
        continue;
      }
 
      if (strcmp (child->name, "cap"))
	return send_error_page (vr, "Config error",
				"Unknown element <tt>%s</tt> in capacities.",
				child->name);

      text = xml_get_string_contents (child);
      if (!text)
	return send_error_page (vr, "Config error",
				"Empty element in capacities.");

      i_item = (int *)apr_array_push (stack);
      *i_item = atoi (text);
    }

  if (stack->nelts < 1)
    return send_error_page (vr, "Config error",
			    "There must be at least one capacity.");

  i_item = (int *)apr_array_push (stack);
  *i_item = 0;
  vr->priv->caps = (const int *)stack->elts;

  /* read the special users */
  stack = apr_array_make (vr->priv->pool, 10, sizeof (char *));
  node = xml_find_child (doc->xmlRootNode, "specialusers");
  if (node)
    {
      for (child = node->children; child; child = child->next)
	{
          if (child->type != XML_ELEMENT_NODE) {
            continue;
          }

	  if (strcmp (child->name, "specialuser"))
 
	    return send_error_page (vr, "Config error",
				    "Unknown element <tt>%s</tt> in special users.",
				    child->name);

	  text = xml_get_string_contents (child);
	  if (!text)
	    return send_error_page (vr, "Config error",
				    "Empty element in special users.");

	  c_item = (const char **)apr_array_push (stack);
          *c_item = apr_pstrdup(vr->priv->pool, text);
	}
    }
  c_item = (const char **)apr_array_push (stack);
  *c_item = NULL;
  vr->priv->special_users = (const char **)stack->elts;

  /* read the translations */
  stack = apr_array_make (vr->priv->pool, 10, sizeof (char *));
  node = xml_find_child (doc->xmlRootNode, "translations");
  if (node)
    {
      for (child = node->children; child; child = child->next)
	{
          if (child->type != XML_ELEMENT_NODE) {
            continue;
          }

	  if (strcmp (child->name, "translate"))
	    return send_error_page (vr, "Config error",
				    "Unknown element <tt>%s</tt> in translations.",
				    child->name);

	  text = xml_get_prop (vr->r->pool, child, "from");
	  c_item = (const char **)apr_array_push (stack);
          *c_item = apr_pstrdup(vr->priv->pool, text);

	  text = xml_get_prop (vr->r->pool, child, "to");
	  c_item = (const char **)apr_array_push (stack);
          *c_item = apr_pstrdup(vr->priv->pool, text);
	}
    }
  c_item = (const char **)apr_array_push (stack);
  *c_item = NULL;
  vr->priv->trans = (const char **)stack->elts;

  /* read the diary rating selection */
  text = xml_find_child_string (doc->xmlRootNode, "diaryrating", "");
  if (!strcasecmp (text, "on"))
    vr->priv->render_diaryratings = 1;
  else
    vr->priv->render_diaryratings = 0;

  /* read the new accounts allowed selection */
  text = xml_find_child_string (doc->xmlRootNode, "accountcreation", "");
  if (!strcasecmp (text, "off"))
    vr->priv->allow_account_creation = 0;
  else
    vr->priv->allow_account_creation = 1;

  /* read the sitemap navigation options */
  stack = apr_array_make (vr->priv->pool, 10, sizeof (NavOption *));
  node = xml_find_child (doc->xmlRootNode, "sitemap");
  if (node)
    {
      for (child = node->children; child; child = child->next)
      {
        if (child->type != XML_ELEMENT_NODE) {
	  continue;
	}
	
	if (strcmp (child->name, "option"))
	  return send_error_page (vr, "Config error",
				  "Unknown element <tt>%s</tt> in sitemap options.",
				  child->name);
	
	url = xml_get_prop (vr->r->pool, child, "url");
	text = xml_get_string_contents (child);
        if (!text)
          return send_error_page (vr, "Config error",
                                      "Empty element in allowed sitemap options.");

        n_item = (const NavOption **)apr_array_push (stack);
        *n_item = add_nav_option (vr, text, url);	
      }
    }
  n_item = (const NavOption **)apr_array_push (stack);
  *n_item = NULL;
  vr->priv->nav_options = (const NavOption **)stack->elts;

  /* read the allowed tags */
  stack = apr_array_make (vr->priv->pool, 10, sizeof (AllowedTag *));
  node = xml_find_child (doc->xmlRootNode, "allowedtags");
  if (node)
    {
	for (child = node->children; child; child = child->next)
	{
	  int empty;

          if (child->type != XML_ELEMENT_NODE) {
            continue;
          }

	  if (strcmp (child->name, "tag"))
	    return send_error_page (vr, "Config error",
				    "Unknown element <tt>%s</tt> in allowed tags.",
				    child->name);

	  text = xml_get_prop (vr->r->pool, child, "canbeempty");
	  empty = text && !strcmp(text, "yes");

	  text = xml_get_string_contents (child);
	  if (!text)
	    return send_error_page (vr, "Config error",
				    "Empty element in allowed tags.");

	  t_item = (const AllowedTag **)apr_array_push (stack);
	  *t_item = add_allowed_tag (vr, text, empty);
	}
    }
  t_item = (const AllowedTag **)apr_array_push (stack);
  *t_item = NULL;
  vr->priv->allowed_tags = (const AllowedTag **)stack->elts;

ap_log_rerror(APLOG_MARK, APLOG_CRIT, APR_SUCCESS, vr->r,"Debug: read config.xml");

  return CONFIG_READ;
}


/**
 * virgule_handler: Generates the content to fill a request
 */
static int virgule_handler(request_rec *r)
{
  virgule_dir_conf *cfg;
  Buffer *b;
  Db *db;
  int status;
  apr_finfo_t finfo;
  apr_status_t ap_status;
  VirguleReq *vr;

  if(strcmp(r->handler, "virgule")) {
    return DECLINED;
  }

  cfg = (virgule_dir_conf *)ap_get_module_config (r->per_dir_config, &virgule_module);
  b = buffer_new (r->pool);
  
  /* Set libxml2 to old-style, incorrect handling of whitespace. This can
     be removed once all existing xml code is updated to handle blank nodes */
  xmlKeepBlanksDefault(0);

  db = db_new_filesystem (r->pool, cfg->db); /* hack */

  vr = (VirguleReq *)apr_pcalloc (r->pool, sizeof (VirguleReq));

  vr->r = r;
  vr->b = b;
  vr->db = db;

  if (cfg->dir && !strncmp (r->uri, cfg->dir, strlen (cfg->dir)))
    {
      vr->prefix = cfg->dir;
      vr->uri = r->uri + strlen (cfg->dir);
    }
  else
    {
      vr->prefix = "";
      vr->uri = r->uri;
    }

  vr->topics = cfg->topics;
 
  vr->u = NULL;
  vr->args = get_args (r);

  vr->lock = NULL;
  vr->tmetric = NULL;
  vr->sitemap_rendered = 0;
  vr->render_data = apr_table_make (r->pool, 4);

  /* Get our thread private data, if any */
  if((ap_status = apr_threadkey_private_get((void *)(&vr->priv),tkey))  
    != APR_SUCCESS)
  {
    ap_log_rerror(APLOG_MARK, APLOG_CRIT, ap_status, r,
                 "mod_virgule: Cannot get thread private data");
    return HTTP_INTERNAL_SERVER_ERROR;
  }

  /* Stat the site config file in case it got updated */
  apr_stat(&finfo, ap_make_full_path (r->pool, cfg->db, "/config.xml"),
           APR_FINFO_MIN, r->pool);

  /* If config file was updated, dump and reload private data */
  if(vr->priv && (finfo.mtime != vr->priv->mtime))
  {
    apr_pool_destroy(vr->priv->pool);
    vr->priv = NULL;
  }

  if(!vr->priv)
  {
    /* read and parse config.xml */
    if(read_site_config (vr) != CONFIG_READ)
    {
      ap_log_rerror(APLOG_MARK, APLOG_CRIT, ap_status, r,
                   "mod_virgule: Cannot load site config file");
      return HTTP_INTERNAL_SERVER_ERROR;
    }
    
    /* Save config to thread private data area */
    if(apr_threadkey_private_set(vr->priv, tkey) != APR_SUCCESS)
    {
      apr_pool_destroy(vr->priv->pool);
      ap_log_rerror(APLOG_MARK, APLOG_CRIT, ap_status, r,
                   "mod_virgule: Cannot set thread private data");
      return HTTP_INTERNAL_SERVER_ERROR;
    }

    /* set mtime for config file */
    vr->priv->mtime = finfo.mtime;
  }

  /* set buffer translations */
  buffer_set_translations (vr->b, vr->priv->trans);

  vr->lock = db_lock (db);
  if (vr->lock == NULL)
    return send_error_page (vr, "Lock error",
			    "There was an error acquiring the lock, %s.",
			    strerror (errno));

  if (!strcmp (match_prefix(r->uri, vr->prefix), "/foo.html"))
    return test_page (vr);

  if (!strcmp (match_prefix(r->uri, vr->prefix), "/cgi-bin/ad"))
    return site_send_banner_ad (vr);

  status = xmlrpc_serve (vr);
  if (status != DECLINED)
    return status;  
  
  status = site_serve (vr);
  if (status != DECLINED)
    return status;

  status = acct_maint_serve (vr);
  if (status != DECLINED)
    return status;

  status = tmetric_serve (vr);
  if (status != DECLINED)
    return status;

#if 0
  status = test_serve (vr);
  if (status != DECLINED)
    return status;

  if (!strcmp (vr->uri, "/test/ht.html"))
    {
      status = test_hashtable_serve (vr);
      if (status != DECLINED)
	return status;
    }
#endif

  status = diary_serve (vr);
  if (status != DECLINED)
    return status;

  status = article_serve (vr);
  if (status != DECLINED)
    return status;

  status = proj_serve (vr);
  if (status != DECLINED)
    return status;

  status = rss_serve (vr);
  if (status != DECLINED)
    return status;

  if (vr->priv->render_diaryratings)
    {
      status = rating_serve (vr);
      if (status != DECLINED)
	return status;
    }

  return HTTP_NOT_FOUND;
}

/**
 * xlat_handler: URI to Filename translator. This function is called by
 * Apache for each request to translate the URI to an appropriate request
 * and determine if the request is to be accepted for handling or declined.
 **/
static int
xlat_handler (request_rec *r)
{
  virgule_dir_conf *cfg = (virgule_dir_conf *)ap_get_module_config (r->per_dir_config, &virgule_module);

  if (cfg->db)
    {
      int i;

      if (cfg->pass_dirs)
	  for (i = 0; i < cfg->pass_dirs->nelts; i++ )
	      if (!strncmp (((char **)cfg->pass_dirs->elts)[i], r->uri,
			    strlen(((char **)cfg->pass_dirs->elts)[i])))
		  return DECLINED;

      r->handler = "virgule";
      r->filename = apr_pstrdup (r->pool, cfg->db);
      /* this is to work around mod_dir.c */
      r->finfo.filetype = APR_REG;
      return OK;
    }
  else
    return DECLINED;

}


/* Dispatch table of functions to handle Virgule httpd.conf directives */
static const command_rec virgule_cmds[] =
{
  {"VirguleDb", set_virgule_db, NULL, OR_ALL, TAKE1, "the virgule database"},
  {"VirguleTopic", set_virgule_topic, NULL, OR_ALL, TAKE2, "virgule article topic"},
  {"VirgulePass", set_virgule_pass, NULL, OR_ALL, ITERATE, "virgule passthrough directories"},
  {NULL}
};


static void register_hooks(apr_pool_t *p)
{
  static const char * const aszPre[]={ "http_core.c",NULL };
  ap_hook_child_init(virgule_child_init, NULL, NULL, APR_HOOK_MIDDLE);
  ap_hook_post_config(virgule_init_handler, NULL, NULL, APR_HOOK_MIDDLE);
  ap_hook_handler(virgule_handler, NULL, NULL, APR_HOOK_MIDDLE);
  ap_hook_translate_name(xlat_handler, aszPre, NULL, APR_HOOK_LAST);
}

/* Dispatch list for API hooks */
module AP_MODULE_DECLARE_DATA virgule_module = {
    STANDARD20_MODULE_STUFF, 
    create_virgule_dir_conf,  /* create per-dir    config structures */
    NULL,                     /* merge  per-dir    config structures */
    NULL,                     /* create per-server config structures */
    NULL,                     /* merge  per-server config structures */
    virgule_cmds,             /* command table */
    register_hooks            /* register hooks function */
};
