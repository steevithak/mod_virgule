/* A module for simple account maintenance. */

#include <time.h>
#include <ctype.h>

#include <apr.h>
#include <apr_hash.h>
#include <apr_strings.h>
#include <httpd.h>
#include <http_log.h>

#include <libxml/tree.h>
#include <libxml/parser.h>
#include <libxml/xmlmemory.h>

#include "private.h"
#include "buffer.h"
#include "db.h"
#include "req.h"
#include "apache_util.h"
#include "util.h"
#include "db_xml.h"
#include "style.h"
#include "auth.h"
#include "xml_util.h"
#include "certs.h"
#include "aggregator.h"
#include "db_ops.h"
#include "proj.h"
#include "rating.h"
#include "hashtable.h"
#include "eigen.h"
#include "diary.h"
#include "site.h"
#include "foaf.h"
#include "acct_maint.h"

typedef struct _ProfileField ProfileField;

struct _ProfileField {
  char *description;
  char *attr_name;
  int size;
  int flags;
};

typedef enum {
  PROFILE_PUBLIC    = 1 << 0,
  PROFILE_TEXTAREA  = 1 << 1,
  PROFILE_BOOLEAN   = 1 << 2,
  PROFILE_SYNDICATE = 1 << 3
} ProfileFlags;

ProfileField prof_fields[] = {
  { "Given (first) name", "givenname", 40, PROFILE_PUBLIC },
  { "Surname (last name)", "surname", 40, PROFILE_PUBLIC },
  { "Surname first?", "snf", 40, PROFILE_PUBLIC | PROFILE_BOOLEAN },
  { "Email", "email", 40, 0 },
  { "Homepage URL", "url", 40, PROFILE_PUBLIC },
  { "External FOAF URI", "foafuri", 40, 0 },
  { "Number of old messages to display", "numold", 4, 0 },
  { "Notes", "notes", 60015, PROFILE_PUBLIC | PROFILE_TEXTAREA },
  { "Syndicate your blog from another site?", "syndicate", 40, PROFILE_PUBLIC | PROFILE_BOOLEAN | PROFILE_SYNDICATE },
  { "RSS or ATOM feed URL of your blog", "feedurl", 60, PROFILE_PUBLIC | PROFILE_SYNDICATE },
  { NULL }
};

typedef struct _NodeInfo NodeInfo;

struct _NodeInfo {
  const char *name;
  const char *givenname;
  const char *surname;
};


/**
 * acct_kill: Remove a user account. Before an account is removed, all cert
 * and cert-in records are cleared. An account or account-alias is accepted 
 * as an argument. Account profile, alias (if any), certs, staff records, 
 * diary entries, eigen data, and any entries in recent lists  will be 
 * removed. Deletion is not reversible!
 **/
static int
acct_kill(VirguleReq *vr, const char *u)
{
  int n;
  const char *user = NULL;
  char *db_key, *db_key2, *user_alias, *diary;
  apr_pool_t *p = vr->r->pool;
  xmlDoc *profile, *staff, *entry, *agglist;
  xmlNode *tree, *cert, *alias, *feed;

  db_key = virgule_acct_dbkey(vr, u);
  profile = virgule_db_xml_get(p, vr->db, db_key);
  
  if (profile == NULL)
    return FALSE;
  
  alias = virgule_xml_find_child (profile->xmlRootNode, "alias");

  if (alias != NULL) /* If this is the alias, get username and kill profile */
    {
      user = virgule_xml_get_prop (p, alias, (xmlChar *)"link");
      virgule_db_xml_free (p, profile);
      virgule_db_del (vr->db, db_key);
      db_key = virgule_acct_dbkey (vr, user);
    }
  else               /* If this is the username, check for lc alias */
    {
      user = u;
      user_alias = apr_pstrdup (p, u);
      ap_str_tolower (user_alias);
      if (! (strcmp (user_alias,u) == 0))
        {
          db_key2 = virgule_acct_dbkey (vr, user_alias);
          if (db_key2 != NULL)
            virgule_db_del (vr->db, db_key2);
	}
    }
    
  /* Clear cert records */
  tree = virgule_xml_find_child (profile->xmlRootNode, "certs");
  if (tree)
    {
      xmlChar *subject, *level;
      for (cert = tree->children; cert != NULL; cert = cert->next)
        if (cert->type == XML_ELEMENT_NODE && ! xmlStrcmp (cert->name, (xmlChar *)"cert"))
	  {
            subject = xmlGetProp (cert, (xmlChar *)"subj");
	    level = xmlGetProp (cert, (xmlChar *)"level");
	    virgule_cert_set (vr, (char *)user, (char *)subject, CERT_LEVEL_NONE);
	    xmlFree(subject);
	    xmlFree(level);
	  }
    }  

  /* Clear cert-in records */
  tree = virgule_xml_find_child (profile->xmlRootNode, "certs-in");
  if (tree)
    {
      xmlChar *issuer, *level;
      for (cert = tree->children; cert != NULL; cert = cert->next)
        if (cert->type == XML_ELEMENT_NODE && ! xmlStrcmp (cert->name, (xmlChar *)"cert"))
	  {
	    issuer = xmlGetProp (cert, (xmlChar *)"issuer");
	    level = xmlGetProp (cert, (xmlChar *)"level");
	    virgule_cert_set (vr, (char *)issuer, user, CERT_LEVEL_NONE);
	    xmlFree(issuer);
	    xmlFree(level);
	  }
    }

  /* Clear staff records */
  db_key2 = apr_psprintf (p, "acct/%s/staff-person.xml", user);
  staff = virgule_db_xml_get (p, vr->db, db_key2);
  if (staff != NULL)
    {
      for (tree = staff->xmlRootNode->children; tree != NULL; tree = tree->next)
        {
	  char *name;
	  name = virgule_xml_get_prop (p, tree, (xmlChar *)"name");
	  virgule_proj_set_relation(vr,name,user,"None");
	}
      virgule_db_xml_free (p, staff);
      virgule_db_del (vr->db, db_key2);
    }

  /* Clear diary entries */
  diary = apr_psprintf(p, "acct/%s/diary", user);
  for (n = virgule_db_dir_max (vr->db, diary); n >= 0; n--)
    {
      db_key2 = apr_psprintf (p, "acct/%s/diary/_%d", user, n);
      entry = virgule_db_xml_get (p, vr->db, db_key2);
      if (entry != NULL)
        {
	  virgule_db_del (vr->db, db_key2);
          virgule_db_xml_free (p, entry);
	}
    }

  /* <articlepointers>, <auth>, and <info> tags don't need attention */

  /* Remove diary backup, if any */
  diary = apr_psprintf (p, "acct/%s/diarybackup", user);
  virgule_db_del (vr->db, diary);

  /* Remove article index, if any */
  db_key2 = apr_psprintf (p, "acct/%s/articles.xml", user);
  virgule_db_del (vr->db, db_key2);

  /* Remove user from recent lists (if present) */
  virgule_remove_recent (vr, "recent/acct.xml", user);
  virgule_remove_recent (vr, "recent/diary.xml", user);

  /* Remove eigen data (if any) */
  virgule_eigen_cleanup (vr, user);

  /* Remove the profile and account */
  virgule_db_del (vr->db, db_key);
  virgule_db_xml_free(p, profile);

  /* Remove blog feed buffer, if any */
  db_key2 = apr_psprintf (p, "acct/%s/feed.xml", user);
  virgule_db_del (vr->db, db_key2);

  /* Remove from feedlist */
  agglist = virgule_db_xml_get (vr->r->pool, vr->db, "feedlist");
  if (agglist != NULL)
    for (feed = agglist->xmlRootNode->children; feed != NULL; feed = feed->next)
    {
        if(!strcmp (user, virgule_xml_get_prop (vr->r->pool, feed, (xmlChar *)"user")))
	  {
	    xmlUnlinkNode (feed);
	    xmlFreeNode (feed);
	    virgule_db_xml_put (vr->r->pool, vr->db, "feedlist", agglist);
	    break;
	  }
    }

  return TRUE;
}


/**
 * virgule_acct_flag_as_spam: Flag the passed user's account as spam. A tag
 * will be added with the flagging user's name and a score of 1, 2, or 3 
 * depending on flagger's cert level. If a flag with the current users name
 * already exist, no action will be taken (you can only flag an account as
 * spam once. After adding the tag, the total spam score will be evaluated.
 * If the spam score exceeds the configured threshold, the account is killed.
 **/
static int
acct_flag_as_spam(VirguleReq *vr, const char *u)
{
  int score = 0;
  int flagged = FALSE;
  char *db_key;
  xmlDoc *profile;
  xmlChar *scorestr;
  xmlNode *spamtree;
  xmlNode *flag;
  xmlNode *spamscore;
  virgule_auth_user (vr);

  if(vr->u == NULL)
    return virgule_send_error_page (vr, vERROR, "account", "You must be logged in to use the spam reporting system.\n");

  if(u == NULL)
    return virgule_send_error_page (vr, vERROR, "account", "The specified user account was not found.\n");

  if (strcmp (virgule_req_get_tmetric_level (vr, u),
	      virgule_cert_level_to_name (vr, CERT_LEVEL_NONE)) != 0)
    return virgule_send_error_page (vr, vERROR, "account", "Only untrusted observer accounts may be reported as spam.\n");

  db_key = virgule_acct_dbkey (vr, u);
  profile = virgule_db_xml_get (vr->r->pool, vr->db, db_key);
  if (profile == NULL)
    return virgule_send_error_page (vr, vERROR, "account", "The specified user account was not found.\n");

  spamtree = virgule_xml_ensure_child (profile->xmlRootNode, "spamflags");

  /* search for an existing flag from vr->u while tallying the spam score */
  for (flag = spamtree->children; flag != NULL; flag = flag->next)
    {
      if (flag->type == XML_ELEMENT_NODE && !xmlStrcmp (flag->name, (xmlChar *)"flag"))
        {
	  char *issuer = (char *)xmlGetProp (flag, (xmlChar *)"issuer");
	  if(issuer)
	    {
	      score += virgule_cert_level_from_name (vr, 
	    		virgule_req_get_tmetric_level (vr, issuer));
	      if(!strcmp (issuer, vr->u))
	        flagged = TRUE;
	      xmlFree (issuer);
	    }
	}
    }

  if (flagged)
    return virgule_send_error_page (vr, vERROR, "spam flag", "You have already flagged this account as spam.\n");

  /* add a new spam flag and increment score */
  flag = xmlNewChild (spamtree, NULL, (xmlChar *)"flag", NULL);
  xmlSetProp (flag, (xmlChar *)"issuer", (xmlChar *)vr->u);

  score += virgule_cert_level_from_name (vr, virgule_req_get_tmetric_level (vr, vr->u));
  scorestr = (xmlChar *)apr_itoa (vr->r->pool, score);
  spamscore = virgule_xml_find_child(profile->xmlRootNode, "spamscore");
  if(spamscore == NULL)
    spamscore = xmlNewChild (profile->xmlRootNode, NULL, (xmlChar *)"spamscore", scorestr);
  else
    xmlNodeSetContent (spamscore, scorestr);
  
  virgule_db_xml_put (vr->r->pool, vr->db, db_key, profile);

  if(score > vr->priv->acct_spam_threshold)
    {
      acct_kill(vr, u);
      return virgule_send_error_page (vr, vINFO, "account deleted", "<blockquote><p>User account [%s] has been deleted.</p><p>Your spam report pushed the total spam score for this account to [%i].</p><p>Accounts are automatically deleted if their spam score exceeds the currently configured score threshold of [%i].</p><p>Thanks for taking the time to help keep %s free of spam!</p><blockquote>\n", u, score, vr->priv->acct_spam_threshold, vr->priv->site_name);
    }
  else 
    return virgule_send_error_page (vr, vINFO, "account flagged", "<blockquote><p>User account [%s] has been flagged as spam.</p><p>Total spam score for this account is [%i].</p><p>Accounts are automatically deleted if their spam score exceeds the currently configured score threshold of [%i].</p><p>Thanks for taking the time to help keep %s free of spam!</p><blockquote>\n", u, score, vr->priv->acct_spam_threshold, vr->priv->site_name);
}


