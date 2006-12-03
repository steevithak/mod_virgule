/* A module for simple account maintenance. */

#include <time.h>
#include <ctype.h>

#include <apr.h>
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
#include "util.h"
#include "certs.h"
#include "diary.h"
#include "db_ops.h"
#include "proj.h"
#include "rating.h"
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
  PROFILE_BOOLEAN   = 1 << 2
} ProfileFlags;

ProfileField prof_fields[] = {
  { "Given (first) name", "givenname", 40, PROFILE_PUBLIC },
  { "Surname (last name)", "surname", 40, PROFILE_PUBLIC },
  { "Surname first?", "snf", 40, PROFILE_PUBLIC | PROFILE_BOOLEAN },
  { "Email", "email", 40, 0 },
  { "Homepage", "url", 40, PROFILE_PUBLIC },
  { "Number of old messages to display", "numold", 4, 0 },
  { "Notes", "notes", 60015, PROFILE_PUBLIC | PROFILE_TEXTAREA },
  { NULL }
};

typedef struct _NodeInfo NodeInfo;

struct _NodeInfo {
  const char *name;
  const char *givenname;
  const char *surname;
};

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

  virgule_db_lock_upgrade(vr->lock);
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
	  !strcmp (msgptr->name, "lastread"))
	{
	  char *old_msgptr = xmlGetProp (msgptr, "location");
	      
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
      msgptr = xmlNewChild (tree, NULL, "lastread", NULL);
      xmlSetProp (msgptr, "location", location);
    }

  xmlSetProp (msgptr, "num", apr_psprintf(p, "%d", last_read));
  xmlSetProp (msgptr, "date", virgule_iso_now(p));

  status = virgule_db_xml_put (p, db, db_key, profile);
  virgule_db_xml_free (p, db, profile);

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
	  !strcmp (msgptr->name, "lastread"))
	{
	  char *old_msgptr = xmlGetProp (msgptr, "location");
	      
	  if (old_msgptr)
	    {
	      if (!strcmp (old_msgptr, location))

		return atoi (xmlGetProp (msgptr, "num"));
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

      num_old = xmlGetProp (tree, "numold");

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
	  !strcmp (msgptr->name, "lastread"))
	{
	  char *old_msgptr = xmlGetProp (msgptr, "location");
	      
	  if (old_msgptr)
	    {
	      if (!strcmp (old_msgptr, location))
		{ 
		  date = xmlGetProp (msgptr, "date");
		  if (date == NULL)
		    date = "1970-01-01 00:00:00";
		  virgule_db_xml_free (p, db, profile);
		  return date;
		}
	    }
	}
    }

  virgule_db_xml_free (p, db, profile);
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
  Buffer *b = vr->b;
  int i;

  virgule_auth_user (vr);

  if (vr->u)
    {
      const char *level;
      char *db_key;
      xmlDoc *profile;
      xmlNode *tree;
      char *value;

      db_key = virgule_acct_dbkey (vr, vr->u);
      profile = virgule_db_xml_get (p, vr->db, db_key);
      tree = virgule_xml_find_child (profile->xmlRootNode, "info");
      level = virgule_req_get_tmetric_level (vr, vr->u);

      virgule_render_header (vr, "User Account Info", NULL);
      virgule_buffer_printf (b, "<p>Welcome, <tt>%s</tt>. The range of functions you "
		     "can access depends on your <a href=\"%s/certs.html\">certification</a> "
		     "level. Remember to certify any other users of this site that you "
		     "know. The trust metric system relies on user participation!</p>\n<p>", 
		     vr->u, vr->prefix);

      virgule_render_cert_level_begin (vr, vr->u, CERT_STYLE_SMALL);
      virgule_buffer_printf (b, "You are currently certified at the %s level by the other users of this site.", level);
      virgule_render_cert_level_end (vr, CERT_STYLE_SMALL);
      virgule_buffer_puts (b, "</p><p>At this level you can:</p>\n<ul>");

      virgule_buffer_printf (b, "<li>Link to your <a href=\"%s/person/%s\">publicly accessible page</a></li>",vr->prefix,ap_escape_uri(vr->r->pool,vr->u));
      virgule_buffer_printf (b, "<li><a href=\"%s/diary/\">Post a blog entry</a></li>\n",vr->prefix);

      if (virgule_req_ok_to_reply (vr))
        {
	  virgule_buffer_puts (b, "<li>Post replies to articles</li>\n");
	}

      if (virgule_req_ok_to_create_project (vr))
        {
	  virgule_buffer_printf (b, "<li><a href=\"%s/proj/new.html\">Create new project pages</a></li>\n",vr->prefix);
	  virgule_buffer_printf (b, "<li>Edit your <a href=\"%s/proj/\">existing projects</a></li>\n",vr->prefix);
	}

      if (virgule_req_ok_to_post (vr))
        {
	  virgule_buffer_printf (b, "<li><a href=\"%s/article/post.html\">Post an article</a></li>\n",vr->prefix);
	}
	
      virgule_buffer_puts (b, "<li><a href=\"logout.html\">Logout</a></li>\n");
      if (vr->priv->projstyle == PROJSTYLE_NICK)
        virgule_buffer_puts (b, "<li><a href=\"/proj/updatepointers.html\">Mark all messags as read</a></li>\n");
      virgule_buffer_puts (b, "</ul><p> Or you can update your account info: </p>\n");
      virgule_buffer_puts (b, "<form method=\"POST\" action=\"update.html\" accept-charset=\"UTF-i\">\n");
      for (i = 0; prof_fields[i].description; i++)
	{
	  if (vr->priv->projstyle != PROJSTYLE_NICK &&
	      !strcmp(prof_fields[i].attr_name, "numold"))
	    continue;

	  value = NULL;
	  if (tree)
	    value = xmlGetProp (tree, prof_fields[i].attr_name);

	  virgule_buffer_printf (b, "<p> %s: <br>\n", prof_fields[i].description);
	  if (prof_fields[i].flags & PROFILE_BOOLEAN)
	    virgule_buffer_printf (b, "<input name=\"%s\" type=checkbox %s> </p>\n",
			   prof_fields[i].attr_name,
			   value ? (strcmp (value, "on") ? "" : " checked") : "");
	  else if (prof_fields[i].flags & PROFILE_TEXTAREA)
	    virgule_buffer_printf (b, "<textarea name=\"%s\" cols=%d rows=%d wrap=hard>%s</textarea> </p>\n",
			   prof_fields[i].attr_name,
			   prof_fields[i].size / 1000,
			   prof_fields[i].size % 1000,
			   value ? ap_escape_html (p, value) : "");
	  else
	    virgule_buffer_printf (b, "<input name=\"%s\" size=%d value=\"%s\"> </p>\n",
			   prof_fields[i].attr_name, prof_fields[i].size,
			   value ? ap_escape_html (p, value) : "");
	  if (value != NULL)
	    xmlFree (value);
	}
      virgule_buffer_puts (b, " <input type=\"submit\" value=\"Update\">\n"
		   "</form>\n");
      return virgule_render_footer_send (vr);
    }
  else
    {
      virgule_render_header (vr, "Login", NULL);
      virgule_buffer_puts (b, "<p> Please login if you have an account.\n"
		   "Otherwise, feel free to <a href=\"new.html\">create a new account</a>. </p>\n"
		   "<p>If you have forgotten your password, fill in your user name, check the "
		   "\"forgot password\" box, and then click the login button. Your password "
		   "be mailed to the email address for your account.</p>\n"
		   "\n"
		   "<form method=\"POST\" action=\"loginsub.html\" accept-charset=\"UTF-8\">\n"
		   " <p> User name: <br/>\n"
		   "  <input name=\"u\" size=\"20\"/> \n"
		   " </p>\n"
		   " <p> Password: <br/>\n"
		   "  <input name=\"pass\" size=\"20\" type=\"password\"/> \n"
		   " </p>\n"
		   "<p> <input name=\"forgot\" type=\"checkbox\"> I forgot my password</p>"
		   " <input type=\"submit\" value=\"Login\"/>\n"
		   "</form>\n"
		   "<p> Note: This site uses cookies to store authentication\n"
		   "information. </p>\n");

      return virgule_render_footer_send (vr);
    }
}

