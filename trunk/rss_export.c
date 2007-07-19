/* This file contains features to export site contents through
   Netscape's RSS.

   Netscape has some documentation up at:
   http://my.netscape.com/publish/help/mnn20/quickstart.html

   Module written by Martijn van Beers.

*/

#include <string.h>
#include <ctype.h>

#include <apr.h>
#include <apr_strings.h>
#include <httpd.h>

#include <libxml/tree.h>
#include <libxml/xmlmemory.h>

#include "private.h"
#include "buffer.h"
#include "db.h"
#include "req.h"
#include "util.h"
#include "db_xml.h"
#include "xml_util.h"
#include "style.h"

#include "rss_export.h"

/* Set the #ifs to 0 to turn off the HREF stipping actions */
static void
rss_render_from_xml (VirguleReq *vr, int art_num, xmlDoc *doc, xmlNodePtr tree)
{
  apr_pool_t *p = vr->r->pool;
  xmlNode *root = doc->xmlRootNode;
  xmlNodePtr subtree;
  char *title;
  char *link;
  char *pubdate, *tmpdate;
  char *raw_description;
  char *clean_description;
#if 1
  char *tmp1, *tmp2;
#endif

  title = virgule_xml_find_child_string (root, "title", "(no title)");
  link = apr_psprintf (p, "%s/article/%d.html", vr->priv->base_uri, art_num);
  raw_description = virgule_xml_find_child_string (root, "lead", "(no description)");
  tmpdate = virgule_xml_find_child_string(root, "date", "(no date)");
  pubdate = virgule_render_date (vr, tmpdate, 2);
  
  /* strip anchor tags */
  clean_description = apr_pstrdup (p, raw_description);  
#if 1
  tmp1 = raw_description;
  tmp2 = clean_description;
  while(*tmp1!=0)
    {
      if(strncasecmp(tmp1,"<a",2)==0)
        {
	  while(*tmp1!=0&&*tmp1!='>')
	    tmp1++;
	  if(*tmp1!=0)
            tmp1++;
        }
      if(strncasecmp(tmp1,"</a>",4)==0)
        tmp1+=4;
      *tmp2 = *tmp1;
      tmp1++;
      tmp2++;
    }
  *tmp2=0;
#endif

  subtree = xmlNewTextChild (tree, NULL, (xmlChar *)"title", (xmlChar *)title);
  subtree = xmlNewTextChild (tree, NULL, (xmlChar *)"link", (xmlChar *)link);
  subtree = xmlNewTextChild (tree, NULL, (xmlChar *)"guid", (xmlChar *)link);
  subtree = xmlNewTextChild (tree, NULL, (xmlChar *)"pubDate", (xmlChar *)pubdate);
  subtree = xmlNewTextChild (tree, NULL, (xmlChar *)"description", (xmlChar *)clean_description);  
}

static void
rss_render (VirguleReq *vr, int art_num, xmlNodePtr tree)
{
  apr_pool_t *p = vr->r->pool;
  char *key;
  xmlDoc *doc;
  xmlNodePtr subtree;

  key = apr_psprintf (p, "articles/_%d/article.xml", art_num);
  doc = virgule_db_xml_get (p, vr->db, key);
  if (doc != NULL)
    {
      subtree = xmlNewChild (tree, NULL, (xmlChar *)"item", NULL);
      rss_render_from_xml (vr, art_num, doc, subtree);
    }
}

static int
rss_index_serve (VirguleReq *vr)
{
  apr_pool_t *p = vr->r->pool;
  Buffer *b = vr->b;
  xmlDocPtr doc;
  xmlNodePtr tree, subtree;
  int art_num;
  int n_arts;
  xmlChar *mem;
  int size;
  char *pubdate;

  doc = xmlNewDoc ((xmlChar *)"1.0");
  if (doc == NULL)
    return virgule_send_error_page (vr, vERROR, "internal", "xmlNewDoc() failed");

  vr->r->content_type = "text/xml; charset=UTF-8";
  doc->xmlRootNode = xmlNewDocNode (doc, NULL, (xmlChar *)"rss", NULL);

  xmlSetProp (doc->xmlRootNode, (xmlChar *)"version", (xmlChar *)"2.0");
  
  tree = xmlNewChild (doc->xmlRootNode, NULL, (xmlChar *)"channel", NULL);
  subtree = xmlNewTextChild (tree, NULL, (xmlChar *)"title", (xmlChar *)vr->priv->site_name);
  subtree = xmlNewTextChild (tree, NULL, (xmlChar *)"link", 
			(xmlChar *)apr_psprintf (p, "%s/", vr->priv->base_uri));
  subtree = xmlNewTextChild (tree, NULL, (xmlChar *)"description", 
			(xmlChar *)apr_psprintf (p, "Recent %s articles", vr->priv->site_name));
  subtree = xmlNewChild (tree, NULL, (xmlChar *)"language", (xmlChar *)"en-us");
  subtree = xmlNewChild (tree, NULL, (xmlChar *)"generator", (xmlChar *)"mod_virgule");

  pubdate = virgule_render_date (vr, virgule_iso_now(vr->r->pool), 2);
  subtree = xmlNewTextChild (tree, NULL, (xmlChar *)"pubDate", (xmlChar *)pubdate);

  art_num = virgule_db_dir_max (vr->db, "articles");

  for (n_arts = 0; n_arts < 15 && art_num >= 0; n_arts++)
    {
      rss_render (vr, art_num, tree);
      art_num--;
    }

  xmlDocDumpFormatMemory (doc, &mem, &size, 1);
  if (size <= 0)
    return virgule_send_error_page (vr, vERROR, "internal", "xmlDocDumpFormatMemory() failed");

  virgule_buffer_write (b, (char *)mem, size);
  xmlFree (mem);
  xmlFreeDoc (doc);
  return virgule_send_response (vr);
}


