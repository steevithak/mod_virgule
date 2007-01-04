/* This module serves xml files into HTML, using a simple hardcoded
   stylesheet. */

#include <apr.h>
#include <apr_strings.h>
#include <httpd.h>
#include <http_log.h>
#include <http_core.h>
#include <http_config.h>
#include <http_protocol.h>
#include <ap_config.h>

#include <stdlib.h> /* for atof */

#include <libxml/tree.h>
#include <libxml/parser.h>

#include "private.h"
#include "buffer.h"
#include "db.h"
#include "req.h"
#include "style.h"
#include "db_xml.h"
#include "xml_util.h"
#include "article.h"
#include "certs.h"
#include "auth.h"
#include "aggregator.h"
#include "hashtable.h"
#include "eigen.h"
#include "diary.h"
#include "proj.h"
#include "wiki.h"
#include "auth.h"
#include "acct_maint.h"

#include "site.h"

typedef struct _RenderCtx RenderCtx;

struct _RenderCtx {
  int in_body;
  VirguleReq *vr;
  char *title;
  char *itag;
  char *istr;
};

typedef struct _AdInfo AdInfo;

struct _AdInfo {
  const char *img;
  const char *url;
  const char *imp;
  const char *max;
};

static void site_render (RenderCtx *ctx, xmlNode *node);

static void
site_render_children (RenderCtx *ctx, xmlNode *node)
{
  xmlNode *child;

  for (child = node->children; child != NULL; child = child->next)
    site_render (ctx, child);
}

/**
 * virgule_site_render_person_link: Render a link to a person.
 * @vr: #VirguleReq context.
 * @name: The person's name.
 *
 * Renders the link to the person, outputting to @vr's buffer.
 **/
void
virgule_site_render_person_link (VirguleReq *vr, const char *name)
{
  virgule_buffer_printf (vr->b, "<a href=\"%s/person/%s/\">%s</a>\n",
		 vr->prefix, ap_escape_uri(vr->r->pool, name), name);
}

/**
 * Reads banner ad information from the XML database. A random banner ad is
 # selected and displayed. Banner ads with 0 remaining impressions are 
 # removed from database. 
 **/
void
virgule_site_render_banner_ad (VirguleReq *vr)
{
  apr_pool_t *p = vr->r->pool;
  const char *imp, *img, *url;
  xmlDoc *adlist;
  xmlNode *ad;
  char new_imp[12];
  char *ad_key = "ads/adlist.xml";
  int impressions, i, sel;

  /* Read and parse the adlist.xml document */
  adlist = virgule_db_xml_get (p, vr->db, ad_key);
  if (adlist == NULL) 
    return;

  /* Find available ads */
  for (i = 0, ad = adlist->xmlRootNode->children; ad != NULL; ad = ad->next)
    if(!xmlIsBlankNode(ad))
      i++;

  if (i <= 0)
    return;
    
  /* Randomly select ad to be displayed */
  sel = (int) (rand() / (RAND_MAX / i + 1));

  /* Find the selected ad */
  for (i = 0, ad = adlist->xmlRootNode->children; i != sel && ad != NULL; ad = ad->next)
    if(!xmlIsBlankNode(ad))
      i++;

  // adjust impresion value
  imp = virgule_xml_get_prop (p, ad, "imp");
  impressions = atoi (imp);
  if (impressions <= 0)
  {
    xmlUnlinkNode (ad);
    xmlFreeNode (ad);
  }
  else
  {
    impressions--;
    snprintf (new_imp, sizeof new_imp, "%d", impressions);
    xmlSetProp (ad, "imp", new_imp);
  }
  virgule_db_xml_put (p, vr->db, ad_key, adlist);
    
  img = virgule_xml_get_prop (p, ad, "img");
  url = virgule_xml_get_prop (p, ad, "url");
  
  virgule_buffer_printf(vr->b, "<a href=\"%s\"><img src=\"%s?%ld\" width=\"468\" height=\"60\" vspace=\"4\" border=\"0\"></a>", url, img, (long)time(NULL) );
}


int
virgule_site_send_banner_ad (VirguleReq *vr)
{
  virgule_site_render_banner_ad (vr);
  return virgule_send_response (vr);
}


