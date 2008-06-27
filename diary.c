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
#include "aggregator.h"
#include "rss_export.h"
#include "hashtable.h"
#include "eigen.h"
#include "site.h"
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


/**
 * virgule_diary_entry_render - renders a single diary entry into the buffer.
 * An informational header is added in one of two styles. If h = 0, a plain
 * header consisting of the date and a permalink will be added. If h = 1, a
 * fancy header wrapped in the user's cert level style will be added, along
 * with an eigenvectory interest ranking if known.
 **/
void
virgule_diary_entry_render (VirguleReq *vr, const char *u, int n, EigenVecEl *ev, int h)
{
  Buffer *b = vr->b;
  char *key = NULL;
  char *contents = NULL;
  char *title = NULL;
  char *localdate = NULL;
  char *localupdate = NULL;
  char *entrylink = NULL;
  char *feedposttime = NULL;
  char *feedupdatetime = NULL;
  char *blogauthor = NULL;
  char *contents_nice = NULL;
  char *error = NULL;
  xmlDoc *entry;
  xmlNode *root;

  key = apr_psprintf (vr->r->pool, "acct/%s/diary/_%d", u, n);
  entry = virgule_db_xml_get (vr->r->pool, vr->db, key);
  if (entry == NULL)
    return;
    
  root = xmlDocGetRootElement (entry);
  if (root == NULL)
    return;

  localdate = virgule_xml_find_child_string (root, "date", NULL);
  localupdate = virgule_xml_find_child_string (root, "update", NULL);

  virgule_buffer_printf (b, "<div class=\"node %s\">\n", virgule_force_legal_css_name (vr, u));

  /* render fancy, recentlog style header if requested */
  if (h)
    {
      CertLevel cl;
      cl = virgule_render_cert_level_begin (vr, u, CERT_STYLE_MEDIUM);
      virgule_buffer_printf (vr->b, " %s ", virgule_render_date (vr, localdate, 0));
      virgule_site_render_person_link (vr, u, cl);
      if (ev)
        {
	  int gray = virgule_conf_to_gray (ev->confidence);
	  virgule_buffer_printf (vr->b,
				"<span style=\"color: #%02x%02x%02x;\">(%.2g)</span>",
				gray, gray, gray, ev->rating);
	}
      virgule_buffer_printf (vr->b,
    			     "&nbsp; <a href=\"%s/person/%s/diary/%u.html\" style=\"text-decoration: none\">&raquo;</a>",
			     vr->prefix, u, n);
      if (vr->u && strcmp (vr->u, u) == 0)
        virgule_buffer_printf (vr->b,
			       "&nbsp; <a href=\"%s/diary/edit.html?key=%u\" style=\"text-decoration: none\">[ Edit ]</a>",
			       vr->prefix, n);
      virgule_render_cert_level_text (vr, u);
      virgule_render_cert_level_end (vr, CERT_STYLE_MEDIUM);
    }
  else
    {
      virgule_buffer_printf (vr->b, "<p><a name=\"%u\"><b>%s</b></a>",
			     n, virgule_render_date (vr, localdate, 0));
      if (localupdate != NULL)
	virgule_buffer_printf (vr->b, " (updated %s)",
			       virgule_render_date (vr, localupdate, 1));
      virgule_buffer_printf (vr->b,
    			     " <a href=\"%s/person/%s/diary/%u.html\" style=\"text-decoration: none\">&raquo;</a>",
			     vr->prefix, u, n);
      if (vr->u && strcmp (vr->u, u) == 0)
        virgule_buffer_printf (vr->b,
			       "&nbsp; <a href=\"%s/diary/edit.html?key=%u\" style=\"text-decoration: none\">[ Edit ]</a> &nbsp;",
			       vr->prefix, n);
    }
        
  contents = virgule_xml_get_string_contents (root);
  title = virgule_xml_find_child_string (root, "title", NULL);
  entrylink = virgule_xml_find_child_string (root, "entrylink", NULL);
  blogauthor = virgule_xml_find_child_string (root, "blogauthor", NULL);
  feedposttime = virgule_xml_find_child_string (root, "feedposttime", NULL);
  feedupdatetime = virgule_xml_find_child_string (root, "feedupdatetime", NULL);
  if(feedupdatetime && (strcmp (feedposttime, feedupdatetime) != 0))
    feedupdatetime = apr_psprintf (vr->r->pool, " (Updated %s) ", feedupdatetime);
  else 
    feedupdatetime = NULL;
  
  if (contents != NULL)
    {    
      contents_nice = virgule_nice_htext (vr, contents, &error);
      virgule_buffer_puts (b, "<div>\n");
      if (title)
        virgule_buffer_printf (b, "<p><b>%s</b></p>\n", title);
      if (strcmp (virgule_req_get_tmetric_level (vr, u),
         virgule_cert_level_to_name (vr, CERT_LEVEL_NONE)) == 0)
        virgule_buffer_puts (b, virgule_add_nofollow (vr, contents_nice));
      else
        virgule_buffer_puts (b, contents_nice);
      if (feedposttime && entrylink)
        {
          virgule_buffer_printf (b, "<p class=\"syndicated\"><a href=\"%s\">Syndicated %s %s from %s</a></p>",
			      entrylink, feedposttime, 
			      feedupdatetime ? feedupdatetime : "",
			      blogauthor ? blogauthor : u);
	}
      virgule_buffer_puts (b, "</div>\n");
    }

    virgule_buffer_puts (b, "</div>\n");
}


