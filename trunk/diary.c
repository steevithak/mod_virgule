/* A module for managing diaries. */

#include <time.h>

#include <apr.h>
#include <apr_strings.h>
#include <httpd.h>

#include <libxml/tree.h>
#include <libxml/HTMLparser.h>

#include "private.h"
#include "buffer.h"
#include "db.h"
#include "req.h"
#include "util.h"
#include "certs.h"
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
  if (key_val < -1 || key_val > virgule_db_dir_max(vr->db, diary) + 1)
    return NULL;

  /* Autoassign next available key for incoming remote post */
  if (key_val == -1)
    key_val = virgule_db_dir_max (vr->db, diary) + 1;

  /* Create XML key */
  return apr_psprintf (p, "acct/%s/diary/_%d", vr->u, key_val);
}

/* renders into @vr's buffer */
void
virgule_diary_render (VirguleReq *vr, const char *u, int max_num, int start)
{
  apr_pool_t *p = vr->r->pool;
  Buffer *b = vr->b;
  char *diary;
  int n, is_owner = 0;

  diary = apr_psprintf (p, "acct/%s/diary", u);

  virgule_auth_user (vr);
  if (vr->u && strcmp(vr->u, u) == 0)
    {
      /* The user is viewing his or her own diary */
      is_owner = 1;
    }

  if (start >= 0)
    n = start;
  else
    n = virgule_db_dir_max (vr->db, diary);

  if (n < 0)
    return;

  for (; n >= 0 && max_num--; n--)
    {
      char *key;
      xmlDoc *entry;

      key = apr_psprintf (p, "acct/%s/diary/_%d", u, n);
#if 0
      virgule_buffer_printf (b, "<p> Key: %s </p>\n", key);
      continue;
#endif
      entry = virgule_db_xml_get (p, vr->db, key);
      if (entry != NULL)
	{
	  xmlNode *date_el;
	  char *contents;

	  date_el = virgule_xml_find_child (entry->xmlRootNode, "date");
	  if (date_el != NULL)
	    {
	      xmlNode *update_el;
	      virgule_buffer_printf (b, "<p> ");
	      virgule_buffer_printf (b, "<a name=\"%u\"><b>%s</b></a>",
			     n,
			     virgule_render_date (vr,
					  virgule_xml_get_string_contents (date_el),
					  0));
	      update_el = virgule_xml_find_child (entry->xmlRootNode, "update");
	      if (update_el != NULL)
		virgule_buffer_printf (b, " (updated %s)",
			       virgule_render_date (vr,
					    virgule_xml_get_string_contents (update_el),
					    1));

              virgule_buffer_printf (b, "&nbsp; <a href=\"%s/person/%s/diary.html?start=%u\">&raquo;</a>",
			     vr->prefix,
                             ap_escape_uri(vr->r->pool, u),
                             n); 
	      if (is_owner)
	          virgule_buffer_printf (b, " <a href=\"%s/diary/edit.html?key=%u\">[ Edit ]</a>",
				 vr->prefix,
				 n);
	      virgule_buffer_printf (b, " </p>\n");
	    }
	  contents = virgule_xml_get_string_contents (entry->xmlRootNode);
	  if (contents != NULL)
	    {
	      virgule_buffer_puts (b, "<blockquote>\n");
              if (strcmp (virgule_req_get_tmetric_level (vr, u),
	           virgule_cert_level_to_name (vr, CERT_LEVEL_NONE)) == 0)
	        virgule_buffer_puts (b, virgule_add_nofollow (vr, contents));
	      else
	        virgule_buffer_puts (b, contents);
	      virgule_buffer_puts (b, "</blockquote>\n");
	    }
	}
    }

  if (n >= 0)
    {
      virgule_buffer_printf (b, "<p> <a href=\"%s/person/%s/diary.html?start=%d\">%d older entr%s...</a> </p>\n",
		     vr->prefix, ap_escape_uri(vr->r->pool, u), n,
		     n + 1, n == 0 ? "y" : "ies");
    }
}

/* renders the latest diary entry into @vr's buffer */
void
virgule_diary_latest_render (VirguleReq *vr, const char *u, int n)
{
  apr_pool_t *p = vr->r->pool;
  Buffer *b = vr->b;
  char *key;
  xmlDoc *entry;

  key = apr_psprintf (p, "acct/%s/diary/_%d", u, n);
  entry = virgule_db_xml_get (p, vr->db, key);
  if (entry != NULL)
    {
      char *contents;
  
      contents = virgule_xml_get_string_contents (entry->xmlRootNode);
      virgule_buffer_puts (b, "<blockquote>\n");
      if (contents != NULL)
	{
          if (strcmp (virgule_req_get_tmetric_level (vr, u),
	      virgule_cert_level_to_name (vr, CERT_LEVEL_NONE)) == 0)
	    virgule_buffer_puts (b, virgule_add_nofollow (vr, contents));
	  else
	    virgule_buffer_puts (b, contents);
	}
      virgule_buffer_puts (b, "</blockquote>\n");
    }
}

