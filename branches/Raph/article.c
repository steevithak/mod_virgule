/* A module for posting news stories. */

#include <time.h>

#include "httpd.h"

#include <tree.h>

#include "buffer.h"
#include "db.h"
#include "req.h"
#include "util.h"
#include "style.h"
#include "auth.h"
#include "db_xml.h"
#include "xml_util.h"
#include "certs.h"
#include "acct_maint.h"

#include "article.h"

static void
article_render_reply (VirguleReq *vr, int art_num, int reply_num)
{
  pool *p = vr->r->pool;
  Buffer *b = vr->b;
  char *key;
  xmlDoc *doc;

  key = ap_psprintf (p, "articles/_%d/_%d/reply.xml", art_num, reply_num);

  doc = db_xml_get (p, vr->db, key);
  if (doc != NULL)
    {
      xmlNode *root = doc->root;
      char *author;
      char *title;
      char *body;
      char *date;

      author = xml_find_child_string (root, "author", "(no author)");
      title = xml_find_child_string (root, "title", "(no title)");
      body = xml_find_child_string (root, "body", "(no body)");
      date = xml_find_child_string (root, "date", "(no date)");

      render_cert_level_begin (vr, author, CERT_STYLE_MEDIUM);
      buffer_printf (b, "<a name=\"%u\"><b>%s</b></a>, posted %s by <a href=\"%s/person/%s/\">%s</a> <a href=\"#%u\" style=\"text-decoration: none\">&raquo;</a>\n",
		     reply_num, title, render_date (vr, date), vr->prefix, author, author, reply_num);
      render_cert_level_text (vr, author);
      render_cert_level_end (vr, CERT_STYLE_MEDIUM);
      buffer_printf (b, "<blockquote>\n%s\n</blockquote>\n", body);
    }
  else
    {
      buffer_printf (b, "<p> Error reading <x>article</x> %d.\n", art_num);
    }
}

static void
article_render_replies (VirguleReq *vr, int art_num)
{
  pool *p = vr->r->pool;
  int lastread;
  char *base;
  int n_art;
  int i;

  base = ap_psprintf (vr->r->pool, "articles/_%d", art_num);
  n_art = db_dir_max (vr->db, base) + 1;
  lastread = acct_get_lastread (vr, "articles", ap_psprintf(p, "%d", art_num));
#if 0
  buffer_printf (vr->b, "<p> Rendering %d replies. </p>\n", n_art);
#endif
  if (n_art > 0)
    {
      buffer_puts (vr->b, "<hr>\n");
      for (i = 0; i < n_art; i++)
	{
	  if (i == lastread)
	    buffer_puts (vr->b, "<a name=\"lastread\">");
	  article_render_reply (vr, art_num, i);
	}
    }
  acct_set_lastread(vr, "articles", ap_psprintf(p, "%d", art_num), n_art - 1);
}


