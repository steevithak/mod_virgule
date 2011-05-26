/**
 *
 *  aggregator.c
 *
 *  Simple Atom, RSS, and RDF Site Summary feed aggregator based on libxml2.
 *
 *  Copyright (C) 2006 by R. Steven Rainwater
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 **/

#include <time.h>

#include <apr.h>
#include <apr_strings.h>
#include <httpd.h>
#include <http_protocol.h>
#include <http_log.h>

#include <libxml/tree.h>
#include <libxml/parser.h>
#include <libxml/xmlmemory.h>
#include <libxml/nanohttp.h>

#include "private.h"
#include "buffer.h"
#include "db.h"
#include "req.h"
#include "util.h"
#include "db_xml.h"
#include "db_ops.h"
#include "xml_util.h"
#include "style.h"
#include "aggregator.h"
#include "hashtable.h"
#include "eigen.h"
#include "diary.h"
#include "acct_maint.h"

typedef enum {
  FEED_TYPE_UNKNOWN,
  FEED_ATOM_03,
  FEED_ATOM_10,
  FEED_RSS_091,
  FEED_RSS_092,
  FEED_RSS_20,
  FEED_RDF_SITE_SUMMARY_09,
  FEED_RDF_SITE_SUMMARY_10,
  FEED_RDF_SITE_SUMMARY_11
} FeedType;

/* random debug crap

char str[90];
strftime(str, 90, "%Y-%m-%dT%H:%M:%S%z", gmtime(&(item->post_time)));
strftime(str, 90, "%Y-%m-%dT%H:%M:%S%z", localtime(&(item->post_time)));
virgule_buffer_printf (vr->b, "<p>post time [%lu] Local :[%s]</p>\n", item->post_time, str);

char str[90];
strftime(str, 90, "%Y-%m-%dT%H:%M:%S%z", gmtime(&(item->post_time)));
virgule_buffer_printf (vr->b, "<p>Title: [%s]</p><p>post time [%lu] Local :[%s]</p>\n", virgule_xml_get_string_contents (item->title), item->post_time, str);

char str[90];
strftime(str, 90, "%Y-%m-%dT%H:%M:%S%z", gmtime(&(item->post_time)));
virgule_buffer_printf (vr->b, "<p>Title: [%s]</p><p>post time [%lu] Local :[%s]</p><p>Content: %s</p>\n", virgule_xml_get_string_contents (item->title), item->post_time, str, virgule_xml_get_string_contents(item->content));

char str[90];
strftime(str, 90, "%Y-%m-%dT%H:%M:%S%z", gmtime(&(item->post_time)));
virgule_buffer_printf (vr->b, "<p>Title: [%s]</p><p>post time [%lu] Local :[%s]</p>\n", virgule_xml_get_string_contents (item->title), item->post_time, str);

char str[90];
strftime(str, 90, "%Y-%m-%dT%H:%M:%S%z", gmtime(&(item->post_time)));
virgule_buffer_printf (vr->b, "<br/>author [%s]<br />entry permalink [%s]<br/>title [%s]<br/>post time [%lu][%s]<br/>\n", item->blogauthor, item->link, virgule_xml_get_string_contents (item->title), item->post_time, str);

virgule_buffer_printf (vr->b, "<p>Latest Entry: [%s][%lu]</p>", user, latest);

virgule_buffer_printf (vr->b, "<p>Posting Entry: [%s][%lu]</p>", virgule_xml_get_string_contents (item->title), item->post_time);

ap_log_rerror(APLOG_MARK,APLOG_CRIT, APR_SUCCESS, vr->r,"mod_virgule: art: %d - %s - %s", art, date, author);

virgule_buffer_printf (vr->b,"<p>Found an item:  [%s]", item->content);
return FALSE;

*/


/**
 * aggregator_identify_feed_type - Attempts to find out what format the
 * specified feed is in by examining the XML structure. Should work on 
 * most common feed formats.
 **/