static void
site_render_recent_acct (VirguleReq *vr, const char *list, int n_max)
{
  apr_pool_t *p = vr->r->pool;
  char *key;
  xmlDoc *doc;
  xmlNode *root, *tree;
  int n;

  key = apr_psprintf (p, "recent/%s.xml", list);
  doc = virgule_db_xml_get (p, vr->db, key);
  if (doc == NULL)
    return;
  root = doc->xmlRootNode;
  n = 0;
  for (tree = root->last; tree != NULL && n < n_max; tree = tree->prev)
    {
      char *name = virgule_xml_get_string_contents (tree);
      char *date = virgule_xml_get_prop (p, tree, "date");
      virgule_render_cert_level_begin (vr, name, CERT_STYLE_SMALL);
      virgule_buffer_printf (vr->b, " %s ", virgule_render_date (vr, date, 0));
      virgule_site_render_person_link (vr, name);
      virgule_render_cert_level_text (vr, name);
      virgule_render_cert_level_end (vr, CERT_STYLE_SMALL);
      n++;
    }
}

/* It would make more sense to use log(), but I didn't feel like adding
   -lm to the build process. */
int
virgule_conf_to_gray (double confidence)
{
  int gray = 0;

  while (gray < 192 && confidence < 1)
    {
      confidence *= 1.02;
      gray++;
    }
  return gray;
}

static void
site_render_recent_changelog (VirguleReq *vr, int n_max)
{
  apr_pool_t *p = vr->r->pool;
  char *key;
  xmlDoc *doc;
  xmlNode *root, *tree;
  int n;
  HashTable *ev = NULL;
  apr_table_t *args;
  const char *thresh_str;
  double thresh = 0;
  int suppress_count = 0;
  apr_table_t *entries = NULL;

  key = apr_psprintf (p, "recent/%s.xml", "diary");
  doc = virgule_db_xml_get (p, vr->db, key);
  if (doc == NULL)
    return;

  args = virgule_get_args_table (vr);
  if (args && (thresh_str = apr_table_get (args, "thresh")))
    {
      thresh = atof (thresh_str);
    }

  virgule_auth_user (vr);
  if (vr->priv->render_diaryratings && vr->u)
    {
        char *eigen_key = apr_pstrcat (p, "eigen/vec/", vr->u, NULL);
	ev = virgule_eigen_vec_load (p, vr, eigen_key);
    }

  if (vr->priv->recentlog_as_posted)
    entries = apr_table_make (p, 4);

  root = doc->xmlRootNode;
  n = 0;
  for (tree = root->last; tree != NULL && n < n_max; tree = tree->prev)
    {
      char *name = virgule_xml_get_string_contents (tree);
      EigenVecEl *eve = NULL;
      int entry;

      if (xmlIsBlankNode(tree))
        continue;

      if (ev)
	{
	  char *dkey = apr_pstrcat (p, "d/", name, NULL);
	  eve = virgule_hash_table_get (ev, dkey);

	  if (eve && eve->rating < thresh)
	    {
	      suppress_count++;
	      continue;
	    }
	}

      if (vr->priv->recentlog_as_posted)
	{
	  const char *result = apr_table_get (entries, name);
	  if (result)
	    entry = atoi(result);
	  else
	    entry = virgule_db_dir_max (vr->db, apr_psprintf (p, "acct/%s/diary", name));

	  apr_table_set (entries, name, apr_psprintf (p, "%d", entry - 1));
	}
      else
	entry = virgule_db_dir_max (vr->db, apr_psprintf (p, "acct/%s/diary", name));

      if (entry >= 0)
	virgule_diary_entry_render (vr, name, entry, eve, 1);
      n++;
    }

  if (suppress_count)
    {
      virgule_buffer_printf (vr->b, "<p><a href=\"%s/recentlog.html\">%d entr%s suppressed</a> at threshold %g.</p>\n",
		     vr->prefix,
		     suppress_count, suppress_count == 1 ? "y" : "ies",
		     thresh);
    }
}