static void
article_render_from_xml (VirguleReq *vr, int art_num, xmlDoc *doc, ArticleRenderStyle style)
{
  Buffer *b = vr->b;
  xmlNode *root = doc->root;
  char *date;
  char *author;
  char *title;
  char *lead;
  char *lead_tag;
  int n_replies;
  char *article_dir;

  title = xml_find_child_string (root, "title", "(no title)");
  date = xml_find_child_string (root, "date", "(no date)");
  author = xml_find_child_string (root, "author", "(no author)");
  lead = xml_find_child_string (root, "lead", "(no lead)");

  lead_tag = (style == ARTICLE_RENDER_LEAD) ? "blockquote" : "p";

  render_cert_level_begin (vr, author, CERT_STYLE_LARGE);
  buffer_puts (b, title);
  render_cert_level_end (vr, CERT_STYLE_LARGE);
  buffer_printf (b, "<b>Posted %s by <a href=\"%s/person/%s/\">%s</a></b>",
		 render_date (vr, date), vr->prefix, author, author);
  render_cert_level_text (vr, author);
  buffer_printf (b, "<%s>\n"
		 "%s\n", lead_tag, lead);
  article_dir = ap_psprintf (vr->r->pool, "articles/_%d", art_num);
  n_replies = db_dir_max (vr->db, article_dir) + 1;
  if (style == ARTICLE_RENDER_FULL)
    {
      char *body;
      body = xml_find_child_string (root, "body", NULL);
      if (body)
	  buffer_printf (b, "<p> %s\n", body);

      article_render_replies (vr, art_num);

      if (req_ok_to_post (vr))
	{
	  buffer_printf (b, "<hr> <p> Post a reply to <x>article</x>: %s. </p>\n"
		 "<form method=\"POST\" action=\"replysubmit.html\">\n"
		 " <p> Reply title: <br>\n"
		 " <input type=\"text\" name=\"title\" size=50> </p>\n"
		 " <p> Body of reply: <br>\n"
		 " <textarea name=\"body\" cols=72 rows=16 wrap=soft>"
		 "</textarea> </p>\n"
		 " <input type=\"hidden\" name=\"art_num\" value=\"%d\">\n"
		 " <p> <input type=\"submit\" name=post value=\"Post\">\n"
		 " <input type=\"submit\" name=preview value=\"Preview\">\n"
		 "</form>\n", title, art_num);

	  render_acceptable_html (vr);
	}
    }
  else if (style == ARTICLE_RENDER_LEAD)
    {
      pool *p = vr->r->pool;
      int n_new, lastread;


      n_new = -1;
      lastread = acct_get_lastread (vr, "articles", ap_psprintf(p, "%d", art_num));

      if (lastread != -1)
	n_new = n_replies - lastread - 1;
      else if (vr->u != NULL)
	n_new = n_replies;


      buffer_printf (b, "<p> <a href=\"%s/article/%d.html\">Read more...</a> (%d repl%s) ",
		     vr->prefix,
		     art_num, n_replies, n_replies == 1 ? "y" : "ies");
      if (n_new > 0)
	buffer_printf (b, "(<a href=\"%s/article/%d.html#lastread\">%d new</a>) ",
		       vr->prefix, art_num, n_new);
      buffer_puts (b, "</p> \n");
    }
  buffer_printf (b, "</%s>\n", lead_tag);
}

/* Render the article to buffer. Need to be auth'd if FULL style. */
static void
article_render (VirguleReq *vr, int art_num, int render_body)
{
  Buffer *b = vr->b;
  pool *p = vr->r->pool;
  char *key;
  xmlDoc *doc;

  key = ap_psprintf (p, "articles/_%d/article.xml", art_num);
#if 0
  buffer_printf (b, "<p> Article %d: key %s</p>\n", art_num, key);
#endif

  doc = db_xml_get (p, vr->db, key);
  if (doc != NULL)
    {
      article_render_from_xml (vr, art_num, doc, render_body);
    }
  else
    {
      buffer_printf (b, "<p> Error reading <x>article</x> %d.\n", art_num);
    }
}

static int
article_form_serve (VirguleReq *vr)
{
  Buffer *b = vr->b;

  auth_user (vr);

  if (vr->u == NULL)
    return send_error_page (vr, "Not logged in", "You can't post <x>an article</x> because you're not logged in.");

  if (!req_ok_to_post (vr))
    return send_error_page (vr, "Not certified", "You can't post because you're not certified. Please see the <a href=\"%s/certs.html\">certification overview</a> for more details.", vr->prefix);

  render_header (vr, "Post a new <x>article</x>");

  buffer_puts (b, "<p> Post a new <x>article</x>. </p>\n"
	       "<form method=\"POST\" action=\"postsubmit.html\">\n"
	       " <p> <x>Article</x> title: <br>\n"
	       " <input type=\"text\" name=\"title\" size=50> </p>\n"
	       " <p> <x>Article</x> lead: <br>\n"
	       " <textarea name=\"lead\" cols=72 rows=4 wrap=soft>"
	       "</textarea> </p>\n"
	       " <p> Body of <x>article</x>: <br>\n"
	       " <textarea name=\"body\" cols=72 rows=16 wrap=soft>"
	       "</textarea> </p>\n"
	       " <p> <input type=\"submit\" name=post value=\"Post\">\n"
	       " <input type=\"submit\" name=preview value=\"Preview\">\n"
	       "</form>\n");

  render_acceptable_html (vr);

  return render_footer_send (vr);
}

