/* A module for posting news stories. */

#include <ctype.h>
#include <time.h>

#include <apr.h>
#include <apr_strings.h>
#include <httpd.h>

#include <libxml/tree.h>

#include "private.h"
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
#include "site.h"

#include "article.h"


struct _Topic {
  char *desc;
  char *url;
};


/**
 * Render an img tag for the topic icon based on the passed topic name
 **/
static void
article_render_topic (VirguleReq *vr, char *topic)
{
  const Topic **t;
  
  if (!vr->priv->use_article_topics || !topic) return;

  for (t = vr->priv->topics; *t; t++)
    if (!strcmp ( (*t)->desc, topic) )
      break;

  if(*t)
    virgule_buffer_printf (vr->b, "<img height=\"50\" width=\"50\" src=\"%s\" alt=\"%s\" title=\"%s\">",
		    (*t)->url,(*t)->desc,(*t)->desc);		    
}


static void
article_render_reply (VirguleReq *vr, int art_num, int reply_num)
{
  apr_pool_t *p = vr->r->pool;
  Buffer *b = vr->b;
  char *key;
  xmlDoc *doc;

  key = apr_psprintf (p, "articles/_%d/_%d/reply.xml", art_num, reply_num);

  doc = virgule_db_xml_get (p, vr->db, key);
  if (doc != NULL)
    {
      xmlNode *root = doc->xmlRootNode;
      char *author;
      char *title;
      char *body;
      char *date;

      author = virgule_xml_find_child_string (root, "author", "(no author)");
      title = virgule_xml_find_child_string (root, "title", "(no title)");
      body = virgule_xml_find_child_string (root, "body", "(no body)");
      date = virgule_xml_find_child_string (root, "date", "(no date)");

      virgule_render_cert_level_begin (vr, author, CERT_STYLE_MEDIUM);
      virgule_buffer_printf (b, "<a name=\"%u\"><b>%s</b></a>, posted %s by <a href=\"%s/person/%s/\">%s</a> <a href=\"#%u\" style=\"text-decoration: none\">&raquo;</a>\n",
		     reply_num, title, virgule_render_date (vr, date, 1), vr->prefix, ap_escape_uri(vr->r->pool, author), author, reply_num);
      virgule_render_cert_level_text (vr, author);
      virgule_render_cert_level_end (vr, CERT_STYLE_MEDIUM);
      virgule_buffer_printf (b, "<blockquote>\n%s\n</blockquote>\n", body);
    }
  else
    {
      virgule_buffer_printf (b, "<p>Error reading <x>article</x> %d.\n", art_num);
    }
}

static void
article_render_replies (VirguleReq *vr, int art_num)
{
  apr_pool_t *p = vr->r->pool;
  int lastread;
  char *base;
  int n_art;
  int i;

  base = apr_psprintf (vr->r->pool, "articles/_%d", art_num);
  n_art = virgule_db_dir_max (vr->db, base) + 1;
  lastread = virgule_acct_get_lastread (vr, "articles", apr_psprintf(p, "%d", art_num));
#if 0
  virgule_buffer_printf (vr->b, "<p>Rendering %d replies.</p>\n", n_art);
#endif
  if (n_art > 0)
    {
      virgule_buffer_puts (vr->b, "<hr>\n");
      for (i = 0; i < n_art; i++)
	{
	  if (i == lastread)
	    virgule_buffer_puts (vr->b, "<a name=\"lastread\">");
	  article_render_reply (vr, art_num, i);
	}
    }
  virgule_acct_set_lastread(vr, "articles", apr_psprintf(p, "%d", art_num), n_art - 1);
}


/**
 * Renders an article from the provided XML document
 **/
