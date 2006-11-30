/* This module serves xml files into HTML, using a simple hardcoded
   stylesheet. */

#include "httpd.h"
#include "http_config.h"
#include "http_protocol.h"
#include "ap_config.h"

#include <stdlib.h> /* for atof */

#include <tree.h>
#include <parser.h>

#include "buffer.h"
#include "db.h"
#include "req.h"
#include "style.h"
#include "db_xml.h"
#include "xml_util.h"
#include "article.h"
#include "certs.h"
#include "diary.h"
#include "proj.h"
#include "wiki.h"
#include "auth.h"
#include "acct_maint.h"

#include "auth.h"
#include "hashtable.h"
#include "eigen.h"

#include "site.h"

typedef struct _RenderCtx RenderCtx;

struct _RenderCtx {
  int in_body;
  VirguleReq *vr;
  char *title;
};

static void site_render (RenderCtx *ctx, xmlNode *node);


static void
site_render_children (RenderCtx *ctx, xmlNode *node)
{
  xmlNode *child;

  for (child = node->childs; child != NULL; child = child->next)
    site_render (ctx, child);
}

/**
 * site_render_person_link: Render a link to a person.
 * @vr: #VirguleReq context.
 * @name: The person's name.
 *
 * Renders the link to the person, outputting to @vr's buffer.
 **/
static void
site_render_person_link (VirguleReq *vr, const char *name)
{
  buffer_printf (vr->b, "<a href=\"%s/person/%s/\" style=\"text-decoration: none\">%s</a>\n",
		 vr->prefix, name, name);
}

static void
site_render_recent_acct (VirguleReq *vr, const char *list, int n_max)
{
  pool *p = vr->r->pool;
  char *key;
  xmlDoc *doc;
  xmlNode *root, *tree;
  int n;

  key = ap_psprintf (p, "recent/%s.xml", list);
  doc = db_xml_get (p, vr->db, key);
  if (doc == NULL)
    return;
  root = doc->root;
  n = 0;
  for (tree = root->last; tree != NULL && n < n_max; tree = tree->prev)
    {
      char *name = xml_get_string_contents (tree);
      char *date = xml_get_prop (p, tree, "date");
      render_cert_level_begin (vr, name, CERT_STYLE_SMALL);
      buffer_printf (vr->b, " %s ", render_date (vr, date));
      site_render_person_link (vr, name);
      render_cert_level_text (vr, name);
      render_cert_level_end (vr, CERT_STYLE_SMALL);
      n++;
    }
}

/* It would make more sense to use log(), but I didn't feel like adding
   -lm to the build process. */
static int
conf_to_gray (double confidence)
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
  pool *p = vr->r->pool;
  char *key;
  xmlDoc *doc;
  xmlNode *root, *tree;
  int n;
  HashTable *ev = NULL;
  table *args;
  const char *thresh_str;
  double thresh = 0;
  int suppress_count = 0;
  table *entries = NULL;

  key = ap_psprintf (p, "recent/%s.xml", "diary");
  doc = db_xml_get (p, vr->db, key);
  if (doc == NULL)
    return;

  auth_user (vr);

  args = get_args_table (vr);
  if (args && (thresh_str = ap_table_get (args, "thresh")))
    {
      thresh = atof (thresh_str);
    }

  if (vr->render_diaryratings)
    {
      if (vr->u)
	{
	  char *eigen_key = ap_pstrcat (p, "eigen/vec/", vr->u, NULL);
	  ev = eigen_vec_load (p, vr, eigen_key);
	}
    }

  if (vr->recentlog_as_posted)
    entries = ap_make_table (p, 4);

  root = doc->root;
  n = 0;
  for (tree = root->last; tree != NULL && n < n_max; tree = tree->prev)
    {
      char *name = xml_get_string_contents (tree);
      char *date;
      EigenVecEl *eve = NULL;
      int entry;

      if (ev)
	{
	  char *dkey = ap_pstrcat (p, "d/", name, NULL);
	  eve = hash_table_get (ev, dkey);

	  if (eve && eve->rating < thresh)
	    {
	      suppress_count++;
	      continue;
	    }
	}

      date = xml_get_prop (p, tree, "date");
      render_cert_level_begin (vr, name, CERT_STYLE_MEDIUM);
      buffer_printf (vr->b, " %s ", render_date (vr, date));
      site_render_person_link (vr, name);
      if (eve)
	{
	  int gray = conf_to_gray (eve->confidence);

	  buffer_printf (vr->b,
			 " <span style=\"{color: #%02x%02x%02x}\">(%.2g)</span>",
			 gray, gray, gray,
			 eve->rating);
	}
      render_cert_level_text (vr, name);

      if (vr->recentlog_as_posted)
	{
	  const char *result = ap_table_get (entries, name);
	  if (result)
	    entry = atoi(result);
	  else
	    entry = db_dir_max (vr->db, ap_psprintf (p, "acct/%s/diary", name));

	  ap_table_set (entries, name, ap_psprintf (p, "%d", entry - 1));
	}
      else
	entry = db_dir_max (vr->db, ap_psprintf (p, "acct/%s/diary", name));

      buffer_printf (vr->b, " <a href=\"%s/person/%s/diary.html?start=%u\" style=\"text-decoration: none\">&raquo;</a>",
		     vr->prefix, name, entry);

      if (vr->u && strcmp(vr->u, name) == 0)
	buffer_printf (vr->b, " &nbsp; <a href=\"%s/diary/edit.html?key=%u\" style=\"text-decoration: none\">[Edit]</a>",
		       vr->prefix, entry);

      render_cert_level_end (vr, CERT_STYLE_MEDIUM);

      if (entry >= 0)
	diary_latest_render (vr, name, entry);
      n++;
    }

  if (suppress_count)
    {
      buffer_printf (vr->b, "<p><a href=\"%s/recentlog.html\">%d entr%s suppressed</a> at threshold %g.</p>\n",
		     vr->prefix,
		     suppress_count, suppress_count == 1 ? "y" : "ies",
		     thresh);
    }
}