/**
 * virgule_diary_exists - returns 1 if at least one diary posts is found.
 * returns 0 if no posts are found.
 **/
int
virgule_diary_exists (VirguleReq *vr, const char *u)
{
    char *key;
    DbCursor *dbc;
    
    key = apr_psprintf (vr->r->pool, "acct/%s/diary", u);
    dbc = virgule_db_open_dir (vr->db, key);
    if (dbc == NULL)
      return 0;

    virgule_db_close_dir (dbc);
    return 1;
}


/**
 * virgule_diary_render - renders a range of diary entries on a page followed
 * by a link that will generated older etnries.
 **/
void
virgule_diary_render (VirguleReq *vr, const char *u, int max_num, int start)
{
  apr_pool_t *p = vr->r->pool;
  Buffer *b = vr->b;
  char *diary;
  int n;

  diary = apr_psprintf (p, "acct/%s/diary", u);

  virgule_auth_user (vr);

  if (start >= 0)
    n = start;
  else
    n = virgule_db_dir_max (vr->db, diary);

  if (n < 0)
    return;

  for (; n >= 0 && max_num--; n--)
    virgule_diary_entry_render (vr, u, n, NULL, 0);

  if (n >= 0)
    virgule_buffer_printf (b, "<p><a href=\"%s/person/%s/diary.html?start=%d\">%d older entr%s...</a></p>\n",
		     vr->prefix, ap_escape_uri(vr->r->pool, u), n,
		     n + 1, n == 0 ? "y" : "ies");
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
  apr_table_t *args;
  const char *key, *entry, *entry_nice;
  const char *date;
  char *diary;
  char *error = NULL;

  virgule_auth_user (vr);
  if (vr->u == NULL)
    return virgule_send_error_page (vr, vERROR, "forbidden", "You can't post a blog entry because you're not logged in.");

  args = virgule_get_args_table (vr);
  date = virgule_iso_now (p);
  diary = apr_psprintf (p, "acct/%s/diary", vr->u);
  key = validate_key(vr, diary, apr_table_get (args, "key"));
  if (key == NULL)
    return virgule_send_error_page (vr, vERROR, "form data", "An invalid blog key was submitted.");

  entry = apr_table_get (args, "entry");
  diary_put_backup (vr, entry);
  entry_nice = virgule_nice_htext (vr, entry, &error);

  if (virgule_set_temp_buffer (vr) != 0)
    return HTTP_INTERNAL_SERVER_ERROR;

  virgule_buffer_printf (vr->b, "<p>%s</p>\n", entry_nice);

  virgule_buffer_printf (vr->b, "<p>Edit your entry:</p>\n"
		 "<form method=\"POST\" action=\"post.html\" accept-charset=\"UTF-8\">\n"
		 "<textarea name=\"entry\" cols=\"60\" rows=\"16\" wrap=\"hard\">%s"
		 "</textarea>\n"
		 "<p><input type=\"submit\" name=\"post\" value=\"Post\">\n"
		 "<input type=\"submit\" name=\"preview\" value=\"Preview\">\n"
		 "<input type=\"hidden\" name=\"key\" value=\"%s\">\n"
		 "</form>\n",
		 ap_escape_html (p, entry), apr_table_get(args, "key"));

  if (error != NULL)
    virgule_buffer_printf (vr->b, "<p><b>Warning:</b> %s</p>\n", error);

  virgule_render_acceptable_html (vr);

  virgule_set_main_buffer (vr);
  
  return virgule_render_in_template (vr, "/templates/default.xml", "content", "Blog Preview");
}