static FeedType
aggregator_identify_feed_type (VirguleReq *vr, xmlDoc *feedbuffer)
{
  xmlChar *version = NULL;
  xmlNode *root = xmlDocGetRootElement(feedbuffer);

  /* Check for variants of RFC-4287 ATOM feeds */
  if (xmlStrcasecmp (root->name, (xmlChar *)"feed") == 0)
    {
      if (xmlStrcasecmp (root->ns->href, (xmlChar *)"http://www.w3.org/2005/Atom") == 0)
        return FEED_ATOM_10;
      version = (xmlChar *)virgule_xml_get_prop (vr->r->pool, root, (xmlChar *)"version");
      if (xmlStrcmp (version, (xmlChar *)"0.3") == 0)
        return FEED_ATOM_03;
    }

  /* Check for variants of RSS feeds */
  if (xmlStrcasecmp (root->name, (xmlChar *)"rss") == 0)
    {
      version = (xmlChar *)virgule_xml_get_prop (vr->r->pool, root, (xmlChar *)"version");
      if (xmlStrcmp (version, (xmlChar *)"0.91") == 0)
        return FEED_RSS_091;
      else if (xmlStrcmp (version, (xmlChar *)"0.92") == 0)
        return FEED_RSS_092;
      else if (xmlStrcmp (version, (xmlChar *)"2.0") == 0)
        return FEED_RSS_20;
    }

  /* Check for variants of RDF Site Summary feeds */
  if (xmlStrcasecmp (root->name, (xmlChar *)"rdf:RDF"))
    {
      xmlNode *n = virgule_xml_find_child (root, "item");
      if (n == NULL)
        return FEED_TYPE_UNKNOWN;
      if (xmlStrcasecmp (n->ns->href, (xmlChar *)"http://my.netscape.com/rdf/simple/0.9/") == 0)
        return FEED_RDF_SITE_SUMMARY_09;
      else if (xmlStrcasecmp (n->ns->href, (xmlChar *)"http://purl.org/rss/1.0/") == 0)
        return FEED_RDF_SITE_SUMMARY_10;
      else if (xmlStrcasecmp (n->ns->href, (xmlChar *)"http://purl.org/net/rss1.1#") == 0)
        return FEED_RDF_SITE_SUMMARY_11;
    }

  return FEED_TYPE_UNKNOWN;
}


/**
 * aggregator_index_atom_10 - Returns a pointer to Apache APR array containing
 * an unsorted index of blog entries from the raw feed. Returns NULL if the
 * feed cannot be parsed or doesn't not contain enough information.
 **/