static void
site_render_recent_proj (VirguleReq *vr, const char *list, int n_max)
{
  pool *p = vr->r->pool;
  char *key;
  xmlDoc *doc;
  xmlNode *root, *tree;
  int n;

  key = ap_psprintf (p, "recent/%s.xml", list);
  doc = db_xml_get (p, vr->db, key);
  if (doc == NULL)
    return;
  root = doc->root;
  n = 0;
  for (tree = root->last; tree != NULL && n < n_max; tree = tree->prev)
    {
      char *name = xml_get_string_contents (tree);
      char *date = xml_get_prop (p, tree, "date");

      if (vr->projstyle == PROJSTYLE_RAPH)
	{
	  buffer_printf (vr->b, " %s %s\n",
			 render_date (vr, date), render_proj_name (vr, name));
	}
      else
	{
	  char *creator;
	  char *db_key = ap_psprintf (p, "proj/%s/info.xml", name);
	  xmlDoc *proj_doc;
	  xmlNode *proj_tree;
	  char *lastread_date;
	  char *newmarker = "";
  
	  proj_doc = db_xml_get (p, vr->db, db_key);
	  if (proj_doc == NULL) {
	    /* the project doesn't exist, so skip it */
	    continue;
	  }
  
	  proj_tree = xml_find_child (proj_doc->root, "info");
	  if (proj_tree != NULL) {
	    creator = xml_get_prop (p, proj_tree, "creator");
	  } else {
	    /* No creator?  Skip it. */
	    continue;
	  }
  
	  /* do new checking here */
	  lastread_date = acct_get_lastread_date (vr, "proj", name);
	  if (lastread_date != NULL)
	    if (strcmp (date, lastread_date) > 0)
	      newmarker = " &raquo; ";
	  render_cert_level_begin (vr, creator, CERT_STYLE_SMALL);
	  buffer_printf (vr->b, " %s %s %s\n",
			 newmarker, render_date (vr, date), 
			 render_proj_name (vr, name));
	  render_cert_level_text (vr, creator);
	  render_cert_level_end (vr, CERT_STYLE_SMALL);
	  db_xml_free (p, vr->db, proj_doc);
	}
      n++;
    }
}