static void
article_render_from_xml (VirguleReq *vr, int art_num, xmlDoc *doc, ArticleRenderStyle style)
{
  Buffer *b = vr->b;
  xmlNode *root = doc->xmlRootNode;
  char *date;
  char *update;
  char *updatestr = "";
  char *author;
  char *topic;
  char *title;
  char *lead;
  char *lead_a_open = "";
  char *lead_a_close = "";
  char *bmbox = "";
  char *editstr = "";
  int n_replies;
  char *article_dir;
  CertLevel cert_level;

  topic = virgule_xml_find_child_string (root, "topic", "(no topic)");
  title = virgule_xml_find_child_string (root, "title", "(no title)");
  date = virgule_xml_find_child_string (root, "date", "(no date)");
  update = virgule_xml_find_child_string (root, "update", NULL);
  author = virgule_xml_find_child_string (root, "author", "(no author)");
  lead = virgule_xml_find_child_string (root, "lead", "(no lead)");

  if (vr->u != NULL)
    {
      if ((virgule_virgule_to_time_t(vr, date) + (vr->priv->article_days_to_edit * 86400)) > time(NULL) && (!strcmp(vr->u, author)))
        editstr = apr_psprintf (vr->r->pool, " [ <a href=\"/article/edit.html?key=%d\">Edit</a> ] ", art_num);
    }
    
  virgule_buffer_puts (b, "<div class=\"node\">");

  if(vr->priv->use_article_topics)
    {
      virgule_buffer_puts (b, "<div class=\"tags\">");
      article_render_topic (vr, topic);
      virgule_buffer_puts (b, "</div>");
    }

  /* check if author is a special user */
  if (virgule_user_is_special(vr,author))
    cert_level = virgule_cert_num_levels (vr);
  else 
    cert_level = virgule_cert_level_from_name(vr, virgule_req_get_tmetric_level (vr, author));

  virgule_buffer_printf (b, "<h1 class=\"level%i\">", cert_level);

  if (style == ARTICLE_RENDER_LEAD && vr->priv->use_article_title_links)
    {
      lead_a_open = apr_psprintf (vr->r->pool,"<a href=\"%s/article/%d.html\">",vr->prefix,art_num);
      lead_a_close = "</a>";
    }

  if (style == ARTICLE_RENDER_FULL)
    {
      char *bmurl = apr_psprintf (vr->r->pool, "%s/article/%d.html", vr->priv->base_uri, art_num);
      char *bmtitle = ap_escape_uri (vr->r->pool, title);
      bmbox = apr_psprintf (vr->r->pool, "<span class=\"bm\">"
                                         "<a href=\"javascript:void(0)\" onclick=\"sbm(event,'%s','%s')\">"
					 "<img src=\"/images/share.png\" alt=\"Share This\" title=\"Share This\" /></a>"
					 "</span>",
					 bmurl, virgule_str_subst (vr->r->pool, bmtitle, "'", "%27"));
    }
  
  virgule_buffer_printf (b, "%s%s%s", lead_a_open, title, lead_a_close);

  if (update != NULL)
    updatestr = apr_psprintf (vr->r->pool, " (updated %s)", virgule_render_date (vr, update, 1));

  virgule_buffer_printf (b, "</h1><h2>Posted %s%s by "
		 "<a href=\"%s/person/%s/\">%s</a>%s %s</h2>\n",
		 virgule_render_date (vr, date, 1), updatestr, vr->prefix, 
		 ap_escape_uri(vr->r->pool, author), author, editstr, bmbox);

  virgule_buffer_printf (b, "<p>%s</p>",lead);

  article_dir = apr_psprintf (vr->r->pool, "articles/_%d", art_num);
  n_replies = virgule_db_dir_max (vr->db, article_dir) + 1;
  if (style == ARTICLE_RENDER_FULL)
    {
      char *body;
      body = virgule_xml_find_child_string (root, "body", NULL);
      if (body)
	  virgule_buffer_printf (b, "<p>%s</p>\n", body);

      article_render_replies (vr, art_num);

      if (virgule_req_ok_to_reply (vr))
        {
	  virgule_buffer_printf (b, "<hr><p>Post a reply to <x>article</x>: %s.</p>\n"
                 "<form method=\"POST\" action=\"replysubmit.html\" accept-charset=\"UTF-8\">\n"
		 "<p>Reply title: <br>\n"
		 "<input type=\"text\" name=\"title\" size=\"50\" maxlength=\"60\"></p>\n"
		 "<p> Body of reply: <br>\n"
		 "<textarea name=\"body\" cols=\"72\" rows=\"16\" wrap=\"soft\">"
		 "</textarea></p>\n"
		 "<input type=\"hidden\" name=\"art_num\" value=\"%d\">\n"
		 "<p><input type=\"submit\" name=\"post\" value=\"Post\">\n"
		 "<input type=\"submit\" name=\"preview\" value=\"Preview\">\n"
		 "</form>\n", title, art_num);

          virgule_render_acceptable_html (vr);
        }


    }
  else if (style == ARTICLE_RENDER_LEAD)
    {
      apr_pool_t *p = vr->r->pool;
      int n_new, lastread;


      n_new = -1;
      lastread = virgule_acct_get_lastread (vr, "articles", apr_psprintf(p, "%d", art_num));

      if (lastread != -1)
	n_new = n_replies - lastread - 1;
      else if (vr->u != NULL)
	n_new = n_replies;


      virgule_buffer_printf (b, "<p><a href=\"%s/article/%d.html\">Read more...</a> (%d repl%s) ",
		     vr->prefix,
		     art_num, n_replies, n_replies == 1 ? "y" : "ies");
      if (n_new > 0)
        virgule_buffer_printf (b, "(<a href=\"%s/article/%d.html#lastread\">%d new</a>) ",
		       vr->prefix, art_num, n_new);
      virgule_buffer_puts (b, "</p>\n");
    }
  virgule_buffer_puts (b, "</div>\n");
}