/**
 * article_generic_submit_serve: Submit article or reply.
 * @vr: The #VirguleReq context.
 * @title: Title, as raw text.
 * @lead: Lead, as raw text.
 * @body: Body, as raw text.
 * @submit_type: "article" or "reply".
 * @key_base: Base pathname of db key.
 * @key_suffix: Suffix of db key, after article number.
 * @art_num_str: The article number being replied to, or NULL if article.
 *
 * Submits article or reply.
 *
 * Return value: Response code.
 **/
static int
article_generic_submit_serve (VirguleReq *vr,
			      const char *title, const char *lead, const char *body,
			      const char *submit_type,
			      const char *key_base, const char *key_suffix,
			      const char *art_num_str)
{
  pool *p = vr->r->pool;
  Buffer *b = vr->b;
  const char *date;
  char *key;
  xmlDoc *doc;
  xmlNode *root;
  xmlNode *tree;
  int status;
  char *str;
  char *lead_error, *body_error;
  char *nice_title;
  char *nice_lead;
  char *nice_body;

  db_lock_upgrade(vr->lock);
  auth_user (vr);
  if (vr->u == NULL)
    return send_error_page (vr, "Not logged in", "You can't post <x>an article</x> because you're not logged in.");

  if (!req_ok_to_post (vr))
    return send_error_page (vr, "Not certified", "You can't post because you're not certified. Please see the <a href=\"%s/certs.html\">certification overview</a> for more details.", vr->prefix);

  date = iso_now (p);

  if (title == NULL || title[0] == 0)
    return send_error_page (vr, "Need title", "Your <x>%s</x> needs a title. Go back and try again.", submit_type);
  if (!strcmp (submit_type, "article") && (lead == NULL || lead[0] == 0))
    return send_error_page (vr, "Need lead", "Your <x>article</x> needs a lead. Go back and try again.");
  if (!strcmp (submit_type, "reply") && (body == NULL || body[0] == 0))
    return send_error_page (vr, "Need body", "Your reply needs a body. Go back and try again.");

  nice_title = nice_text (p, title);
  nice_lead = lead == NULL ? NULL : nice_htext (vr, lead, &lead_error);
  nice_body = body == NULL ? NULL : nice_htext (vr, body, &body_error);

  if (ap_table_get (get_args_table (vr), "preview"))
    {
      /* render a preview */
      if (!strcmp (submit_type, "reply"))
	{
	  render_header (vr, "Reply preview");
	  render_cert_level_begin (vr, vr->u, CERT_STYLE_MEDIUM);
	  buffer_puts (b, nice_title);
	  render_cert_level_end (vr, CERT_STYLE_MEDIUM);
	  buffer_printf (b, "<p> %s </p>\n", nice_body);
	  buffer_puts (b, "<hr>\n");
	  buffer_printf (b, "<p> Edit your reply: </p>\n"
			 "<form method=\"POST\" action=\"replysubmit.html\">\n"
			 " <p> <x>Article</x> title: <br>\n"
			 " <input type=\"text\" name=\"title\" value=\"%s\" size=50> </p>\n"
			 " <p> Body of <x>article</x>: <br>\n"
			 " <textarea name=\"body\" cols=72 rows=16 wrap=soft>%s"
			 "</textarea> </p>\n"
			 " <input type=\"hidden\" name=\"art_num\" value=\"%s\">\n"
			 " <p> <input type=\"submit\" name=post value=\"Post\">\n"
			 " <input type=\"submit\" name=preview value=\"Preview\">\n"
			 "</form>\n",
			 escape_html_attr (p, title),
			 ap_escape_html (p, body),
			 art_num_str);
	  
	  render_acceptable_html (vr);
	}
      else if (!strcmp (submit_type, "article"))
	{
	  render_header (vr, "<x>Article</x> preview");
	  render_cert_level_begin (vr, vr->u, CERT_STYLE_LARGE);
	  buffer_puts (b, nice_title);
	  render_cert_level_end (vr, CERT_STYLE_LARGE);
	  buffer_printf (b, "<p>\n%s\n", nice_lead);
	  buffer_printf (b, "<p> %s </p>\n", nice_body);
	  buffer_puts (b, "<hr>\n");
	  buffer_printf (b, "<p> Edit your <x>article</x>: </p>\n"
			 "<form method=\"POST\" action=\"postsubmit.html\">\n"
			 " <p> <x>Article</x> title: <br>\n"
			 " <input type=\"text\" name=\"title\" value=\"%s\" size=50> </p>\n"
			 " <p> <x>Article</x> lead: <br>\n"
			 " <textarea name=\"lead\" cols=72 rows=4 wrap=soft>%s"
			 "</textarea> </p>\n",
			 escape_html_attr (p, title),
			 ap_escape_html (p, lead));
	  if (lead_error != NULL)
	    buffer_printf (b, "<p> <b>Warning:</b> %s </p>\n", lead_error);

	  buffer_printf (b, " <p> Body of <x>article</x>: <br>\n"
			 " <textarea name=\"body\" cols=72 rows=16 wrap=soft>%s"
			 "</textarea> </p>\n"
			 " <p> <input type=\"submit\" name=post value=\"Post\">\n"
			 " <input type=\"submit\" name=preview value=\"Preview\">\n"
			 "</form>\n",
			 ap_escape_html (p, body));
	  if (body_error != NULL)
	    buffer_printf (b, "<p> <b>Warning:</b> %s </p>\n", body_error);

	  render_acceptable_html (vr);

	}
      return render_footer_send (vr);
    }

  key = ap_psprintf (p, "%s/_%d%s",
		     key_base,
		     db_dir_max (vr->db, key_base) + 1,
		     key_suffix);

  doc = db_xml_doc_new (p);
  root = xmlNewDocNode (doc, NULL, "article", NULL);
  doc->root = root;

  tree = xmlNewChild (root, NULL, "date", date);
  tree = xmlNewChild (root, NULL, "author", vr->u);

  tree = xmlNewChild (root, NULL, "title", NULL);
  xmlAddChild (tree, xmlNewDocText (doc, nice_title));

  if (lead && lead[0])
  {
    tree = xmlNewChild (root, NULL, "lead", NULL);
    xmlAddChild (tree, xmlNewDocText (doc, nice_lead));
  }

  if (body != NULL && body[0])
    {
      tree = xmlNewChild (root, NULL, "body", NULL);
      xmlAddChild (tree, xmlNewDocText (doc, nice_body));
    }

  status = db_xml_put (p, vr->db, key, doc);

  if (status)
    return send_error_page (vr,
			    "Error storing <x>article</x>",
			    "There was an error storing the <x>%s</x>. This means there's something wrong with the site.", submit_type);

  if (!strcmp (submit_type, "reply"))
    ap_table_add (vr->r->headers_out, "refresh", 
		  ap_psprintf(p, "0;URL=/article/%s.html#lastread", art_num_str));
  else
    ap_table_add (vr->r->headers_out, "refresh", 
		  ap_psprintf(p, "0;URL=/article/%d.html",
			      db_dir_max (vr->db, key_base)));

  str = ap_psprintf (p, "Ok, your <x>%s</x> was posted. Thanks!", submit_type);
  return send_error_page (vr, "Posted", str);
}