/* update an arbitrary pointer */
int
virgule_acct_set_lastread(VirguleReq *vr, const char *section, const char *location, int last_read)
{
  apr_pool_t *p = vr->r->pool;
  Db *db = vr->db;
  char *db_key;
  xmlDoc *profile;
  xmlNode *tree, *msgptr;
  int status;

  virgule_auth_user(vr);
  if (vr->u == NULL)
    return 0;

  db_key = virgule_acct_dbkey (vr, vr->u);
  profile = virgule_db_xml_get (p, db, db_key);
  if (profile == NULL)
    return -1;

  tree = virgule_xml_ensure_child (profile->xmlRootNode, apr_psprintf(p, "%spointers", section));

  for (msgptr = tree->children; msgptr != NULL; msgptr = msgptr->next)
    {
      if (msgptr->type == XML_ELEMENT_NODE &&
	  !xmlStrcmp (msgptr->name, (xmlChar *)"lastread"))
	{
	  char *old_msgptr = (char *)xmlGetProp (msgptr, (xmlChar *)"location");
	      
	  if (old_msgptr)
	    {
	      if (!strcmp (old_msgptr, location))
		{
		  xmlFree (old_msgptr);
		  break;
		}
	      xmlFree (old_msgptr);
	    }
	}
    }
  if (msgptr == NULL)
    {
      msgptr = xmlNewChild (tree, NULL, (xmlChar *)"lastread", NULL);
      xmlSetProp (msgptr, (xmlChar *)"location", (xmlChar *)location);
    }

  xmlSetProp (msgptr, (xmlChar *)"num", (xmlChar *)apr_psprintf(p, "%d", last_read));
  xmlSetProp (msgptr, (xmlChar *)"date", (xmlChar *)virgule_iso_now(p));

  status = virgule_db_xml_put (p, db, db_key, profile);
  virgule_db_xml_free (p, profile);

  return status;
}

int
virgule_acct_get_lastread(VirguleReq *vr, const char *section, const char *location)
{
  apr_pool_t *p = vr->r->pool;
  Db *db = vr->db;
  char *db_key;
  xmlDoc *profile;
  xmlNode *tree, *msgptr;

  virgule_auth_user(vr);
  if (vr->u == NULL)
    return -1;

  db_key = virgule_acct_dbkey (vr, vr->u);
  profile = virgule_db_xml_get (p, db, db_key);
  if (profile == NULL)
    return -1;

  tree = virgule_xml_find_child (profile->xmlRootNode, apr_psprintf(p, "%spointers", section));
  if (tree == NULL)
    return -1;

  for (msgptr = tree->children; msgptr != NULL; msgptr = msgptr->next)
    {
      if (msgptr->type == XML_ELEMENT_NODE &&
	  !xmlStrcmp (msgptr->name, (xmlChar *)"lastread"))
	{
	  char *old_msgptr = (char *)xmlGetProp (msgptr, (xmlChar *)"location");
	      
	  if (old_msgptr)
	    {
	      if (!strcmp (old_msgptr, location))
		return atoi ((char *)xmlGetProp (msgptr, (xmlChar *)"num"));
	    }
	}
    }

  return -1;
}

int
virgule_acct_get_num_old(VirguleReq *vr)
{
  virgule_auth_user(vr);

  if (vr->u)
    {
      apr_pool_t *p = vr->r->pool;
      char *db_key;
      xmlDoc *profile;
      xmlNode *tree;
      char *num_old;

      db_key = virgule_acct_dbkey (vr, vr->u);
      profile = virgule_db_xml_get (p, vr->db, db_key);
      tree = virgule_xml_find_child (profile->xmlRootNode, "info");

      num_old = (char *)xmlGetProp (tree, (xmlChar *)"numold");

      if (num_old == NULL || *num_old == '\0')
	return 30;
      else
	return atoi(num_old);
    }

  return -1;
}

char *
virgule_acct_get_lastread_date(VirguleReq *vr, const char *section, const char *location)
{
  apr_pool_t *p = vr->r->pool;
  Db *db = vr->db;
  char *db_key;
  xmlDoc *profile;
  xmlNode *tree, *msgptr;
  char *date;


  virgule_auth_user(vr);
  if (vr->u == NULL)
    return NULL;

  db_key = virgule_acct_dbkey (vr, vr->u);
  profile = virgule_db_xml_get (p, db, db_key);
  if (profile == NULL)
    return NULL;

  tree = virgule_xml_find_child (profile->xmlRootNode, apr_psprintf(p, "%spointers", section));
  if (tree == NULL)
    return NULL;

  for (msgptr = tree->children; msgptr != NULL; msgptr = msgptr->next)
    {
      if (msgptr->type == XML_ELEMENT_NODE &&
	  !xmlStrcmp (msgptr->name, (xmlChar *)"lastread"))
	{
	  char *old_msgptr = (char *)xmlGetProp (msgptr, (xmlChar *)"location");
	      
	  if (old_msgptr)
	    {
	      if (!strcmp (old_msgptr, location))
		{ 
		  date = (char *)xmlGetProp (msgptr, (xmlChar *)"date");
		  if (date == NULL)
		    date = "1970-01-01 00:00:00";
		  virgule_db_xml_free (p, profile);
		  return date;
		}
	    }
	}
    }

  virgule_db_xml_free (p, profile);
  return "1970-01-01 00:00:00"; 
}

/**
 * validate_username: Ensure that username is valid.
 * @u: Putative username.
 *
 * Return value: NULL if valid, or reason as string if not.
 **/
char *
virgule_validate_username (VirguleReq *vr, const char *u)
{
  int len;
  int i;

  if (u == NULL || !u[0])
    return "You must specify a username.";

  len = strlen (u);
  if (len > 20)
    return "The username must be 20 characters or less.";

  if (vr->priv->allow_account_extendedcharset)
    {
      if (!isalnum(u[0]))
        return "First character must be alphanumeric.";
  
      for (i = 0; i < len; i++)
        {
          if (!isalnum (u[i]) && u[i]!='-' && u[i]!='_' && u[i]!=' ' && u[i]!= '.' )
	    return "The username must contain only alphanumeric, dash, underscore, space, or dot characters.";
        }	
    }

  else
    {
      for (i = 0; i < len; i++)
        {
	  if (!isalnum (u[i]))
	    return "The username must contain only alphanumeric characters.";
	}
    }

  return NULL;
}


/* Make the db key. Sanity check the username. */
char *
virgule_acct_dbkey (VirguleReq *vr, const char *u)
{
  if (virgule_validate_username (vr, u) != NULL)
    return NULL;

  return apr_pstrcat (vr->r->pool, "acct/", u, "/profile.xml", NULL);
}


static void
acct_set_cookie (VirguleReq *vr, const char *u, const char *cookie,
		 time_t lifetime)
{
  request_rec *r = vr->r;
  char *id_cookie, *exp_date;
  time_t exp_time;

  id_cookie = apr_pstrcat (r->pool, u, ":", cookie, NULL);
  exp_time = time (NULL) + lifetime;
  exp_date = ap_ht_time (r->pool, (apr_time_t)exp_time * 1000000,
                         "%A, %d-%b-%Y %H:%M:%S %Z", 1);

  apr_table_add (r->headers_out, "Set-Cookie",
		apr_psprintf (r->pool, "id=%s; path=/; Expires=%s",
			     id_cookie, exp_date));
}