static void
site_render (RenderCtx *ctx, xmlNode *node)
{
  VirguleReq *vr = ctx->vr;
  pool *p = vr->r->pool;
  Buffer *b = vr->b;
  if (node->type == XML_TEXT_NODE)
    {
      buffer_puts (b, node->content);
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
      else if (!strcmp (node->name, "thetitle"))
	{
	  buffer_puts (b, ctx->title);
	}
      else if (!strcmp (node->name, "br"))
	{
	  buffer_puts (b, "<br>\n");
	}
      else if (!strcmp (node->name, "dt"))
	{
	  buffer_puts (b, "<dt>\n");
	  site_render_children (ctx, node);
	}
      else if (!strcmp (node->name, "recent"))
	{
	  char *list, *nmax_str;
	  int nmax;

	  list = xml_get_prop (p, node, "list");
	  nmax_str = xml_get_prop (p, node, "nmax");
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

	  nmax_str = xml_get_prop (p, node, "nmax");
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

	  list = xml_get_prop (p, node, "list");
	  nmax_str = xml_get_prop (p, node, "nmax");
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

	  list = xml_get_prop (p, node, "list");
	  nmax_str = xml_get_prop (p, node, "nmax");
	  if (nmax_str)
	    nmax = atoi (nmax_str);
	  else
	    nmax = 10;
	  article_recent_render (vr, nmax, -1);
	}
      else if (!strcmp (node->name, "sitemap"))
	{
	  render_sitemap (vr, 0);
	}
      else if (!strcmp (node->name, "acctname"))
        {
	  auth_user(vr);
	  if (vr->u == NULL)
	      buffer_printf (b, "Not logged in");
	  else
	      buffer_printf (b, vr->u);
        }
      else if (!strcmp (node->name, "isloggedin"))
	{
	    auth_user(vr);
	    if (vr->u != NULL)
		site_render_children (ctx, node);
	}
      else if (!strcmp (node->name, "notloggedin"))
	{
	    auth_user(vr);
	    if (vr->u == NULL)
		site_render_children (ctx, node);
	}
      else if (!strcmp (node->name, "newaccountsallowed"))
	{
	    if (vr->allow_account_creation)
		site_render_children (ctx, node);
	}
      else if (!strcmp (node->name, "nonewaccountsallowed"))
	{
	    if (!vr->allow_account_creation)
		site_render_children (ctx, node);
	}
      else if (!strcmp (node->name, "canpost"))
	{
	    if (req_ok_to_post (vr))
		site_render_children (ctx, node);
	}
      else if (!strcmp (node->name, "diarybox"))
	{
	    const char *key, *diary;
	    diary = ap_psprintf (p, "acct/%s/diary", vr->u);
	    key = ap_psprintf (p, "%d", db_dir_max (vr->db, diary) + 1);

            buffer_printf (b, 
		 "<form method=\"POST\" action=\"/diary/post.html\">\n"
		 " <textarea name=\"entry\" cols=60 rows=8 wrap=soft>%s"
		 "</textarea>\n"
		 " <p> <input type=\"submit\" name=post value=\"Post\">\n"
		 " <input type=\"submit\" name=preview value=\"Preview\">\n"
		 " <input type=\"hidden\" name=key value=\"%s\">\n"
		 "</form>\n", ap_escape_html(p, diary_get_backup(vr)), key);
	}
#if 0
      else if (!strcmp (node->name, "wiki"))
	{
	  buffer_puts (b, wiki_link (vr, xml_get_string_contents (node)));
	}
#endif
      else
	{
	  xmlAttr *a;
	  /* default: just pass through */
	  buffer_append (b, "<", node->name, NULL);
	  for (a = node->properties; a != NULL; a = a->next)
	    {
	      buffer_append (b, " ", a->name, "=\"", NULL);
	      site_render (ctx, a->val);
	      buffer_puts (b, "\"");
	    }
	  buffer_puts (b, ">");
	  if (!strcmp (node->name, "td"))
	    render_table_open (vr);
	  site_render_children (ctx, node);
	  if (!strcmp (node->name, "td"))
	    render_table_close (vr);
	  if (strcmp (node->name, "input") && strcmp (node->name, "img"))
	    buffer_append (b, "</", node->name, ">\n", NULL);
	}
    }
}

static int
site_render_page (VirguleReq *vr, xmlNode *node)
{
  RenderCtx ctx;
  xmlNode *title_node;
  char *title;
  char *raw;

  ctx.vr = vr;

  title_node = xml_find_child (node, "title");
  if (title_node == NULL)
    title = "(no title)";
  else
    title = xml_get_string_contents (title_node);

  ctx.title = title;
  raw = xml_get_prop (vr->r->pool, node, "raw");
  if (raw)
    render_header_raw (vr, title);
  else
    render_header (vr, title);
  site_render (&ctx, node);

  return render_footer_send (vr);
}

/* Return Apache error code. */
int
site_serve (VirguleReq *vr)
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
    uri = ap_pstrcat (r->pool, uri, "/index.html", NULL);
  else if (len > 0 && uri[len - 1] == '/')
    uri = ap_pstrcat (r->pool, uri, "index.html", NULL);

#if 0
  r->content_type = "text/plain; charset=ISO-8859-1";
  buffer_puts (b, uri);
  return send_response (vr);
#endif

  for (ix = strlen (uri) - 1; ix >= 0; ix--)
    if (uri[ix] == '.' || uri[ix] == '/')
      break;
  if (ix >= 0 && uri[ix] == '.')
    {
      /* there is an extension */
      if (!strcmp (uri + ix + 1, "html"))
	{
	  content_type = "text/html; charset=ISO-8859-1";

	  is_xml = 1;
	  uri = ap_pstrcat (r->pool, ap_pstrndup (r->pool, uri, ix + 1),
			    "xml", NULL);
	}
    }

  key = ap_pstrcat (r->pool, "/site", uri, NULL);

  /* The ordering (doing content type first, dir detection second) is
     not pleasing. */
  if (db_is_dir (db, key))
    {
      ap_table_add (r->headers_out, "Location",
		    ap_make_full_path (r->pool, vr->uri, ""));
      return REDIRECT;
    }

  if (content_type == NULL)
    return DECLINED;

  r->content_type = content_type;

  val = db_get (db, key, &val_size);

  if (val == NULL)
    return DECLINED;

  if (is_xml)
    {
      xmlDoc *doc;

      doc = xmlParseMemory (val, val_size);
      if (doc == NULL)
	{
	  buffer_puts (b, "xml parsing error\n");
	}
      else
	{
	  xmlNode *root;

	  root = doc->root;
	  return_code = site_render_page (vr, root);
	  xmlFreeDoc (doc);
	  return return_code;
	}
    }
  else
    {
      buffer_write (b, val, val_size);
    }

  return send_response (vr);
}