/* Render the article to buffer. Need to be auth'd if FULL style. */
static void
article_render (VirguleReq *vr, int art_num, int render_body)
{
  Buffer *b = vr->b;
  apr_pool_t *p = vr->r->pool;
  char *key;
  xmlDoc *doc;

  key = apr_psprintf (p, "articles/_%d/article.xml", art_num);
#if 0
  virgule_buffer_printf (b, "<p>Article %d: key %s</p>\n", art_num, key);
#endif

  doc = virgule_db_xml_get (p, vr->db, key);
  if (doc != NULL)
    {
      article_render_from_xml (vr, art_num, doc, render_body);
    }
  else
    {
      virgule_buffer_printf (b, "<p>Error reading <x>article</x> %d.\n", art_num);
    }
}


static int
article_form_serve (VirguleReq *vr)
{
  const Topic **t;

  virgule_auth_user (vr);

  if (vr->u == NULL)
    return virgule_send_error_page (vr, vERROR, "forbidden", "You can't post <x>an article</x> because you're not logged in.");

  if (!virgule_req_ok_to_post (vr))
    return virgule_send_error_page (vr, vERROR, "forbidden", "You can't post because you're not certified. Please see the <a href=\"%s/certs.html\">certification overview</a> for more details.", vr->prefix);

  if (virgule_set_temp_buffer (vr) != 0)
    return HTTP_INTERNAL_SERVER_ERROR;

  virgule_buffer_puts (vr->b, "<form method=\"POST\" action=\"postsubmit.html\" accept-charset=\"UTF-8\">\n");

  if(vr->priv->use_article_topics)
    {
      virgule_buffer_puts (vr->b,"<p><b><x>Article</x> topic</b>:<br>\n <select name=\"topic\">\n");

      for (t = vr->priv->topics; *t; t++)
	virgule_buffer_printf (vr->b, "<option>%s</option>\n", (*t)->desc);
	       
      virgule_buffer_puts (vr->b,"</select></p>\n");
    }
  
  virgule_buffer_puts (vr->b,"<p><b><x>Article</x> title</b>:<br>\n");

  virgule_buffer_printf (vr->b,"<input type=\"text\" name=\"title\" size=\"40\" maxlength=\"%i\"></p>\n", vr->priv->article_title_maxsize);

  virgule_buffer_puts (vr->b,"<p><b><x>Article</x> lead</b>. This should be a one paragraph summary "
	       "of the story complete with links to original sources when "
	       "appropriate.<br>"
	       " <textarea name=\"lead\" cols=72 rows=6 wrap=hard>"
	       "</textarea></p>\n"
	       "<p><b><x>Article</x> Body</b>. This should contain the body "
	       "of your article and may be as long as needed. If your entire "
	       "article is only one paragraph, put it in the lead field above "
	       "and leave this one empty.<br>"
	       "<textarea name=\"body\" cols=72 rows=16 wrap=hard>"
	       "</textarea></p>\n"
	       "<p><input type=\"submit\" name=preview value=\"Preview\">\n"
	       "</form>\n");

  virgule_render_acceptable_html (vr);

  virgule_set_main_buffer (vr);

  return virgule_render_in_template (vr, "/templates/article-post.xml", "content", "Post a new <x>article</x>");
}

/**
 * article_generic_submit_serve: Submit article or reply.
 * @vr: The #VirguleReq context.
 * @title: Title, as raw text.
 * @lead: Lead, as raw text.
 * @body: Body, as raw text.
 * @olddate: Null for new posts. Original post date for edit of existing post
 * @oldkey: Null for new posts. Original article/reply key for edits
 * @submit_type: "article" or "reply".
 * @key_base: Base pathname of db key.
 * @key_suffix: Suffix of db key, after article number.
 * @art_num_str: The article number being replied to, or NULL if article.
 *
 * Submits article or reply.
 *
 * Return value: Response code.
 *
 * ToDo: There are a lot potential conflicts between char and xmlChar 
 * pointers that should be resolved to make the code more consistent.
 **/