static void
site_render_recent_proj (VirguleReq *vr, const char *list, int n_max)
{
  apr_pool_t *p = vr->r->pool;
  char *key;
  xmlDoc *doc;
  xmlNode *root, *tree;
  int n;

  key = apr_psprintf (p, "recent/%s.xml", list);
  doc = virgule_db_xml_get (p, vr->db, key);
  if (doc == NULL)
    return;
  root = doc->xmlRootNode;
  n = 0;
  for (tree = root->last; tree != NULL && n < n_max; tree = tree->prev)
    {
      char *name = virgule_xml_get_string_contents (tree);
      char *date = virgule_xml_get_prop (p, tree, "date");
      
      if (vr->priv->projstyle == PROJSTYLE_NICK)
	{
	  char *creator;
	  char *db_key = apr_psprintf (p, "proj/%s/info.xml", name);
	  xmlDoc *proj_doc;
	  xmlNode *proj_tree;
	  char *lastread_date;
	  char *newmarker = "";
  
	  proj_doc = virgule_db_xml_get (p, vr->db, db_key);
	  if (proj_doc == NULL) {
	    /* the project doesn't exist, so skip it */
	    continue;
	  }
  
	  proj_tree = virgule_xml_find_child (proj_doc->xmlRootNode, "info");
	  if (proj_tree != NULL) {
	    creator = virgule_xml_get_prop (p, proj_tree, "creator");
	  } else {
	    /* No creator?  Skip it. */
	    continue;
	  }
  
	  /* do new checking here */
	  lastread_date = virgule_acct_get_lastread_date (vr, "proj", name);
	  if (lastread_date != NULL)
	    if (strcmp (date, lastread_date) > 0)
	      newmarker = " &raquo; ";
	  virgule_render_cert_level_begin (vr, creator, CERT_STYLE_SMALL);
	  virgule_buffer_printf (vr->b, " %s %s %s\n",
			 newmarker, virgule_render_date (vr, date, 0), 
			 virgule_render_proj_name (vr, name));
	  virgule_render_cert_level_text (vr, creator);
	  virgule_render_cert_level_end (vr, CERT_STYLE_SMALL);
	  virgule_db_xml_free (p, vr->db, proj_doc);
	}        
      else
        {
	  virgule_buffer_printf (vr->b, "<div class=\"recentproj\"> %s %s</div>\n",
			 virgule_render_date (vr, date, 0), virgule_render_proj_name (vr, name));
	}
      n++;
    }
}


static void
site_render_include (RenderCtx *ctx, VirguleReq *vr, char *path)
{
  apr_pool_t *p = vr->r->pool;
  xmlDoc *doc;
  xmlNode *root;

  doc = virgule_db_xml_get (p, vr->db, path);
  if (doc == NULL)
    return;
  root = doc->xmlRootNode;
  site_render_children (ctx, root);
}



/** 
 * Render tags within an XML template
 **/