static apr_array_header_t *
aggregator_index_atom_10 (VirguleReq *vr, xmlDoc *feedbuffer)
{
  char *author = NULL;
  char *link = NULL;
  apr_array_header_t *result;
  xmlNode *tmp, *entry;
  xmlNode *root = xmlDocGetRootElement(feedbuffer);
  xmlAttr *rel, *type;

  /* Get pointer to author's name or blog title (or set to NULL) */
  tmp = virgule_xml_find_child (root, "author");
  if (tmp != NULL)
    author = virgule_xml_find_child_string (tmp, "name", NULL);
  if(author == NULL)
    author = virgule_xml_find_child_string (root, "title", NULL);

  /*
    Get pointer to blog URL or set to NULL. There will only be one link tag
    with a relation of alternate and a type of text/html; this will be the 
    link to the blog. If it is not present, there should be a single link 
    with no relation set. If neither is present, there is no blog link.
  */
  for (tmp = root->children; tmp != NULL; tmp = tmp->next)
    if (!strcmp ((char *)tmp->name, "link"))
      {
	rel = xmlHasProp (tmp, (xmlChar *)"rel");
	type = xmlHasProp (tmp, (xmlChar *)"type");
	if(rel == NULL && type == NULL)
	  {
            link = virgule_xml_get_prop (vr->r->pool, tmp, (xmlChar *)"href");
	    break;
          }
        else
	  {
	    char *r = virgule_xml_get_prop (vr->r->pool, tmp, (xmlChar *)"rel");
	    char *t = virgule_xml_get_prop (vr->r->pool, tmp, (xmlChar *)"type");
	    if( strcasecmp(r,"alternate") == 0 && ( t == NULL || strcasecmp(t,"text/html") == 0) )
	      {
	        link = virgule_xml_get_prop (vr->r->pool, tmp, (xmlChar *)"href");
	        break;
	      }
          }
      }

  /* Walk the document tree, parsing each blog entry */
  result = apr_array_make (vr->r->pool, 16, sizeof(FeedItem));
  for (entry = root->children; entry != NULL; entry = entry->next)
   if (!strcmp ((char *)entry->name, "entry"))
    {
      FeedItem *item = (FeedItem *)apr_array_push (result);
      item->content = NULL;
      item->strcontent = NULL;
      item->content_type = NULL;
      item->blogauthor = author;
      item->bloglink = link;
      item->id = virgule_xml_find_child_string (entry, "id", NULL);
      
      tmp = virgule_xml_find_child (entry, "link");
      if (tmp == NULL)
        item->link = NULL;
      else
        item->link = virgule_xml_get_prop (vr->r->pool, tmp, (xmlChar *)"href");
	
      item->title = virgule_xml_find_child (entry, "title");

      item->content = virgule_xml_find_child (entry, "content");
      item->content_type = virgule_xml_get_prop (vr->r->pool, item->content, (xmlChar *)"type");
      if (item->content == NULL)
        {
          item->content = virgule_xml_find_child (entry, "summary");
          item->content_type = virgule_xml_get_prop (vr->r->pool, item->content, (xmlChar *)"type");
        }
      
      item->post_time = virgule_rfc3339_to_time_t (vr, virgule_xml_find_child_string (entry, "published", NULL));
      if(item->post_time == -1)
	item->post_time = virgule_rfc3339_to_time_t (vr, virgule_xml_find_child_string (entry, "issued", NULL));
      if(item->post_time == -1)
	item->post_time = virgule_rfc3339_to_time_t (vr, virgule_xml_find_child_string (entry, "updated", NULL));
      
      item->update_time = virgule_rfc3339_to_time_t (vr, virgule_xml_find_child_string (entry, "updated", NULL));
      if(item->update_time == -1)
	item->update_time = virgule_rfc3339_to_time_t (vr, virgule_xml_find_child_string (entry, "modified", NULL));
    }
      
  return result;
}


/**
 * aggregator_index_rss_20 - Returns a pointer to Apache APR array containing
 * an unsorted index of blog entries from the raw feed. Returns NULL if the
 * feed cannot be parsed or does not contain enough information.
 **/
static apr_array_header_t *
aggregator_index_rss_20 (VirguleReq *vr, xmlDoc *feedbuffer)
{
  char *author = NULL;
  char *link = NULL;
  apr_array_header_t *result;
  xmlNode *tmp, *entry;
  xmlNode *root = xmlDocGetRootElement(feedbuffer);

  /* Get pointer to blog title (or set to NULL) */
  tmp = virgule_xml_find_child (root, "channel");
  if(tmp != NULL)
    author = virgule_xml_find_child_string (tmp, "creator", NULL);
  if(author == NULL)
    author = virgule_xml_find_child_string (tmp, "title", NULL);
  
  /* Get pointer to blog link (or set to NULL) */
  link = virgule_xml_find_child_string (tmp, "link", NULL);

  /* Walk the document tree, parsing each blog entry */
  result = apr_array_make (vr->r->pool, 16, sizeof(FeedItem));
  for (entry = tmp->children; entry != NULL; entry = entry->next)
   if (!strcmp ((char *)entry->name, "item"))
    {
      /* We can't use feeds that don't have a date */
      tmp = virgule_xml_find_child (entry, "pubDate");
      if (tmp == NULL) 
        tmp = virgule_xml_find_child (entry, "date");
      if (tmp == NULL)
        continue;

      FeedItem *item = (FeedItem *)apr_array_push (result);
      item->content = NULL;
      item->strcontent = NULL;
      item->content_type = NULL;
      item->blogauthor = author;
      item->bloglink = link;

      item->id = virgule_xml_find_child_string (entry, "guid", NULL);
      if(item->id == NULL)
        item->id = virgule_xml_find_child_string (entry, "link", NULL);
      if(item->id == NULL)
        item->id = virgule_xml_find_child_string (entry, "title", NULL);

      item->link = virgule_xml_find_child_string (entry, "link", NULL);
      if(item->link == NULL)
        item->link = virgule_xml_find_child_string (entry, "guid", NULL);
      
      item->title = virgule_xml_find_child (entry, "title");

      item->content = virgule_xml_find_child (entry, "encoded");
      if(item->content == NULL)
        item->content = virgule_xml_find_child (entry, "description");
        
      item->post_time = virgule_rfc822_to_time_t (vr, virgule_xml_find_child_string (entry, "pubDate", NULL));
      if(item->post_time == -1)
        item->post_time = virgule_rfc3339_to_time_t (vr, virgule_xml_find_child_string (entry, "date", NULL));
	
      item->update_time = -1;
    }

  return result;
  
}