static int
article_generic_submit_serve (VirguleReq *vr,
			      const char *topic,
			      const char *title, 
			      const char *lead, 
			      const char *body,
			      const char *olddate,
			      const char *oldkey,
			      const char *submit_type,
			      const char *key_base, 
			      const char *key_suffix,
			      const char *art_num_str)
{
  apr_pool_t *p = vr->r->pool;
  apr_table_t *args;
  Buffer *b = NULL;
  const Topic **t;
  const char *date;
  char *key;
  xmlDoc *doc;
  xmlNode *root;
  xmlNode *tree;
  int status;
  char *str = NULL;
  char *lead_error, *body_error;
  char *nice_title;
  char *nice_lead;
  char *nice_body;

  virgule_auth_user (vr);
  if (vr->u == NULL)
    return virgule_send_error_page (vr, vERROR, "forbidden", "You can't post <x>an article</x> because you're not logged in.");

  if (!virgule_req_ok_to_reply (vr))
    return virgule_send_error_page (vr, vERROR, "forbidden", "You can't post because you're not certified. Please see the <a href=\"%s/certs.html\">certification overview</a> for more details.", vr->prefix);

  date = virgule_iso_now (p);

  if (title == NULL || title[0] == 0)
    return virgule_send_error_page (vr, vERROR, "Need title", "Your <x>%s</x> needs a title. Go back and try again.", submit_type);
  if (!strcmp (submit_type, "article") && (lead == NULL || lead[0] == 0))
    return virgule_send_error_page (vr, vERROR, "Need lead", "Your <x>article</x> needs a lead. Go back and try again.");
  if (!strcmp (submit_type, "reply") && (body == NULL || body[0] == 0))
    return virgule_send_error_page (vr, vERROR, "Need body", "Your reply needs a body. Go back and try again.");

  nice_title = virgule_nice_text (p, title);
  nice_lead = lead == NULL ? "" : virgule_nice_htext (vr, lead, &lead_error);
  nice_body = body == NULL ? "" : virgule_nice_htext (vr, body, &body_error);

  args = virgule_get_args_table (vr);
  if(olddate != NULL)
    apr_table_set (args, "preview", "Preview");
  else
    olddate = apr_table_get (args, "olddate");
  if(oldkey == NULL)
    oldkey = apr_table_get (args, "oldkey");

  if (apr_table_get (args, "preview"))
    {
      /* render a preview */
      if (virgule_set_temp_buffer (vr) != 0)
        return HTTP_INTERNAL_SERVER_ERROR;

      b = vr->b;

      if (!strcmp (submit_type, "reply"))
	{
          str = apr_pstrdup (p, "Reply preview");
	  virgule_render_cert_level_begin (vr, vr->u, CERT_STYLE_MEDIUM);
	  virgule_buffer_printf (b, "<font size=+2><b>%s</b></font><br>\n", nice_title);
	  virgule_render_cert_level_end (vr, CERT_STYLE_MEDIUM);
	  virgule_buffer_printf (b, "<p>%s</p>\n", nice_body);
	  virgule_buffer_puts (b, "<hr>\n");
	  virgule_buffer_printf (b, "<p>Edit your reply:</p>\n"
			 "<form method=\"POST\" action=\"replysubmit.html\" accept-charset=\"UTF-8\">\n"
			 "<p><x>Article</x> title: <br>\n"
			 "<input type=\"text\" name=\"title\" value=\"%s\" size=\"40\" maxlength=\"60\"></p>\n"
			 "<p>Body of <x>article</x>: <br>\n"
			 "<textarea name=\"body\" cols=\"72\" rows=\"16\" wrap=\"hard\">%s"
			 "</textarea></p>\n"
			 "<input type=\"hidden\" name=\"art_num\" value=\"%s\">\n"
			 "<p><input type=\"submit\" name=\"post\" value=\"Post\">\n"
			 "<input type=\"submit\" name=\"preview\" value=\"Preview\">\n"
			 "</form>\n",
			 virgule_str_subst (p, title, "\"", "&quot;"),
			 ap_escape_html (p, body),
			 art_num_str);
	  
	  virgule_render_acceptable_html (vr);
	}
      else if (!strcmp (submit_type, "article"))
	{
          str = apr_pstrdup (p, "<x>Article</x>  preview");
	  if(vr->priv->use_article_topics)
	    {
	      virgule_buffer_puts (b, "<table><tr><td>");
              article_render_topic (vr, (char *)topic);
              virgule_buffer_puts (b, "</td><td>");
	    }
          virgule_render_cert_level_begin (vr, vr->u, CERT_STYLE_LARGE);
          virgule_buffer_printf (b, "<span class=\"article-title\">%s</span>",nice_title);
          virgule_render_cert_level_end (vr, CERT_STYLE_LARGE);
	  
	  if(vr->priv->use_article_topics)
            virgule_buffer_puts (b, "</td></tr></table>\n");

	  virgule_buffer_printf (b, "<p>%s</p>\n", nice_lead);
	  virgule_buffer_printf (b, "<p>%s</p>\n", nice_body);
	  virgule_buffer_puts (b, "<hr>\n");
	  virgule_buffer_puts (b, "<p>Edit your <x>article</x>:</p>\n"
		"<form method=\"POST\" action=\"postsubmit.html\" accept-charset=\"UTF-8\">\n");

          if(olddate && oldkey)
	    {
	      virgule_buffer_printf (b, "<input type=\"hidden\" name=\"olddate\" value=\"%s\" />\n", olddate);
	      virgule_buffer_printf (b, "<input type=\"hidden\" name=\"oldkey\" value=\"%s\" />\n", oldkey);
	    }
	    
	  if(vr->priv->use_article_topics)
	    {
	      virgule_buffer_puts (b, "<p><b><x>Article</x> topic</b>:<br>\n <select name=\"topic\">\n");

              for (t = vr->priv->topics; *t; t++)
	        virgule_buffer_printf (b, "<option%s>%s</option>\n",
		  strcmp((*t)->desc,topic) ? "" : " selected",(*t)->desc);

	      virgule_buffer_puts (b, " </select></p>\n");
	    }

	  virgule_buffer_printf (b,
			 "<p><b><x>Article</x> title</b>:<br>\n"
			 "<input type=\"text\" name=\"title\" value=\"%s\" size=\"40\" maxlength=\"%i\"></p>\n"
	        	 "<p><b><x>Article</x> lead</b>. This should be a one paragraph summary "
	    		 "of the story complete with links to the original "
	    		 "sources when appropriate.<br>"			 
			 "<textarea name=\"lead\" cols=72 rows=6 wrap=hard>%s"
			 "</textarea> </p>\n",
			 virgule_str_subst (p, title, "\"", "&quot;"),
			 vr->priv->article_title_maxsize,
			 ap_escape_html (p, lead));
	  if (lead_error != NULL)
	    virgule_buffer_printf (b, "<p><b>Warning:</b> %s</p>\n", lead_error);

	  virgule_buffer_printf (b,"<p><b><x>Article</x> Body</b>. This should "
	    		 "contain the body of your article and may be as long as "
			 "needed. If your entire article is only one paragraph, "
			 "put it in the lead field above and leave this one empty<br>"
			 "<textarea name=\"body\" cols=72 rows=16 wrap=hard>%s"
			 "</textarea></p>\n"
			 "<p><b>Warning:</b> Please proof read your article "
			 "and verify spelling and any html markup before posting. "
			 "Click the <b>Preview</b> button to see changes. Once "
			 "you click the <b>Post</b> button your article will be "
			 "posted and changes are no longer possible."
			 "<p><input type=\"submit\" name=post value=\"Post\">\n"
			 "<input type=\"submit\" name=preview value=\"Preview\">\n"
			 "</form>\n",
			 ap_escape_html (p, (body ? body : "")));
	  if (body_error != NULL)
	    virgule_buffer_printf (b, "<p><b>Warning:</b> %s </p>\n", body_error);

	  virgule_render_acceptable_html (vr);

	}
	virgule_set_main_buffer (vr);
	return virgule_render_in_template (vr, "/templates/default.xml", "content", str);
    }

  key = apr_psprintf (p, "%s/_%d%s",
		      key_base,
		      oldkey ? atoi (oldkey) : virgule_db_dir_max (vr->db, key_base) + 1,
		      key_suffix);

  doc = virgule_db_xml_doc_new (p);
  root = xmlNewDocNode (doc, NULL, (xmlChar *)"article", NULL);
  doc->xmlRootNode = root;

  if(olddate != NULL)
    {
      xmlNewChild (root, NULL, (xmlChar *)"date", (xmlChar *)olddate);
      xmlNewChild (root, NULL, (xmlChar *)"update", (xmlChar *)date);
    }
  else
    {
      tree = xmlNewChild (root, NULL, (xmlChar *)"date", (xmlChar *)date);
    }
  tree = xmlNewChild (root, NULL, (xmlChar *)"author", (xmlChar *)vr->u);

  tree = xmlNewChild (root, NULL, (xmlChar *)"title", NULL);
  xmlAddChild (tree, xmlNewDocText (doc, (xmlChar *)nice_title));

  if(vr->priv->use_article_topics)
    {
      tree = xmlNewChild (root, NULL, (xmlChar *)"topic", NULL);
      xmlAddChild (tree, xmlNewDocText (doc, (xmlChar *)topic));
    }

  if (lead && lead[0])
    {
      tree = xmlNewChild (root, NULL, (xmlChar *)"lead", NULL);
      xmlAddChild (tree, xmlNewDocText (doc, (xmlChar *)nice_lead));
    }

  if (body != NULL && body[0])
    {
      tree = xmlNewChild (root, NULL, (xmlChar *)"body", NULL);
      xmlAddChild (tree, xmlNewDocText (doc, (xmlChar *)nice_body));
    }

  /* sanity-check edit qualifications before saving */
  if (olddate || oldkey) 
    {
      char *a, *d;
      time_t t;
      xmlNodePtr r;
      int art_num = atoi (oldkey);
      char *k = apr_psprintf (vr->r->pool, "articles/_%d/article.xml", art_num);
      xmlDocPtr old = virgule_db_xml_get (vr->r->pool, vr->db, k);
      if (old == NULL)
        return virgule_send_error_page (vr, vERROR, "not found", "The specified <x>article</x> does not exist.");
      r = xmlDocGetRootElement (old);

      /* verify the article is not too old to edit */
      d = virgule_xml_find_child_string (r, "date", NULL);
      t = virgule_virgule_to_time_t (vr, d);
      if (t + (vr->priv->article_days_to_edit * 86400) < time (NULL))
        return virgule_send_error_page (vr, vERROR, "forbidden", "This <x>article</x> is too old to be edited.");

      /* verify this user can edit this article */
      a = virgule_xml_find_child_string (r, "author", NULL);
      if (strcmp (vr->u, a))
        return virgule_send_error_page (vr, vERROR, "forbidden", "Only <x>articles</x> posted by you may be edited.");
    }

  status = virgule_db_xml_put (p, vr->db, key, doc);

  if (status)
    return virgule_send_error_page (vr, vERROR,
			    "database",
			    "There was an error storing the <x>%s</x>. This means there's something wrong with the site.", submit_type);

  if (!strcmp (submit_type, "reply"))
    apr_table_add (vr->r->headers_out, "refresh", 
		  apr_psprintf(p, "0;URL=/article/%s.html#lastread", art_num_str));
  else 
    apr_table_add (vr->r->headers_out, "refresh", 
		  apr_psprintf(p, "0;URL=/article/%d.html", 
		              oldkey ? atoi (oldkey) : virgule_db_dir_max (vr->db, key_base)));

  str = apr_psprintf (p, "Ok, your <x>%s</x> was posted. Thanks!", submit_type);
  return virgule_send_error_page (vr, vINFO, "Posted", str);
}