static int
acct_newsub_serve (VirguleReq *vr)
{
  request_rec *r = vr->r;
  apr_pool_t *p = r->pool;
  Db *db = vr->db;
  apr_table_t *args;
  const char *u, *pass, *pass2;
  char *db_key, *db_key_lc;
  xmlDoc *profile;
  xmlNode *root, *tree;
  int status;
  char *cookie;
  int i;
  const char *date;
  char *u_lc;

  if (!vr->priv->allow_account_creation)
    return virgule_send_error_page (vr, "Account creation forbidden", "No new accounts may be created at this time.\n");

  virgule_db_lock_upgrade(vr->lock);
  args = virgule_get_args_table (vr);

  if (args == NULL)
    return virgule_send_error_page (vr, "Need form data", "This page requires a form submission. If you're not playing around manually with URLs, it suggests there's something wrong with the site.\n");

  u = apr_table_get (args, "u");
  pass = apr_table_get (args, "pass");
  pass2 = apr_table_get (args, "pass2");

#if 0
  virgule_buffer_printf (b, "Username: %s\n", u);
  virgule_buffer_printf (b, "Password: %s\n", pass);
  virgule_buffer_printf (b, "Password 2: %s\n", pass2);
#endif

  if (u == NULL || !u[0])
    return virgule_send_error_page (vr, "Specify a username",
			    "You must specify a username.");

  if (strlen (u) > 20)
    return virgule_send_error_page (vr, "Username too long",
			    "The username must be 20 characters or less.");

  /* sanity check user name */
  db_key = virgule_acct_dbkey (vr, u);
  if (db_key == NULL && vr->priv->allow_account_extendedcharset)
    return virgule_send_error_page (vr, "Invalid username",
			    "Username must begin with an alphanumeric character and contain only alphanumerics, spaces, dashes, underscores, or periods.");
  else if (db_key == NULL)
    return virgule_send_error_page (vr, "Invalid username",
			    "Username must contain only alphanumeric characters.");

  u_lc = apr_pstrdup (p, u);
  ap_str_tolower (u_lc);
  db_key_lc = virgule_acct_dbkey (vr, u_lc);
  profile = virgule_db_xml_get (p, db, db_key_lc);
  if (profile != NULL)
    return virgule_send_error_page (vr,
			    "Account already exists",
			    "The account name <tt>%s</tt> already exists.",
			    u);

  if (pass == NULL || pass2 == NULL)
    return virgule_send_error_page (vr,
			    "Specify a password",
			    "You must specify a password and enter it twice.");

  if (strcmp (pass, pass2))
    return virgule_send_error_page (vr,
			    "Passwords must match",
			    "The passwords must match. Have a cup of coffee and try again.");

  profile = virgule_db_xml_doc_new (p);

  root = xmlNewDocNode (profile, NULL, "profile", NULL);
  profile->xmlRootNode = root;

  date = virgule_iso_now (p);
  tree = xmlNewChild (root, NULL, "date", date);
  tree = xmlNewChild (root, NULL, "auth", NULL);
  xmlSetProp (tree, "pass", pass);
  cookie = virgule_rand_cookie (p);
#if 0
  virgule_buffer_printf (b, "Cookie is %s\n", cookie);
#endif
  xmlSetProp (tree, "cookie", cookie);

  tree = xmlNewChild (root, NULL, "info", NULL);
  for (i = 0; prof_fields[i].description; i++)
    {
      const char *val;
      val = apr_table_get (args, prof_fields[i].attr_name);
      if (val == NULL)
        continue;
      if (virgule_is_input_valid(val))
        xmlSetProp (tree, prof_fields[i].attr_name, val);
      else
        return virgule_send_error_page (vr,
                                "Invalid Characters Submitted",
                                "Only valid characters that use valid UTF-8 sequences may be submitted.");
    }

  status = virgule_db_xml_put (p, db, db_key, profile);
  if (status)
    return virgule_send_error_page (vr,
			    "Error storing account profile",
			    "There was an error storing the account profile. This means there's something wrong with the site.");

  acct_set_cookie (vr, u, cookie, 86400 * 365);

  vr->u = u;

  virgule_add_recent (p, db, "recent/acct.xml", u, 50, 0);

  /* store lower case alias if necessary */
  if (! (strcmp (u_lc, u) == 0))
    {
      profile = virgule_db_xml_doc_new (p);

      root = xmlNewDocNode (profile, NULL, "profile", NULL);
      profile->xmlRootNode = root;
      tree = xmlNewChild (root, NULL, "alias", NULL);
      xmlSetProp (tree, "link", u);

      status = virgule_db_xml_put (p, db, db_key_lc, profile);
    }

  return virgule_send_error_page (vr,
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
	  *ret2 = apr_psprintf (p, "Account <tt>%s</tt> does not exist. Try the <a href=\"new.html\">new account creation</a> page.", u);
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

      u = virgule_xml_get_prop (p, tree, "link");
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
      *ret2 = apr_psprintf (p, "Account <tt>%s</tt> is missing its auth field. This is a problem with the server.", u);
      return 0;
    }

  stored_pass = xmlGetProp (tree, "pass");

  if (strcmp (pass, stored_pass))
    {
      xmlFree (stored_pass);
      *ret1 = "Incorrect password";
      *ret2 = "Incorrect password, try again.";
      return 0;
    }

  xmlFree (stored_pass);
  cookie = xmlGetProp (tree, "cookie");

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
     return virgule_send_error_page (vr, "Error", "There was an error sending mail to <tt>%s</tt>.\n", mail);

  fprintf(fp,"To: %s\n", mail);
  fprintf(fp,"From: %s\n", vr->priv->admin_email);
  fprintf(fp,"Subject: Your %s password\n\n", vr->priv->site_name);
  fprintf(fp,"You, or someone else, recently asked for a password ");
  fprintf(fp,"reminder to be sent for your %s account.\n\n", vr->priv->site_name);
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
    return virgule_send_error_page (vr, "Need form data", " This page requires a form submission. If you're not playing around manually with URLs, it suggests there's something wrong with the site.\n");

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
          return virgule_send_error_page (vr, "Invalid username", "Username contain invalid characters.");
	}

      /* verify that user name is in DB */
      profile = virgule_db_xml_get (p, vr->db, db_key);
      if (profile == NULL)
        {
          return virgule_send_error_page (vr, "Account does not exist", "The specified account could not be found.");
	}

      /* check for an account alias */
      tree = virgule_xml_find_child (profile->xmlRootNode, "alias");
      if (tree != NULL) 
        {
          db_key_lc = virgule_acct_dbkey (vr, virgule_xml_get_prop (p, tree, "link"));
          profile = virgule_db_xml_get (p, vr->db, db_key_lc);
        }
      
      /* Get the email and password. */
      tree = virgule_xml_find_child (profile->xmlRootNode, "info");
      mail = virgule_xml_get_prop (p, tree, "email");
      if (mail == NULL)
	{
	  return virgule_send_error_page(vr,
				 "Email not found",
				 "The account name <tt>%s</tt> doesn't have an email address associated with it.",
				 u);
	}

      tree = virgule_xml_find_child (profile->xmlRootNode, "auth");
      pass = virgule_xml_get_prop (p, tree, "pass");
      if (pass == NULL)
	{
	  return virgule_send_error_page(vr,
				 "Password not found",
				 "The account name <tt>%s</tt> doesn't have an email password associated with it.",
				 u);
	}

      /* Mail it. */
      send_email( vr, mail, u, pass );

      /* Tell the user */
      return virgule_send_error_page( vr, "Password mailed.",
			      "The password for <tt>%s</tt> has now been mailed to <tt>%s</tt>", 
			      u, mail );
  }

  if (!virgule_acct_login (vr, u, pass, &ret1, &ret2))
      return virgule_send_error_page (vr, ret1, ret2);

  u = ret1;
  cookie = ret2;
  
  acct_set_cookie (vr, u, cookie, 86400 * 365);
  
  vr->u = u;

  return virgule_send_error_page (vr,
			  "Login ok",
			  "Login to account <tt>%s</tt> ok.\n", u);

}