/**
 * virgule_diary_store_feed_item - Store a diary entry based on the contents
 * of the passed feed item. This will store a couple of extra elements in
 * the XML document such the original permalink.
 *
 * ToDo: Eventually, it would be good to refactor the various diary storage
 * functions to reduce the amount of duplicated code.
 **/
int
virgule_diary_store_feed_item (VirguleReq *vr, xmlChar *user, FeedItem *item)
{
  char *content = NULL;
  const char *date = virgule_time_t_to_iso (vr,-1);
  const char *diary, *key;
  xmlDoc *entry_doc;
  xmlNode *root, *tree;
  xmlOutputBuffer *xbuf;
  
  diary = apr_psprintf (vr->r->pool, "acct/%s/diary", (char *)user);
  key = apr_psprintf (vr->r->pool, "%s/_%d", 
                      diary, virgule_db_dir_max (vr->db, diary) + 1);
  
  entry_doc = virgule_db_xml_doc_new (vr->r->pool);
  root = xmlNewDocNode (entry_doc, NULL, (xmlChar *)"entry", NULL);

  /* use text node content directly */
  content = virgule_xml_get_string_contents (item->content);
  /* if not a text node, content is a tree, dump it to text */
  if (content == NULL)
    {
      xbuf = xmlAllocOutputBuffer (NULL);
      if (xbuf != NULL)
        {
	  xmlNodeDumpOutput (xbuf, NULL, item->content, 0, 0, NULL);
	  content = (char *) xbuf->buffer->content;
        }
    }

  xmlAddChild (root, xmlNewDocText (entry_doc, (xmlChar *)content));
  entry_doc->xmlRootNode = root;
  tree = xmlNewChild (root, NULL, (xmlChar *)"date", (xmlChar *)date);
  tree = xmlNewTextChild (root, NULL, (xmlChar *)"title", (xmlChar *)virgule_xml_get_string_contents(item->title));
  if(item->id)
    tree = xmlNewTextChild (root, NULL, (xmlChar *)"id", (xmlChar *)item->link);
  if(item->link)
    tree = xmlNewTextChild (root, NULL, (xmlChar *)"entrylink", (xmlChar *)item->link);
  if(item->bloglink)
    tree = xmlNewTextChild (root, NULL, (xmlChar *)"bloglink", (xmlChar *)item->bloglink);
  if(item->blogauthor)
    tree = xmlNewTextChild (root, NULL, (xmlChar *)"blogauthor", (xmlChar *)item->blogauthor);
  if(item->post_time != -1)
    tree = xmlNewChild (root, NULL, (xmlChar *)"feedposttime", (xmlChar *)virgule_time_t_to_iso(vr,item->post_time));
  if(item->update_time != -1)
    tree = xmlNewChild (root, NULL, (xmlChar *)"feedupdatetime", (xmlChar *)virgule_time_t_to_iso(vr,item->update_time));

  virgule_buffer_printf (vr->b, "<br />Posted entry: [%s]", virgule_time_t_to_iso(vr,item->post_time));

  return virgule_db_xml_put (vr->r->pool, vr->db, key, entry_doc);
}


/**
 * find_entry_by_feedposttime - Search for an entry that has a matching
 * feedposttime field. Start with the most recent entry and search
 * backwards to minimize search time.
 **/
static char *
find_entry_by_feedposttime (VirguleReq *vr, xmlChar *user, time_t posttime)
{
  int n, i;
  time_t ptime;
  char *key = NULL;
  char *diary = NULL;
  char *feedposttime = NULL;
  xmlDoc *entry = NULL;
  xmlNode *root = NULL;

  diary = apr_psprintf (vr->r->pool, "acct/%s/diary", (char *)user);
  n = virgule_db_dir_max (vr->db, diary);

  for (i = n; i >= 0; i--)
    {
      key = apr_psprintf (vr->r->pool, "acct/%s/diary/_%d", (char *)user, i);
      entry = virgule_db_xml_get (vr->r->pool, vr->db, key);
      if (entry != NULL)
        {
	  root = xmlDocGetRootElement (entry);
	  if (root == NULL)
	    continue;
	  feedposttime = virgule_xml_find_child_string (root, "feedposttime", NULL);
	  if (feedposttime == NULL)
	    continue;
	  ptime = virgule_virgule_to_time_t (vr, feedposttime);
	  if (posttime == ptime)
	    return key;
	}
    }
    
  return NULL;
}