static int
article_submit_serve (VirguleReq *vr)
{
  int art_num;
  char *key;
  xmlDoc *doc;
  const char *old_author, *old_title;

  table *args;
  const char *title, *lead, *body;

  args = get_args_table (vr);
  if (args == NULL)
    return send_error_page (vr, "Need form data", "This page requires a form submission. If you're not playing around manually with URLs, it suggests there's something wrong with the site.");

  title = ap_table_get (args, "title");
  lead = ap_table_get (args, "lead");
  body = ap_table_get (args, "body");

  /* Auth user and check for duplicate post */
  auth_user (vr);
  if (vr->u == NULL)
    return send_error_page (vr, "Not logged in", "You can't post <x>an article</x> because you're not logged in.");

  art_num = db_dir_max (vr->db, "articles");
  if (art_num >= 0)
    {
      key = ap_psprintf (vr->r->pool, "articles/_%d/article.xml", art_num);
      doc = db_xml_get (vr->r->pool, vr->db, key);
      old_title = xml_find_child_string (doc->root, "title", "(no title)");
      old_author = xml_find_child_string (doc->root, "author", "(no author)");
      if(strcmp (old_author, vr->u) == 0 && strcmp (old_title, title) == 0)
	  return send_error_page (vr, "Duplicate <x>Article</x>", "Please post your <x>article</x> only once.");
    }

  return article_generic_submit_serve (vr, title, lead, body,
				       "article",
				       "articles", "/article.xml", NULL);
}