static int
article_submit_serve (VirguleReq *vr)
{
  int art_num;
  char *key;
  xmlDoc *doc;
  const char *old_author, *old_title;

  apr_table_t *args;
  const char *title, *lead, *body, *oldkey;
  const char *topic = NULL;

  args = virgule_get_args_table (vr);
  if (args == NULL)
    return virgule_send_error_page (vr, vERROR, "form data", "Invalid form submission.");

  if(vr->priv->use_article_topics)
    topic = apr_table_get (args, "topic");
    
  title = apr_table_get (args, "title");
  lead = apr_table_get (args, "lead");
  body = apr_table_get (args, "body");
  oldkey = apr_table_get (args, "oldkey");

  /* Auth user and check for duplicate post */
  virgule_auth_user (vr);
  if (vr->u == NULL)
    return virgule_send_error_page (vr, vERROR, "forbidden", "You can't post <x>an article</x> because you're not logged in.");

  art_num = virgule_db_dir_max (vr->db, "articles");
  if (art_num >= 0 && oldkey == NULL)
    {
      key = apr_psprintf (vr->r->pool, "articles/_%d/article.xml", art_num);
      doc = virgule_db_xml_get (vr->r->pool, vr->db, key);
      old_title = virgule_xml_find_child_string (doc->xmlRootNode, "title", "(no title)");
      old_author = virgule_xml_find_child_string (doc->xmlRootNode, "author", "(no author)");
      if(strcmp (old_author, vr->u) == 0 && strcmp (old_title, title) == 0)
	  return virgule_send_error_page (vr, vERROR, "duplicate", "Duplicate. Please post your <x>article</x> only once.");
    }

  return article_generic_submit_serve (vr, topic, title, lead, body, NULL, NULL,
				       "article",
				       "articles", "/article.xml", NULL);
}


