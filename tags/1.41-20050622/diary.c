/* A module for managing diaries. */

#include <time.h>

#include <apr.h>
#include <apr_strings.h>
#include <httpd.h>

#include <libxml/tree.h>
#include <libxml/HTMLparser.h>

#include "buffer.h"
#include "db.h"
#include "req.h"
#include "util.h"
#include "style.h"
#include "auth.h"
#include "db_xml.h"
#include "xml_util.h"
#include "db_ops.h"

#include "rss_export.h"
#include "diary.h"

static char *
validate_key(VirguleReq *vr, const char *diary, const char *key)
{
  apr_pool_t *p = vr->r->pool;
  int key_val;

  /* Validate key */
  if (!key)
    return NULL;
  key_val = atoi(key);
  if (key_val < -1 || key_val > db_dir_max(vr->db, diary) + 1)
    return NULL;

  /* Autoassign next available key for incoming remote post */
  if (key_val == -1)
    key_val = db_dir_max (vr->db, diary) + 1;

  /* Create XML key */
  return apr_psprintf (p, "acct/%s/diary/_%d", vr->u, key_val);
}

/* renders into @vr's buffer */
void
diary_render (VirguleReq *vr, const char *u, int max_num, int start)
{
  apr_pool_t *p = vr->r->pool;
  Buffer *b = vr->b;
  char *diary;
  int n, is_owner = 0;

  diary = apr_psprintf (p, "acct/%s/diary", u);

  auth_user (vr);
  if (vr->u && strcmp(vr->u, u) == 0)
    {
      /* The user is viewing his or her own diary */
      is_owner = 1;
    }

  if (start >= 0)
    n = start;
  else
    n = db_dir_max (vr->db, diary);

  if (n < 0)
    return;

  for (; n >= 0 && max_num--; n--)
    {
      char *key;
      xmlDoc *entry;

      key = apr_psprintf (p, "acct/%s/diary/_%d", u, n);
#if 0
      buffer_printf (b, "<p> Key: %s </p>\n", key);
      continue;
#endif
      entry = db_xml_get (p, vr->db, key);
      if (entry != NULL)
	{
	  xmlNode *date_el;
	  char *contents;

	  date_el = xml_find_child (entry->xmlRootNode, "date");
	  if (date_el != NULL)
	    {
	      xmlNode *update_el;
	      buffer_printf (b, "<p> ");
	      buffer_printf (b, "<a name=\"%u\"><b>%s</b></a>",
			     n,
			     render_date (vr,
					  xml_get_string_contents (date_el),
					  0));
	      update_el = xml_find_child (entry->xmlRootNode, "update");
	      if (update_el != NULL)
		buffer_printf (b, " (updated %s)",
			       render_date (vr,
					    xml_get_string_contents (update_el),
					    1));

              buffer_printf (b, "&nbsp; <a href=\"%s/person/%s/diary.html?start=%u\">&raquo;</a>",
			     vr->prefix,
                             ap_escape_uri(vr->r->pool, u),
                             n); 
	      if (is_owner)
	          buffer_printf (b, " <a href=\"%s/diary/edit.html?key=%u\">[ Edit ]</a>",
				 vr->prefix,
				 n);
	      buffer_printf (b, " </p>\n");
	    }
	  contents = xml_get_string_contents (entry->xmlRootNode);
	  if (contents != NULL)
	    {
	      buffer_puts (b, "<blockquote>\n");
	      buffer_puts (b, contents);
	      buffer_puts (b, "</blockquote>\n");
	    }
	}
    }

  if (n >= 0)
    {
      buffer_printf (b, "<p> <a href=\"%s/person/%s/diary.html?start=%d\">%d older entr%s...</a> </p>\n",
		     vr->prefix, ap_escape_uri(vr->r->pool, u), n,
		     n + 1, n == 0 ? "y" : "ies");
    }
}

/* renders the latest diary entry into @vr's buffer */
void
diary_latest_render (VirguleReq *vr, const char *u, int n)
{
  apr_pool_t *p = vr->r->pool;
  Buffer *b = vr->b;
  char *key;
  xmlDoc *entry;

  key = apr_psprintf (p, "acct/%s/diary/_%d", u, n);
  entry = db_xml_get (p, vr->db, key);
  if (entry != NULL)
    {
      char *contents;
  
      contents = xml_get_string_contents (entry->xmlRootNode);
      buffer_puts (b, "<blockquote>\n");
      if (contents != NULL)
	{
	  buffer_puts (b, contents);
	}
      buffer_puts (b, "</blockquote>\n");
    }
}

char *
diary_get_backup(VirguleReq *vr)
{
  apr_pool_t *p = vr->r->pool;
  char *key, *val;
  int val_size;

  auth_user(vr);
  if (vr->u == NULL)
    return NULL;

  key = apr_psprintf (p, "acct/%s/diarybackup", vr->u);
  val = db_get (vr->db, key, &val_size);
  if (val == NULL)
    return "";

  return val;
}