static int
article_reply_form_serve (VirguleReq *vr)
{
  pool *p = vr->r->pool;
  Buffer *b = vr->b;
  table *args;
  const char *art_num_str;
  int art_num;
  char *key;
  xmlDoc *doc;
  xmlNode *root, *tree;
  char *title;

  auth_user (vr);

  if (vr->u == NULL)
    return send_error_page (vr, "Not logged in", "You can't post a reply because you're not logged in.");

  if (!req_ok_to_post (vr))
    return send_error_page (vr, "Not certified", "You can't post because you're not certified. Please see the <a href=\"%s/certs.html\">certification overview</a> for more details.", vr->prefix);

  args = get_args_table (vr);
  art_num_str = ap_table_get (args, "art_num");
  if (art_num_str == NULL)
    return send_error_page (vr, "Need <x>article</x> number", "Need <x>article</x> number to reply to.");

  art_num = atoi (art_num_str);

  key = ap_psprintf (p, "articles/_%d/article.xml", art_num);
  doc = db_xml_get (p, vr->db, key);
  if (doc == NULL)
    return send_error_page (vr, "<x>Article</x> not found", "<x>Article</x> %d not found.", art_num);
  root = doc->root;
  tree = xml_find_child (root, "title");
  if (tree)
    title = xml_get_string_contents (tree);
  else
    title = "(no title)";

  render_header (vr, "Post a reply");

  buffer_printf (b, "<p> Post a reply to <x>article</x>: %s. </p>\n"
		 "<form method=\"POST\" action=\"replysubmit.html\">\n"
		 " <p> Reply title: <br>\n"
		 " <input type=\"text\" name=\"title\" size=50> </p>\n"
		 " <p> Body of reply: <br>\n"
		 " <textarea name=\"body\" cols=72 rows=16 wrap=soft>"
		 "</textarea> </p>\n"
		 " <input type=\"hidden\" name=\"art_num\" value=\"%d\">\n"
		 " <p> <input type=\"submit\" name=post value=\"Post\">\n"
		 " <input type=\"submit\" name=preview value=\"Preview\">\n"
		 "</form>\n", title, art_num);

  render_acceptable_html (vr);

  return render_footer_send (vr);
}

static int
article_reply_submit_serve (VirguleReq *vr)
{
  pool *p = vr->r->pool;
  table *args;
  const char *title, *art_num_str, *body;
  char *key_base;

  args = get_args_table (vr);
  if (args == NULL)
    return send_error_page (vr, "Need form data", "This page requires a form submission. If you're not playing around manually with URLs, it suggests there's something wrong with the site.");

  title = ap_table_get (args, "title");
  art_num_str = ap_table_get (args, "art_num");
  body = ap_table_get (args, "body");

  if (art_num_str == NULL)
    return send_error_page (vr, "Need <x>article</x> number", "This page requires <x>an article</x> number. If you're not playing around manually with URLs, it suggests there's something wrong with the site.");

  key_base = ap_psprintf (p, "articles/_%d", atoi (art_num_str));

  return article_generic_submit_serve (vr, title, NULL, body,
				       "reply",
				       key_base, "/reply.xml", art_num_str);
}

/**
 * article_recent_render: Render a set of articles.
 * @vr: The #VirguleReq context.
 * @n_arts_max: Maximum number of articles to render.
 * @start: article number of start article, or -1 for most recent.
 *
 * Renders up to @n_arts_max articles, starting at @start (or most
 * recent if -1), from more to less recent.
 *
 * Return value: Apache format status.
 **/