/**
 * article_edit_serve - edit an existing article number
 **/
static int
article_edit_serve (VirguleReq *vr)
{
  int art_num;
  time_t t;
  char *key, *date, *author, *title, *lead, *body, *topic;
  const char *art_num_str;
  apr_table_t *args;
  xmlDocPtr doc;
  xmlNodePtr root;

  /* user must be logged in */
  virgule_auth_user (vr);
  if (vr->u == NULL)
    return virgule_send_error_page (vr, vERROR, "forbidden", "You must be logged in to edit an <x>article</x>.");

  /* verify that we got an article key */
  args = virgule_get_args_table (vr);
  if (args == NULL)
    return virgule_send_error_page (vr, vERROR, "form data", "No <x>article</x> key was specified.");

  /* try to read the article */
  art_num_str = apr_table_get (args, "key");
  art_num = atoi (art_num_str);
  key = apr_psprintf (vr->r->pool, "articles/_%d/article.xml", art_num);
  doc = virgule_db_xml_get (vr->r->pool, vr->db, key);
  if (doc == NULL)
    return virgule_send_error_page (vr, vERROR, "database", "The specified <x>article</x> could not be loaded.");

  root = xmlDocGetRootElement (doc);

  /* verify the article is not too old to edit */
  date = virgule_xml_find_child_string (root, "date", NULL);
  t = virgule_virgule_to_time_t (vr, date);
  if (t + (vr->priv->article_days_to_edit * 86400) < time (NULL))
    return virgule_send_error_page (vr, vERROR, "forbidden", "This <x>article</x> is too old to be edited.");

  /* verify this user can edit this article */
  author = virgule_xml_find_child_string (root, "author", NULL);
  if (strcmp (vr->u, author))
    return virgule_send_error_page (vr, vERROR, "forbidden", "Only <x>articles</x> posted by you may be edited.");

  topic = virgule_xml_find_child_string (root, "topic", NULL);
  title = virgule_xml_find_child_string (root, "title", NULL);
  lead = virgule_xml_find_child_string (root, "lead", NULL);
  body = virgule_xml_find_child_string (root, "body", NULL);

  /* load the editor in preview mode */
  return article_generic_submit_serve (vr, topic, title, lead, body, date, 
                                       art_num_str,
				       "article",
				       "articles", "/article.xml", NULL);
}