static int
diary_put_backup(VirguleReq *vr, const char *entry)
{
  apr_pool_t *p = vr->r->pool;
  char *key;

  auth_user(vr);
  if (vr->u == NULL)
    return -1;

  key = apr_psprintf (p, "acct/%s/diarybackup", vr->u);
  return db_put (vr->db, key, entry, strlen(entry));
}

static int
diary_preview_serve (VirguleReq *vr)
{
  apr_pool_t *p = vr->r->pool;
  Buffer *b = vr->b;
  apr_table_t *args;
  const char *key, *entry, *entry_nice;
  const char *date;
  char *diary;
  char *error;

  auth_user (vr);
  if (vr->u == NULL)
    return send_error_page (vr, "Not logged in", "You can't post a diary entry because you're not logged in.");

  args = get_args_table (vr);
  date = iso_now (p);
  diary = apr_psprintf (p, "acct/%s/diary", vr->u);
  key = validate_key(vr, diary, apr_table_get (args, "key"));
  if (key == NULL)
    return send_error_page (vr, "Invalid Key", "An invalid diary key was submitted.");

  entry = apr_table_get (args, "entry");
  diary_put_backup (vr, entry);
  entry_nice = nice_htext (vr, entry, &error);

  render_header (vr, "Diary preview", NULL);

  buffer_printf (b, "<p> %s </p>\n", entry_nice);

  buffer_printf (b, "<p> Edit your entry: </p>\n"
		 "<form method=\"POST\" action=\"post.html\" accept-charset=\"UTF-8\">\n"
		 " <textarea name=\"entry\" cols=60 rows=16 wrap=hard>%s"
		 "</textarea>\n"
		 " <p> <input type=\"submit\" name=post value=\"Post\">\n"
		 " <input type=\"submit\" name=preview value=\"Preview\">\n"
		 " <input type=\"hidden\" name=key value=\"%s\">\n"
		 "</form>\n",
		 ap_escape_html (p, entry), apr_table_get(args, "key"));

  if (error != NULL)
    buffer_printf (b, "<p> <b>Warning:</b> %s </p>\n", error);

  render_acceptable_html (vr);

  return render_footer_send (vr);
}

int
diary_store_entry (VirguleReq *vr, const char *key, const char *entry)
{
  apr_pool_t *p = vr->r->pool;
  const char *date = iso_now (p);
  xmlDoc *entry_doc, *old_entry_doc;
  xmlNode *root, *tree;

  entry_doc = db_xml_doc_new (p);
  root = xmlNewDocNode (entry_doc, NULL, "entry", NULL);
  xmlAddChild (root, xmlNewDocText (entry_doc, entry));
  entry_doc->xmlRootNode = root;

  old_entry_doc = db_xml_get (p, vr->db, key);

  if (old_entry_doc == NULL)
    {
      tree = xmlNewChild (root, NULL, "date", date);

      add_recent (p, vr->db, "recent/diary.xml", vr->u, 100,
		  vr->recentlog_as_posted);
    }
  else
    {
      xmlNode *old_date_el;

      old_date_el = xml_find_child (old_entry_doc->xmlRootNode, "date");
      if (old_date_el != NULL)
	tree = xmlNewChild (root, NULL, "date",
			    xml_get_string_contents (old_date_el));
      tree = xmlNewChild (root, NULL, "update", date);
	
    }
  return db_xml_put (p, vr->db, key, entry_doc);
}
    
static int
diary_post_serve (VirguleReq *vr)
{
  apr_pool_t *p = vr->r->pool;
  apr_table_t *args;
  const char *entry, *diary;
  const char *key;
  int status;
  char *error;

  db_lock_upgrade(vr->lock);
  auth_user (vr);
  if (vr->u == NULL)
    return send_error_page (vr, "Not logged in", "You can't post a diary entry because you're not logged in.");

  diary = apr_psprintf (p, "acct/%s/diary", vr->u);

  args = get_args_table (vr);

  if (apr_table_get (args, "preview"))
    return diary_preview_serve (vr);

  key = validate_key(vr, diary, apr_table_get (args, "key"));
  if (key == NULL)
    return send_error_page (vr, "Invalid Key", "An invalid diary key was submitted.");

  entry = apr_table_get (args, "entry");
  entry = nice_htext (vr, entry, &error);

  status = diary_store_entry (vr, key, entry);
  
  if (status)
    return send_error_page (vr,
			    "Error storing diary entry",
			    "There was an error storing the diary entry. This means there's something wrong with the site.");

  diary_put_backup (vr, "");

  apr_table_add (vr->r->headers_out, "refresh", "0;URL=/recentlog.html");
  return send_error_page (vr, "Diary", "Ok, your <a href=\"/recentlog.html\">diary</a> entry was posted. Thanks!");
}