static int
acct_index_serve (VirguleReq *vr)
{
  apr_pool_t *p = vr->r->pool;
  int i;

  virgule_auth_user (vr);

  if (vr->u)
    {
      Buffer *b;
      const char *level;
      char *db_key;
      xmlDoc *profile;
      xmlNode *info, *aggregate, *tree;
      char *value;

      db_key = virgule_acct_dbkey (vr, vr->u);
      profile = virgule_db_xml_get (p, vr->db, db_key);
      info = virgule_xml_find_child (profile->xmlRootNode, "info");
      aggregate = virgule_xml_find_child (profile->xmlRootNode, "aggregate");
      level = virgule_req_get_tmetric_level (vr, vr->u);

      if (virgule_set_temp_buffer (vr) != 0)
        return HTTP_INTERNAL_SERVER_ERROR;

      b = vr->b;

      virgule_buffer_printf (b, "<p>Welcome, <em>%s</em>. The range of functions you "
		     "can access depends on your <a href=\"%s/certs.html\">certification</a> "
		     "level. Remember to certify any other users of this site that you "
		     "know. The trust metric system relies on user participation!</p>\n<p>", 
		     vr->u, vr->prefix);

      virgule_render_cert_level_begin (vr, vr->u, CERT_STYLE_SMALL);
      virgule_buffer_printf (b, "You are currently certified at the %s level by the other users of this site.", level);
      virgule_render_cert_level_end (vr, CERT_STYLE_SMALL);
      virgule_buffer_puts (b, "</p><p>At this level you can:</p>\n<ul>");

      virgule_buffer_printf (b, "<li>Link to your <a href=\"%s/person/%s\">public profile</a></li>",vr->prefix,ap_escape_uri(vr->r->pool,vr->u));
      virgule_buffer_printf (b, "<li><a href=\"%s/diary/\">Post a blog entry</a></li>\n",vr->prefix);

      if (virgule_req_ok_to_reply (vr))
        {
	  virgule_buffer_puts (b, "<li>Post replies to articles</li>\n");
	}

      if (virgule_req_ok_to_create_project (vr))
        {
	  virgule_buffer_printf (b, "<li><a href=\"%s/proj/new.html\">Create new a project</a></li>\n",vr->prefix);
	  virgule_buffer_printf (b, "<li>Edit your <a href=\"%s/proj/\">existing projects</a></li>\n",vr->prefix);
	}

      if (virgule_req_ok_to_post (vr))
        {
	  virgule_buffer_printf (b, "<li><a href=\"%s/article/post.html\">Post an article</a></li>\n",vr->prefix);
	}

      if (virgule_req_ok_to_syndicate_blog (vr))
        {
	  virgule_buffer_printf (b, "<li>Syndicate your blog from an external site</li>\n");
	}
	
      virgule_buffer_puts (b, "<li><a href=\"logout.html\">Logout</a></li>\n");
      if (vr->priv->projstyle == PROJSTYLE_NICK)
        virgule_buffer_puts (b, "<li><a href=\"/proj/updatepointers.html\">Mark all messags as read</a></li>\n");
      virgule_buffer_puts (b, "</ul><p> Or you can update your account info: </p>\n");
      virgule_buffer_puts (b, "<form method=\"POST\" action=\"update.html\" accept-charset=\"UTF-8\">\n");
      for (i = 0; prof_fields[i].description; i++)
	{
	  if (prof_fields[i].flags & PROFILE_SYNDICATE && 
	      !virgule_req_ok_to_syndicate_blog (vr))
	    continue;
	    
	  if (vr->priv->projstyle != PROJSTYLE_NICK &&
	      !strcmp(prof_fields[i].attr_name, "numold"))
	    continue;

	  if (prof_fields[i].flags & PROFILE_SYNDICATE)
	    tree = aggregate;
	  else
	    tree = info;

	  value = NULL;
	  if (tree)
	    value = (char *)xmlGetProp (tree, (xmlChar *)prof_fields[i].attr_name);

	  virgule_buffer_printf (b, "<p> %s: <br>\n", prof_fields[i].description);
	  if (prof_fields[i].flags & PROFILE_BOOLEAN)
	    virgule_buffer_printf (b, "<input name=\"%s\" type=\"checkbox\" %s></p>\n",
			   prof_fields[i].attr_name,
			   value ? (strcmp (value, "on") ? "" : " checked") : "");
	  else if (prof_fields[i].flags & PROFILE_TEXTAREA)
	    virgule_buffer_printf (b, "<textarea name=\"%s\" cols=\"%d\" rows=\"%d\" wrap=\"hard\">%s</textarea></p>\n",
			   prof_fields[i].attr_name,
			   prof_fields[i].size / 1000,
			   prof_fields[i].size % 1000,
			   value ? ap_escape_html (p, value) : "");
	  else
	    virgule_buffer_printf (b, "<input name=\"%s\" size=\"%d\" value=\"%s\"> </p>\n",
			   prof_fields[i].attr_name, prof_fields[i].size,
			   value ? ap_escape_html (p, value) : "");
	  if (value != NULL)
	    xmlFree (value);
	}
      virgule_buffer_puts (b, " <input type=\"submit\" value=\"Update\">\n</form>\n");

      virgule_set_main_buffer (vr);
      
      return virgule_render_in_template (vr, "/templates/acct-info.xml", "content", "User Account Info");
    }
  else
    {
      return virgule_render_in_template (vr, "/templates/acct-login.xml", NULL, NULL);
    }
}


static int
acct_newsub_serve (VirguleReq *vr)
{
  request_rec *r = vr->r;
  apr_pool_t *p = r->pool;
  Db *db = vr->db;
  apr_table_t *args;
  const char *u, *pass, *pass2, *email, *subcode;
  char *db_key, *db_key_lc;
  xmlDoc *profile;
  xmlNode *root, *tree;
  int status;
  char *cookie;
  int i;
  const char *date;
  char *u_lc;

  if (!vr->priv->allow_account_creation)
    return virgule_send_error_page (vr, vERROR, "forbidden", "No new accounts may be created at this time.\n");

  args = virgule_get_args_table (vr);

  if (args == NULL)
    return virgule_send_error_page (vr, vERROR, "form data", "Invalid form submission.\n");

  u = apr_table_get (args, "u");
  pass = apr_table_get (args, "pass");
  pass2 = apr_table_get (args, "pass2");
  email = apr_table_get (args, "email");
  subcode = apr_table_get (args, "subcode");

  if (subcode != NULL && subcode[0]) 
    return virgule_send_error_page (vr, vERROR, "forbidden",
			    "Submission from a banned user agent or IP address.");

  if (u == NULL || !u[0])
    return virgule_send_error_page (vr, vERROR, "username",
			    "You must specify a username.");

  if (strlen (u) > 20)
    return virgule_send_error_page (vr, vERROR, "username",
			    "The username must be 20 characters or less.");

  /* sanity check user name */
  db_key = virgule_acct_dbkey (vr, u);
  if (db_key == NULL && vr->priv->allow_account_extendedcharset)
    return virgule_send_error_page (vr, vERROR, "username",
			    "Username must begin with an alphanumeric character and contain only alphanumerics, spaces, dashes, underscores, or periods.");
  else if (db_key == NULL)
    return virgule_send_error_page (vr, vERROR, "username",
			    "Username must contain only alphanumeric characters.");

  u_lc = apr_pstrdup (p, u);
  ap_str_tolower (u_lc);
  db_key_lc = virgule_acct_dbkey (vr, u_lc);
  profile = virgule_db_xml_get (p, db, db_key_lc);
  if (profile != NULL)
    return virgule_send_error_page (vr, vERROR,
			    "username",
			    "The user name <em>%s</em> is already in use.",
			    u);

  if (pass == NULL || !pass[0])
    return virgule_send_error_page (vr, vERROR, "password",
			    "You must specify a password.");

  if (pass == NULL || pass2 == NULL)
    return virgule_send_error_page (vr, vERROR, 
			    "password",
			    "You must specify a password and enter it twice.");

  if (!strcmp (pass, u))
    return virgule_send_error_page (vr, vERROR,
			    "password",
			    "The username may not be used as the password.");

  if (strcmp (pass, pass2))
    return virgule_send_error_page (vr, vERROR,
			    "password",
			    "The passwords must match. Have a cup of coffee and try again.");

  if (email == NULL || !email[0])
    return virgule_send_error_page (vr, vERROR, "email address",
			    "You must specify a valid email address. The email address is needed for account authentication, and password recovery purposes.");

  profile = virgule_db_xml_doc_new (p);

  root = xmlNewDocNode (profile, NULL, (xmlChar *)"profile", NULL);
  profile->xmlRootNode = root;

  date = virgule_iso_now (p);
  tree = xmlNewChild (root, NULL, (xmlChar *)"date", (xmlChar *)date);
  tree = xmlNewChild (root, NULL, (xmlChar *)"auth", NULL);
  xmlSetProp (tree, (xmlChar *)"pass", (xmlChar *)pass);
  cookie = virgule_rand_cookie (p);
#if 0
  virgule_buffer_printf (b, "Cookie is %s\n", cookie);
#endif
  xmlSetProp (tree, (xmlChar *)"cookie", (xmlChar *)cookie);

  tree = xmlNewChild (root, NULL, (xmlChar *)"info", NULL);
  for (i = 0; prof_fields[i].description; i++)
    {
      const char *val;
      val = apr_table_get (args, prof_fields[i].attr_name);
      if (val == NULL)
        continue;
      if (virgule_is_input_valid(val))
        xmlSetProp (tree, (xmlChar *)prof_fields[i].attr_name, (xmlChar *)val);
      else
        return virgule_send_error_page (vr, vERROR,
                                "invalid UTF-8",
                                "Only valid characters that use valid UTF-8 sequences may be submitted.");
    }

  status = virgule_db_xml_put (p, db, db_key, profile);
  if (status)
    return virgule_send_error_page (vr, vERROR,
			    "internal",
			    "There was an error storing the account profile.");

  acct_set_cookie (vr, u, cookie, 86400 * 365);

  vr->u = u;

  virgule_add_recent (p, db, "recent/acct.xml", u, 100, 0);

  /* store lower case alias if necessary */
  if (! (strcmp (u_lc, u) == 0))
    {
      profile = virgule_db_xml_doc_new (p);

      root = xmlNewDocNode (profile, NULL, (xmlChar *)"profile", NULL);
      profile->xmlRootNode = root;
      tree = xmlNewChild (root, NULL, (xmlChar *)"alias", NULL);
      xmlSetProp (tree, (xmlChar *)"link", (xmlChar *)u);

      status = virgule_db_xml_put (p, db, db_key_lc, profile);
    }

  return virgule_send_error_page (vr, vINFO,
			  "Account created",
			  "Account <a href=\"%s/person/%s/\">%s</a> created.\n",
			  vr->prefix, ap_escape_uri (vr->r->pool,u), u);
}

/* Success: return 1, set *ret1 to username and *ret2 to cookie
 * Failure: return 0, set *ret1 and *ret2 to short and long error messages
 * FIXME: this function's interface is _nasty_
 */
int
virgule_acct_login (VirguleReq *vr, const char *u, const char *pass,
	    const char **ret1, const char **ret2)
{
  request_rec *r = vr->r;
  apr_pool_t *p = r->pool;
  Db *db = vr->db;
  char *db_key, *stored_pass;
  xmlDoc *profile;
  xmlNode *root, *tree;
  char *cookie;
  const int n_iter_max = 10;
  int i;

  *ret1 = *ret2 = NULL;
  
  if (u == NULL || !u[0])
    {
      *ret1 = "Specify a username";
      *ret2 = "You must specify a username.";
      return 0;
    }
  if (pass == NULL || !pass[0])
    {
      *ret1 = "Specify a password";
      *ret2 = "You must specify a password.";
      return 0;
    }
  
  for (i = 0; i < n_iter_max; i++)
    {
      /* sanity check user name */
      db_key = virgule_acct_dbkey (vr, u);
      if (db_key == NULL)
        {
	  *ret1 = "Invalid username";
	  *ret2 = "Username must contain only alphanumeric characters.";
	  return 0;
        }

      profile = virgule_db_xml_get (p, db, db_key);
      if (profile == NULL)
        {
	  *ret1 = "Account does not exist";
	  *ret2 = apr_psprintf (p, "Account <em>%s</em> does not exist. Try the <a href=\"new.html\">new account creation</a> page.", u);
	  return 0;
        }
      
#if 0
      virgule_buffer_printf (b, "Profile: %s\n", profile->name);
      return virgule_buffer_send_response (r, b);
#endif

      root = profile->xmlRootNode;

      tree = virgule_xml_find_child (root, "alias");
      if (tree == NULL)
	break;

      u = virgule_xml_get_prop (p, tree, (xmlChar *)"link");
      db_key = virgule_acct_dbkey (vr, u);
      profile = virgule_db_xml_get (p, db, db_key);
    }

  if (i == n_iter_max)
    {
      *ret1 = "Alias loop";
      *ret2 = apr_psprintf (p, "More than %d levels of alias indirection from %s, indicating an alias loop. This is a problem with the server.",
			n_iter_max, u);
      return 0;
    }

  tree = virgule_xml_find_child (root, "auth");

  if (tree == NULL)
    {
      *ret1 = "Account is missing auth field";
      *ret2 = apr_psprintf (p, "Account <em>%s</em> is missing its auth field. This is a problem with the server.", u);
      return 0;
    }

  stored_pass = (char *)xmlGetProp (tree, (xmlChar *)"pass");

  if (strcmp (pass, stored_pass))
    {
      xmlFree (stored_pass);
      *ret1 = "Incorrect password";
      *ret2 = "Incorrect password, try again.";
      return 0;
    }

  xmlFree (stored_pass);
  cookie = (char *)xmlGetProp (tree, (xmlChar *)"cookie");

  *ret1 = apr_pstrdup (p, u);
  *ret2 = apr_pstrdup (p, cookie);
  xmlFree (cookie);
  return 1;
}