/**
 * virgule_diary_update_feed_item - Check to see if the specified entry
 * needs to be updated. If it does, replace it with the updated entry.
 * If the entry number is not specified, use the item post date to search
 * for the entry.
 **/
int
virgule_diary_update_feed_item (VirguleReq *vr, xmlChar *user, FeedItem *item, int e)
{
  char *content = NULL;
  char *key = NULL;
  char *feedupdatetime = NULL;
  time_t utime;
  xmlNode *root, *tmpNode;
  xmlDoc *entry;
  xmlOutputBuffer *xbuf;

  if (user == NULL || item == NULL)
    return 0;

  /* find the entry */
  if (e != -1)
    key = apr_psprintf (vr->r->pool, "acct/%s/diary/_%d", (char *)user, e);
  else
    key = find_entry_by_feedposttime (vr, user, item->post_time);

  if (key == NULL)
    return 0;
    
  entry = virgule_db_xml_get (vr->r->pool, vr->db, key);
  root = xmlDocGetRootElement (entry);

  /* skip the update if we've already done it */
  feedupdatetime = virgule_xml_find_child_string (root, "feedupdatetime", NULL);
  if (feedupdatetime != NULL)
    {
      utime = virgule_virgule_to_time_t (vr, feedupdatetime);
      if (utime >= item->update_time)
        return 0;
    }

  /* update the entry */
  virgule_xml_del_string_contents (root);

  /* use text node content directly */
  content = virgule_xml_get_string_contents (item->content);
  /* if not a text node, content is a tree, dump it to text */
  if (content == NULL)
    {
      xbuf = xmlAllocOutputBuffer (NULL);
      if (xbuf != NULL)
        {
	  xmlNodeDumpOutput (xbuf, NULL, item->content, 0, 0, NULL);
	  content = (char *) xbuf->buffer->content;
        }
    }

  xmlNodeAddContent (root, (xmlChar *)content);  
  tmpNode = virgule_xml_ensure_child (root, "title");
  xmlNodeSetContent (tmpNode, (xmlChar *)virgule_xml_get_string_contents(item->title));  
  tmpNode = virgule_xml_ensure_child (root, "feedupdatetime");
  xmlNodeSetContent (tmpNode, (xmlChar *)virgule_time_t_to_iso(vr,item->update_time));
  tmpNode = virgule_xml_ensure_child (root, "update");
  xmlNodeSetContent (tmpNode, (xmlChar *)virgule_iso_now (vr->r->pool));
  
  virgule_buffer_printf (vr->b, "<br />Updated entry: [%s]", virgule_time_t_to_iso(vr,item->post_time));

  return virgule_db_xml_put (vr->r->pool, vr->db, key, entry);
}


/**
 * virgule_diary_store_entry - this a total rewrite of the original code.
 * Instead of wiping out the old entry and replacing it with a new one, we
 * read the old one and update only the entry contents themselves, leaving
 * other tags (such as syndication info) intact.
 */