static int
diary_index_serve (VirguleReq *vr)
{
  apr_pool_t *p = vr->r->pool;
  Buffer *b = vr->b;
  char *str;
  const char *key, *diary;

  auth_user (vr);

  if (vr->u == NULL)
    return send_error_page (vr, "Not logged in", "You can't access your diary page because you're not logged in.");

  str = apr_psprintf (p, "Diary: %s\n", vr->u);
  render_header (vr, str, NULL);
  diary = apr_psprintf (p, "acct/%s/diary", vr->u);
  key = apr_psprintf (p, "%d", db_dir_max (vr->db, diary) + 1);

  buffer_printf (b, "<p> Post a new entry: </p>\n"
		 "<form method=\"POST\" action=\"post.html\" accept-charset=\"UTF-8\">\n"
		 " <textarea name=\"entry\" cols=60 rows=16 wrap=hard>%s"
		 "</textarea>\n"
		 " <p> <input type=\"submit\" name=post value=\"Post\">\n"
		 " <input type=\"submit\" name=preview value=\"Preview\">\n"
		 " <input type=\"hidden\" name=key value=\"%s\">\n"
		 "</form>\n", ap_escape_html(p, diary_get_backup(vr)), key);

  render_acceptable_html (vr);

  buffer_printf (b, "<p> Recent diary entries for %s: </p>\n", vr->u);

  diary_render (vr, vr->u, 5, -1);

  return render_footer_send (vr);
}

static int
diary_edit_serve (VirguleReq *vr)
{
  apr_pool_t *p = vr->r->pool;
  Buffer *b = vr->b;
  apr_table_t *args;
  char *str;
  const char *key, *diary;
  xmlDoc *entry;

  auth_user (vr);
  if (vr->u == NULL)
    return send_error_page (vr, "Not logged in", "You can't access your diary page because you're not logged in.");

  args = get_args_table (vr);

  if (args == NULL)
    return send_error_page (vr, "Need key", "Need to specify key to edit.");

  diary = apr_psprintf (p, "acct/%s/diary", vr->u);
  key = validate_key(vr, diary, apr_table_get (args, "key"));
  if (key == NULL)
    return send_error_page (vr, "Invalid Key", "An invalid diary key was submitted.");

  entry = db_xml_get (p, vr->db, key);

  if (entry != NULL)
    {
      const char *entry_nice;
      xmlNode *date_el;
      char *error;
      char *contents;

      date_el = xml_find_child (entry->xmlRootNode, "date");
      if (date_el != NULL)
        {
	  str = apr_psprintf (p, "Diary: %s\n",
			     render_date (vr,
					  xml_get_string_contents (date_el),
					  1));
	  render_header (vr, str, NULL);
	}

      contents = xml_get_string_contents (entry->xmlRootNode);
      if (contents != NULL)
	{
	  entry_nice = nice_htext (vr, contents, &error);
	  buffer_printf (b, "<p> %s </p>\n", entry_nice);
	}

      buffer_printf (b, "<p> Edit your entry: </p>\n"
		     "<form method=\"POST\" action=\"post.html\" accept-charset=\"UTF-8\">\n"
		     " <textarea name=\"entry\" cols=60 rows=16 wrap=hard>%s"
		     "</textarea>\n"
		     " <p> <input type=\"submit\" name=post value=\"Post\">\n"
		     " <input type=\"submit\" name=preview value=\"Preview\">\n"
		     " <input type=\"hidden\" name=key value=\"%s\">\n"
		     "</form>\n",
		     contents == NULL ? "" : ap_escape_html(p, contents),
		     apr_table_get (args, "key"));
    }
  else
    {
      return send_error_page (vr, "Invalid Key", "An invalid diary key was submitted.");
    }

  return render_footer_send (vr);
}


int
diary_serve (VirguleReq *vr)
{
  const char *p;

  if (!strcmp (vr->uri, "/diary"))
    {
      apr_table_add (vr->r->headers_out, "Location",
		    ap_make_full_path (vr->r->pool, vr->r->uri, ""));
      return HTTP_MOVED_PERMANENTLY;
    }

  if ((p = match_prefix (vr->uri, "/diary/")) == NULL)
    return DECLINED;

  if (!strcmp (p, "post.html"))
    return diary_post_serve (vr);

  if (!strcmp (p, "edit.html"))
    return diary_edit_serve (vr);

#if 0
  /* this isn't needed, it's actually done with the name= arg to the
     submit buttons */
  if (!strcmp (p, "preview.html"))
    return diary_preview_serve (vr);
#endif

  if (!strcmp (p, ""))
    return diary_index_serve (vr);

  return DECLINED;
#if 0
  return send_error_page (vr, "Diary", "Welcome to your diary.");
#endif
}