/**
 * aggregator_index_rdf_site_summary_10 - Returns a pointer to Apache APR 
 * array containing an unsorted index of blog entries from the raw feed. 
 * Returns NULL if the feed cannot be parsed or doesn't not contain enough
 * information. This code assume the date tag dc:date from the Dublin Core
 * module will be present and that the content will be contained in the
 * content:encoded tag of the Draft version of the Content module. If the
 * content:encoded tag is not found, we'll fall back on the description tag
 **/
static apr_array_header_t *
aggregator_index_rdf_site_summary_10 (VirguleReq *vr, xmlDoc *feedbuffer)
{
  char *author = NULL;
  char *link = NULL;
  apr_array_header_t *result;
  xmlNode *tmp, *entry;
  xmlNode *root = xmlDocGetRootElement(feedbuffer);

  /* Get pointer to blog title (or set to NULL) */
  tmp = virgule_xml_find_child (root, "channel");
  author = virgule_xml_find_child_string (tmp, "title", NULL);

  /* Get pointer to blog link (or set to NULL) */
  link = virgule_xml_find_child_string (tmp, "link", NULL);

  /* Walk the document tree, parsing each blog entry */
  result = apr_array_make (vr->r->pool, 16, sizeof(FeedItem));
  for (entry = root->children; entry != NULL; entry = entry->next)
   if (!strcmp ((char *)entry->name, "item"))
    {
      /* We can't use feeds that don't have a date */
      tmp = virgule_xml_find_child (entry, "date");
      if (tmp == NULL) 
        continue;

      FeedItem *item = (FeedItem *)apr_array_push (result);
      item->content = NULL;
      item->strcontent = NULL;
      item->content_type = NULL;
      item->blogauthor = author;
      item->bloglink = link;
      item->id = virgule_xml_get_prop (vr->r->pool, entry, (xmlChar *)"about");
      item->link = virgule_xml_find_child_string (entry, "link", NULL);
      item->title = virgule_xml_find_child (entry, "title");
      item->content = virgule_xml_find_child (entry, "encoded");
      if(item->content == NULL)
          item->content = virgule_xml_find_child (entry, "description");
      item->post_time = virgule_rfc3339_to_time_t (vr, virgule_xml_find_child_string (entry, "date", NULL));
      item->update_time = -1;

    }

  return result;
}


/**
 * item_compare - qsort comparison function to sort items by the posted
 * timestamp field.
 **/
static int
item_compare (const void *i1, const void *i2)
{
  FeedItem *item1 = (FeedItem *)i1;
  FeedItem *item2 = (FeedItem *)i2;
  return item1->post_time - item2->post_time;
}


/**
 * aggregator_normalize_feed - Returns a date sorted array of pointers to the
 * normalized items in the specified feed.
 **/