int
virgule_diary_store_entry (VirguleReq *vr, const char *key, const char *entry)
{
  apr_pool_t *p = vr->r->pool;
  const char *date = virgule_iso_now (p);
  xmlDoc *entry_doc;
  xmlNode *root, *tree;

  /* read the old entry */
  entry_doc = virgule_db_xml_get (p, vr->db, key);

  if (entry_doc == NULL)
    {
      /* if no old entry is found, generate a new one */
      entry_doc = virgule_db_xml_doc_new (p);
      root = xmlNewDocNode (entry_doc, NULL, (xmlChar *)"entry", NULL);
      xmlAddChild (root, xmlNewDocText (entry_doc, (xmlChar *)entry));
      entry_doc->xmlRootNode = root;
      tree = xmlNewChild (root, NULL, (xmlChar *)"date", (xmlChar *)date);
      virgule_add_recent (p, vr->db, "recent/diary.xml", vr->u, 100,
		  vr->priv->recentlog_as_posted);
    }
  else
    {
      /* replace old entry with new entry */
      root = xmlDocGetRootElement (entry_doc);
      virgule_xml_del_string_contents(root);
      xmlNodeAddContent (root, (xmlChar *)entry);
      tree = virgule_xml_ensure_child (root, "update");
      xmlNodeSetContent (tree, (xmlChar *)date);
    }

  /* write the entry back to the data store */
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

  virgule_auth_user (vr);
  if (vr->u == NULL)
    return virgule_send_error_page (vr, vERROR, "forbidden", "You can't post a blog entry because you're not logged in.");

  diary = apr_psprintf (p, "acct/%s/diary", vr->u);

  args = virgule_get_args_table (vr);

  if (apr_table_get (args, "preview"))
    return diary_preview_serve (vr);

  key = validate_key(vr, diary, apr_table_get (args, "key"));
  if (key == NULL)
    return virgule_send_error_page (vr, vERROR, "form data", "An invalid blog key was submitted.");

  entry = apr_table_get (args, "entry");
  entry = virgule_nice_htext (vr, entry, &error);

  status = virgule_diary_store_entry (vr, key, entry);
  
  if (status)
    return virgule_send_error_page (vr, vERROR, "database",
			    "There was an error storing the blog entry.");

  diary_put_backup (vr, "");

  apr_table_add (vr->r->headers_out, "refresh", "0;URL=/recentlog.html");
  return virgule_send_error_page (vr, vINFO, "blog", "Ok, your <a href=\"/recentlog.html\">blog</a> entry was posted. Thanks!");
}


static int
diary_index_serve (VirguleReq *vr)
{
  apr_pool_t *p = vr->r->pool;
  char *title;
  const char *key, *diary;

  virgule_auth_user (vr);

  if (vr->u == NULL)
    return virgule_send_error_page (vr, vERROR, "forbidden", "You can't access your blog page because you're not logged in.");

  title = apr_psprintf (p, "Blog: %s\n", vr->u);
  diary = apr_psprintf (p, "acct/%s/diary", vr->u);
  key = apr_psprintf (p, "%d", virgule_db_dir_max (vr->db, diary) + 1);

  if (virgule_set_temp_buffer (vr) != 0)
    return HTTP_INTERNAL_SERVER_ERROR;
    
  virgule_buffer_printf (vr->b, "<p> Post a new entry: </p>\n"
		 "<form method=\"POST\" action=\"post.html\" accept-charset=\"UTF-8\">\n"
		 "<textarea name=\"entry\" cols=\"60\" rows=\"16\" wrap=\"hard\">%s"
		 "</textarea>\n"
		 "<p><input type=\"submit\" name=\"post\" value=\"Post\">\n"
		 "<input type=\"submit\" name=\"preview\" value=\"Preview\">\n"
		 "<input type=\"hidden\" name=\"key\" value=\"%s\">\n"
		 "</form>\n", ap_escape_html(p, virgule_diary_get_backup(vr)), key);

  virgule_render_acceptable_html (vr);

  virgule_buffer_printf (vr->b, "<p>Recent blog entries for %s:</p>\n", vr->u);

  virgule_diary_render (vr, vr->u, 5, -1);

  virgule_set_main_buffer (vr);
  
  return virgule_render_in_template (vr, "/templates/default.xml", "content", title);
}