/**
 * diary_export: Export a diary into an xml tree.
 * @root: Where to add diary child nodes.
 * @u: Username for diary to export.
 *
 * Return value: status (zero on success).
 **/
int
diary_export (VirguleReq *vr, xmlNode *root, char *u)
{
  apr_pool_t *p = vr->r->pool;
  xmlNode *tree, *subtree;
  xmlNode *content_tree;
  htmlDocPtr content_html;
  char *diary;
  char *content_str;
  int n;
  int i;

  diary = apr_psprintf (p, "acct/%s/diary", u);
  n = db_dir_max (vr->db, diary);

  for (i = 0; i <= n; i++)
    {
      char *key;
      xmlDoc *entry;

      key = apr_psprintf (p, "acct/%s/diary/_%d", u, i);
      tree = xmlNewChild (root, NULL, "entry", NULL);
      entry = db_xml_get (p, vr->db, key);
      if (entry != NULL)
	{
	  xmlNode *date_el;
	  date_el = xml_find_child (entry->xmlRootNode, "date");
	  if (date_el != NULL)
	    {
	      subtree = xmlNewChild (tree, NULL, "date", xml_get_string_contents (date_el));
	    }
	  content_str = xml_get_string_contents (entry->xmlRootNode);
	  content_str = apr_pstrcat (p, "<html>", content_str, "</html>", NULL);
	  subtree = xmlNewChild (tree, NULL, "contents", NULL);

	  content_html = htmlParseDoc (content_str, NULL);

	  content_tree = content_html->xmlRootNode;
	  while (content_tree->type != XML_ELEMENT_NODE) {
	    content_tree = content_tree->next;
	  }
	  if (content_tree != NULL)
	    {
	      /* this moves nodes from doc to doc and is not cool */
	      xmlUnlinkNode (content_tree);
	      content_html->xmlRootNode = NULL;
	      xmlAddChild (subtree, content_tree);
	      xmlFreeDoc ((xmlDocPtr)content_html);
	    }

	}
    }

  return 0;
}

/**
 * diary_rss_export: Export a diary into an rss xml tree.
 * @root: Where to add diary child nodes.
 * @u: Username for diary to export.
 *
 * Return value: status (zero on success).
 **/
int
diary_rss_export (VirguleReq *vr, xmlNode *root, char *u)
{
  apr_pool_t *p = vr->r->pool;
  xmlNode *channel;
  char *diary;
  char *content_str;
  char *url;
  int n;
  int i;

  diary = apr_psprintf (p, "acct/%s/diary", u);
  n = db_dir_max (vr->db, diary);

  channel = xmlNewChild (root, NULL, "channel", NULL);
  xmlNewChild (channel, NULL, "title",
	       apr_pstrcat (p, vr->site_name, " diary for ", u, NULL));
  xmlNewChild (channel, NULL, "description",
	       apr_pstrcat (p, vr->site_name, " diary for ", u, NULL));
  url = ap_make_full_path (p, vr->base_uri,
			   apr_psprintf (p, "person/%s/", u));
  xmlNewChild (channel, NULL, "link", url);

  for (i = n; i >= 0 && i > n - 10; i--)
    {
      xmlNode *item;
      char *key;
      xmlDoc *entry;

      key = apr_psprintf (p, "acct/%s/diary/_%d", u, i);
      item = xmlNewChild (channel, NULL, "item", NULL);
      entry = db_xml_get (p, vr->db, key);
      if (entry != NULL)
	{
	  xmlNode *date_el;
	  xmlNode *subtree;

	  date_el = xml_find_child (entry->xmlRootNode, "date");
	  if (date_el != NULL)
	    {
	      char *iso = xml_get_string_contents (date_el);
	      time_t t = iso_to_time_t (iso);
	      /* Warning: the following code incorrectly assumes a timezone. */
	      char *rfc822_s = ap_ht_time (p, (apr_time_t)t * 1000000,
	                                   "%a, %d %b %Y %H:%M:%S -0700", 1);

	      subtree = xmlNewChild (item, NULL, "title",
				     render_date (vr, iso, 0));
	      subtree = xmlNewChild (item, NULL, "pubDate", rfc822_s);
	    }
	  url = ap_make_full_path (p, vr->base_uri,
		    apr_psprintf (p, "person/%s/diary.html?start=%d", u, i));
	  xmlNewChild (item, NULL, "link", url);

	  content_str = xml_get_string_contents (entry->xmlRootNode);
	  content_str = rss_massage_text (p, content_str, vr->base_uri);
	  subtree = xmlNewChild (item, NULL, "description", content_str);

	}
    }

  return 0;
}