static void
site_render (RenderCtx *ctx, xmlNode *node)
{
  VirguleReq *vr = ctx->vr;
  apr_pool_t *p = vr->r->pool;
  Buffer *b = vr->b;
  apr_table_t *args;

  args = virgule_get_args_table (vr);

  if (node->type == XML_TEXT_NODE)
    {
      virgule_buffer_puts (b, node->content);
    }
  else if (node->type == XML_ELEMENT_NODE)
    {

      if (!strcmp (node->name, "page"))
	{
	  site_render_children (ctx, node);
	}
      else if (!strcmp (node->name, "title"))
	{
	  /* skip */
	}
      else if (!strcmp (node->name, "head_content"))
        {
	  /* skip */
	}
      else if (!strcmp (node->name, "thetitle"))
	{
	  virgule_buffer_puts (b, ctx->title);
	}
      else if (!strcmp (node->name, "br"))
	{
	  virgule_buffer_puts (b, "<br>\n");
	}
      else if (!strcmp (node->name, "dt"))
	{
	  virgule_buffer_puts (b, "<dt>\n");
	  site_render_children (ctx, node);
	}
      else if (!strcmp (node->name, "recent"))
	{
	  char *list, *nmax_str;
	  int nmax;

	  list = virgule_xml_get_prop (p, node, "list");
	  nmax_str = virgule_xml_get_prop (p, node, "nmax");
	  if (nmax_str)
	    nmax = atoi (nmax_str);
	  else
	    nmax = 10;
	  site_render_recent_acct (vr, list, nmax);
	}
      else if (!strcmp (node->name, "recentlog"))
	{
	  char *nmax_str;
	  int nmax;

	  nmax_str = virgule_xml_get_prop (p, node, "nmax");
	  if (nmax_str)
	    nmax = atoi (nmax_str);
	  else
	    nmax = 10;
	  site_render_recent_changelog (vr, nmax);
	}
      else if (!strcmp (node->name, "recentproj"))
	{
	  char *list, *nmax_str;
	  int nmax;

	  list = virgule_xml_get_prop (p, node, "list");
	  nmax_str = virgule_xml_get_prop (p, node, "nmax");
	  if (nmax_str)
	    nmax = atoi (nmax_str);
	  else
	    nmax = 10;
	  site_render_recent_proj (vr, list, nmax);
	}
      else if (!strcmp (node->name, "articles"))
	{
	  char *list, *nmax_str;
	  int nmax;
	  int start = -1;

      	  list = virgule_xml_get_prop (p, node, "list");
	  nmax_str = virgule_xml_get_prop (p, node, "nmax");
	  if (nmax_str)
	    nmax = atoi (nmax_str);
	  else
	    nmax = 10;

	  errno = 0;
	  if (args)
            start = atoi (apr_table_get (args, "start"));
	  if (errno)
	    start = -1;
	    
	  virgule_article_recent_render (vr, nmax, start);
	}
      else if (!strcmp (node->name, "userlist"))
        {
	  char *nmax_str;
	  int nmax;

	  nmax_str = virgule_xml_get_prop (p, node, "nmax");
	  if (nmax_str)
	    nmax = atoi (nmax_str);
	  else
	    nmax = 30;
	  virgule_acct_person_index_serve (vr, nmax);
	}
      else if (!strcmp (node->name, "sitemap"))
	{
	  virgule_render_sitemap (vr, 0);
	}
      else if (!strcmp (node->name, "acctname"))
        {
	  virgule_auth_user(vr);
	  if (vr->u == NULL)
	      virgule_buffer_printf (b, "Not logged in");
	  else
	      virgule_buffer_printf (b, vr->u);
        }
      else if (!strcmp (node->name, "isloggedin"))
	{
	    virgule_auth_user(vr);
	    if (vr->u != NULL)
		site_render_children (ctx, node);
	}
      else if (!strcmp (node->name, "notloggedin"))
	{
	    virgule_auth_user(vr);
	    if (vr->u == NULL)
		site_render_children (ctx, node);
	}
      else if (!strcmp (node->name, "newaccountsallowed"))
        {
	    if (vr->priv->allow_account_creation)
	        site_render_children (ctx, node);
	}
      else if (!strcmp (node->name, "nonewaccountsallowed"))
        {
	    if (!vr->priv->allow_account_creation)
	        site_render_children (ctx, node);
	}
      else if (!strcmp (node->name, "canpost"))
	{
	    if (virgule_req_ok_to_post (vr))
		site_render_children (ctx, node);
	}
      else if (!strcmp (node->name, "diarybox"))
	{
	    const char *key, *diary;
	    diary = apr_psprintf (p, "acct/%s/diary", vr->u);
	    key = apr_psprintf (p, "%d", virgule_db_dir_max (vr->db, diary) + 1);

            virgule_buffer_printf (b, 
		 "<form method=\"POST\" action=\"/diary/post.html\">\n"
		 " <textarea name=\"entry\" cols=60 rows=8 wrap=soft>%s"
		 "</textarea>\n"
		 " <p> <input type=\"submit\" name=post value=\"Post\">\n"
		 " <input type=\"submit\" name=preview value=\"Preview\">\n"
		 " <input type=\"hidden\" name=key value=\"%s\">\n"
		 "</form>\n", ap_escape_html(p, virgule_diary_get_backup(vr)), key);
	}
#if 0
      else if (!strcmp (node->name, "wiki"))
        {
	  virgule_buffer_puts (b, virgule_wiki_link (vr, virgule_xml_get_string_contents (node)));
	}
#endif
      else if (!strcmp (node->name, "bannerad"))
        {
	  virgule_site_render_banner_ad (vr);
	}
      else if (!strcmp (node->name, "include"))
        {
	  char *inc_path;
	  
	  inc_path = virgule_xml_get_prop (p, node, "path");
	  site_render_include (ctx, vr, inc_path);
	}
      else if ((ctx->itag != NULL) && (!strcmp (node->name, ctx->itag)))
        {
	  virgule_buffer_puts (b, ctx->istr);
	}
      else
	{
	  xmlAttr *a;
	  /* default: just pass through */
	  virgule_buffer_append (b, "<", node->name, NULL);
	  for (a = node->properties; a != NULL; a = a->next)
	    {
	      virgule_buffer_append (b, " ", a->name, "=\"", NULL);
	      site_render (ctx, a->children);
	      virgule_buffer_puts (b, "\"");
	    }
	  virgule_buffer_puts (b, ">");
	  site_render_children (ctx, node);
	  if (strcmp (node->name, "input") && strcmp (node->name, "img"))
	    virgule_buffer_append (b, "</", node->name, ">", NULL);
	}
    }
}