/* maybe not needed anymore since form is on article page? */
static int
article_reply_form_serve (VirguleReq *vr)
{
  apr_pool_t *p = vr->r->pool;
  apr_table_t *args;
  const char *art_num_str;
  int art_num;
  char *key;
  xmlDoc *doc;
  xmlNode *root, *tree;
  char *title;

  virgule_auth_user (vr);

  if (vr->u == NULL)
    return virgule_send_error_page (vr, vERROR, "forbidden", "You can't post a reply because you're not logged in.");

  if (!virgule_req_ok_to_reply (vr))
    return virgule_send_error_page (vr, vERROR, "forbidden", "You can't post because you're not certified. Please see the <a href=\"%s/certs.html\">certification overview</a> for more details.", vr->prefix);

  args = virgule_get_args_table (vr);
  art_num_str = apr_table_get (args, "art_num");
  if (art_num_str == NULL)
    return virgule_send_error_page (vr, vERROR, "form data", "Need <x>article</x> number to reply to.");

  art_num = atoi (art_num_str);

  key = apr_psprintf (p, "articles/_%d/article.xml", art_num);
  doc = virgule_db_xml_get (p, vr->db, key);
  if (doc == NULL)
    return virgule_send_error_page (vr, vERROR, "database", "<x>Article</x> %d not found.", art_num);
  root = doc->xmlRootNode;
  tree = virgule_xml_find_child (root, "title");
  if (tree)
    title = virgule_xml_get_string_contents (tree);
  else
    title = "(no title)";

  if (virgule_set_temp_buffer (vr) != 0)
    return HTTP_INTERNAL_SERVER_ERROR;

  virgule_buffer_printf (vr->b, "<p>Post a reply to <x>article</x>: %s.</p>\n"
		 "<form method=\"POST\" action=\"replysubmit.html\" accept-charset=\"UTF-8\">\n"
		 "<p>Reply title: <br>\n"
		 "<input type=\"text\" name=\"title\" size=50 maxlength=50></p>\n"
		 "<p>Body of reply: <br>\n"
		 "<textarea name=\"body\" cols=72 rows=16 wrap=hard>"
		 "</textarea></p>\n"
		 "<input type=\"hidden\" name=\"art_num\" value=\"%d\">\n"
		 "<p><input type=\"submit\" name=post value=\"Post\">\n"
		 "<input type=\"submit\" name=preview value=\"Preview\">\n"
		 "</form>\n", title, art_num);

  virgule_render_acceptable_html (vr);

  virgule_set_main_buffer (vr);
  
  return virgule_render_in_template (vr, "/templates/default.xml", "content", "Post a reply");
}

static int
article_reply_submit_serve (VirguleReq *vr)
{
  apr_pool_t *p = vr->r->pool;
  apr_table_t *args;
  const char *title, *art_num_str, *body;
  char *key_base;
  char *key_reply;
  int last_reply;
  xmlDoc *doc;

  virgule_auth_user (vr);
  
  if (vr->u == NULL)
    return virgule_send_error_page (vr, vERROR, "forbidden", "You can't post a reply because you're not logged in.");

  args = virgule_get_args_table (vr);
  if (args == NULL)
    return virgule_send_error_page (vr, vERROR, "form data", "Invalid form submission.");

  title = apr_table_get (args, "title");
  art_num_str = apr_table_get (args, "art_num");
  body = apr_table_get (args, "body");

  if (art_num_str == NULL)
    return virgule_send_error_page (vr, vERROR, "form data", "This page requires <x>an article</x> number. If you're not playing around manually with URLs, it suggests there's something wrong with the site.");

  key_base = apr_psprintf (p, "articles/_%d", atoi (art_num_str));

  /* Reject duplicate replies */
  last_reply = virgule_db_dir_max (vr->db, key_base);
  key_reply = apr_psprintf (p, "articles/_%d/_%d/reply.xml", atoi (art_num_str),last_reply);
  doc = virgule_db_xml_get (p, vr->db, key_reply);
  if (doc != NULL)
    {
      const char *old_author;
      const char *old_title;
      old_author = virgule_xml_find_child_string (doc->xmlRootNode, "author", "(no author)");
      old_title = virgule_xml_find_child_string (doc->xmlRootNode, "title", "(no author)");
      if(strcmp (old_author, vr->u) == 0 && strcmp (old_title, title) == 0)
	  return virgule_send_error_page (vr, vERROR, "duplicate", "Duplicate. Please post your reply only once.");
    }

  return article_generic_submit_serve (vr, NULL, title, NULL, body, NULL, NULL,
				       "reply",
				       key_base, "/reply.xml", art_num_str);
}