char *
virgule_diary_get_backup(VirguleReq *vr)
{
  apr_pool_t *p = vr->r->pool;
  char *key, *val;
  int val_size;

  virgule_auth_user(vr);
  if (vr->u == NULL)
    return NULL;

  key = apr_psprintf (p, "acct/%s/diarybackup", vr->u);
  val = virgule_db_get (vr->db, key, &val_size);
  if (val == NULL)
    return "";

  return val;
}

static int
diary_put_backup(VirguleReq *vr, const char *entry)
{
  apr_pool_t *p = vr->r->pool;
  char *key;

  virgule_auth_user(vr);
  if (vr->u == NULL)
    return -1;

  key = apr_psprintf (p, "acct/%s/diarybackup", vr->u);
  return virgule_db_put (vr->db, key, entry, strlen(entry));
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

  virgule_auth_user (vr);
  if (vr->u == NULL)
    return virgule_send_error_page (vr, "Not logged in", "You can't post a blog entry because you're not logged in.");

  args = virgule_get_args_table (vr);
  date = virgule_iso_now (p);
  diary = apr_psprintf (p, "acct/%s/diary", vr->u);
  key = validate_key(vr, diary, apr_table_get (args, "key"));
  if (key == NULL)
    return virgule_send_error_page (vr, "Invalid Key", "An invalid blog key was submitted.");

  entry = apr_table_get (args, "entry");
  diary_put_backup (vr, entry);
  entry_nice = virgule_nice_htext (vr, entry, &error);

  virgule_render_header (vr, "blog preview", NULL);

  virgule_buffer_printf (b, "<p> %s </p>\n", entry_nice);

  virgule_buffer_printf (b, "<p> Edit your entry: </p>\n"
		 "<form method=\"POST\" action=\"post.html\" accept-charset=\"UTF-8\">\n"
		 " <textarea name=\"entry\" cols=60 rows=16 wrap=hard>%s"
		 "</textarea>\n"
		 " <p> <input type=\"submit\" name=post value=\"Post\">\n"
		 " <input type=\"submit\" name=preview value=\"Preview\">\n"
		 " <input type=\"hidden\" name=key value=\"%s\">\n"
		 "</form>\n",
		 ap_escape_html (p, entry), apr_table_get(args, "key"));

  if (error != NULL)
    virgule_buffer_printf (b, "<p> <b>Warning:</b> %s </p>\n", error);

  virgule_render_acceptable_html (vr);

  return virgule_render_footer_send (vr);
}

int
virgule_diary_store_entry (VirguleReq *vr, const char *key, const char *entry)
{
  apr_pool_t *p = vr->r->pool;
  const char *date = virgule_iso_now (p);
  xmlDoc *entry_doc, *old_entry_doc;
  xmlNode *root, *tree;

  entry_doc = virgule_db_xml_doc_new (p);
  root = xmlNewDocNode (entry_doc, NULL, "entry", NULL);
  xmlAddChild (root, xmlNewDocText (entry_doc, entry));
  entry_doc->xmlRootNode = root;

  old_entry_doc = virgule_db_xml_get (p, vr->db, key);

  if (old_entry_doc == NULL)
    {
      tree = xmlNewChild (root, NULL, "date", date);

      virgule_add_recent (p, vr->db, "recent/diary.xml", vr->u, 100,
		  vr->priv->recentlog_as_posted);
    }
  else
    {
      xmlNode *old_date_el;

      old_date_el = virgule_xml_find_child (old_entry_doc->xmlRootNode, "date");
      if (old_date_el != NULL)
	tree = xmlNewChild (root, NULL, "date",
			    virgule_xml_get_string_contents (old_date_el));
      tree = xmlNewChild (root, NULL, "update", date);
	
    }
  return virgule_db_xml_put (p, vr->db, key, entry_doc);
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

  virgule_db_lock_upgrade(vr->lock);
  virgule_auth_user (vr);
  if (vr->u == NULL)
    return virgule_send_error_page (vr, "Not logged in", "You can't post a blog entry because you're not logged in.");

  diary = apr_psprintf (p, "acct/%s/diary", vr->u);

  args = virgule_get_args_table (vr);

  if (apr_table_get (args, "preview"))
    return diary_preview_serve (vr);

  key = validate_key(vr, diary, apr_table_get (args, "key"));
  if (key == NULL)
    return virgule_send_error_page (vr, "Invalid Key", "An invalid blog key was submitted.");

  entry = apr_table_get (args, "entry");
  entry = virgule_nice_htext (vr, entry, &error);

  status = virgule_diary_store_entry (vr, key, entry);
  
  if (status)
    return virgule_send_error_page (vr,
			    "Error storing blog entry",
			    "There was an error storing the blog entry. This means there's something wrong with the site.");

  diary_put_backup (vr, "");

  apr_table_add (vr->r->headers_out, "refresh", "0;URL=/recentlog.html");
  return virgule_send_error_page (vr, "Blog", "Ok, your <a href=\"/recentlog.html\">blog</a> entry was posted. Thanks!");
}