static apr_array_header_t *
aggregator_normalize_feed (VirguleReq *vr, xmlDoc *feedbuffer, FeedType ft)
{
  apr_array_header_t *result = NULL;

  switch(ft)
    {
      case FEED_ATOM_03:
      case FEED_ATOM_10:
            result = aggregator_index_atom_10 (vr, feedbuffer);
            break;
      case FEED_RSS_091:
      case FEED_RSS_092:
      case FEED_RSS_20:
            result = aggregator_index_rss_20 (vr, feedbuffer);
            break;
      case FEED_RDF_SITE_SUMMARY_09:
      case FEED_RDF_SITE_SUMMARY_10:
      case FEED_RDF_SITE_SUMMARY_11:
            result = aggregator_index_rdf_site_summary_10 (vr, feedbuffer);
            break;
      default:  // unknown
            virgule_buffer_printf (vr->b,"<p><b>Warning:</b> Feed type unknown, skipping...</p>\n");
            break;
    }

  /* sort the array with oldest item first and newest last */
  if (result != NULL)
    qsort (result->elts, result->nelts, sizeof(FeedItem), item_compare);

  return result;
}


/**
 * aggregator_post_feed - open the specified user's feed buffer, and append
 * any unposted entries to the user's blog. Feed entries old than the most 
 * recent blog entry will be ignored.
 **/
static int
aggregator_post_feed (VirguleReq *vr, xmlChar *user)
{
  int i;
  int post = 0;
  char *key, *fn;
  xmlDoc *feedbuffer = NULL;
  xmlError *e;
  apr_array_header_t *item_list;
  FeedType ft = FEED_TYPE_UNKNOWN;

  /* Get the timestamp of the user's most recent blog entry */
  time_t latest = virgule_diary_latest_feed_entry (vr, user);

  /* Open and parse the feed buffer */
  key = apr_psprintf (vr->r->pool, "acct/%s/feed.xml", (char *)user);
  
  fn = virgule_db_mk_filename (vr->r->pool, vr->db, key);
  feedbuffer = xmlParseFile (fn);
  if (feedbuffer == NULL)
  {
    e = xmlGetLastError();
    virgule_buffer_printf (vr->b, "<p>feedbuffer: [%s] xml parsing error [%s]\n",
        virgule_db_mk_filename(vr->r->pool, vr->db, key),
        e->message);
    return FALSE;
  }

  /* Identify the feed type */
  ft = aggregator_identify_feed_type (vr, feedbuffer);  

  /* Get a sorted, normalized array of items in the feed */
  item_list = aggregator_normalize_feed (vr, feedbuffer, ft);

  if(item_list == NULL)
    return FALSE;

  /* Post any new, unposted entries or updated entries */
  for (i = 0; i < item_list->nelts; i++)
    {
      int e;
      FeedItem *item = &((FeedItem *)(item_list->elts))[i];

      if (item->content == NULL)
        continue;

      if (item->post_time > latest)
	{
	  item->strcontent = extract_content (vr, item);
          if(!item->strcontent || strlen(item->strcontent) == 0) {
            ap_log_rerror(APLOG_MARK,APLOG_CRIT, APR_SUCCESS, vr->r,"mod_virgule aggregator.c: extract_content(): Failed for item [%s]: %s", (item->content_type ? item->content_type : "unknown"), item->id);
            continue;
          }	  
	  e = virgule_diary_entry_id_exists(vr, user, item->id);
	  /* some broken feeds alter post time on existing posts, check ID */
	  if (e > -1)
	    virgule_diary_update_feed_item (vr, user, item, e);
	  /* it should be safe to assume this is really a new entry */
	  else
	    {
	      virgule_diary_store_feed_item (vr, user, item);
	      post = 1;
	    }
	}
      else if ((item->update_time != -1) && (item->post_time != item->update_time))
	{
	  /* some broken feeds alter post time on existing posts, check ID */
	  item->strcontent = extract_content (vr, item);
          if(!item->strcontent || strlen(item->strcontent) == 0) {
            ap_log_rerror(APLOG_MARK,APLOG_CRIT, APR_SUCCESS, vr->r,"mod_virgule aggregator.c: extract_content(): Failed for item [%s]: %s", (item->content_type ? item->content_type : "unknown"), item->id);
            continue;
          }
	  e = virgule_diary_entry_id_exists(vr, user, item->id);
	  virgule_diary_update_feed_item (vr, user, item, e);
	}
	
      /* Free content tree and title to recover some space */
      xmlFreeNode(item->content);
      xmlFreeNode(item->title);
    }

  /* Post only one recentlog entry even if we get multiple new posts */
  if (post == 1)
    virgule_add_recent (vr->r->pool, vr->db, "recent/diary.xml", (char *)user,
			100, vr->priv->recentlog_as_posted);

  return TRUE;
}


