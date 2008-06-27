/*
 * mod_virgule.c -- Apache module for building community sites
 *
 * Copyright 1999 Raph Levien <raph@acm.org>
 *
 * Released under GPL v2.
 */ 

#define VIRGULE_VERSION "mod_virgule-rsr/1.41-20070308"

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
#include "certs.h"
#include "site.h"
#include "apache_util.h"
#include "acct_maint.h"
#include "aggregator.h"
#include "hashtable.h"
#include "eigen.h"
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

/* Process specific pool */
static apr_pool_t *ppool = NULL;

/* Thread private key */
static apr_threadkey_t *tkey;

/* Per-directory configuration record structure. */
typedef struct {
  char *dir;
  char *db;
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
virgule_create_dir_config (apr_pool_t *p, char *dir)
{
  virgule_dir_conf *result = (virgule_dir_conf *)apr_palloc (p, sizeof (virgule_dir_conf));

  result->dir = apr_pstrdup (p, dir);
  result->db = NULL;
  result->pass_dirs = apr_array_make(p, 16, sizeof(char *));

  return result;
}


/**
 * Apache config file option handler (called by Apache).
 * Sets the Virgule database from the httpd.conf option
 **/
static const char *
set_virgule_db (cmd_parms *parms, void *mconfig, const char *db)
{
  apr_finfo_t finfo;
  virgule_dir_conf *cfg = (virgule_dir_conf *)mconfig;

  /* temp!!! used by ad function in site.c */
  srand((unsigned int) time(NULL));

  if( apr_stat(&finfo,db,APR_FINFO_TYPE,parms->pool) != APR_SUCCESS ||
      finfo.filetype != APR_DIR )
    return apr_pstrcat(parms->pool,"Invalid VirguleDB path: ",db,NULL);

  cfg->db = (char *)apr_pstrdup (parms->pool, db);
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


/* Prints header data. Used only in info_page() */
static int header_trace (void *data, const char *key, const char *val)
{
  Buffer *b = (Buffer *)data;
  virgule_buffer_printf (b, "%s: <b>%s</b><br />\n", key, val);
  return 1;
}


/* Displays a useful diagnostic page if /admin/info.html is requested */
static int
info_page (VirguleReq *vr)
{
  request_rec *r = vr->r;
  virgule_dir_conf *cfg = (virgule_dir_conf *)ap_get_module_config (r->per_dir_config, &virgule_module);
  Buffer *b = vr->b;
  int i;
  char tm[APR_CTIME_LEN];
  char *args;

  r->content_type = "text/html; charset=UTF-8";

  virgule_buffer_puts(b, "<!DOCTYPE HTML PUBLIC \"-//W3C//DTD HTML 4.01 Transitional//EN\">\n"
	      "<html>\n<head><title>mod_virgule diagnostics page</title>\n"
	      "<style type=\"text/css\">\n"
	      "  #diag td { border: 1px solid black; background: #ddeeff; }\n"
	      "  #diag td+td { background: #ffffff; }\n"
	      "</style>\n</head>\n<body bgcolor=white>");
  virgule_buffer_puts (b, "<h1>mod_virgule diagnostics</h1>\n<table id=\"diag\">\n");
  apr_ctime(tm,vr->priv->mtime);
  virgule_buffer_printf (b, "<tr><td>mod_virgule version</td><td>%s</td></tr>", VIRGULE_VERSION);
  virgule_buffer_printf (b, "<tr><td>Timestamp of loaded configuration</td><td>%s</td></tr>\n", tm);
  virgule_buffer_printf (b, "<tr><td>Site name (vr->priv->site_name)</td><td>%s</td></tr>\n", vr->priv->site_name);
  virgule_buffer_printf (b, "<tr><td>Admin email</td><td>%s</td></tr>\n", vr->priv->admin_email);
  virgule_buffer_printf (b, "<tr><td>Google Analytics ID</td><td>%s</td></tr>\n", vr->priv->google_analytics);
  virgule_buffer_printf (b, "<tr><td>FOAF SHA-1 Test</td><td>Input [mailto:%s] Output [%s]</td></tr>\n",
         vr->priv->admin_email,
	 virgule_sha1 (vr->r->pool, apr_psprintf (vr->r->pool, "mailto:%s", vr->priv->admin_email)));
  virgule_buffer_printf (b, "<tr><td>Unparsed uri (r->unparse_uri)</td><td>%s</td></tr>\n", r->unparsed_uri);
  virgule_buffer_printf (b, "<tr><td>uri (r->uri)</td><td>%s</td></tr>\n", r->uri);
  virgule_buffer_printf (b, "<tr><td>adjusted uri (vr->uri)</td><td>%s</td></tr>\n",vr->uri);
  virgule_buffer_printf (b, "<tr><td>base_uri (vr->priv->base_uri)</td><td>%s</td></tr>\n",vr->priv->base_uri);
  virgule_buffer_printf (b, "<tr><td>base_path (vr->priv->base_path)</td><td>%s</td></tr>\n",vr->priv->base_path);
  virgule_buffer_printf (b, "<tr><td>path prefix (vr->prefix)</td><td>%s</td></tr>", vr->prefix);
  virgule_buffer_printf (b, "<tr><td>filename (r->filename)</td><td>%s</td></tr>\n", r->filename);
  virgule_buffer_printf (b, "<tr><td>path_info (r->path_info)</td><td>%s</td></tr>\n", r->path_info);
  virgule_buffer_printf (b, "<tr><td>document root (ap_document_root())</td><td>%s</td></tr>\n", ap_document_root (r));
  virgule_buffer_printf (b, "<tr><td>Request protocol (r->protocol)</td><td>%s</td></tr>\n", r->protocol);
  virgule_buffer_printf (b, "<tr><td>Request hostname (r->hostname)</td><td>%s</td></tr>\n", r->hostname);
  virgule_buffer_printf (b, "<tr><td>Request handler (r->handler)</td><td>%s</td></tr>\n", r->handler);
  virgule_buffer_printf (b, "<tr><td>Apache thread ID (apr_os_thread_current())</td><td>%lu</td></tr>\n", apr_os_thread_current());
  if (cfg)
    virgule_buffer_printf (b, "<tr><td>Configured virgule DB</td><td>"
                           "[cfg->db=\"%s\"] [cfg->dir=\"%s\"]</td></tr>\n",
			   cfg->db, cfg->dir);

  /* Dump pass-through directory names read from httpd.conf */
  virgule_buffer_puts (b, "<tr><td>Configured Pass-through Directories:</td><td><ul>");
  if (cfg->pass_dirs)
    {
      for ( i = 0; i < cfg->pass_dirs->nelts; i++ )
        virgule_buffer_printf (b, "<li><b>%s</b></li>", ((char **)cfg->pass_dirs->elts)[i]);
      virgule_buffer_puts (b, "</ul></td></tr>\n");
    }
  else 
    {
      virgule_buffer_puts (b, "<li><b>None</b></li></ul></td></tr>");
    }

  args = vr->args;
  if (args)
    virgule_buffer_printf (b, "<tr><td>URL args</td><td>%s</td></tr>\n", args);

  virgule_auth_user (vr);
  if (vr->u)
    virgule_buffer_printf (b, "<tr><td>Authenticated user</td><td>[%s]</td></tr>\n", vr->u);
  
  virgule_buffer_puts (b, "<tr><td>Headers in</td><td>");
  apr_table_do (header_trace, b, r->headers_in, NULL);  
  virgule_buffer_puts (b, "</td></tr>\n<tr><td>Headers out</td><td>");
  apr_table_do (header_trace, b, r->headers_out, NULL);
  virgule_buffer_puts (b, "</td></tr>");
  
  if (vr->priv->cert_level_names)
    {
      const char **l;

      virgule_buffer_puts (b, "<tr><td>Certification levels</td><td><ol>");
      for (l = vr->priv->cert_level_names; *l; l++)
	virgule_buffer_printf (b, "<li>%s</li>\n", *l);
      virgule_buffer_puts (b, "</ol></td></tr>\n");
    }

  if (vr->priv->editors)
    {
      const char **s;

      virgule_buffer_puts (b, "<tr><td>Editors</td><td><ul>");
      for (s = vr->priv->editors; *s; s++)
	virgule_buffer_printf (b, "<li>%s</li>\n", *s);
      virgule_buffer_puts (b, "</ul></td></tr>\n");
    }

  if (vr->priv->article_post_by_editors_only)
    virgule_buffer_puts (b, "<tr><td>Article posts by editors only</td><td>On</td></tr>\n");
  else
    virgule_buffer_puts (b, "<tr><td>Article posts by editors only</td><td>Off</td></tr>\n");

  if (vr->priv->seeds)
    {
      const char **s;

      virgule_buffer_puts (b, "<tr><td>Trust metric seeds</td><td><ul>");
      for (s = vr->priv->seeds; *s; s++)
	virgule_buffer_printf (b, "<li>%s</li>\n", *s);
      virgule_buffer_puts (b, "</ul></td></tr>\n");
    }

  if (vr->priv->caps)
    {
      const int *c;

      virgule_buffer_puts (b, "<tr><td>Trust flow capacities</td><td><ol>");
      for (c = vr->priv->caps; *c; c++)
	virgule_buffer_printf (b, "<li>%d</li>\n", *c);
      virgule_buffer_puts (b, "</ol></td></tr>\n");
    }

  if (vr->priv->special_users)
    {
      const char **u;

      virgule_buffer_puts (b, "<tr><td>Special (admin) users</td><td><ul>");
      for (u = vr->priv->special_users; *u; u++)
	virgule_buffer_printf (b, "<li>%s</li>\n", *u);
      virgule_buffer_puts (b, "</ul></td></tr>\n");
    }

  if (vr->priv->render_diaryratings)
    virgule_buffer_puts (b, "<tr><td>Diary rating</td><td>On</td></tr>\n");
  else
    virgule_buffer_puts (b, "<tr><td>Diary rating</td><td>Off</td></tr>\n");

  virgule_buffer_printf (b, "<tr><td>Recentlog style</td><td>%s</td></tr>\n",
		 vr->priv->recentlog_as_posted ? "As Posted" : "Unique");

  virgule_buffer_printf (b, "<tr><td>Account creation</td><td>%s</td></tr>\n",
		 vr->priv->allow_account_creation ? "allowed" : "not allowed");

  virgule_buffer_printf (b, "<tr><td>Account spam threshold</td><td>%i points</td></tr>\n", 
                 vr->priv->acct_spam_threshold);

  virgule_buffer_printf (b, "<tr><td>Article Topics (categories)</td><td>%s</td></tr>\n",
		 vr->priv->use_article_topics ? "on" : "off");

  virgule_buffer_printf (b, "<tr><td>Article title links</td><td>%s</td></tr>\n",
		 vr->priv->use_article_title_links ? "on" : "off");

  virgule_buffer_printf (b, "<tr><td>Article title maximum length</td><td>%i chars</td></tr>\n", 
                 vr->priv->article_title_maxsize);

  virgule_buffer_printf (b, "<tr><td>Article editable period</td><td>%i days</td></tr>\n", 
                 vr->priv->article_days_to_edit);

  virgule_buffer_puts (b, "</table></body></html>\n");

  return virgule_send_response (vr);
}


/**
 * private_destroy: Destroys the virgule thread specific pool
 **/
static void private_destroy(void *data)
{
  apr_pool_destroy(((virgule_private_t *)data)->pool);
}


/**
 * virgule_child_init: Called for each child process immediately after 
 * start up.
 **/
static void virgule_child_init(apr_pool_t *p, server_rec *s)
{
  ppool = s->process->pool;
  apr_status_t status;
  
  /* Create a thread private key for later use */
  if((status = apr_threadkey_private_create(&tkey, private_destroy, ppool))
     != APR_SUCCESS)
    ap_log_error(APLOG_MARK,APLOG_CRIT,status,s,"mod_virgule: Unable to create thread private key");

  xmlInitParser();
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

static const AllowedTag *
read_allowed_tag (VirguleReq *vr, xmlNode *node)
{
  xmlNode *node_attributes, *child;
  char *name, *text;
  char **p;
  int empty;
  apr_array_header_t *stack;

  name = virgule_xml_find_child_string (node, "name", NULL);
  if (!name)
    {
      virgule_send_error_page (vr, vERROR, "Config error",
		    "Allowed tag name not defined.");
      return NULL;
    }

  text = virgule_xml_find_child_string (node, "canbeempty", NULL);

  empty = text && !strcmp (text, "yes");

  stack = apr_array_make (vr->priv->pool, 8, sizeof (char *));
  node_attributes = virgule_xml_find_child (node, "allowedattributes");

  if (node_attributes)
    {
      for (child = node_attributes->children; child; child = child->next)
        {
	  if (child->type != XML_ELEMENT_NODE)
	    continue;

          if (xmlStrcmp (child->name, (xmlChar *)"attribute"))
	    {
	      virgule_send_error_page (vr, vERROR, "Config error",
			    "Unknown element <tt>%s</tt> in allowed attributes.",
			    child->name);
	      return NULL;
	    }
    
          text = virgule_xml_get_string_contents (child);
    
          p = (char **)apr_array_push (stack);
          *p = apr_pstrdup (vr->priv->pool, virgule_xml_get_string_contents (child));
        }
    }

    p = (char **)apr_array_push (stack);
    *p = NULL;

    return virgule_add_allowed_tag (vr, name, empty, (char **)stack->elts);
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
  const Topic **at_item;
  apr_pool_t *privpool = NULL;
  time_t now;
  struct tm tm;

  /* Allocate thread private data struct and memory pool */
  if (apr_pool_create(&privpool,ppool) != APR_SUCCESS)
    return virgule_send_error_page (vr, vERROR, "config",
                            "Unable to create thread private memory pool");
  if (!(vr->priv = apr_pcalloc(privpool,sizeof(virgule_private_t))))
    return virgule_send_error_page (vr, vERROR, "config",
			    "Unable to allocate virgule_private_t");
  vr->priv->pool = privpool;
  
  /* Don't bother reading in the tmetric data until we need it */
  vr->priv->tmetric = NULL;
  vr->priv->tm_pool = NULL;
  vr->priv->tm_mtime = 0L;

  /* Figure out the local time zone offset using thread-safe POSIX func */
  now = time(NULL);
  localtime_r(&now, &tm);
  vr->priv->utc_offset = tm.tm_gmtoff;

  /* Load and parse the site configuration */
  doc = virgule_db_xml_get (vr->r->pool, vr->db, "config.xml");
  if (doc == NULL)
    return virgule_send_error_page (vr, vERROR, "config",
			    "Unable to read the site config.");

  /* read the site name */
  vr->priv->site_name = apr_pstrdup(vr->priv->pool, virgule_xml_find_child_string (doc->xmlRootNode, "name", ""));
  if (vr->priv->site_name != NULL && !strlen (vr->priv->site_name))
    return virgule_send_error_page (vr, vERROR, "config", "No name found in site config.");

  /* read the admin email */
  vr->priv->admin_email = apr_pstrdup(vr->priv->pool, virgule_xml_find_child_string (doc->xmlRootNode, "adminemail", ""));
  if (vr->priv->admin_email != NULL && !strlen (vr->priv->admin_email))
    return virgule_send_error_page (vr, vERROR, "config", "No admin email found in site config.");

  /* read the google analytics ID */
  vr->priv->google_analytics = apr_pstrdup(vr->priv->pool, virgule_xml_find_child_string (doc->xmlRootNode, "googleanalytics", ""));
  if (vr->priv->google_analytics != NULL && !strlen (vr->priv->google_analytics))
    vr->priv->google_analytics = NULL;

  /* read the site's base uri, and trim any trailing slashes */
  uri = virgule_xml_find_child_string (doc->xmlRootNode, "baseuri", "");
  if (uri == NULL)
    return virgule_send_error_page (vr, vERROR, "config", "No base URI found in site config.");
  do
    {
      int len = strlen (uri);
      if (!len)
	return virgule_send_error_page (vr, vERROR, "config",
				"No base URI found in site config.");
      if (uri[len - 1] != '/')
	break;
      uri[len - 1] = 0;
    }
  while (1);
  vr->priv->base_uri = apr_pstrdup(vr->priv->pool, uri);

  /* read the project style */
  text = virgule_xml_find_child_string (doc->xmlRootNode, "projstyle", "raph");
  if (!strcasecmp (text, "Raph"))
    vr->priv->projstyle = PROJSTYLE_RAPH;
  else if (!strcasecmp (text, "Nick"))
    vr->priv->projstyle = PROJSTYLE_NICK;
  else if (!strcasecmp (text, "Steve"))
    vr->priv->projstyle = PROJSTYLE_STEVE;
  else
    return virgule_send_error_page (vr, vERROR, "config",
			    "Unknown project style found in site config.");

  /* read the recentlog style */
  text = virgule_xml_find_child_string (doc->xmlRootNode, "recentlogstyle", "Unique");
  if (!strcasecmp (text, "Unique"))
    vr->priv->recentlog_as_posted = 0;
  else if (!strcasecmp (text, "As posted"))
    vr->priv->recentlog_as_posted = 1;
  else
    return virgule_send_error_page (vr, vERROR, "config",
			    "Unknown recentlog style found in site config.");

  /* read the cert levels */
  /* cert levels must be processed before processing action levels */
  stack = apr_array_make (vr->priv->pool, 10, sizeof (char *));
  node = virgule_xml_find_child (doc->xmlRootNode, "levels");
  if (node == NULL)
    return virgule_send_error_page (vr, vERROR, "config",
			    "No cert levels found in site config.");

  for (child = node->children; child; child = child->next)
    {
      if (child->type != XML_ELEMENT_NODE) {
        continue;
      }
      if (xmlStrcmp (child->name, (xmlChar *)"level"))
	return virgule_send_error_page (vr, vERROR, "config",
				"Unknown element <em>%s</em> in cert levels.",
				child->name);

      text = virgule_xml_get_string_contents (child);
      if (!text)
	return virgule_send_error_page (vr, vERROR, "config",
				"Empty element in cert levels.");

      c_item = (const char **)apr_array_push (stack);
      *c_item = apr_pstrdup(vr->priv->pool, text);
    }

  if (stack->nelts < 2)
    return virgule_send_error_page (vr, vERROR, "config",
			    "There must be at least two cert levels.");

  c_item = (const char **)apr_array_push (stack);
  *c_item = NULL;
  vr->priv->cert_level_names = (const char **)stack->elts;

  /* read the action levels (the cert level at which users can perform
     specific actions such as creating projects, posting articles, etc.
     Action levels cannot be processed *before* the cert levels */

  /* read the article post action level */
  vr->priv->level_articlepost = 0;
  text = virgule_xml_find_child_string (doc->xmlRootNode, "articlepost", NULL);
  if(text)
    vr->priv->level_articlepost = virgule_cert_level_from_name(vr, text);

  /* read the article reply action level */
  vr->priv->level_articlereply = 0;
  text = virgule_xml_find_child_string (doc->xmlRootNode, "articlereply", NULL);
  if(text)
    vr->priv->level_articlereply = virgule_cert_level_from_name(vr, text);

  /* read the project create  action level */
  vr->priv->level_projectcreate = 0;
  text = virgule_xml_find_child_string (doc->xmlRootNode, "projectcreate", NULL);
  if(text)
    vr->priv->level_projectcreate = virgule_cert_level_from_name(vr, text);

  /* read the blog syndication action level */
  vr->priv->level_blogsyndicate = 0;
  text = virgule_xml_find_child_string (doc->xmlRootNode, "blogsyndicate", NULL);
  if(text)
    vr->priv->level_blogsyndicate = virgule_cert_level_from_name(vr, text);

  /* read the optional list of editors */
  vr->priv->editors = NULL;
  stack = apr_array_make (vr->priv->pool, 10, sizeof (char *));
  node = virgule_xml_find_child (doc->xmlRootNode, "editors");
  if (node != NULL)
    {
      for (child = node->children; child; child = child->next)
        {
          if (child->type != XML_ELEMENT_NODE) {
            continue;
          } 
          if (xmlStrcmp (child->name, (xmlChar *)"editor"))
	    return virgule_send_error_page (vr, vERROR, "config",
				"Unknown element <em>%s</em> in editors.",
				child->name);

          text = virgule_xml_get_string_contents (child);
          if (!text)
	    return virgule_send_error_page (vr, vERROR, "config",
				"Empty element in seeds.");

          c_item = (const char **)apr_array_push (stack);
          *c_item = apr_pstrdup(vr->priv->pool, text);
        }

      if (stack->nelts > 0)
        {
          c_item = (const char **)apr_array_push (stack);
          *c_item = NULL;
          vr->priv->editors = (const char **)stack->elts;
        }
    }  


  /* read the trust metric seeds */
  stack = apr_array_make (vr->priv->pool, 10, sizeof (char *));
  node = virgule_xml_find_child (doc->xmlRootNode, "seeds");
  if (node == NULL)
    return virgule_send_error_page (vr, vERROR, "config",
			    "No seeds found in site config.");

  for (child = node->children; child; child = child->next)
    {
      if (child->type != XML_ELEMENT_NODE) {
        continue;
      } 
      if (xmlStrcmp (child->name, (xmlChar *)"seed"))
	return virgule_send_error_page (vr, vERROR, "config",
				"Unknown element <em>%s</em> in seeds.",
				child->name);

      text = virgule_xml_get_string_contents (child);
      if (!text)
	return virgule_send_error_page (vr, vERROR, "config",
				"Empty element in seeds.");

      c_item = (const char **)apr_array_push (stack);
      *c_item = apr_pstrdup(vr->priv->pool, text);
    }

  if (stack->nelts < 1)
    return virgule_send_error_page (vr, vERROR, "config",
			    "There must be at least one seed.");

  c_item = (const char **)apr_array_push (stack);
  *c_item = NULL;
  vr->priv->seeds = (const char **)stack->elts;

  /* read the capacities */
  stack = apr_array_make (vr->priv->pool, 10, sizeof (int));
  node = virgule_xml_find_child (doc->xmlRootNode, "caps");
  if (node == NULL)
    return virgule_send_error_page (vr, vERROR, "config",
			    "No capacities found in site config.");

  for (child = node->children; child; child = child->next)
    {
      if (child->type != XML_ELEMENT_NODE) {
        continue;
      }
      if (xmlStrcmp (child->name, (xmlChar *)"cap"))
	return virgule_send_error_page (vr, vERROR, "config",
				"Unknown element <em>%s</em> in capacities.",
				child->name);

      text = virgule_xml_get_string_contents (child);
      if (!text)
	return virgule_send_error_page (vr, vERROR, "config",
				"Empty element in capacities.");

      i_item = (int *)apr_array_push (stack);
      *i_item = atoi (text);
    }

  if (stack->nelts < 1)
    return virgule_send_error_page (vr, vERROR, "config",
			    "There must be at least one capacity.");

  i_item = (int *)apr_array_push (stack);
  *i_item = 0;
  vr->priv->caps = (const int *)stack->elts;

  /* read the special users */
  stack = apr_array_make (vr->priv->pool, 10, sizeof (char *));
  node = virgule_xml_find_child (doc->xmlRootNode, "specialusers");
  if (node)
    {
      for (child = node->children; child; child = child->next)
	{
          if (child->type != XML_ELEMENT_NODE) {
            continue;
          }
	  if (xmlStrcmp (child->name, (xmlChar *)"specialuser"))
	    return virgule_send_error_page (vr, vERROR, "config",
				    "Unknown element <em>%s</em> in special users.",
				    child->name);

	  text = virgule_xml_get_string_contents (child);
	  if (!text)
	    return virgule_send_error_page (vr, vERROR, "config",
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
  node = virgule_xml_find_child (doc->xmlRootNode, "translations");
  if (node)
    {
      for (child = node->children; child; child = child->next)
	{
          if (child->type != XML_ELEMENT_NODE) {
            continue;
          }
	  if (xmlStrcmp (child->name, (xmlChar *)"translate"))
	    return virgule_send_error_page (vr, vERROR, "config",
				    "Unknown element <em>%s</em> in translations.",
				    child->name);

	  text = virgule_xml_get_prop (vr->r->pool, child, (xmlChar *)"from");
	  c_item = (const char **)apr_array_push (stack);
          *c_item = apr_pstrdup(vr->priv->pool, text);

	  text = virgule_xml_get_prop (vr->r->pool, child, (xmlChar *)"to");
	  c_item = (const char **)apr_array_push (stack);
          *c_item = apr_pstrdup(vr->priv->pool, text);
	}
    }
  c_item = (const char **)apr_array_push (stack);
  *c_item = NULL;
  vr->priv->trans = (const char **)stack->elts;

  /* read the diary rating selection */
  text = virgule_xml_find_child_string (doc->xmlRootNode, "diaryrating", "");
  if (!strcasecmp (text, "on"))
    vr->priv->render_diaryratings = 1;
  else
    vr->priv->render_diaryratings = 0;

  /* read the new accounts allowed selection */
  text = virgule_xml_find_child_string (doc->xmlRootNode, "accountcreation", "");
  if (!strcasecmp (text, "off"))
    vr->priv->allow_account_creation = 0;
  else
    vr->priv->allow_account_creation = 1;

  /* read the new accounts charset allowed style */
  text = virgule_xml_find_child_string (doc->xmlRootNode, "accountextendedcharset", "");
  if (!strcasecmp (text, "off"))
    vr->priv->allow_account_extendedcharset = 0;
  else
    vr->priv->allow_account_extendedcharset = 1;

  /* read the article title links setting */
  text = virgule_xml_find_child_string (doc->xmlRootNode, "articletitlelinks", "");
  if (!strcasecmp (text, "off"))
    vr->priv->use_article_title_links = 0;
  else
    vr->priv->use_article_title_links = 1;

  /* read the article posts by editors only setting */
  text = virgule_xml_find_child_string (doc->xmlRootNode, "articlepostbyeditorsonly", "");
  if (!strcasecmp (text, "on"))
    vr->priv->article_post_by_editors_only = 1;
  else
    vr->priv->article_post_by_editors_only = 0;

  /* read the max article title size setting */
  text = virgule_xml_find_child_string (doc->xmlRootNode, "articletitlesize", "");
  vr->priv->article_title_maxsize = atoi (text);
  if(vr->priv->article_title_maxsize == 0)
    vr->priv->article_title_maxsize = 80; 

  /* read the number of days after posting to allow article editing */
  text = virgule_xml_find_child_string (doc->xmlRootNode, "articledays2edit", "");
  vr->priv->article_days_to_edit = atoi (text);
  if(vr->priv->article_days_to_edit == 0)
    vr->priv->article_days_to_edit = 0;

  /* read the article topic setting */
  text = virgule_xml_find_child_string (doc->xmlRootNode, "articletopics", "");
  if (!strcasecmp (text, "off"))
    vr->priv->use_article_topics = 0;
  else
    vr->priv->use_article_topics = 1;

  /* read the article topics */
  stack = apr_array_make (vr->priv->pool, 10, sizeof (Topic *));
  node = virgule_xml_find_child (doc->xmlRootNode, "topics");
  if (node)
    {
      for (child = node->children; child; child = child->next)
      {
        if (child->type != XML_ELEMENT_NODE) {
	  continue;
	}	
	if (xmlStrcmp (child->name, (xmlChar *)"topic"))
	  return virgule_send_error_page (vr, vERROR, "config",
				  "Unknown element <em>%s</em> in article topic.",
				  child->name);
	
	url = virgule_xml_get_prop (vr->r->pool, child, (xmlChar *)"url");
	text = virgule_xml_get_string_contents (child);
        if (!text)
          return virgule_send_error_page (vr, vERROR, "config",
                                      "Empty element in article topic.");

        at_item = (const Topic **)apr_array_push (stack);
        *at_item = virgule_add_topic (vr, text, url);	
      }
    }
  at_item = (const Topic **)apr_array_push (stack);
  *at_item = NULL;
  vr->priv->topics = (const Topic **)stack->elts;

  /* read the account spam score threshold setting */
  text = virgule_xml_find_child_string (doc->xmlRootNode, "accountspamthreshold", "");
  vr->priv->acct_spam_threshold = atoi (text);
  if(vr->priv->acct_spam_threshold == 0)
    vr->priv->acct_spam_threshold = 15; 

  /* read the sitemap navigation options */
  stack = apr_array_make (vr->priv->pool, 10, sizeof (NavOption *));
  node = virgule_xml_find_child (doc->xmlRootNode, "sitemap");
  if (node)
    {
      for (child = node->children; child; child = child->next)
      {
        if (child->type != XML_ELEMENT_NODE) {
	  continue;
	}	
	if (xmlStrcmp (child->name, (xmlChar *)"option"))
	  return virgule_send_error_page (vr, vERROR, "config",
				  "Unknown element <em>%s</em> in sitemap options.",
				  child->name);
	
	url = virgule_xml_get_prop (vr->r->pool, child, (xmlChar *)"url");
	text = virgule_xml_get_string_contents (child);
        if (!text)
          return virgule_send_error_page (vr, vERROR, "config",
                                      "Empty element in allowed sitemap options.");

        n_item = (const NavOption **)apr_array_push (stack);
        *n_item = virgule_add_nav_option (vr, text, url);	
      }
    }
  n_item = (const NavOption **)apr_array_push (stack);
  *n_item = NULL;
  vr->priv->nav_options = (const NavOption **)stack->elts;

  /* read the allowed tags */
  stack = apr_array_make (vr->priv->pool, 10, sizeof (AllowedTag *));
  node = virgule_xml_find_child (doc->xmlRootNode, "allowedtags");
  if (node)
    {
	for (child = node->children; child; child = child->next)
	{
	  const AllowedTag *allowed_tag;

          if (child->type != XML_ELEMENT_NODE) {
            continue;
          }
	  if (xmlStrcmp (child->name, (xmlChar *)"tag"))
	    return virgule_send_error_page (vr, vERROR, "config",
				    "Unknown element <em>%s</em> in allowed tags.",
				    child->name);

	  allowed_tag = read_allowed_tag (vr, child);
	  if (!allowed_tag)
	    return !CONFIG_READ;

	  t_item = (const AllowedTag **)apr_array_push (stack);
	  *t_item = allowed_tag;
	}
    }
  t_item = (const AllowedTag **)apr_array_push (stack);
  *t_item = NULL;
  vr->priv->allowed_tags = (const AllowedTag **)stack->elts;

/* debug 
ap_log_rerror(APLOG_MARK, APLOG_CRIT, APR_SUCCESS, vr->r,"Debug: read config.xml");
*/

  return CONFIG_READ;
}


/**
 * virgule_handler: Generates the content to fill a request
 */
static int virgule_handler(request_rec *r)
{
  virgule_dir_conf *cfg;
  int status;
  apr_finfo_t finfo;
  apr_status_t ap_status = APR_SUCCESS;
  VirguleReq *vr;

  if(strcmp(r->handler, "virgule")) {
    return DECLINED;
  }

  cfg = (virgule_dir_conf *)ap_get_module_config (r->per_dir_config, &virgule_module);

  /* Set libxml2 to old-style, incorrect handling of whitespace. This can
     be removed once all existing xml code is updated to handle blank nodes */
  xmlKeepBlanksDefault(0);

  vr = (VirguleReq *)apr_pcalloc (r->pool, sizeof (VirguleReq));

  vr->r = r;
  vr->b = virgule_buffer_new (r->pool);
  vr->hb = virgule_buffer_new (r->pool);
  vr->tb = NULL;
  vr->db = virgule_db_new_filesystem (r->pool, cfg->db); /* hack */

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

  vr->u = NULL;
  vr->args = virgule_get_args (r);
  vr->priv = NULL;
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
  apr_stat(&finfo, ap_make_full_path (r->pool, cfg->db, "config.xml"),
           APR_FINFO_MIN, r->pool);

  /* If config file was updated, dump and reload private data */
  if(vr->priv && (finfo.mtime != vr->priv->mtime))
  {
    if(vr->priv->tm_pool != NULL)
      apr_pool_destroy(vr->priv->tm_pool);
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
    
    /* remember the base path */
    vr->priv->base_path = apr_pstrdup(vr->priv->pool, cfg->db);
  }

  /* set buffer translations */
  virgule_buffer_set_translations (vr->b, vr->priv->trans);

  status = virgule_rss_serve (vr);
  if (status != DECLINED)
    return status;

  status = virgule_site_serve (vr);
  if (status != DECLINED)
    return status;

  status = virgule_acct_maint_serve (vr);
  if (status != DECLINED)
    return status;

  status = virgule_diary_serve (vr);
  if (status != DECLINED)
    return status;

  status = virgule_article_serve (vr);
  if (status != DECLINED)
    return status;

  status = virgule_proj_serve (vr);
  if (status != DECLINED)
    return status;

  status = virgule_xmlrpc_serve (vr);
  if (status != DECLINED)
    return status;  
  
  if (vr->priv->render_diaryratings)
    {
      status = virgule_rating_serve (vr);
      if (status != DECLINED)
	return status;
    }

  status = virgule_tmetric_serve (vr);
  if (status != DECLINED)
    return status;

  status = virgule_aggregator_serve (vr);
  if (status != DECLINED)
    return status;
    
  if (!strcmp (virgule_match_prefix(r->uri, vr->prefix), "/admin/info.html"))
    return info_page (vr);

  return HTTP_NOT_FOUND;
}


/**
 * xlat_handler: URI to Filename translator. This function is called by
 * Apache for each request to translate the URI to an appropriate request
 * and determine if the request is to be accepted for handling or declined.
 *
 * RSR Notes: I'm not certain that this is needed with Apache 2.x. The
 * cfg->db and cfg->pass_dirs checks should probably be moved to 
 * virgule_handler. The test would be whether or not the APR_REG work 
 * around for mod_dir works from virgule_handler.
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
  AP_INIT_TAKE1("VirguleDb", set_virgule_db, NULL, OR_ALL, "the virgule database"),
  AP_INIT_ITERATE("VirgulePass", set_virgule_pass, NULL, OR_ALL, "virgule passthrough directories"),
  {NULL}
};


static void virgule_register_hooks(apr_pool_t *p)
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
    virgule_create_dir_config,/* create per-dir    config structures */
    NULL,                     /* merge  per-dir    config structures */
    NULL,                     /* create per-server config structures */
    NULL,                     /* merge  per-server config structures */
    virgule_cmds,             /* command table */
    virgule_register_hooks            /* register hooks function */
};