int
article_recent_render (VirguleReq *vr, int n_arts_max, int start)
{
  Buffer *b = vr->b;
  int art_num;
  int n_arts;

  auth_user (vr);

  if (start >= 0)
    art_num = start;
  else
    art_num = db_dir_max (vr->db, "articles");

  for (n_arts = 0; n_arts < n_arts_max && art_num >= 0; n_arts++)
    {
      article_render (vr, art_num, ARTICLE_RENDER_LEAD);
      art_num--;
    }

  if (n_arts == 0)
    buffer_puts (b, "<p> No <x>articles</x>. </p>");
  if (art_num >= 0)
    {
    buffer_printf (b, "<p> <a href=\"%s/article/older.html?start=%d\">%d older <x>article%s</x>...</a> </p>\n",
		   vr->prefix, art_num,
		   art_num + 1, art_num == 0 ? "" : "s");
    }

  if (req_ok_to_post (vr))
    buffer_printf (b, "<p> <a href=\"%s/article/post.html\">Post</a> a new <x>article</x>... </p>\n", vr->prefix);

  return 0;
}

static int
article_index_serve (VirguleReq *vr)
{
  Buffer *b = vr->b;

  auth_user (vr);

  render_header (vr, "Recent <x>articles</x>");

  render_sitemap (vr, 1);

  buffer_puts (b, "<p> Recently posted <x>articles</x>:</p>\n");

  article_recent_render (vr, 20, -1);

  return render_footer_send (vr);
}

static int
article_older_serve (VirguleReq *vr)
{
  Buffer *b = vr->b;
  table *args;
  int start;

  args = get_args_table (vr);
  if (args == NULL)
    return send_error_page (vr, "Need form data", "This page requires a form submission. If you're not playing around manually with URLs, it suggests there's something wrong with the site.");

  start = atoi (ap_table_get (args, "start"));

  auth_user (vr);

  render_header (vr, "Older <x>articles</x>");

  render_sitemap (vr, 1);

  buffer_printf (b, "<p> Older <x>articles</x> (starting at number %d):</p>\n", start);

  article_recent_render (vr, 20, start);

  return render_footer_send (vr);
}

static int
article_num_serve (VirguleReq *vr, const char *t)
{
  pool *p = vr->r->pool;
  Buffer *b = vr->b;
  char *tail;
  int n;
  char *key;
  xmlDoc *doc;
  xmlNode *root;
  char *title;

  auth_user (vr);

  n = strtol (t, &tail, 10);
  if (strcmp (tail, ".html"))
    return send_error_page (vr, "Extra junk", "Extra junk %s not understood. If you're not playing around manually with urls, that means something's wrong with the site.", tail);
#if 0
  return send_error_page (vr, "Article", "Thank you for requesting article number %d. Have a nice day!", n);
#endif
  key = ap_psprintf (p, "articles/_%d/article.xml", n);
  doc = db_xml_get (p, vr->db, key);
  if (doc == NULL)
    return send_error_page (vr, "<x>Article</x> not found", "<x>Article</x> %d not found.", n);
  root = doc->root;
  title = xml_find_child_string (root, "title", "(no title)");


  render_header_raw (vr, title);
  buffer_puts (b, "<blockquote>\n");

  article_render_from_xml (vr, n, doc, ARTICLE_RENDER_FULL);

  buffer_puts (b, "</blockquote>\n");
  return render_footer_send (vr);
}

int
article_serve (VirguleReq *vr)
{
  const char *p;
  if ((p = match_prefix (vr->uri, "/article/")) == NULL)
    return DECLINED;

  if (!strcmp (p, "post.html"))
    return article_form_serve (vr);

  if (!strcmp (p, "postsubmit.html"))
    return article_submit_serve (vr);

  if (!strcmp (p, "reply.html"))
    return article_reply_form_serve (vr);

  if (!strcmp (p, "replysubmit.html"))
    return article_reply_submit_serve (vr);

  if (!strcmp (p, ""))
    return article_index_serve (vr);

  if (!strcmp (p, "older.html"))
    return article_older_serve (vr);

  if (isdigit (p[0]))
    return article_num_serve (vr, p);

  return DECLINED;
}