/*
 *  Send an email to the given user reminding them of their password
 */
static int
send_email(VirguleReq *vr, const char *mail, const char *u, const char *pass)
{
  FILE *fp;

  char cmd[1024];
  snprintf( cmd, sizeof(cmd)-1, "/usr/lib/sendmail -t -f %s", vr->priv->admin_email);

  if ((fp = popen( cmd, "w")) == NULL) 
     return virgule_send_error_page (vr, vERROR, "SMTP", "There was an error sending mail to <em>%s</em>.\n", mail);

  fprintf(fp,"To: %s\n", mail);
#if 1
  fprintf(fp,"Bcc: %s\n", vr->priv->admin_email);
#endif
  fprintf(fp,"From: %s\n", vr->priv->admin_email);
  fprintf(fp,"Subject: Your %s password\n\n", vr->priv->site_name);
  fprintf(fp,"Someone at IP address %s requested a ", vr->r->connection->remote_ip);
  fprintf(fp,"password reminder for your %s account.\n\n", vr->priv->site_name);
  fprintf(fp, "Your username is: %s\n", u);
  fprintf(fp, "Your password is: %s\n\n", pass);
  fprintf(fp, "If you did not request this reminder don't worry, as ");
  fprintf(fp, "only you will receive this email.\n" );
  pclose(fp);

  return 1;
}


static int
acct_loginsub_serve (VirguleReq *vr)
{
  request_rec *r = vr->r;
  apr_table_t *args;
  const char *u, *pass, *forgot;
  const char *ret1, *ret2;
  const char *cookie;
  
  r->content_type = "text/plain; charset=UTF-8";

  args = virgule_get_args_table (vr);

  if (args == NULL)
    return virgule_send_error_page (vr, vERROR, "form data", "Invalid form submission.\n");

  u = apr_table_get (args, "u");
  pass = apr_table_get (args, "pass");
  forgot = apr_table_get (args, "forgot");

#if 0
  virgule_buffer_printf (b, "Username: %s\n", u);
  virgule_buffer_printf (b, "Password: %s\n", pass);
#endif

  /* User has forgotten their password. */
  if ( forgot != NULL )
    {
      char *db_key, *db_key_lc, *mail;
      apr_pool_t *p = vr->r->pool;        
      xmlDoc *profile;
      xmlNode *tree;

      /* sanity check user name */
      db_key = virgule_acct_dbkey(vr, u);
      if (db_key == NULL)
        {
          return virgule_send_error_page (vr, vERROR, "username", "Username contains invalid characters.");
	}

      /* verify that user name is in DB */
      profile = virgule_db_xml_get (p, vr->db, db_key);
      if (profile == NULL)
        {
          return virgule_send_error_page (vr, vERROR, "username", "The specified account could not be found.");
	}

      /* check for an account alias */
      tree = virgule_xml_find_child (profile->xmlRootNode, "alias");
      if (tree != NULL) 
        {
          db_key_lc = virgule_acct_dbkey (vr, virgule_xml_get_prop (p, tree, (xmlChar *)"link"));
          profile = virgule_db_xml_get (p, vr->db, db_key_lc);
        }
      
      /* Get the email and password. */
      tree = virgule_xml_find_child (profile->xmlRootNode, "info");
      mail = virgule_xml_get_prop (p, tree, (xmlChar *)"email");
      if (mail == NULL)
	{
	  return virgule_send_error_page(vr, vERROR,
				 "email",
				 "The profile for <em>%s</em> doesn't have an email address associated with it.",
				 u);
	}

      tree = virgule_xml_find_child (profile->xmlRootNode, "auth");
      pass = virgule_xml_get_prop (p, tree, (xmlChar *)"pass");
      if (pass == NULL)
	{
	  return virgule_send_error_page(vr, vERROR,
				 "password",
				 "The profile for <em>%s</em> doesn't have a password associated with it.",
				 u);
	}

      /* Mail it. */
      send_email( vr, mail, u, pass );

      /* Tell the user */
      return virgule_send_error_page( vr, vINFO, "Password mailed.",
			      "The password for <em>%s</em> has been mailed to <em>%s</em>", 
			      u, mail );
  }

  if (!virgule_acct_login (vr, u, pass, &ret1, &ret2))
      return virgule_send_error_page (vr, vERROR, ret1, ret2);

  u = ret1;
  cookie = ret2;
  
  acct_set_cookie (vr, u, cookie, 86400 * 365);
  
  vr->u = u;

  return virgule_send_error_page (vr, vINFO,
			  "Login ok",
			  "Login to account <em>%s</em> ok.\n", u);

}

static int
acct_logout_serve (VirguleReq *vr)
{

  virgule_auth_user (vr);
  
  vr->r->no_cache = TRUE;
  vr->r->no_local_copy = TRUE;

  if (vr->u)
    {
      acct_set_cookie (vr, vr->u, "", -86400);
      return virgule_send_error_page (vr, vINFO,
			      "Logged out",
			      "Logout of account <em>%s</em> ok.\n", vr->u);
    }
  else
    return virgule_send_error_page (vr, vERROR,
			    "forbidden",
			    "You were already logged out.\n");

}


/**
 * acct_update_serve: Update the users account info. Blog syndication info
 * comes in piggy-backed on the same profile array but is processed 
 * separately. This isn't very elegant but will do for now.
 **/
static int
acct_update_serve (VirguleReq *vr)
{
  apr_pool_t *p = vr->r->pool;
  apr_table_t *args;

  virgule_auth_user (vr);

  args = virgule_get_args_table (vr);

  if (vr->u)
    {
      char *db_key;
      xmlDoc *profile;
      xmlNode *info, *aggregate, *tree;
      int i;
      int status;

      db_key = virgule_acct_dbkey (vr, vr->u);
      profile = virgule_db_xml_get (p, vr->db, db_key);

      info = virgule_xml_ensure_child (profile->xmlRootNode, "info");
      aggregate = virgule_xml_ensure_child (profile->xmlRootNode, "aggregate");

      for (i = 0; prof_fields[i].description; i++)
	{
	  if(prof_fields[i].flags & PROFILE_SYNDICATE)
	    tree = aggregate;
          else
	    tree = info;
	    
	  const char *val;
	  val = apr_table_get (args, prof_fields[i].attr_name);
	  if (val == NULL && prof_fields[i].flags & PROFILE_BOOLEAN)
	    val = "off";
          if (prof_fields[i].flags & PROFILE_BOOLEAN && 
	      prof_fields[i].flags & PROFILE_SYNDICATE &&
	      !virgule_req_ok_to_syndicate_blog (vr))
	    val = "off";
          if (virgule_is_input_valid(val))
	    {
              xmlSetProp (tree, (xmlChar *)prof_fields[i].attr_name, (xmlChar *)val);
	    }
          else
            return virgule_send_error_page (vr, vERROR,
                                    "UTF-8",
                                    "Only valid characters that use valid UTF-8 sequences may be submitted.");
	}

      status = virgule_db_xml_put (p, vr->db, db_key, profile);

      if (status)
	return virgule_send_error_page (vr, vERROR,
				"database",
				"There was an error storing the account profile.");

      virgule_update_aggregator_list (vr);

      apr_table_add (vr->r->headers_out, "refresh",
		    apr_psprintf(p, "0;URL=/person/%s/", vr->u));

      return virgule_send_error_page (vr, vINFO,
			      "Updated",
			      "Updates to account <a href=\"../person/%s/\">%s</a> ok",
			      ap_escape_uri(vr->r->pool,vr->u), vr->u);
    }
  else
    return virgule_send_error_page (vr, vERROR,
			    "forbidden",
			    "You need to be logged in to update your info.");
}

static int
node_info_compare (const void *ni1, const void *ni2)
{
  const char *name1 = ((NodeInfo *)ni1)->surname;
  const char *name2 = ((NodeInfo *)ni2)->surname;
  int i;

  if (name1 == NULL || name1[0] == 0) name1 = ((NodeInfo *)ni1)->name;
  if (name2 == NULL || name2[0] == 0) name2 = ((NodeInfo *)ni2)->name;
  for (i = 0; name2[i]; i++)
    {
      int c1, c2;
      c1 = tolower (name1[i]);
      c2 = tolower (name2[i]);
      if (c1 != c2) return c1 - c2;
    }
  return name1[i];
}


/*
 * Generate a list of users with cert info. Paging navigation links are 
 * included at the bottom of the page. Sort order follows /tmetric/default
 *
 */
void
virgule_acct_person_index_serve (VirguleReq *vr, int max)
{
  char *tmetric = virgule_req_get_tmetric (vr);
  int start = 0;
  int line = 0;
  int i, j, k;
  int len;
  char *user;
  char *givenname = NULL;
  char *surname = NULL;
  char *db_key;
  char *uri = vr->uri;
  xmlDoc *profile;
  xmlNode *tree;
  apr_table_t *args;
  CertLevel cl;

  if (tmetric == NULL)
    return;

  args = virgule_get_args_table (vr);
  if (args != NULL)
    start = atoi (apr_table_get (args, "start"));

  for (i = 0; tmetric[i] && line < (start + max); line++)
    {
      if(line < start)  /* skip to the start page */
        {
	  while (tmetric[i] && tmetric[i] != '\n')
	    i++;
	}
      else  /* render max users */
        {
	  for(j = 0; tmetric[i + j] && tmetric[i + j] != '\n'; j++); /* EOL */
          for(k = j; k > 0 && tmetric[i + k] != ' '; k--); /* username */
	  user = apr_palloc (vr->r->pool, k + 1);
	  memcpy (user, tmetric + i, k);
	  user[k] = 0;
	  i += j;

          ap_unescape_url(user);
          db_key = virgule_acct_dbkey (vr, user);
	  if (db_key == NULL)
	    {
	      i++;
	      continue;
	    }
          profile = virgule_db_xml_get (vr->r->pool, vr->db, db_key);
          if (profile == NULL)
	    {
	      i++;
	      continue;
	    }
          tree = virgule_xml_find_child (profile->xmlRootNode, "info");
          if (tree != NULL)
	    {
	      givenname = virgule_xml_get_prop (vr->r->pool, tree, (xmlChar *)"givenname");
	      surname = virgule_xml_get_prop (vr->r->pool, tree, (xmlChar *)"surname");
	    }
          virgule_db_xml_free (vr->r->pool, profile);

          cl = virgule_render_cert_level_begin (vr, user, CERT_STYLE_SMALL);
          virgule_buffer_printf (vr->b, "<a href=\"%s/\"%s>%s</a>, %s %s, %s\n",
	                 ap_escape_uri(vr->r->pool,user),
			 cl == CERT_LEVEL_NONE ? " rel=\"nofollow\"" : "",
			 user, givenname, surname,
	                 virgule_req_get_tmetric_level (vr, user));
          virgule_render_cert_level_end (vr, CERT_STYLE_SMALL);
        }
      if (tmetric[i] == '\n')
        i++;
    }
  len = strlen (vr->uri);
  if (len == 0)
    uri = apr_pstrcat (vr->r->pool, uri, "/index.html", NULL);
  else if (len > 0 && uri[len -1] == '/')
    uri = apr_pstrcat (vr->r->pool, uri, "index.html", NULL);
  if (start-max >= 0)
    virgule_buffer_printf (vr->b, "<a href=\"%s?start=%i\"><< Previous Page</a>&nbsp;&nbsp;\n", uri, start-max);
  if (line == start+max && tmetric[i])
    virgule_buffer_printf (vr->b, "<a href=\"%s?start=%i\">Next Page >></a>\n", uri, start+max);
}