static int
diary_edit_serve (VirguleReq *vr)
{
  apr_pool_t *p = vr->r->pool;
  apr_table_t *args;
  char *str1, *str2;
  const char *key, *diary;
  xmlDoc *entry;

  virgule_auth_user (vr);
  if (vr->u == NULL)
    return virgule_send_error_page (vr, vERROR, "forbidden", "You can't access your blog page because you're not logged in.");

  args = virgule_get_args_table (vr);

  if (args == NULL)
    return virgule_send_error_page (vr, vERROR, "form data", "Need to specify key to edit.");

  diary = apr_psprintf (p, "acct/%s/diary", vr->u);
  key = validate_key(vr, diary, apr_table_get (args, "key"));
  if (key == NULL)
    return virgule_send_error_page (vr, vERROR, "form data", "An invalid blog key was submitted.");

  entry = virgule_db_xml_get (p, vr->db, key);

  if (entry != NULL)
    {
      const char *entry_nice;
      char *feedposttime = NULL;
      char *feedupdatetime = NULL;
      char *title;
      char *error;
      char *contents;

      str1 = virgule_xml_find_child_string (entry->xmlRootNode, "date", NULL);
      str2 = apr_psprintf (p, "Blog: %s\n", str1 ? virgule_render_date (vr, str1, 1) : "Unknown");

      if (virgule_set_temp_buffer (vr) != 0)
        return HTTP_INTERNAL_SERVER_ERROR;

      feedposttime = virgule_xml_find_child_string (entry->xmlRootNode, "feedposttime", NULL);
      feedupdatetime = virgule_xml_find_child_string (entry->xmlRootNode, "feedupdatetime", NULL);
      if (feedposttime != NULL)
        {
	  virgule_buffer_printf (vr->b, "<p>This is a syndicated blog entry. "
				    "Editing locally is not recommended."
				    "<br />Syndicated on %s "
				    "(last syndication update: %s)</p>\n",
				    feedposttime,
				    feedupdatetime ? feedupdatetime : "None");
	}

      title = virgule_xml_find_child_string (entry->xmlRootNode, "title", NULL);
      if (title != NULL)
        {
	  virgule_buffer_printf (vr->b, "<p><b>%s</b></p>\n", title);
	}

      contents = virgule_xml_get_string_contents (entry->xmlRootNode);
      if (contents != NULL)
	{
	  entry_nice = virgule_nice_htext (vr, contents, &error);
	  virgule_buffer_printf (vr->b, "<p>%s</p>\n", entry_nice);
	}

      virgule_buffer_printf (vr->b, "<p> Edit your entry: </p>\n"
		     "<form method=\"POST\" action=\"post.html\" accept-charset=\"UTF-8\">\n"
		     "<textarea name=\"entry\" cols=\"60\" rows=\"16\" wrap=\"hard\">%s"
		     "</textarea>\n"
		     "<p><input type=\"submit\" name=\"post\" value=\"Post\">\n"
		     "<input type=\"submit\" name=\"preview\" value=\"Preview\">\n"
		     "<input type=\"hidden\" name=\"key\" value=\"%s\">\n",
		     contents == NULL ? "" : ap_escape_html(p, contents),
		     apr_table_get (args, "key"));

      if(title)
        virgule_buffer_printf (vr->b, "<input type=\"hidden\" name=\"title\" value=\"%s\" >\n", ap_escape_quotes(p, title));

      virgule_buffer_puts (vr->b, "</form>\n");
      virgule_set_main_buffer (vr);
      return virgule_render_in_template (vr, "/templates/default.xml", "content", str2);
    }
  else
    {
      return virgule_send_error_page (vr, vERROR, "database", "The request blog entry was not found.");
    }
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
  return virgule_send_error_page (vr, vINFO, "blog", "Welcome to your blog.");
#endif
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
  char *diary, *content_str;
  char *url, *guid, *pubdate;
  int n;
  int i;

  diary = apr_psprintf (p, "acct/%s/diary", u);
  n = virgule_db_dir_max (vr->db, diary);

  channel = xmlNewChild (root, NULL, (xmlChar *)"channel", NULL);
  xmlNewChild (channel, NULL, (xmlChar *)"title",
	       (xmlChar *)apr_pstrcat (p, vr->priv->site_name, " blog for ", u, NULL));
  url = ap_make_full_path (p, vr->priv->base_uri,
			   apr_psprintf (p, "person/%s/", u));
  xmlNewChild (channel, NULL, (xmlChar *)"link", (xmlChar *)url);
  xmlNewChild (channel, NULL, (xmlChar *)"description",
	       (xmlChar *)apr_pstrcat (p, vr->priv->site_name, " blog for ", u, NULL));
  xmlNewChild (channel, NULL, (xmlChar *)"language", (xmlChar *)"en-us");
  xmlNewChild (channel, NULL, (xmlChar *)"generator", (xmlChar *)"mod_virgule");
  pubdate = virgule_render_date (vr, virgule_iso_now(vr->r->pool), 2);
  xmlNewChild (channel, NULL, (xmlChar *)"pubDate", (xmlChar *)pubdate);

  for (i = n; i >= 0 && i > n - 10; i--)
    {
      xmlNode *item, *root;
      char *key;
      xmlDoc *entry;
      
      key = apr_psprintf (p, "acct/%s/diary/_%d", u, i);
      item = xmlNewChild (channel, NULL, (xmlChar *)"item", NULL);
      entry = virgule_db_xml_get (p, vr->db, key);
      root = xmlDocGetRootElement (entry);

      pubdate = NULL;
      if (entry != NULL)
	{
	  xmlNode *date_el;
	  xmlNode *subtree;
	  char *iso = NULL;
	  char *title = NULL;

	  date_el = virgule_xml_find_child (root, "date");
	  if (date_el != NULL)
	    {
	      iso = virgule_xml_get_string_contents (date_el);
	      pubdate = virgule_render_date (vr, iso, 2);	      
	      subtree = xmlNewChild (item, NULL, (xmlChar *)"pubDate", (xmlChar *)pubdate);
	    }

	  title = virgule_xml_find_child_string (root, "title", NULL);
	  if(title != NULL)
            subtree = xmlNewChild (item, NULL, (xmlChar *)"title", (xmlChar *)title);
	  else if(iso != NULL)
	    {
	      pubdate = virgule_render_date (vr, iso, 0);
              subtree = xmlNewChild (item, NULL, (xmlChar *)"title", (xmlChar *)pubdate);
	    }
	  else
	    subtree = xmlNewChild (item, NULL, (xmlChar *)"title",
	       (xmlChar *)apr_pstrcat (p, vr->priv->site_name, " blog for ", u, NULL));

	  url = ap_make_full_path (p, vr->priv->base_uri,
		    apr_psprintf (p, "person/%s/diary.html?start=%d", u, i));
	  xmlNewChild (item, NULL, (xmlChar *)"link", (xmlChar *)url);

	  guid = virgule_xml_find_child_string (root, "id", NULL);
	  if(guid != NULL)
	    xmlNewChild (item, NULL, (xmlChar *)"guid", (xmlChar *)guid);
	  else
	    xmlNewChild (item, NULL, (xmlChar *)"guid", (xmlChar *)url);
	  
	  content_str = virgule_xml_get_string_contents (root);
	  if (content_str != NULL)
	    {
	      content_str = virgule_rss_massage_text (p, content_str, vr->priv->base_uri);
	      subtree = xmlNewChild (item, NULL, (xmlChar *)"description", (xmlChar *)content_str);
            }
	}
    }

  return 0;
}