static int
acct_logout_serve (VirguleReq *vr)
{

  virgule_auth_user (vr);

  if (vr->u)
    {
      acct_set_cookie (vr, vr->u, "", -86400);
      return virgule_send_error_page (vr,
			      "Logged out",
			      "Logout of account <tt>%s</tt> ok.\n", vr->u);
    }
  else
    return virgule_send_error_page (vr,
			    "Already logged out",
			    "You were already logged out.\n");

}

static int
acct_update_serve (VirguleReq *vr)
{
  apr_pool_t *p = vr->r->pool;
  apr_table_t *args;

  virgule_db_lock_upgrade(vr->lock);
  virgule_auth_user (vr);

  args = virgule_get_args_table (vr);

  if (vr->u)
    {
      char *db_key;
      xmlDoc *profile;
      xmlNode *tree;
      int i;
      int status;

      db_key = virgule_acct_dbkey (vr, vr->u);
      profile = virgule_db_xml_get (p, vr->db, db_key);

      tree = virgule_xml_ensure_child (profile->xmlRootNode, "info");

      for (i = 0; prof_fields[i].description; i++)
	{
	  const char *val;
	  val = apr_table_get (args, prof_fields[i].attr_name);
	  if (val == NULL && prof_fields[i].flags & PROFILE_BOOLEAN)
	    val = "off";
          if (virgule_is_input_valid(val))
	    {
#if 0
	      g_print ("Setting field %s to %s\n",
		       prof_fields[i].attr_name, val);
#endif
              xmlSetProp (tree, prof_fields[i].attr_name, val);
	    }
          else
            return virgule_send_error_page (vr,
                                    "Invalid Characters Submitted",
                                    "Only valid characters that use valid UTF-8 sequences may be submitted.");
	}


      status = virgule_db_xml_put (p, vr->db, db_key, profile);
      if (status)
	return virgule_send_error_page (vr,
				"Error storing account profile",
				"There was an error storing the account profile. This means there's something wrong with the site.");
      apr_table_add (vr->r->headers_out, "refresh",
		    apr_psprintf(p, "0;URL=/person/%s/", vr->u));
      return virgule_send_error_page (vr,
			      "Updated",
			      "Updates to account <a href=\"../person/%s/\">%s</a> ok",
			      ap_escape_uri(vr->r->pool,vr->u), vr->u);
    }
  else
    return virgule_send_error_page (vr,
			    "Not logged in",
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
	    continue;
          profile = virgule_db_xml_get (vr->r->pool, vr->db, db_key);
          if (profile == NULL)
            continue;
          tree = virgule_xml_find_child (profile->xmlRootNode, "info");
          if (tree != NULL)
	    {
	      givenname = virgule_xml_get_prop (vr->r->pool, tree, "givenname");
	      surname = virgule_xml_get_prop (vr->r->pool, tree, "surname");
	    }
          virgule_db_xml_free (vr->r->pool, vr->db, profile);

          virgule_render_cert_level_begin (vr, user, CERT_STYLE_SMALL);
          virgule_buffer_printf (vr->b, "<a href=\"%s/\">%s</a>, %s %s, %s\n",
	                 ap_escape_uri(vr->r->pool,user), user,
			 givenname, surname,
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
  virgule_buffer_puts (vr->b, "<p> Go to <x>a person</x>'s page to certify them. </p>\n");
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
      profile = virgule_db_xml_get (p, db, db_key);
      tree = virgule_xml_find_child (profile->xmlRootNode, "certs");
      if (tree == NULL)
	continue;
      if (virgule_cert_level_from_name (vr, virgule_req_get_tmetric_level (vr, issuer)) < threshold)
	continue;
      for (cert = tree->children; cert != NULL; cert = cert->next)
	{
	  if (cert->type == XML_ELEMENT_NODE &&
	      !strcmp (cert->name, "cert"))
	    {
	      char *cert_subj;

	      cert_subj = virgule_xml_get_prop (p, cert, "subj");
	      if (cert_subj &&
		  virgule_cert_level_from_name (vr, virgule_req_get_tmetric_level (vr, cert_subj)) >= threshold)
		{
		  char *cert_level;

                  cert_level = virgule_xml_get_prop (p, cert, "level");
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

static int
acct_person_diary_xml_serve (VirguleReq *vr, char *u)
{
  Buffer *b = vr->b;
  xmlDocPtr doc;
  xmlChar *mem;
  int size;

  doc = xmlNewDoc ("1.0");

  doc->xmlRootNode = xmlNewDocNode (doc, NULL, "tdif", NULL);
  virgule_diary_export (vr, doc->xmlRootNode, u);

  xmlDocDumpFormatMemory (doc, &mem, &size, 1);
  virgule_buffer_write (b, mem, size);
  xmlFree (mem);
  xmlFreeDoc (doc);
  return virgule_send_response (vr);
}

static int
acct_person_diary_rss_serve (VirguleReq *vr, char *u)
{
  Buffer *b = vr->b;
  xmlDocPtr doc;
  xmlChar *mem;
  int size;

  doc = xmlNewDoc ("1.0");

  vr->r->content_type = "text/xml; charset=UTF-8";

  xmlCreateIntSubset(doc, "rss",
		    "-//Netscape Communications//DTD RSS 0.91//EN",
		    "http://my.netscape.com/publish/formats/rss-0.91.dtd");

  doc->xmlRootNode = xmlNewDocNode (doc, NULL, "rss", NULL);
  xmlSetProp (doc->xmlRootNode, "version", "0.91");
  virgule_diary_rss_export (vr, doc->xmlRootNode, u);
  xmlDocDumpFormatMemory (doc, &mem, &size, 1);
  virgule_buffer_write (b, mem, size);
  xmlFree (mem);
  xmlFreeDoc (doc);
  return virgule_send_response (vr);
}

static int
acct_person_diary_serve (VirguleReq *vr, char *u)
{
  apr_pool_t *p = vr->r->pool;
  Buffer *b = vr->b;
  char *str;
  apr_table_t *args;
  int start;

  args = virgule_get_args_table (vr);
  if (args == NULL)
    start = -1;
  else
    start = atoi (apr_table_get (args, "start"));

  str = apr_psprintf (p, "Diary for %s", u);

  virgule_render_header (vr, str, NULL);
  if (start == -1)
    virgule_buffer_printf (b, "<p> Recent blog entries for <a href=\"%s/person/%s/\">%s</a>: </p>\n",
		   vr->prefix, ap_escape_uri(vr->r->pool, u), u);
  else
    virgule_buffer_printf (b, "<p> Older blog entries for <a href=\"%s/person/%s/\">%s</a> (starting at number %d): </p>\n",
		   vr->prefix, ap_escape_uri(vr->r->pool, u), u, start);

  virgule_diary_render (vr, u, 10, start);

  return virgule_render_footer_send (vr);
}

static int
acct_person_serve (VirguleReq *vr, const char *path)
{
  request_rec *r = vr->r;
  apr_pool_t *p = r->pool;
  char *q;
  char *u;
  char *db_key;
  xmlDoc *profile, *staff;
  xmlNode *tree, *lastlogin;
  Buffer *b = vr->b;
  char *str;
  char *surname, *givenname;
  char *url;
  char *date = NULL;
  char *notes;
  int any;
  int observer = FALSE;
  char *err;
  char *first;

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

  if (!strcmp (q + 1, "diary.xml"))
    return acct_person_diary_xml_serve (vr, u);

  if (!strcmp (q + 1, "rss.xml"))
    return acct_person_diary_rss_serve (vr, u);

  if (q[1] != '\0')
    return virgule_send_error_page (vr,
			    "Extra junk",
			    "Extra junk after <x>person</x>'s name not allowed.");

  db_key = virgule_acct_dbkey (vr, u);
  if (db_key == NULL)
    {
      return virgule_send_error_page (vr, "User name not valid", "The user name doesn't even look valid, much less exist in the database.");
    }
    
  profile = virgule_db_xml_get (p, vr->db, db_key);
  if (profile == NULL)
    return virgule_send_error_page (vr,
			    "<x>Person</x> not found",
			    "Account <tt>%s</tt> was not found.", u);

  tree = virgule_xml_find_child (profile->xmlRootNode, "alias");
  if (tree != NULL)
    {
      apr_table_add (r->headers_out, "Location",
		    apr_pstrcat (p, vr->prefix, "/person/",
				virgule_xml_get_prop (p, tree, "link"), "/", NULL));
      return HTTP_MOVED_PERMANENTLY;
				
    }

  str = apr_psprintf (p, "Personal info for %s", u);
  virgule_render_header (vr, str,
		"<link rel=\"alternate\" type=\"application/rss+xml\" "
		"title=\"RSS\" href=\"rss.xml\" />\n");

  if (strcmp (virgule_req_get_tmetric_level (vr, u),
	       virgule_cert_level_to_name (vr, CERT_LEVEL_NONE)))
    {
      virgule_render_cert_level_begin (vr, u, CERT_STYLE_SMALL);
      virgule_buffer_printf (b, "This <x>person</x> is currently certified at %s level.\n", virgule_req_get_tmetric_level (vr, u));
      virgule_render_cert_level_end (vr, CERT_STYLE_SMALL);
    }
  else
    observer = TRUE;

  lastlogin = virgule_xml_find_child(profile->xmlRootNode, "lastlogin");
  if(lastlogin) 
    date = virgule_xml_get_prop (p, lastlogin, "date");
  if(!date)
    date = "N/A";
  
  any = 0;
  tree = virgule_xml_find_child (profile->xmlRootNode, "info");
  if (tree)
    {
      givenname = virgule_xml_get_prop (p, tree, "givenname");
      surname = virgule_xml_get_prop (p, tree, "surname");
      virgule_buffer_printf (b, "<p> Name: %s %s<br />Last login: %s</p>\n",
		     givenname ? virgule_nice_text(p, givenname) : "",
		     surname ? virgule_nice_text(p, surname) : "", date);

      url = virgule_xml_get_prop (p, tree, "url");
      if (url && url[0])
	{
	  char *url2;
	  char *colon;

	  url2 = url;
	  colon = strchr (url, ':');
	  if (!colon || colon[1] != '/' || colon[2] != '/')
	    url2 = apr_pstrcat (p, "http://", url, NULL);
	  virgule_buffer_printf (b, "<p> Homepage: <a href=\"%s\">%s</a> </p>\n",
			 url2, virgule_nice_text (p, url));
	  any = 1;
	}
      notes = virgule_xml_get_prop (p, tree, "notes");
      if (notes && notes[0])
	{
	  if(observer)
  	    virgule_buffer_printf (b, "<p> <b>Notes:</b> %s </p>\n", virgule_nice_htext (vr, virgule_strip_a (vr, notes), &err));
	  else 
	    virgule_buffer_printf (b, "<p> <b>Notes:</b> %s </p>\n", virgule_nice_htext (vr, notes, &err));
	  any = 1;
	}
    }
  if (!any)
    virgule_buffer_puts (b, "<p> No personal information is available. </p>\n");

  /* Render staff listings */
  first = "<p> This <x>person</x> is: </p>\n"
    "<ul>\n";
  db_key = apr_psprintf (p, "acct/%s/staff-person.xml", u);

  staff = virgule_db_xml_get (p, vr->db, db_key);
  if (staff != NULL)
    {
      for (tree = staff->xmlRootNode->children; tree != NULL; tree = tree->next)
	{
	  char *name;
	  char *type;

	  name = virgule_xml_get_prop (p, tree, "name");
	  type = virgule_xml_get_prop (p, tree, "type");

	  if (! !strcmp (type, "None"))
	    {
	      virgule_buffer_puts (b, first);
	      first = "";
	      virgule_buffer_printf (b, "<li>a %s on <x>project</x> %s.\n",
			     type, virgule_render_proj_name (vr, name));
	    }
	}
    }
  if (first[0] == 0)
    virgule_buffer_puts (b, "</ul>\n");

  virgule_buffer_printf (b, "<p> Recent blog entries for %s: <br />\n", u);
  virgule_buffer_printf (b, "<a href=\"rss.xml\"><img src=\"/images/rss.png\" width=36 height=20 border=0 alt=\"RSS\" /></a></p>\n", u);

  virgule_diary_render (vr, u, 5, -1);

  /* Browse certifications */
  virgule_buffer_puts (b, "<a name=\"certs\">&nbsp;</a>\n");
  tree = virgule_xml_find_child (profile->xmlRootNode, "certs");
  if (tree)
    {
      xmlNode *cert;
      int any = 0;
      for (cert = tree->children; cert != NULL; cert = cert->next)
	if (cert->type == XML_ELEMENT_NODE &&
	    !strcmp (cert->name, "cert"))
	  {
	    char *subject, *level;
	    subject = xmlGetProp (cert, "subj");
	    level = xmlGetProp (cert, "level");
	    if (strcmp (level, virgule_cert_level_to_name (vr, 0)))
	      {
		if (!any)
		  {
		    virgule_buffer_puts (b, "<p> This <x>person</x> has certified others as follows: </p>\n"
				 "<ul>\n");
		    any = 1;
		  }
	      virgule_buffer_printf (b, "<li>%s certified <a href=\"../%s/\">%s</a> as %s\n",
			     u, ap_escape_uri(vr->r->pool, subject), subject, level);
	      }
	  }
      if (any)
	virgule_buffer_puts (b, "</ul>\n");
    }

  tree = virgule_xml_find_child (profile->xmlRootNode, "certs-in");
  if (tree)
    {
      xmlNode *cert;
      int any = 0;
      for (cert = tree->children; cert != NULL; cert = cert->next)
	if (cert->type == XML_ELEMENT_NODE &&
	    !strcmp (cert->name, "cert"))
	  {
	    char *issuer, *level;
	    issuer = xmlGetProp (cert, "issuer");
	    level = xmlGetProp (cert, "level");
	    if (strcmp (level, virgule_cert_level_to_name (vr, 0)))
	      {
		if (!any)
		  {
		    virgule_buffer_puts (b, "<p> Others have certified this <x>person</x> as follows: </p>\n"
		   "<ul>\n");
		    any = 1;
		  }
		virgule_buffer_printf (b, "<li><a href=\"../%s/\">%s</a> certified %s as %s\n",
			       issuer, issuer, u, level);
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
		     "<p><b>Note</b>: By certifying a user you are making a "
		     "public statment that you know this person and can "
		     "vouch for their identity.</p>"
		     "<p> See the <a href=\"%s/certs.html\">Certification</a> overview for more information.</p>\n",
		     u, vr->prefix);

      if (vr->priv->render_diaryratings) 
	virgule_rating_diary_form (vr, u);
    }

  return virgule_render_footer_send (vr);
}

static int
acct_certify_serve (VirguleReq *vr)
{
  apr_table_t *args;
  const char *subject;
  const char *level;
  int status;

  virgule_db_lock_upgrade(vr->lock);
  virgule_auth_user (vr);

  args = virgule_get_args_table (vr);

  if (vr->u)
    {
      subject = apr_table_get (args, "subject");
      level = apr_table_get (args, "level");

      if (!strcmp(subject,vr->u))
        return virgule_send_error_page(vr,
	                       "Error in certification",
			       "Sorry, you can't certify yourself.");

      status = virgule_cert_set (vr, vr->u, subject,
			 virgule_cert_level_from_name (vr, level));

      if (status)
	return virgule_send_error_page (vr,
				"Error storing certificate",
				"There was an error storing the certificate. This means there's something wrong with the site.");
      apr_table_add (vr->r->headers_out, "refresh",
		    apr_psprintf(vr->r->pool, "0;URL=/person/%s/#certs",
				subject));
      return virgule_send_error_page (vr,
			      "Updated",
			      "Certification of <a href=\"../person/%s/\">%s</a> to %s level ok.",
			      ap_escape_uri(vr->r->pool,subject), subject, level);
    }
  else
    return virgule_send_error_page (vr,
			    "Not logged in",
			    "You need to be logged in to certify another <x>person</x>.");
}

/**
 * acct_kill: Remove a user account. Before an account is removed, all cert
 * and cert-in records are cleared. An account or account-alias may be
 # passed to this function as an argument. If an account-alias exists for
 * the account, it will also be removed.
 **/
static void
acct_kill(VirguleReq *vr, const char *u)
{
  int n;
  const char *user;
  char *db_key, *db_key2, *user_alias, *diary;
  apr_pool_t *p = vr->r->pool;
  xmlDoc *profile, *staff, *entry;
  xmlNode *tree, *cert, *alias;
  
  db_key = virgule_acct_dbkey(vr, u);
  profile = virgule_db_xml_get(p, vr->db, db_key);
  alias = virgule_xml_find_child (profile->xmlRootNode, "alias");

  if (alias != NULL) /* If this is the alias, kill it and find the username */
    {
      user = virgule_xml_get_prop (p, alias, "link");
      virgule_db_xml_free (p, vr->db, profile);
      virgule_db_del (vr->db, db_key);
      db_key = virgule_acct_dbkey (vr, user);
    }
  else               /* If this is the username, check for and kill alias */
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
      char *subject, *level;
      for (cert = tree->children; cert != NULL; cert = cert->next)
        if (cert->type == XML_ELEMENT_NODE && ! strcmp (cert->name, "cert"))
	  {
            subject = xmlGetProp (cert, "subj");
	    level = xmlGetProp (cert, "level");
	    virgule_cert_set (vr, user, subject, CERT_LEVEL_NONE);
	    xmlFree(subject);
	    xmlFree(level);
	  }
    }  

  /* Clear cert-in records */
  tree = virgule_xml_find_child (profile->xmlRootNode, "certs-in");
  if (tree)
    {
      char *issuer, *level;
      for (cert = tree->children; cert != NULL; cert = cert->next)
        if (cert->type == XML_ELEMENT_NODE && ! strcmp (cert->name, "cert"))
	  {
	    issuer = xmlGetProp (cert, "issuer");
	    level = xmlGetProp (cert, "level");
	    virgule_cert_set (vr, issuer, user, CERT_LEVEL_NONE);
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
	  name = virgule_xml_get_prop (p, tree, "name");
	  virgule_proj_set_relation(vr,name,user,"None");
	}
      virgule_db_xml_free (p, vr->db, staff);
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
          virgule_db_xml_free (p, vr->db, entry);
	}
    }

  /* Remove the profile and account */
  virgule_db_del (vr->db, db_key);
  virgule_db_xml_free(p, vr->db, profile);
}

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
    lastlogin = xmlNewChild (root, NULL, "lastlogin", NULL);
    xmlSetProp (lastlogin, "date", newdate);
  }
  else
  {
    xmlSetProp (lastlogin, "date", newdate);
  }
  
  virgule_db_xml_put (p, vr->db, db_key, profile);  
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
  if ((p = virgule_match_prefix (vr->uri, "/person/")) != NULL)
    return acct_person_serve (vr, p);
  if (!strcmp (vr->uri, "/acct/certify.html"))
    return acct_certify_serve (vr);
  return DECLINED;
}