/* Outputs a text file showing certification information when the URL
   /person/graph.dot is requested */
static int
acct_person_graph_serve (VirguleReq *vr)
{
  request_rec *r = vr->r;
  apr_pool_t *p = r->pool;
  Buffer *b = vr->b;
  Db *db = vr->db;
  DbCursor *dbc;
  char *issuer;
  const int threshold = 0;

  r->content_type = "text/plain; charset=UTF-8";
  virgule_buffer_printf (b, "digraph G {\n");
  dbc = virgule_db_open_dir (db, "acct");
  while ((issuer = virgule_db_read_dir_raw (dbc)) != NULL)
    {
      char *db_key;
      xmlDoc *profile;
      xmlNode *tree;
      xmlNode *cert;

      virgule_buffer_printf (b, "   /* %s */\n", issuer);

      db_key = virgule_acct_dbkey (vr, issuer);
      if (db_key == NULL)
        continue;
      profile = virgule_db_xml_get (p, db, db_key);
      if (profile == NULL)
        continue;
      tree = virgule_xml_find_child (profile->xmlRootNode, "certs");
      if (tree == NULL)
	continue;
      if (virgule_cert_level_from_name (vr, virgule_req_get_tmetric_level (vr, issuer)) < threshold)
	continue;
      for (cert = tree->children; cert != NULL; cert = cert->next)
	{
	  if (cert->type == XML_ELEMENT_NODE &&
	      !xmlStrcmp (cert->name, (xmlChar *)"cert"))
	    {
	      char *cert_subj;

	      cert_subj = virgule_xml_get_prop (p, cert, (xmlChar *)"subj");
	      if (cert_subj &&
		  virgule_cert_level_from_name (vr, virgule_req_get_tmetric_level (vr, cert_subj)) >= threshold)
		{
		  char *cert_level;

                  cert_level = virgule_xml_get_prop (p, cert, (xmlChar *)"level");
		  virgule_buffer_printf (b, "   %s -> %s [level=\"%s\"];\n",
				 issuer, cert_subj, cert_level);
		}
	    }
	}
    }
  virgule_db_close_dir (dbc);

  virgule_buffer_printf (b, "}\n");
  return virgule_send_response (vr);
}

/**
 * acct_person_articles_serve - render a page containing an index of all
 * articles posted by the specified user
 */
static int
acct_person_articles_serve (VirguleReq *vr, char *u)
{
  xmlDoc *artidx;
  xmlNode *tree;
  char *pagetitle, *db_key;

  if (virgule_set_temp_buffer (vr) != 0)
    return HTTP_INTERNAL_SERVER_ERROR;

  /* Render the user's  article list */
  db_key = apr_psprintf (vr->r->pool, "acct/%s/articles.xml", u);
  artidx = virgule_db_xml_get (vr->r->pool, vr->db, db_key);
  if (artidx == NULL)
    return virgule_send_error_page (vr, vERROR, "No articles found", "No articles have been posted by this user.\n");

  pagetitle = apr_psprintf (vr->r->pool, "Articles Posted by %s", u);
  virgule_buffer_printf (vr->b, "<h3>Articles Posted by <a href=\"%s/person/%s/\">%s</a></h3>\n<ul>\n", vr->prefix, ap_escape_uri(vr->r->pool, u), u);
  for (tree = xmlGetLastChild(artidx->xmlRootNode); tree != NULL; tree = tree->prev)
    {
      char *title, *date, *artnum;
      title = virgule_xml_get_prop (vr->r->pool, tree, (xmlChar *)"title");
      date = virgule_xml_get_prop (vr->r->pool, tree, (xmlChar *)"date");
      artnum = virgule_xml_get_string_contents (tree);
      virgule_buffer_printf (vr->b, "<li><a href=\"%s/article/%s.html\">%s</a> <span class=\"date\">%s</span></li>\n", vr->prefix, artnum, title, virgule_render_date (vr, date, 1));
    }
  virgule_buffer_puts (vr->b, "</ul>\n");

  virgule_set_main_buffer (vr);      
  return virgule_render_in_template (vr, "/templates/acct-articles.xml", "content", pagetitle);
}


/*
 * RSR notes - Some of this code should be moved to virgule_diary_rss_export
 * and/or possibly merged with similar code in rss_export.c.
 */
static int
acct_person_diary_rss_serve (VirguleReq *vr, char *u)
{
  Buffer *b = vr->b;
  xmlDocPtr doc;
  xmlChar *mem;
  int size;

  if ((!strcmp (virgule_req_get_tmetric_level (vr, u), 
      virgule_cert_level_to_name (vr, CERT_LEVEL_NONE))) ||
      virgule_diary_exists (vr, u) == 0)
  {
    vr->r->status = 404;
    return virgule_send_error_page (vr, vERROR, "User RSS feed not found", "No RSS feed is available for this user at this time.");
  }

  doc = xmlNewDoc ((xmlChar *)"1.0");
  vr->r->content_type = "text/xml; charset=UTF-8";
  doc->xmlRootNode = xmlNewDocNode (doc, NULL, (xmlChar *)"rss", NULL);
  xmlSetProp (doc->xmlRootNode, (xmlChar *)"version", (xmlChar *)"2.0");
  virgule_diary_rss_export (vr, doc->xmlRootNode, u);
  xmlDocDumpFormatMemory (doc, &mem, &size, 1);
  xmlFreeDoc (doc);
  if (size <= 0)
      return virgule_send_error_page (vr, vERROR, "internal", "xmlDocDumpFormatMemory() failed");
  virgule_buffer_write (b, (char *)mem, size);
  xmlFree (mem);
  return virgule_send_response (vr);
}


/**
 * generate a FOAF RDF file for this user
 */
static int
acct_person_foaf_serve (VirguleReq *vr, char *u)
{
  xmlDocPtr foaf;
  xmlChar *mem;
  int size;

  if (!strcmp (virgule_req_get_tmetric_level (vr, u), 
      virgule_cert_level_to_name (vr, CERT_LEVEL_NONE)))
  {
    vr->r->status = 404;
    return virgule_send_error_page (vr, vERROR, "not found", "No FOAF RDF record exists for this user at this time.");
  }
      
  foaf = virgule_foaf_person (vr, u);
  if (foaf == NULL)
  {
    vr->r->status = 404;
    return virgule_send_error_page (vr, vERROR, "not found", "No FOAF RDF record exists for this user at this time.");
  }

  xmlDocDumpFormatMemory (foaf, &mem, &size, 1);
  virgule_buffer_write (vr->b, (char *)mem, size);
  xmlFree (mem);
  xmlFreeDoc (foaf);
  return virgule_send_response (vr);
}


static int
acct_person_diary_static_serve (VirguleReq *vr, char *u, char *d)
{
  xmlDoc *profile;
  char *tail, *db_key, *title;
  int n = strtol (d, &tail, 10);
  if (strcmp (tail, ".html"))
    return virgule_send_error_page (vr, vERROR, "form data", "Invalid blog URL: %s", d);

  db_key = virgule_acct_dbkey (vr, u);
  if (db_key == NULL)
    return virgule_send_error_page (vr, vERROR, "username", "The user name doesn't even look valid, much less exist in the database.");
    
  profile = virgule_db_xml_get (vr->r->pool, vr->db, db_key);
  if (profile == NULL)
    {
      vr->r->status = 404;
      return virgule_send_error_page (vr, vERROR, "not found","Account <em>%s</em> was not found.", u);
    }

  if (virgule_diary_exists (vr, u) == 0)
  {
    vr->r->status = 404;
    return virgule_send_error_page (vr, vERROR, "not found", "No blog entries exist for this user.");
  }

  title = apr_psprintf (vr->r->pool, "Blog for %s", u);

  /* initialize the temporary buffer */
  if (virgule_set_temp_buffer (vr) != 0)
    return HTTP_INTERNAL_SERVER_ERROR;

  /* render the diary entry to the temp buffer */
  virgule_diary_entry_render (vr, u, n, NULL, TRUE);

  virgule_buffer_printf (vr->b, "<p><a href=\"%s/person/%s/diary.html\">Latest blog entries</a> &nbsp;&nbsp;&nbsp; ",
		   vr->prefix, ap_escape_uri(vr->r->pool, u));

  virgule_buffer_printf (vr->b, "<a href=\"%s/person/%s/diary.html?start=%d\">Older blog entries</a></p>\n",
		   vr->prefix, ap_escape_uri(vr->r->pool, u), n);


  /* switch back to the main buffer */  
  virgule_set_main_buffer (vr);
  
  return virgule_render_in_template (vr, "/templates/acct-diary.xml", "diary", title);
}


static int
acct_person_diary_serve (VirguleReq *vr, char *u)
{
  xmlDoc *profile;
  char *title, *db_key;
  apr_table_t *args;
  int start;

  db_key = virgule_acct_dbkey (vr, u);
  if (db_key == NULL)
    return virgule_send_error_page (vr, vERROR, "username", "The user name doesn't even look valid, much less exist in the database.");
    
  profile = virgule_db_xml_get (vr->r->pool, vr->db, db_key);
  if (profile == NULL)
    {
      vr->r->status = 404;
      return virgule_send_error_page (vr, vERROR, "not found","Account <em>%s</em> was not found.", u);
    }

  args = virgule_get_args_table (vr);
  if (args == NULL)
    start = -1;
  else
    start = atoi (apr_table_get (args, "start"));

  if (virgule_diary_exists (vr, u) == 0)
  {
    vr->r->status = 404;
    return virgule_send_error_page (vr, vERROR, "not found", "No blog entries exist for this user.");
  }

  title = apr_psprintf (vr->r->pool, "Blog for %s", u);

  /* initialize the temporary buffer */
  if (virgule_set_temp_buffer (vr) != 0)
    return HTTP_INTERNAL_SERVER_ERROR;

  /* render the diary entries to the temp buffer */
  if (start == -1)
    virgule_buffer_printf (vr->b, "<h1>Recent blog entries for <a href=\"%s/person/%s/\">%s</a></h1>\n",
		   vr->prefix, ap_escape_uri(vr->r->pool, u), u);
  else
    virgule_buffer_printf (vr->b, "<h1>Older blog entries for <a href=\"%s/person/%s/\">%s</a> (starting at number %d)</h1>\n",
		   vr->prefix, ap_escape_uri(vr->r->pool, u), u, start);

  virgule_diary_render (vr, u, 10, start);

  /* switch back to the main buffer */  
  virgule_set_main_buffer (vr);
  
  return virgule_render_in_template (vr, "/templates/acct-diary.xml", "diary", title);
}