/**
 * virgule_diary_latest_feed_entry - return the Unix time_t value of the most
 * recent syndicated diary entry for the specified user. If the user has no 
 * diary entries yet, a value of 0 is returned. This function should only be
 * called for valid usernames. It does not validate the username.
 *
 * Because some users post entries locally and use the syndication feature,
 * it's possible that the most recent entry doesn't have a feedposttime tag.
 * So, we search back a maximum of 20 entries looking for one.
 **/
time_t
virgule_diary_latest_feed_entry (VirguleReq *vr, xmlChar *u)
{
  int i, n;
  char *diary, *key;
  xmlDoc *entry = NULL;
  xmlNode *date = NULL;
  time_t result;
  
  diary = apr_psprintf (vr->r->pool, "acct/%s/diary", (char *)u);
  n = virgule_db_dir_max (vr->db, diary);
  if (n < 0)
    return 0;

  for (result = 0, i = n; result == 0 && i > n - 20; i--)
    {
      key = apr_psprintf (vr->r->pool, "acct/%s/diary/_%d", (char *)u, i);
      entry = virgule_db_xml_get (vr->r->pool, vr->db, key);
      if (entry == NULL)
        continue;

      date = virgule_xml_find_child (entry->xmlRootNode, "feedposttime");
      if (date == NULL)
        continue;

      result = virgule_iso_to_time_t (virgule_xml_get_string_contents (date));
    }

  return result;
}


/**
 * Search for the specified entry ID value. If no match is found, the return
 * value is -1. If a match is found, the diary entry number is returned.
 */
int
virgule_diary_entry_id_exists(VirguleReq *vr, xmlChar *u, char *id)
{
  int i, n;
  int e = -1;
  char *diary, *key, *eid;
  xmlDoc *entry = NULL;

  if (id == NULL)
    return -1;

  diary = apr_psprintf (vr->r->pool, "acct/%s/diary", (char *)u);
  n = virgule_db_dir_max (vr->db, diary);
  if (n < 0)
    return -1;

  for (i = n; e < 0 && i >= 0; i--)
    {
      key = apr_psprintf (vr->r->pool, "acct/%s/diary/_%d", (char *)u, i);
      entry = virgule_db_xml_get (vr->r->pool, vr->db, key);
      if (entry == NULL)
        continue;

      eid = virgule_xml_find_child_string (entry->xmlRootNode, "id", NULL);
      if (eid == NULL)
        continue;

      if (strcmp (eid, id) == 0)
        e = i;
    }
    
  return e;
}