/**
 * virgule_article_recent_render: Render a set of articles.
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
virgule_article_recent_render (VirguleReq *vr, int n_arts_max, int start)
{
  Buffer *b = vr->b;
  int art_num;
  int n_arts;

  virgule_auth_user (vr);

  if (start >= 0)
    art_num = start;
  else
    art_num = virgule_db_dir_max (vr->db, "articles");

  for (n_arts = 0; n_arts < n_arts_max && art_num >= 0; n_arts++)
    {
      article_render (vr, art_num, ARTICLE_RENDER_LEAD);
      art_num--;
    }

  if (n_arts == 0)
    virgule_buffer_puts (b, "<p>No <x>articles</x>.</p>");
  if (art_num >= 0)
    {
    virgule_buffer_printf (b, "<p><a href=\"%s/article/older.html?start=%d\">%d older <x>article%s</x>...</a></p>\n",
		   vr->prefix, art_num,
		   art_num + 1, art_num == 0 ? "" : "s");
    }

  if (virgule_req_ok_to_post (vr))
    virgule_buffer_printf (b, "<p><a href=\"%s/article/post.html\">Post</a> a new <x>article</x>...</p>\n", vr->prefix);
  else 
    virgule_buffer_printf (b, "<p><a href=\"mailto:%s?subject=Story suggestion\">Suggest a story</a></p>\n", vr->priv->admin_email);

  return 0;
}


/**
 * article_num_serve: Renders one article within the article template
 **/
static int
article_num_serve (VirguleReq *vr, const char *t)
{
  char *tail, *key, *title;
  int n;
  xmlDoc *doc;
  xmlNode *root;

  virgule_auth_user (vr);

  n = strtol (t, &tail, 10);
  if (strcmp (tail, ".html"))
    return virgule_send_error_page (vr, vERROR, "form data", "Invalid <x>Article</x> URL: %s", tail);
  key = apr_psprintf (vr->r->pool, "articles/_%d/article.xml", n);
  doc = virgule_db_xml_get (vr->r->pool, vr->db, key);
  if (doc == NULL)
    return virgule_send_error_page (vr, vERROR, "database", "<x>Article</x> %d not found.", n);
  root = xmlDocGetRootElement (doc);
  title = virgule_xml_find_child_string (root, "title", "(no title)");

  /* initialize the temp buffer */
  if (virgule_set_temp_buffer (vr) != 0)
    return HTTP_INTERNAL_SERVER_ERROR;

  /* render the article to the temp buffer */
  article_render_from_xml (vr, n, doc, ARTICLE_RENDER_FULL);

  /* switch back to the main buffer */
  virgule_set_main_buffer (vr);

  return virgule_render_in_template (vr, "/templates/article.xml", "article", title);
}


/**
 * article_maint: Update article index records
 **/
static int
article_maint (VirguleReq *vr)
{
    int art;
    int art_num = virgule_db_dir_max (vr->db, "articles");

    /* check each article in the database from the earliest to the newest */
    for (art = 0; art <= art_num; art++)
	virgule_acct_update_art_index(vr, art);

    return virgule_send_error_page (vr, vINFO, "Article maintenance", "Article indexing completed.");
}


int
virgule_article_serve (VirguleReq *vr)
{
  const char *p;

  if (!strcmp (vr->uri, "/admin/articlemaint.html"))
    return article_maint (vr);

  if ((p = virgule_match_prefix (vr->uri, "/article/")) == NULL)
    return DECLINED;

  if (!strcmp (p, "post.html"))
    return article_form_serve (vr);

  if (!strcmp (p, "postsubmit.html"))
    return article_submit_serve (vr);

  if (!strcmp (p, "reply.html"))
    return article_reply_form_serve (vr);

  if (!strcmp (p, "replysubmit.html"))
    return article_reply_submit_serve (vr);

  if (!strcmp (p, "edit.html"))
    return article_edit_serve (vr);

  if (isdigit (p[0]))
    return article_num_serve (vr, p);

  return DECLINED;
}