/**
 *  acct_person_serve - dispatches requests specfic to a user's account and
 *  renders a user profile page.
 **/
static int
acct_person_serve (VirguleReq *vr, const char *path)
{
  request_rec *r = vr->r;
  apr_pool_t *p = r->pool;
  char *q;
  char *u;
  char *db_key;
  xmlDoc *profile, *staff, *artidx;
  xmlNode *tree, *lastlogin;
  Buffer *b = vr->b;
  char *title;
  char *surname, *givenname;
  char *url;
  char *date = NULL;
  char *notes;
  int diaryused = 0;
  int any;
  int observer = FALSE;
  char *err;
  char *first;
  time_t cn = time(NULL) - 2592000;

//  if (!path[0])
//    return acct_person_index_serve (vr);

  if (!strcmp (path, "graph.dot"))
    return acct_person_graph_serve (vr);

  virgule_auth_user (vr);

  q = strchr ((char *)path, '/');
  if (q == NULL)
    {
      apr_table_add (r->headers_out, "Location",
		    ap_make_full_path (p, r->uri, ""));
      return HTTP_MOVED_PERMANENTLY;
    }

  u = apr_pstrndup(p, path, q - path);

  if (!strcmp (q + 1, "diary.html"))
    return acct_person_diary_serve (vr, u);

  if (!strncmp (q + 1, "diary/", 6) && isdigit (*(q+7)))
    return acct_person_diary_static_serve (vr, u, q+7);

  if (!strcmp (q + 1, "articles.html"))
    return acct_person_articles_serve (vr, u);

  if (!strcmp (q + 1, "rss.xml"))
    return acct_person_diary_rss_serve (vr, u);

  if (!strcmp (q + 1, "foaf.rdf"))
    return acct_person_foaf_serve (vr, u);

  if (!strcmp (q + 1, "spam"))
    return acct_flag_as_spam (vr, u);

  if (q[1] != '\0')
    {
      vr->r->status = 404;
      return virgule_send_error_page (vr, vERROR, "not found", "The requested page does not exist.");
    }

  db_key = virgule_acct_dbkey (vr, u);
  if (db_key == NULL)
    {
      vr->r->status = 404;
      return virgule_send_error_page (vr, vERROR, "username", "The user name doesn't even look valid, much less exist in the database.");
    }
    
  profile = virgule_db_xml_get (p, vr->db, db_key);
  if (profile == NULL)
    {
      vr->r->status = 404;
      return virgule_send_error_page (vr, vERROR,
			    "not found",
			    "Account <em>%s</em> was not found.", u);
    }
    
  tree = virgule_xml_find_child (profile->xmlRootNode, "alias");
  if (tree != NULL)
    {
      apr_table_add (r->headers_out, "Location",
		    apr_pstrcat (p, vr->prefix, "/person/",
				virgule_xml_get_prop (p, tree, (xmlChar *)"link"), "/", NULL));
      return HTTP_MOVED_PERMANENTLY;
				
    }

  diaryused = virgule_diary_exists (vr, u);

  /* add a few things to the page header */
  if (!strcmp (virgule_req_get_tmetric_level (vr, u), 
      virgule_cert_level_to_name (vr, CERT_LEVEL_NONE)))
    {
      observer = TRUE;
    }
  else
    {
      virgule_buffer_puts (vr->hb, "<link rel=\"meta\" type=\"application/rdf+xml\" title=\"FOAF\" href=\"foaf.rdf\" />\n");
      if (diaryused > 0)
        virgule_buffer_puts (vr->hb, "<link rel=\"alternate\" type=\"application/rss+xml\" title=\"RSS\" href=\"rss.xml\" />\n");
    }

  if (virgule_user_is_special(vr,vr->u))
    virgule_buffer_puts (vr->hb,
	"<script language=\"JavaScript\" type=\"text/javascript\">\n"
	"<!-- \n"
	"function confirmdel(name)\n"
	"{\n"
	"  var rc = confirm(\"Are you sure you want to delete user account: \"+name+\"?\");\n"
	"  return rc;\n"
	"}\n"
	"// -->\n"
	"</script>\n");

  title = apr_psprintf (p, "Personal info for %s", u);

  /* initialize the temporary buffer */
  if (virgule_set_temp_buffer (vr) != 0)
    return HTTP_INTERNAL_SERVER_ERROR;
  b = vr->b;

  if (virgule_user_is_special(vr,vr->u))
    {
      virgule_buffer_printf (b, "<div class=\"adminbox\">Admin Functions <form action=\"/acct/killsub.html\" method=\"POST\" accept-charset=\"UTF-8\" onSubmit=\"return confirmdel('%s');\"><input type=\"hidden\" name=\"u\" value=\"%s\" /><input type=\"submit\" value=\" Delete User \" /></form></div>", u, u);
    }

  if (!observer)
    {
      virgule_render_cert_level_begin (vr, u, CERT_STYLE_SMALL);
      virgule_buffer_printf (b, "<b>%s</b> is currently certified at %s level.\n", u, virgule_req_get_tmetric_level (vr, u));
      virgule_render_cert_level_end (vr, CERT_STYLE_SMALL);
    }
  else
    {
      if (strcmp (virgule_req_get_tmetric_level (vr, vr->u),
    		  virgule_cert_level_to_name (vr, CERT_LEVEL_NONE)) != 0)
        {
          virgule_buffer_printf(b, "<div class=\"spamscore\">Spam rating: %s "
	                           "&nbsp;<form method=\"POST\" action=\"/person/%s/spam\">"
				   "<input type=\"submit\" value=\"Flag account as spam\"></form></div>\n",
    				   virgule_xml_find_child_string (profile->xmlRootNode, "spamscore", "not spam"),
				   ap_escape_uri(p, u));
	}
    }

  any = 0;
  tree = virgule_xml_find_child (profile->xmlRootNode, "info");
  if (tree)
    {
      givenname = virgule_xml_get_prop (p, tree, (xmlChar *)"givenname");
      surname = virgule_xml_get_prop (p, tree, (xmlChar *)"surname");
      virgule_buffer_printf (b, "<p>Name: %s %s<br />\n",
// rsr		     givenname ? virgule_nice_utf8(p, givenname) : "",
//		     surname ? virgule_nice_utf8(p, surname) : "", date);
// raph		     givenname ? virgule_nice_text(p, givenname) : "",
//		     surname ? virgule_nice_text(p, surname) : "", date);
		     givenname ? virgule_strip_a (vr, givenname) : "",
		     surname ? virgule_strip_a (vr, surname) : "");


      date = virgule_xml_find_child_string (profile->xmlRootNode, "date", "N/A");
      virgule_buffer_printf (b, "Member since: %s<br />\n", date);
      date = NULL;

      lastlogin = virgule_xml_find_child(profile->xmlRootNode, "lastlogin");
      if(lastlogin) 
        date = virgule_xml_get_prop (p, lastlogin, (xmlChar *)"date");
      if(!date)
        date = "N/A";
	
      virgule_buffer_printf (b, "Last Login: %s</p>\n", date);

      if (!observer)
        {
	  char *bmurl = apr_psprintf (p, "%s/person/%s/", vr->priv->base_uri, ap_escape_uri(p,u));
	  char *bmtitle = ap_escape_uri (p, title);
	  virgule_buffer_printf (b,
				 "<p><a href=\"foaf.rdf\"><img src=\"/images/foaf.png\" height=\"20\" width=\"40\" border=\"none\" alt=\"FOAF RDF\" title=\"FOAF RDF\" /></a> "
                                 "<a href=\"javascript:void(0)\" onclick=\"sbm(event,'%s','%s')\">"
				 "<img src=\"/images/share.png\" height=\"16\" width=\"16\" border=\"none\" alt=\"Share This\" title=\"Share This\" /></a></p>",
				 bmurl, virgule_str_subst (vr->r->pool, bmtitle, "'", "%27"));
	}
      url = virgule_xml_get_prop (p, tree, (xmlChar *)"url");
      if (url && url[0])
	{
	  char *url2;
	  char *colon;

	  url2 = url;
	  colon = strchr (url, ':');
	  if (!colon || colon[1] != '/' || colon[2] != '/')
	    url2 = apr_pstrcat (p, "http://", url, NULL);

	  virgule_buffer_printf (b, "<p>Homepage: <a href=\"%s\"", url2);
	  if(observer)
	    virgule_buffer_puts (b, " rel=\"nofollow\"");
	  virgule_buffer_printf (b, ">%s</a></p>\n", virgule_nice_text (p, url));
	  any = 1;
	}
      notes = virgule_xml_get_prop (p, tree, (xmlChar *)"notes");
      if (notes && notes[0])
	{
	  if(observer)
  	    virgule_buffer_printf (b, "<p><b>Notes:</b> %s</p>\n", virgule_nice_htext (vr, virgule_strip_a (vr, notes), &err));
	  else 
	    virgule_buffer_printf (b, "<p><b>Notes:</b> %s</p>\n", virgule_nice_htext (vr, notes, &err));
	  any = 1;
	}
    }
  if (!any)
    virgule_buffer_puts (b, "<p>No personal information is available.</p>\n");

  /* Render staff listings */
  first = "<h3><x>Projects</x></h3>\n"
    "<ul>\n";
  db_key = apr_psprintf (p, "acct/%s/staff-person.xml", u);

  staff = virgule_db_xml_get (p, vr->db, db_key);
  if (staff != NULL)
    {
      for (tree = staff->xmlRootNode->children; tree != NULL; tree = tree->next)
	{
	  char *name;
	  char *type;

	  name = virgule_xml_get_prop (p, tree, (xmlChar *)"name");
	  type = virgule_xml_get_prop (p, tree, (xmlChar *)"type");

	  if (! !strcmp (type, "None"))
	    {
	      virgule_buffer_puts (b, first);
	      first = "";
	      virgule_buffer_printf (b, "<li>%s on %s</li>\n",
			     type, virgule_render_proj_name (vr, name));
	    }
	}
    }
  if (first[0] == 0)
    virgule_buffer_puts (b, "</ul>\n");

  /* Render recent article list */
  db_key = apr_psprintf (p, "acct/%s/articles.xml", u);
  artidx = virgule_db_xml_get (p, vr->db, db_key);
  if (artidx != NULL)
    {
      int a = 0;
      virgule_buffer_printf (b, "<h3>Articles Posted by %s</h3>\n<ul>\n", u);
      for (tree = xmlGetLastChild(artidx->xmlRootNode); tree != NULL && a < 10; tree = tree->prev, a++)
        {
	  char *atitle, *adate, *artnum;
	  atitle = virgule_xml_get_prop (p, tree, (xmlChar *)"title");
	  adate = virgule_xml_get_prop (p, tree, (xmlChar *)"date");
	  artnum = virgule_xml_get_string_contents (tree);
          virgule_buffer_printf (vr->b, "<li><a href=\"%s/article/%s.html\">%s</a> <span class=\"date\">%s</span></li>\n", vr->prefix, artnum, atitle, virgule_render_date (vr, adate, 1));
	}
      virgule_buffer_puts (b, "</ul>\n");

      if (tree != NULL)
        virgule_buffer_printf (b, "<p><a href=\"articles.html\">Complete list of articles by %s</a></p>", u, u);
    }
    
  if(diaryused > 0)
    {
      virgule_buffer_printf (b, "<h3>Recent blog entries by %s</h3>\n", u);
      if(!observer)
        virgule_buffer_printf (b, "<div class=\"feeds\">Syndication: <a href=\"rss.xml\">RSS 2.0</a></div>\n", u);
      virgule_diary_render (vr, u, 5, -1);
    }

  /* Browse certifications */
  virgule_buffer_puts (b, "<a name=\"certs\">&nbsp;</a>\n");
  tree = virgule_xml_find_child (profile->xmlRootNode, "certs");
  if (tree)
    {
      time_t t;
      xmlNode *cert;
      int any = 0;
      for (cert = tree->children; cert != NULL; cert = cert->next)
	if (cert->type == XML_ELEMENT_NODE &&
	    !xmlStrcmp (cert->name, (xmlChar *)"cert"))
	  {
	    xmlChar *subject, *level;
	    subject = xmlGetProp (cert, (xmlChar *)"subj");
	    level = xmlGetProp (cert, (xmlChar *)"level");
	    t = virgule_virgule_to_time_t (vr, (char *)xmlGetProp (cert, (xmlChar *)"date"));
	    if (xmlStrcmp (level, (xmlChar *)virgule_cert_level_to_name (vr, 0)))
	      {
		if (!any)
		  {
		    virgule_buffer_printf (b, "<p>%s certified others as follows:</p><ul>\n", u);
		    any = 1;
		  }
	        virgule_buffer_printf (b, "<li>%s certified <a href=\"../%s/\">%s</a> as %s %s</li>\n",
			     u, ap_escape_uri(vr->r->pool, (char *)subject), subject, level,
			     (t > cn) ? "<span class=\"newcert\"> new</span>" : "");
	      }
	  }
      if (any)
	virgule_buffer_puts (b, "</ul>\n");
    }

  tree = virgule_xml_find_child (profile->xmlRootNode, "certs-in");
  if (tree)
    {
      time_t t;
      xmlNode *cert;
      int any = 0;
      for (cert = tree->children; cert != NULL; cert = cert->next)
	if (cert->type == XML_ELEMENT_NODE &&
	    !xmlStrcmp (cert->name, (xmlChar *)"cert"))
	  {
	    xmlChar *issuer, *level;
	    issuer = xmlGetProp (cert, (xmlChar *)"issuer");
	    level = xmlGetProp (cert, (xmlChar *)"level");
	    t = virgule_virgule_to_time_t (vr, (char *)xmlGetProp (cert, (xmlChar *)"date"));
	    if (xmlStrcmp (level, (xmlChar *)virgule_cert_level_to_name (vr, 0)))
	      {
		if (!any)
		  {
		    virgule_buffer_printf (b, "<p>Others have certified %s as follows:</p>\n<ul>\n", u);
		    any = 1;
		  }
		virgule_buffer_printf (b, "<li><a href=\"../%s/\">%s</a> certified %s as %s %s</li>\n",
			       ap_escape_uri(vr->r->pool, (char *)issuer), issuer, u, level,
			       (t > cn) ? "<span class=\"newcert\"> new</span>" : "");
	      }
	  }
      if (any)
	virgule_buffer_puts (b, "</ul>\n");
    }

  /* Certification form; need to be authenticated, and disallow self-cert. */
  if (vr->u == NULL)
    virgule_buffer_puts (b, "<p> [ Certification disabled because you're not logged in. ] </p>\n");
#if 1
  /* disable self-cert */
  else if (!strcmp (u, vr->u))
    virgule_buffer_puts (b, "<p> [ Certification disabled for yourself. ] </p>\n");
#endif
  else
    {
      int i;
      CertLevel level;

      level = virgule_cert_get (vr, vr->u, u);

      virgule_buffer_printf (b, "<form method=\"POST\" action=\"%s/acct/certify.html\">\n"
		     "Certify %s as:\n"
		     " <select name=\"level\" value=\"level\">\n", vr->prefix, u);

      for (i = virgule_cert_num_levels (vr) - 1; i >= 0; i--)
	virgule_buffer_printf (b, "  <option%s> %s\n",
		       level == i ? " selected" : "",
		       virgule_cert_level_to_name (vr, i));

      virgule_buffer_printf (b, " </select>\n"
		     " <input type=\"submit\" value=\"Certify\">\n"
		     " <input type=\"hidden\" name=\"subject\" value=\"%s\">\n"
		     "</form>\n"
		     "<p>Certify this user if you know them. See the "
                     "<a href=\"%s/certs.html\">certification overview</a> "
		     "for more information.</p>\n",
		     u, vr->prefix);

      if (vr->priv->render_diaryratings) 
	virgule_rating_diary_form (vr, u);
    }

  virgule_set_main_buffer (vr);

  return virgule_render_in_template (vr, "/templates/acct-profile.xml", "profile", title);
}