/**
 * Read XML template, parse title and header, and pass to site_render()
 **/
int
virgule_site_render_page (VirguleReq *vr, xmlNode *node, char *itag, char *istr, char *ititle)
{
  RenderCtx ctx;
  xmlNode *title_node;
  xmlNode *head_node;
  char *title = NULL;
  char *head = NULL;
  char *raw;

  ctx.vr = vr;
  
  if(ititle)
  {
    title = ititle;
  }
  else
  {
    title_node = virgule_xml_find_child (node, "title");
    if (title_node == NULL)
      title = "(no title)";
    else
      title = virgule_xml_get_string_contents (title_node);
  }
  
  head_node = virgule_xml_find_child (node, "head_content");
  if (head_node != NULL)
    head = virgule_xml_get_string_contents (head_node);

//
//ap_log_rerror(APLOG_MARK, APLOG_CRIT, 0, vr->r, "title: [%s]", title);
//

  ctx.title = title;
  ctx.itag = itag;
  ctx.istr = istr;
  raw = virgule_xml_get_prop (vr->r->pool, node, "raw");
  if (raw)
    virgule_render_header_raw (vr, title, head);
  else
    virgule_render_header (vr, title, head);
  site_render (&ctx, node);

  return virgule_render_footer_send (vr);
}

/**
* Attempts to match the URI in the request to an XML template in the site
* directory. If a match is found, the XML template is parsed and rendered.
* If no match is found, an Apache error code should be returned.
**/
int
virgule_site_serve (VirguleReq *vr)
{
  request_rec *r = vr->r;
  Buffer *b = vr->b;
  Db *db = vr->db;
  char *uri = vr->uri;
  char *key;
  char *val;
  int val_size;
  int ix;
  char *content_type = NULL;
  int is_xml = 0;
  int len;
  int return_code;

  len = strlen (uri);
  if (len == 0)
    uri = apr_pstrcat (r->pool, uri, "/index.html", NULL);
  else if (len > 0 && uri[len - 1] == '/')
    uri = apr_pstrcat (r->pool, uri, "index.html", NULL);

#if 0
  r->content_type = "text/plain; charset=UTF-8";
  virgule_buffer_puts (b, uri);
  return virgule_send_response (vr);
#endif

  for (ix = strlen (uri) - 1; ix >= 0; ix--)
    if (uri[ix] == '.' || uri[ix] == '/')
      break;
  if (ix >= 0 && uri[ix] == '.')
    {
      /* there is an extension */
      if (!strcmp (uri + ix + 1, "html"))
	{
	  content_type = "text/html; charset=UTF-8";

	  is_xml = 1;
	  uri = apr_pstrcat (r->pool, apr_pstrndup (r->pool, uri, ix + 1),
			    "xml", NULL);
	}
    }

  key = apr_pstrcat (r->pool, "/site", uri, NULL);

  /* The ordering (doing content type first, dir detection second) is
     not pleasing. */
  if (virgule_db_is_dir (db, key))
    {
      apr_table_add (r->headers_out, "Location",
		    ap_make_full_path (r->pool, vr->uri, ""));
      return HTTP_MOVED_PERMANENTLY;
    }

  if (content_type == NULL)
    return DECLINED;

  r->content_type = content_type;

  val = virgule_db_get (db, key, &val_size);

  if (val == NULL)
    return DECLINED;

  if (is_xml)
    {
      xmlDoc *doc;

      doc = xmlParseMemory (val, val_size);
      if (doc == NULL)
	{
	  virgule_buffer_puts (b, "xml parsing error\n");
	}
      else
	{
	  xmlNode *root;

	  root = doc->xmlRootNode;
	  return_code = virgule_site_render_page (vr, root, NULL, NULL, NULL);
	  xmlFreeDoc (doc);
	  return return_code;
	}
    }
  else
    {
      virgule_buffer_write (b, val, val_size);
    }

  return virgule_send_response (vr);
}