/**
 * extract_content - attempt to identify the content type of the feed item
 * and convert it to a string. Types that we should be able to handle:
 * text/html - Raw HTML encoded as CDATA or XML escaped HTML
 * html - Raw HTML encoded as CDATA or XML escaped HTML
 * xhtml - dump a valid xml tree to a text buffer
 * If type is NULL, assume html, this should be right 99.9% of the time.
 *  ^^ is that safe? what if type NULL contains text with no HTML markup?
 *     will HTML normalization wipe out the text or wrap it?
 **/
char *
extract_content (VirguleReq *vr, FeedItem *i)
{
    char *content = NULL;
    char *str = NULL;

    if(i->content_type == NULL)
      {
	content = virgule_xml_get_all_string_contents (vr->r->pool, i->content);
	if(content)
	    str = virgule_normalize_html (vr, content, i->bloglink);
      }

    else if(!strcasecmp (i->content_type, "html") || !strcasecmp (i->content_type, "text/html"))
      {
	content = virgule_xml_get_all_string_contents (vr->r->pool, i->content);
	if(content)
	    str = virgule_normalize_html (vr, content, i->bloglink);
      }

    else if(!strcasecmp (i->content_type, "xhtml"))
      {
	str = virgule_normalize_html_tree (vr, i->content, i->bloglink);
      }
      
    return str;
}


/**
 * aggregator_getfeed_serve - Open the feedlist, and attempt to retrieve
 * the feed for each entry. If successful, each user's feed buffer will
 * be updated.
 *
 * NOTE: xmlNanoHTTPFetch has a hard-coded timeout of 60 seconds. If the
 * feed list is large and several feeds timeout, it's very likely the
 * browser or agent that triggered the aggregator will timeout itself
 * before the final report is completed and sent. This won't prevent
 * the aggregator from successfully updating the feeds. The long-term fix
 * for this is to break out the aggregator as a separate daemon unrelated
 * to mod_virgule or apache.
 **/
static int
aggregator_getfeeds_serve(VirguleReq *vr)
{
  apr_pool_t *tp = NULL;
  apr_pool_t *op = vr->r->pool;
  xmlDoc *agglist;
  xmlNode *feed;
  xmlChar *user;
  char *feedbuffer, *feedurl, *contentType;
  int feed_status = 0;
  
  agglist = virgule_db_xml_get (vr->r->pool, vr->db, "feedlist");
  if (agglist == NULL)
    return FALSE;

  if (virgule_set_temp_buffer (vr) != 0)
    return HTTP_INTERNAL_SERVER_ERROR;

  xmlNanoHTTPInit();
    
  for (feed = agglist->xmlRootNode->children; feed != NULL; feed = feed->next)
    {
        if (tp != NULL)
	    apr_pool_destroy (tp);    
	apr_pool_create (&tp, op);
	vr->r->pool = tp;
	
        user = (xmlChar *)virgule_xml_get_prop (vr->r->pool, feed, (xmlChar *)"user");
        feedurl = virgule_xml_get_prop (vr->r->pool, feed, (xmlChar *)"feedurl");
        feedbuffer = apr_psprintf (vr->r->pool, "/acct/%s/feed.xml", (char *)user);
        virgule_db_del (vr->db, feedbuffer);
	feedbuffer = virgule_db_mk_filename (vr->r->pool, vr->db, feedbuffer);
	feed_status = xmlNanoHTTPFetch(feedurl, feedbuffer, &contentType);
        virgule_buffer_printf (vr->b,
	                       "<p><b>User:</b> %s <b>FeedURL:</b> %s <b>ContentType:</b> %s <b>Retrieval:</b> %s</p>\n",
			       user,feedurl,contentType,
			       (feed_status ? "Error": "Ok"));

	ap_log_rerror(APLOG_MARK,APLOG_CRIT,APR_SUCCESS, vr->r,"mod_virgule: aggregator: %s (%s) - %s", user, feedurl, (feed_status ? "Error": "Ok"));

	if(feed_status == 0)
	  aggregator_post_feed (vr, user);
    }

  xmlNanoHTTPCleanup();

  virgule_set_main_buffer (vr);
  return virgule_render_in_template (vr, "/templates/default.xml", "content", "Feedlist Aggregation Results");
}