/**
 * Handles POSTs from the certification form on the user profile page
 */
static int
acct_certify_serve (VirguleReq *vr)
{
  apr_table_t *args;
  const char *subject;
  const char *level;
  int status;

  virgule_auth_user (vr);
  args = virgule_get_args_table (vr);

  if (vr->u)
    {
      subject = apr_table_get (args, "subject");
      level = apr_table_get (args, "level");

      if (!strcmp(subject,vr->u))
        return virgule_send_error_page(vr, vERROR,
	                       "forbidden",
			       "Sorry, you can't certify yourself.");

      status = virgule_cert_set (vr, vr->u, subject,
			 virgule_cert_level_from_name (vr, level));

      if (status)
	return virgule_send_error_page (vr, vERROR,
				"database",
				"There was an error storing the certificate. This means there's something wrong with the site.");
      apr_table_add (vr->r->headers_out, "refresh",
		    apr_psprintf(vr->r->pool, "0;URL=/person/%s/#certs",
				subject));
      return virgule_send_error_page (vr, vINFO,
			      "Updated",
			      "Certification of <a href=\"../person/%s/\">%s</a> to %s level ok.",
			      ap_escape_uri(vr->r->pool,subject), subject, level);
    }
  else
    return virgule_send_error_page (vr, vERROR,
			    "forbidden",
			    "You need to be logged in to certify another <x>person</x>.");
}



/**
 * acct_killsub_serve: Sanity checks a web request for account deletion.
 * Only requests submitted by a logged in, special user will be honored.
 **/
static int
acct_killsub_serve(VirguleReq *vr)
{
  const char *u = NULL;
  apr_table_t *args;

  args = virgule_get_args_table (vr);

  /* do a few sanity checks before going any further */
  if (args == NULL)
    return virgule_send_error_page (vr, vERROR, "form data", "Invalid form submission.\n");

  virgule_auth_user (vr);
  if (vr->u == NULL)
    return virgule_send_error_page (vr, vERROR, "forbidden", "You must log in to access admin fucntions!");

  if (!virgule_user_is_special(vr,vr->u))
    return virgule_send_error_page (vr, vERROR, "forbidden", "You are not authorized to use admin functions!\n");
  
  u = apr_table_get (args, "u");
  
  if (u == NULL)
    return virgule_send_error_page (vr, vERROR, "username", "The user name was bad!\n");

  if (acct_kill (vr, u))
    return virgule_send_error_page (vr, vINFO, "Account Deleted", "User account: [%s] has been deleted!\n", u);
  else     
    return virgule_send_error_page (vr, vERROR, "internal", "User account: [%s] was not deleted. acct_kill() failed!\n", u);
}

/**
 * virgule_acct_touch: Touch user profile to record timestamp of this visit
 **/
void
virgule_acct_touch(VirguleReq *vr, const char *u)
{
  const char *newdate;
  char *db_key, *db_key_lc, *u_lc;
  xmlDoc *profile;
  xmlNode *root, *tree, *lastlogin;
  apr_pool_t *p = vr->r->pool;
  
  db_key = virgule_acct_dbkey (vr, u);
  if (db_key == NULL) return;
  
  profile = virgule_db_xml_get (p, vr->db, db_key);
  if (profile == NULL) return;
  
  root = profile->xmlRootNode;
  tree = virgule_xml_find_child (root, "alias");

  if (tree != NULL) {
    u_lc = apr_pstrdup (p, u);
    ap_str_tolower (u_lc);
    db_key_lc = virgule_acct_dbkey (vr, u_lc);
    profile = virgule_db_xml_get (p, vr->db, db_key);
    if (profile == NULL) return;
    root = profile->xmlRootNode;
  }
      
  newdate = virgule_iso_now (p);

  lastlogin = virgule_xml_find_child(root, "lastlogin");
  if(lastlogin == NULL) 
  {
    lastlogin = xmlNewChild (root, NULL, (xmlChar *)"lastlogin", NULL);
    xmlSetProp (lastlogin, (xmlChar *)"date", (xmlChar *)newdate);
  }
  else
  {
    xmlSetProp (lastlogin, (xmlChar *)"date", (xmlChar *)newdate);
  }
  
  virgule_db_xml_put (p, vr->db, db_key, profile);  
}


/**
 * acct_maint - sequentially analyzes and repairs, if needed, each user
 * profile. Some simple statistics are also gathered during the process
 * and written to a stats XML file for user elsewhere.
 *
 *  Certificate symmetry - Restores missing inbound or outbound certs.
 *
 *  Cert level symmetry - correct mismatching levels to issuer level.
 *
 *  Alias links - Missing or corrupt  profile aliases are reported.
 *
 *  XML validity - Corrupt profiles are reported for manual repair.
 *
 * ToDo
 * - remove observer certs
 * - remove self certs
 *
 **/