int
virgule_rss_serve (VirguleReq *vr)
{
   const char *p;
   if ((p = virgule_match_prefix (vr->uri, "/rss/")) == NULL)
     return DECLINED;

   if (!strcmp (p, "articles.xml") || !strcmp (p, "articles-2.0.xml"))
     return rss_index_serve (vr);

   return DECLINED;
}

static int
rss_massage_parse_attr (const char *text, int offs[4])
{
  int i = 0;

  offs[0] = 0;
  offs[1] = 0;
  offs[2] = 0;
  offs[3] = 0;

  while (isspace(text[i])) i++;
  if (text[i] == 0 || text[i] == '/' || text[i] == '>')
    return i;
  offs[0] = i;
  while (isalnum(text[i])) i++;
  offs[1] = i;
  while (isspace(text[i])) i++;
  if (text[i] != '=')
    return i;
  i++;
  while (isspace(text[i])) i++;
  if (text[i] == '"')
    {
      i++;
      offs[2] = i;
      while (text[i] && text[i] != '"') i++;
      offs[3] = i;
      if (text[i] == '"') i++;
    }
  else
    {
      offs[2] = i;
      while (isalnum(text[i])) i++;
      offs[3] = i;
    }
  return i;
}

static void
rss_massage_write_attr (Buffer *b, const char *val, int size)
{
  int i, last = 0;
  char *repl;
  char c;

  for (i = 0; i < size; i++)
    {
      if ((c = val[i]) == '&') repl = "&amp;";
      else if (c == '<') repl = "&lt;";
      else if (c == '>') repl = "&gt;";
      else if (c == '"') repl = "&quot;";
      else continue;
      if (i > last)
	virgule_buffer_write (b, val + last, i - last);
      virgule_buffer_puts (b, repl);
      last = i + 1;
    }
  if (i > last)
    virgule_buffer_write (b, val + last, i - last);
}

static void
rss_massage_write_url (Buffer *b, const char *url, int urlsize,
		       const char *baseurl)
{
  if (url[0] == '/')
    {
      int i;

      for (i = 0; baseurl[i] && baseurl[i] != '/'; i++);
      if (baseurl[i] == '/' && baseurl[i + 1] == '/')
	{
	  for (i += 2; baseurl[i] && baseurl[i] != '/'; i++);
	  virgule_buffer_write (b, baseurl, i);
	}
    }
  rss_massage_write_attr (b, url, urlsize);
}

static int
rss_massage_tag (Buffer *b, const char *text, const char *baseurl)
{
  int i, next;

  if (!memcmp(text, "<a ", 3))
    {
      virgule_buffer_puts (b, "&lt;a ");
      for (i = 3; text[i] && text[i] != '>' && text[i] != '/'; i = next)
	{
	  int offs[4];

	  next = i + rss_massage_parse_attr (text + i, offs);
	  if (offs[0] == offs[1]) break;

	  virgule_buffer_write (b, text + i + offs[0], offs[1] - offs[0]);
	  if (offs[2])
	    {
	      virgule_buffer_puts (b, "=&quot;");
	      if (offs[1] - offs[0] == 4 && !memcmp (text + i + offs[0], "href", 4))
		rss_massage_write_url (b, text + i + offs[2], offs[3] - offs[2], baseurl);
	      else
		rss_massage_write_attr (b, text + i + offs[2], offs[3] - offs[2]);
	      virgule_buffer_puts (b, "&quot;");
	    }
	  virgule_buffer_puts (b, " ");
	}
      return i;
    }
  else
    {
      virgule_buffer_puts (b, "&lt;");
      return 1;
    }
}

/**
 * rss_massage_text: Massage description into valid RSS.
 *
 * There are two primary functions here: first, convert valid HTML
 * references such as "&nbsp;" into XML, and second, convert all
 * relative URL's into absolute.
 **/
char *
virgule_rss_massage_text (apr_pool_t *p, const char *text, const char *baseurl)
{
  Buffer *b = virgule_buffer_new (p);
  int i, end;
  char c;

  for (i = 0; text[i]; i = end)
    {
      for (end = i;
	   (c = text[end]) &&
	     c != '&' && c != '<' && c != '>';
	   end++);
      if (end > i)
	virgule_buffer_write (b, text + i, end - i);
      if (c == '&')
	{
	  virgule_buffer_puts (b, "&amp;");
	  end++;
	}
      else if (c == '<')
	{
	  end += rss_massage_tag (b, text + end, baseurl);
	}
      else if (c == '>')
	{
	  virgule_buffer_puts (b, "&gt;");
	  end++;
	}
    }
  return virgule_buffer_extract (b);
}