static int
diary_index_serve (VirguleReq *vr)
{
  apr_pool_t *p = vr->r->pool;
  Buffer *b = vr->b;
  char *str;
  const char *key, *diary;

  virgule_auth_user (vr);

  if (vr->u == NULL)
    return virgule_send_error_page (vr, "Not logged in", "You can't access your blog page because you're not logged in.");

  str = apr_psprintf (p, "Blog: %s\n", vr->u);
  virgule_render_header (vr, str, NULL);
  diary = apr_psprintf (p, "acct/%s/diary", vr->u);
  key = apr_psprintf (p, "%d", virgule_db_dir_max (vr->db, diary) + 1);

  virgule_buffer_printf (b, "<p> Post a new entry: </p>\n"
		 "<form method=\"POST\" action=\"post.html\" accept-charset=\"UTF-8\">\n"
		 " <textarea name=\"entry\" cols=60 rows=16 wrap=hard>%s"
		 "</textarea>\n"
		 " <p> <input type=\"submit\" name=post value=\"Post\">\n"
		 " <input type=\"submit\" name=preview value=\"Preview\">\n"
		 " <input type=\"hidden\" name=key value=\"%s\">\n"
		 "</form>\n", ap_escape_html(p, virgule_diary_get_backup(vr)), key);

  virgule_render_acceptable_html (vr);

  virgule_buffer_printf (b, "<p> Recent blog entries for %s: </p>\n", vr->u);

  virgule_diary_render (vr, vr->u, 5, -1);

  return virgule_render_footer_send (vr);
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

  virgule_auth_user (vr);
  if (vr->u == NULL)
    return virgule_send_error_page (vr, "Not logged in", "You can't access your blog page because you're not logged in.");

  args = virgule_get_args_table (vr);

  if (args == NULL)
    return virgule_send_error_page (vr, "Need key", "Need to specify key to edit.");

  diary = apr_psprintf (p, "acct/%s/diary", vr->u);
  key = validate_key(vr, diary, apr_table_get (args, "key"));
  if (key == NULL)
    return virgule_send_error_page (vr, "Invalid Key", "An invalid blog key was submitted.");

  entry = virgule_db_xml_get (p, vr->db, key);

  if (entry != NULL)
    {
      const char *entry_nice;
      xmlNode *date_el;
      char *error;
      char *contents;

      date_el = virgule_xml_find_child (entry->xmlRootNode, "date");
      if (date_el != NULL)
        {
	  str = apr_psprintf (p, "Blog: %s\n",
			     virgule_render_date (vr,
					  virgule_xml_get_string_contents (date_el),
					  1));
	  virgule_render_header (vr, str, NULL);
	}

      contents = virgule_xml_get_string_contents (entry->xmlRootNode);
      if (contents != NULL)
	{
	  entry_nice = virgule_nice_htext (vr, contents, &error);
	  virgule_buffer_printf (b, "<p> %s </p>\n", entry_nice);
	}

      virgule_buffer_printf (b, "<p> Edit your entry: </p>\n"
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
      return virgule_send_error_page (vr, "Invalid Key", "An invalid blog key was submitted.");
    }

  return virgule_render_footer_send (vr);
}


int
virgule_diary_serve (VirguleReq *vr)
{
  const char *p;

  if (!strcmp (vr->uri, "/diary"))
    {
      apr_table_add (vr->r->headers_out, "Location",
		    ap_make_full_path (vr->r->pool, vr->r->uri, ""));
      return HTTP_MOVED_PERMANENTLY;
    }

  if ((p = virgule_match_prefix (vr->uri, "/diary/")) == NULL)
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
  return virgule_send_error_page (vr, "Blog", "Welcome to your blog.");
#endif
}

/**
 * diary_export: Export a diary into an xml tree.
 * @root: Where to add diary child nodes.
 * @u: Username for diary to export.
 *
 * Return value: status (zero on success).
 *
 * ToDo: I think this code is incorrect. It seems to assume that diary
 * entries contain HTML and then attempts to parse the HTML and incorporate
 * it into the XML document tree. It would make more sense to me for the
 * diary entry including HTML, if any, to be made into content for an "entry"
 * node in the XML tree. This way we don't have to worry about whether the
 * HTML used is well-formed, XML-compatible markup.
 *
 * It may even be possible to remove this function altogether. The newer
 * XML-RPC code should replace it.
 **/
int
virgule_diary_export (VirguleReq *vr, xmlNode *root, char *u)
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
  n = virgule_db_dir_max (vr->db, diary);

  for (i = 0; i <= n; i++)
    {
      char *key;
      xmlDoc *entry;

      key = apr_psprintf (p, "acct/%s/diary/_%d", u, i);
      tree = xmlNewChild (root, NULL, "entry", NULL);
      entry = virgule_db_xml_get (p, vr->db, key);
      if (entry != NULL)
	{
	  xmlNode *date_el;
	  date_el = virgule_xml_find_child (entry->xmlRootNode, "date");
	  if (date_el != NULL)
	    {
	      subtree = xmlNewChild (tree, NULL, "date", virgule_xml_get_string_contents (date_el));
	    }
	  content_str = virgule_xml_get_string_contents (entry->xmlRootNode);
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
virgule_diary_rss_export (VirguleReq *vr, xmlNode *root, char *u)
{
  apr_pool_t *p = vr->r->pool;
  xmlNode *channel;
  char *diary;
  char *content_str;
  char *url;
  int n;
  int i;

  diary = apr_psprintf (p, "acct/%s/diary", u);
  n = virgule_db_dir_max (vr->db, diary);

  channel = xmlNewChild (root, NULL, "channel", NULL);
  xmlNewChild (channel, NULL, "title",
	       apr_pstrcat (p, vr->priv->site_name, " blog for ", u, NULL));
  xmlNewChild (channel, NULL, "description",
	       apr_pstrcat (p, vr->priv->site_name, " blog for ", u, NULL));
  url = ap_make_full_path (p, vr->priv->base_uri,
			   apr_psprintf (p, "person/%s/", u));
  xmlNewChild (channel, NULL, "link", url);

  for (i = n; i >= 0 && i > n - 10; i--)
    {
      xmlNode *item;
      char *key;
      xmlDoc *entry;

      key = apr_psprintf (p, "acct/%s/diary/_%d", u, i);
      item = xmlNewChild (channel, NULL, "item", NULL);
      entry = virgule_db_xml_get (p, vr->db, key);
      if (entry != NULL)
	{
	  xmlNode *date_el;
	  xmlNode *subtree;

	  date_el = virgule_xml_find_child (entry->xmlRootNode, "date");
	  if (date_el != NULL)
	    {
	      char *iso = virgule_xml_get_string_contents (date_el);
	      time_t t = virgule_iso_to_time_t (iso);
	      /* Warning: the following code incorrectly assumes a timezone. */
	      char *rfc822_s = ap_ht_time (p, (apr_time_t)t * 1000000,
	                                   "%a, %d %b %Y %H:%M:%S -0700", 1);

	      subtree = xmlNewChild (item, NULL, "title",
				     virgule_render_date (vr, iso, 0));
	      subtree = xmlNewChild (item, NULL, "pubDate", rfc822_s);
	    }
	  url = ap_make_full_path (p, vr->priv->base_uri,
		    apr_psprintf (p, "person/%s/diary.html?start=%d", u, i));
	  xmlNewChild (item, NULL, "link", url);
	  content_str = virgule_xml_get_string_contents (entry->xmlRootNode);
	  if (content_str != NULL)
	    {
	      content_str = virgule_rss_massage_text (p, content_str, vr->priv->base_uri);
	      subtree = xmlNewChild (item, NULL, "description", content_str);
            }
	}
    }

  return 0;
}


/**
 * virgule_diary_latest_entry - return the Unix time_t value of the most
 * recent diary entry for the specified user. If the user has no diary
 * entries yet, a value of 0 is returned. This function should only be
 * called for value usernames. It does not validate the username.
 **/
time_t
virgule_diary_latest_entry (VirguleReq *vr, char *u)
{
  int n;
  char *diary, *key;
  xmlDoc *entry = NULL;
  xmlNode *date = NULL;
  
  diary = apr_psprintf (vr->r->pool, "acct/%s/diary", u);
  n = virgule_db_dir_max (vr->db, diary);
  if (n < 0)
    return 0;
    
  key = apr_psprintf (vr->r->pool, "acct/%s/diary/_%d", u, n);
  entry = virgule_db_xml_get (vr->r->pool, vr->db, key);
  if (entry == NULL)
    return 0;
    
  date = virgule_xml_find_child (entry->xmlRootNode, "date");
  return virgule_iso_to_time_t (virgule_xml_get_string_contents (date));
}