static int
acct_maint (VirguleReq *vr)
{
    xmlDocPtr statdoc = NULL;
    xmlDocPtr profile = NULL;
    xmlNodePtr root, alias, ctree, cert;
    DbCursor *dbc;
    apr_pool_t *sp = NULL;
    apr_hash_t *stat;
    char *statk;
    int *statv;
    int ecount = 1;
    char *u, *dbkey;
    char *cerr[] =
     {
       "",
       "Normalized assymetric cert levels.",
       "Created missing subject cert.",
       "Subject profile missing.",
       "Created missing issuer cert.",
       "Issuer cert level mismatch.",
       "Issuer profile missing."
     };

    /* initialize the statistics hash */
    stat = apr_hash_make(vr->r->pool);
    statv = apr_pcalloc (vr->r->pool, sizeof(int));
    apr_hash_set (stat, "Users", APR_HASH_KEY_STRING, statv);    

    if (virgule_set_temp_buffer (vr) != 0)
      return HTTP_INTERNAL_SERVER_ERROR;

    virgule_buffer_puts (vr->b, "<h2>Analyzing account profiles</h2>\n");

    dbc = virgule_db_open_dir (vr->db, "acct");
    while ((u = virgule_db_read_dir_raw (dbc)) != NULL)
      {
        if (sp != NULL)
	    apr_pool_destroy (sp);
	    
	apr_pool_create (&sp, vr->r->pool);

	dbkey = apr_pstrcat (sp, "acct/", u, "/profile.xml", NULL);
	profile = virgule_db_xml_get (sp, vr->db, dbkey);
	if (profile == NULL)
	  {
	    virgule_buffer_printf (vr->b, "%i [%s] : invalid profile [%s]<br />\n", ecount++, u, dbkey);
	    continue;
	  }

	root = xmlDocGetRootElement (profile);
	if (root == NULL)
	  {
	    virgule_buffer_printf (vr->b, "%i [%s] : profile [%s] has no root node<br />\n", ecount++, u, dbkey);
	    continue;	    
	  }

	/* check for an alias */
	alias = virgule_xml_find_child (root, "alias");
	if(alias != NULL)
	  {
	    u = virgule_xml_get_prop (sp, alias, (xmlChar *)"link");
	    virgule_db_xml_free (sp, profile);
	    dbkey = apr_pstrcat (sp, "acct/", u, "/profile.xml", NULL);
	    profile = virgule_db_xml_get (sp, vr->db, dbkey);
	    if (profile == NULL)
	      {
		virgule_buffer_printf (vr->b, "%i [%s] : invalid alias proflie [%s]<br />\n", ecount++, u, dbkey);
		continue;
	      }
	    root = xmlDocGetRootElement (profile);
	    if (root == NULL)
	      {
		virgule_buffer_printf (vr->b, "%i [%s] : alias profile [%s] has no root node<br />\n", ecount++, u, dbkey);
		continue;
	      }
    	  }

	/* Add this user to the stats */
	statv = apr_hash_get (stat, "Users", APR_HASH_KEY_STRING);
	(*statv)++;
	apr_hash_set (stat, "Users", APR_HASH_KEY_STRING, statv);

	/* Add this user's cert level to the stats */
	statk = (char *)virgule_req_get_tmetric_level (vr, u);
	statv = apr_hash_get (stat, statk, APR_HASH_KEY_STRING);
	if (statv == NULL)
	  statv = apr_pcalloc (vr->r->pool, sizeof(int));
	else
	  (*statv)++;	  
	apr_hash_set (stat, statk, APR_HASH_KEY_STRING, statv);
	  
	/* loop through outbound certs */
	ctree = virgule_xml_find_child (root, "certs");
	if (ctree != NULL)
	  {	
	    for (cert = ctree->children; cert != NULL; cert = cert->next)
	      {
	        int rc;
		char *subject = NULL;
		char *level = NULL;
		char *date = NULL;
	    
		if (cert->type != XML_ELEMENT_NODE || xmlStrcmp (cert->name, (xmlChar *)"cert"))
		    continue;

		subject = (char *)xmlGetProp (cert, (xmlChar *)"subj");
		if (subject == NULL)
		  {
		    virgule_buffer_printf (vr->b, "%i [%s] : Invalid outbound cert - no subject<br />\n", ecount++, u);
		    continue;
		  }

		level = (char *)xmlGetProp (cert, (xmlChar *)"level");
		date = (char *)xmlGetProp (cert, (xmlChar *)"date");

		rc = virgule_cert_verify_outbound (vr, sp, u, subject, level, date);
		if(rc != 0) 
		    virgule_buffer_printf (vr->b, "%i [%s] : %s Cert subject [%s]<br />\n", ecount++, u, cerr[rc], subject);

		xmlFree (subject);
		if (level != NULL)
		  xmlFree (level);
		if (date != NULL)
		  xmlFree (date);

		apr_sleep(1);
	      }
	  }
	  	      

	/* loop through inbound certs */
	ctree = virgule_xml_find_child (root, "certs-in");
	if (ctree != NULL)
	  {	
	    for (cert = ctree->children; cert != NULL; cert = cert->next)
	      {
	        int rc;
		char *issuer = NULL;
		char *level = NULL;
		char *date = NULL;
	    
		if (cert->type != XML_ELEMENT_NODE || xmlStrcmp (cert->name, (xmlChar *)"cert"))
		    continue;

		issuer = (char *)xmlGetProp (cert, (xmlChar *)"issuer");
		if (issuer == NULL)
		  {
		    virgule_buffer_printf (vr->b, "%i [%s] : Invalid inbound cert - no issuer<br />\n", ecount++, u);
		    continue;
		  }

		level = (char *)xmlGetProp (cert, (xmlChar *)"level");
		date = (char *)xmlGetProp (cert, (xmlChar *)"date");

		rc = virgule_cert_verify_inbound (vr, sp, u, issuer, level, date);
		if(rc != 0) 
		    virgule_buffer_printf (vr->b, "%i [%s] : %s Cert issuer [%s]<br />\n", ecount++, u, cerr[rc], issuer);

		xmlFree (issuer);
		if (level != NULL)
		  xmlFree (level);
		if (date != NULL)
		  xmlFree (date);

		apr_sleep(1);
	      }
	  }
	  
	apr_sleep(1);
      }

    if (ecount > 1)
      virgule_buffer_printf (vr->b, "<p><b>%i total errors found</b></p>\n", ecount-1);
    else
      virgule_buffer_puts (vr->b, "<p><b>No Errors found</b></p>\n");      

    /* Update user stats */
    statdoc = virgule_db_xml_doc_new (vr->r->pool);
    statdoc->xmlRootNode = xmlNewDocNode (statdoc, NULL, (xmlChar *)"stats", NULL);
    statv = apr_hash_get (stat, "Users", APR_HASH_KEY_STRING);
    xmlNewChild (statdoc->xmlRootNode, NULL, (xmlChar *)"Users", (xmlChar *)apr_itoa (vr->r->pool, *statv));
    virgule_buffer_printf (vr->b, "<p><b>Users:</b> %i</p>\n", *statv);
    if (*vr->priv->cert_level_names)
      {
	const char **l;
	for (l = vr->priv->cert_level_names; *l; l++)
	  {
	    statv = apr_hash_get (stat, *l, APR_HASH_KEY_STRING);
	    xmlNewChild (statdoc->xmlRootNode, NULL, (xmlChar *)*l, (xmlChar *)apr_itoa (vr->r->pool, *statv));
	    virgule_buffer_printf (vr->b, "<p><b>%s:</b> %i</p>\n", *l, *statv);
	  }
      }
    virgule_db_xml_put (vr->r->pool, vr->db, "userstats.xml", statdoc);

    virgule_set_main_buffer (vr);
    
    return virgule_render_in_template (vr, "/templates/default.xml", "content", "Account maintenance");
}


/**
 * virgule_acct_update_art_index - update or create an article index for
 * this user account.
 */
void
virgule_acct_update_art_index(VirguleReq *vr, int art)
{
    xmlDocPtr profile, article, artindex;
    xmlNodePtr artroot, a;
    int art_num;
    char *title, *author, *date, *art_num_str;
    char *artidxkey, *profilekey;
    char *artkey = apr_psprintf (vr->r->pool, "articles/_%d/article.xml", art);

    /* open the article and get the info we need */
    article = virgule_db_xml_get (vr->r->pool, vr->db, artkey);

    if (article == NULL)
      return;
    artroot = xmlDocGetRootElement (article);
    title = virgule_xml_find_child_string (artroot, "title", "(no title)");
    author = virgule_xml_find_child_string (artroot, "author", NULL);
    date = virgule_xml_find_child_string (artroot, "date", NULL);


// ap_log_rerror(APLOG_MARK,APLOG_CRIT, APR_SUCCESS, vr->r,"mod_virgule: art: %d - %s - %s", art, date, author);

    /* open the user profile */
    profilekey = apr_pstrcat (vr->r->pool, "acct/", author, "/profile.xml", NULL);
    profile = virgule_db_xml_get (vr->r->pool, vr->db, profilekey);
    if (profile == NULL)
      {
        virgule_db_xml_free(vr->r->pool, article);
        return;
      }
    /* open or create the article index */
    artidxkey = apr_pstrcat (vr->r->pool, "acct/", author, "/articles.xml", NULL);
    artindex = virgule_db_xml_get (vr->r->pool, vr->db, artidxkey);
    if (artindex == NULL)
      {
	artindex = virgule_db_xml_doc_new (vr->r->pool);
	artindex->xmlRootNode = xmlNewDocNode (artindex, NULL, (xmlChar *)"articles", NULL);
      }

    /* if this article is already in the article index, return now */
    for (a = artindex->xmlRootNode->children; a != NULL; a = a->next)
      {
        art_num_str = virgule_xml_get_string_contents (a);
	art_num = atoi (art_num_str);	
	if(art_num == art)
	  {
            virgule_db_xml_free(vr->r->pool, article);
            virgule_db_xml_free(vr->r->pool, profile);
            virgule_db_xml_free(vr->r->pool, artindex);
	    return;
	  }
      }

    /* Add the new article reference to the index */
    a = xmlNewTextChild (artindex->xmlRootNode, NULL, (xmlChar *)"article", (xmlChar *)apr_itoa (vr->r->pool, art));
    if (a == NULL)
      return;

    xmlSetProp (a, (xmlChar *)"date", (xmlChar *)date);
    xmlSetProp (a, (xmlChar *)"title", (xmlChar *)title);

    /* write the updated index */
    virgule_db_xml_put (vr->r->pool, vr->db, artidxkey, artindex);

    /* free the article index doc */
    virgule_db_xml_free(vr->r->pool, article);
    virgule_db_xml_free(vr->r->pool, profile);
    virgule_db_xml_free(vr->r->pool, artindex);
}


int
virgule_acct_maint_serve (VirguleReq *vr)
{
  const char *p;

  if (!strcmp (vr->uri, "/acct/"))
    return acct_index_serve (vr);
  if (!strcmp (vr->uri, "/acct/newsub.html"))
    return acct_newsub_serve (vr);
  if (!strcmp (vr->uri, "/acct/loginsub.html"))
    return acct_loginsub_serve (vr);
  if (!strcmp (vr->uri, "/acct/logout.html"))
    return acct_logout_serve (vr);
  if (!strcmp (vr->uri, "/acct/update.html"))
    return acct_update_serve (vr);
  if (!strcmp (vr->uri, "/acct/killsub.html"))
    return acct_killsub_serve (vr);
  if ((p = virgule_match_prefix (vr->uri, "/person/")) != NULL)
    return acct_person_serve (vr, p);
  if (!strcmp (vr->uri, "/acct/certify.html"))
    return acct_certify_serve (vr);
  if (!strcmp (vr->uri, "/admin/acctmaint.html"))
    return acct_maint (vr);
  return DECLINED;
}
