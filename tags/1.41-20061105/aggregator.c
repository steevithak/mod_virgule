#include <apr.h>
#include <apr_strings.h>
#include <httpd.h>

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
#include "diary.h"
#include "acct_maint.h"
#include "aggregator.h"


/**
 * aggregator_post_feed - open the specified user's feed buffer, and append
 * any unposted entries to the user's blog. Feed entries old than the most 
 * recent blog entry will be ignored.
 **/
static int
aggregator_post_feed (VirguleReq *vr, xmlChar *user)
{
  char *key;
  xmlDoc *feedbuffer;

  /* Get the timestamp of the user's most recent blog entry */
  time_t latest = virgule_diary_latest_entry (vr, user);

  /* Open and parse the feed buffer */
//  key = apr_psprintf (vr->r->pool, "acct/%s/feed.xml", (char *)user);
//  feedbuffer = virgule_db_xml_get (vr->r->pool, vr->db, key);

  // load/parse feed into xml doc
  // determine feed type
}


/**
 * aggregator_getfeed_serve - Open the feedlist, and attempt to retrieve
 * the feed for each entry. If successful, each user's feed buffer will
 * be updated.
 **/
static int
aggregator_getfeeds_serve(VirguleReq *vr)
{
  xmlDoc *agglist;
  xmlNode *feed;
  xmlChar *user, *feedurl;
  char *feedbuffer;
  int status;
  
  virgule_render_header (vr, "mod_virgule Aggregator", NULL);

  agglist = virgule_db_xml_get (vr->r->pool, vr->db, "feedlist");
  if (agglist == NULL)
    return FALSE;

  virgule_buffer_puts (vr->b, "<h3>Processing feed list</h3>");

  xmlNanoHTTPInit();
    
  for (feed = agglist->xmlRootNode->children; feed != NULL; feed = feed->next)
    {
        user = virgule_xml_get_prop (vr->r->pool, feed, (xmlChar *)"user");
        feedurl = virgule_xml_get_prop (vr->r->pool, feed, (xmlChar *)"feedurl");
        feedbuffer = apr_psprintf (vr->r->pool, "/acct/%s/feed.xml", (char *)user);
	feedbuffer = virgule_db_mk_filename (vr->r->pool, vr->db, feedbuffer);
	status = xmlNanoHTTPFetch(feedurl, feedbuffer, NULL);	
        virgule_buffer_printf (vr->b, "<b>User:</b> %s <b>FeedURL:</b> %s <b>Result:</b> %s<br />\n",user,feedurl, (status ? "Error": "Ok"));
	if(status == 0)
	  aggregator_post_feed (vr, user);
    }
    xmlNanoHTTPCleanup();

  virgule_buffer_puts (vr->b, "<p>Processed feeds!</p>\n");
  return virgule_render_footer_send (vr);
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

  if (!xmlStrcmp (syndicate, (xmlChar *)"on"))
    {
      feedurl = virgule_xml_get_prop (vr->r->pool, aggregate, (xmlChar *)"feedurl");
      sflag = TRUE;
    }
    
  /* Determine if this user is current in or out of the feedlist */
  agglist = virgule_db_xml_get (vr->r->pool, vr->db, "feedlist");
  if (agglist == NULL)
    {
      agglist = virgule_db_xml_doc_new (vr->r->pool);
      agglist->xmlRootNode = xmlNewDocNode (agglist, NULL, "feedlist", NULL);
    }

  if (agglist == NULL)
    return FALSE;

  for (feed = agglist->xmlRootNode->children; feed != NULL; feed = feed->next)
    {
        if(!xmlStrcmp (vr->u, virgule_xml_get_prop (vr->r->pool, feed, (xmlChar *)"user")))
	  {
	    fflag = TRUE;
	    xmlUnlinkNode (feed);
	    xmlFreeNode (feed);
	    break;
	  }
    }
    
  if(sflag == TRUE)
    {
      feed = xmlNewTextChild (agglist->xmlRootNode, NULL, "feed", NULL);
      if (feed == NULL)
        return FALSE;
      xmlSetProp (feed, "user", vr->u);
      xmlSetProp (feed, "feedurl", feedurl);
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
  if (!strcmp (vr->uri, "/aggregator/getfeeds.html"))
    return aggregator_getfeeds_serve (vr);
  return DECLINED;
}