/**
 * virgule_update_aggregator_list - check the current user's syndication
 * flag. If it's on, make sure they're on the aggregation list. If it's off,
 * make sure they're not on the aggregation list
 *
 * Returns FALSE on any error, TRUE on successful update.
 **/
int
virgule_update_aggregator_list (VirguleReq *vr)
{
  char *db_key = NULL;
  char *syndicate = NULL;
  char *feedurl = NULL;
  int sflag = FALSE;
  int fflag = FALSE;
  xmlDoc *profile;
  xmlDoc *agglist;
  xmlNode *aggregate, *feed;

  /* Determine if this user needs to be in or out of the feedlist */
  if (vr->u == NULL) 
    return FALSE;

  db_key = virgule_acct_dbkey (vr, vr->u);
  if (db_key == NULL)
    return FALSE;
    
  profile = virgule_db_xml_get (vr->r->pool, vr->db, db_key);
  if (profile == NULL)
    return FALSE;

  aggregate = virgule_xml_find_child (profile->xmlRootNode, "aggregate");
  if (aggregate == NULL)
    return FALSE;

  syndicate = virgule_xml_get_prop (vr->r->pool, aggregate, (xmlChar *)"syndicate");
  if(syndicate == NULL)
    return FALSE;

  if (!strcmp (syndicate, "on"))
    {
      /* normalize feed url to make sure it has http:// */
      char *tmpurl = virgule_xml_get_prop (vr->r->pool, aggregate, (xmlChar *)"feedurl");
      if (tmpurl)
	{
	  char *colon = strchr (tmpurl, ':');
	  if (!colon || colon[1] != '/' || colon[2] != '/')
	    feedurl = apr_pstrcat (vr->r->pool, "http://", tmpurl, NULL);
	  else
	    feedurl = tmpurl;
	}
      sflag = TRUE;
    }
    
  /* Determine if this user is current in or out of the feedlist */
  agglist = virgule_db_xml_get (vr->r->pool, vr->db, "feedlist");
  if (agglist == NULL)
    {
      agglist = virgule_db_xml_doc_new (vr->r->pool);
      agglist->xmlRootNode = xmlNewDocNode (agglist, NULL, (xmlChar *)"feedlist", NULL);
    }

  if (agglist == NULL)
    return FALSE;

  for (feed = agglist->xmlRootNode->children; feed != NULL; feed = feed->next)
    {
        if(!strcmp (vr->u, virgule_xml_get_prop (vr->r->pool, feed, (xmlChar *)"user")))
	  {
	    fflag = TRUE;
	    xmlUnlinkNode (feed);
	    xmlFreeNode (feed);
	    break;
	  }
    }
    
  if(sflag == TRUE)
    {
      feed = xmlNewTextChild (agglist->xmlRootNode, NULL, (xmlChar *)"feed", NULL);
      if (feed == NULL)
        return FALSE;
      xmlSetProp (feed, (xmlChar *)"user", (xmlChar *)vr->u);
      xmlSetProp (feed, (xmlChar *)"feedurl", (xmlChar *)feedurl);
    }
      
  /* Write the updated feed list */
  if(virgule_db_xml_put (vr->r->pool, vr->db, "feedlist", agglist) != 0)
    return FALSE;
  
  return TRUE;
}


/**
 * virgule_aggregator_serve - handle hits on /aggregator/getfeeds.html
 **/
int
virgule_aggregator_serve (VirguleReq *vr)
{
  if (!strcmp (vr->uri, "/admin/crank-aggregator.html"))
    return aggregator_getfeeds_serve (vr);
  return DECLINED;
}
