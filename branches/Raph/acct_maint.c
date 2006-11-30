/* A module for simple account maintenance. */

#include <time.h>
#include <ctype.h>
#include "httpd.h"

#include <tree.h>
#include <parser.h>
#include <xmlmemory.h>

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
acct_set_lastread(VirguleReq *vr, const char *section, const char *location, int last_read)
{
  pool *p = vr->r->pool;
  Db *db = vr->db;
  char *db_key;
  xmlDoc *profile;
  xmlNode *tree, *msgptr;
  int status;

  db_lock_upgrade(vr->lock);
  auth_user(vr);
  if (vr->u == NULL)
    return 0;

  db_key = acct_dbkey (p, vr->u);
  profile = db_xml_get (p, db, db_key);
  if (profile == NULL)
    return -1;

  tree = xml_ensure_child (profile->root, ap_psprintf(p, "%spointers", section));

  for (msgptr = tree->childs; msgptr != NULL; msgptr = msgptr->next)
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

  xmlSetProp (msgptr, "num", ap_psprintf(p, "%d", last_read));
  xmlSetProp (msgptr, "date", iso_now(p));

  status = db_xml_put (p, db, db_key, profile);
  db_xml_free (p, db, profile);

  return status;
}

int
acct_get_lastread(VirguleReq *vr, const char *section, const char *location)
{
  pool *p = vr->r->pool;
  Db *db = vr->db;
  char *db_key;
  xmlDoc *profile;
  xmlNode *tree, *msgptr;

  auth_user(vr);
  if (vr->u == NULL)
    return -1;

  db_key = acct_dbkey (p, vr->u);
  profile = db_xml_get (p, db, db_key);
  if (profile == NULL)
    return -1;

  tree = xml_find_child (profile->root, ap_psprintf(p, "%spointers", section));
  if (tree == NULL)
    return -1;

  for (msgptr = tree->childs; msgptr != NULL; msgptr = msgptr->next)
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
acct_get_num_old(VirguleReq *vr)
{
  auth_user(vr);

  if (vr->u)
    {
      pool *p = vr->r->pool;
      char *db_key;
      xmlDoc *profile;
      xmlNode *tree;
      char *num_old;

      db_key = acct_dbkey (p, vr->u);
      profile = db_xml_get (p, vr->db, db_key);
      tree = xml_find_child (profile->root, "info");

      num_old = xmlGetProp (tree, "numold");

      if (num_old == NULL || *num_old == '\0')
	return 30;
      else
	return atoi(num_old);
    }

  return -1;
}

char *
acct_get_lastread_date(VirguleReq *vr, const char *section, const char *location)
{
  pool *p = vr->r->pool;
  Db *db = vr->db;
  char *db_key;
  xmlDoc *profile;
  xmlNode *tree, *msgptr;
  char *date;


  auth_user(vr);
  if (vr->u == NULL)
    return NULL;

  db_key = acct_dbkey (p, vr->u);
  profile = db_xml_get (p, db, db_key);
  if (profile == NULL)
    return NULL;

  tree = xml_find_child (profile->root, ap_psprintf(p, "%spointers", section));
  if (tree == NULL)
    return NULL;

  for (msgptr = tree->childs; msgptr != NULL; msgptr = msgptr->next)
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
		  db_xml_free (p, db, profile);
		  return date;
		}
	    }
	}
    }

  db_xml_free (p, db, profile);
  return "1970-01-01 00:00:00"; 
}

/**
 * validate_username: Ensure that username is valid.
 * @u: Putative username.
 *
 * Return value: NULL if valid, or reason as string if not.
 **/
char *
validate_username (const char *u)
{
  int len;
  int i;

  if (u == NULL || !u[0])
    return "You must specify a username.";

  len = strlen (u);
  if (len > 20)
    return "The username must be 20 characters or less.";

  for (i = 0; i < len; i++)
    {
      if (!isalnum (u[i]))
	return "The username must contain only alphanumeric characters.";
    }

  return NULL;
}

/* Make the db key. Sanity check the username. */
char *
acct_dbkey (pool *p, const char *u)
{
  if (validate_username (u) != NULL)
    return NULL;

  return ap_pstrcat (p, "acct/", u, "/profile.xml", NULL);
}

static void
acct_set_cookie (VirguleReq *vr, const char *u, const char *cookie,
		 time_t lifetime)
{
  request_rec *r = vr->r;
  char *id_cookie, *exp_date;
  time_t exp_time;

  id_cookie = ap_pstrcat (r->pool, u, ":", cookie, NULL);
  exp_time = time (NULL) + lifetime;
  exp_date = ap_ht_time (r->pool, exp_time, "%A, %d-%b-%Y %H:%M:%S %Z", 1);

  ap_table_add (r->headers_out, "Set-Cookie",
		ap_psprintf (r->pool, "id=%s; path=/; Expires=%s",
			     id_cookie, exp_date));
}

static int
acct_index_serve (VirguleReq *vr)
{
  pool *p = vr->r->pool;
  Buffer *b = vr->b;
  int i;

  auth_user (vr);

  if (vr->u)
    {
      char *db_key;
      xmlDoc *profile;
      xmlNode *tree;
      char *value;

      db_key = acct_dbkey (p, vr->u);
      profile = db_xml_get (p, vr->db, db_key);
      tree = xml_find_child (profile->root, "info");

      render_header (vr, "Welcome");
      buffer_printf (b, "<p> Welcome, <tt>%s</tt>. The range of stuff you can "
		     "do with your account is increasing. "
		     "Now you can <a href=\"%s/certs.html\">certify</a>\n"
		     "other <x>people</x> on the site. </p>\n",
		     vr->u, vr->prefix);


      buffer_printf (b, "<p> By all means, feel free to link to your <a href=\"%s/person/%s/\">publicly accessible page</a> from your homepage. </p>",
		     vr->prefix, vr->u);

      if (req_ok_to_post (vr))
	{
	  const char *level;

	  level = req_get_tmetric_level (vr, vr->u);
	  render_cert_level_begin (vr, vr->u, CERT_STYLE_SMALL);
	  buffer_printf (b, "You can post <a href=\"../diary/\">diary</a> entries and <a href=\"../article/post.html\"><x>articles</x></a>. You are currently certified at ");
	  buffer_puts (b, level);
	  buffer_puts (b, " level.\n");
	  render_cert_level_end (vr, CERT_STYLE_SMALL);
	}
      else
	buffer_printf (b, "<p> You can post <a href=\"../diary/\">diary</a> entries. However, you can't post <x>articles</x> yet, as you have to be certified at %s level or higher to post. See the <a href=\"%s/certs.html\">certification overview</a> for more information. </p>\n", cert_level_to_name (vr, 1), vr->prefix);

      buffer_puts (b, "<p> You can <a href=\"logout.html\">logout</a>. </p>\n");
      if (vr->projstyle == PROJSTYLE_NICK)
	buffer_puts (b, "<p> You can <a href=\"/proj/updatepointers.html\">mark all messages as read</a>. </p>\n");
      buffer_puts (b, "<p> Or you can update your account info: </p>\n");
      buffer_puts (b, "<form method=\"POST\" action=\"update.html\">\n");
      for (i = 0; prof_fields[i].description; i++)
	{
	  if (vr->projstyle == PROJSTYLE_RAPH &&
	      !strcmp(prof_fields[i].attr_name, "numold"))
	    continue;

	  value = NULL;
	  if (tree)
	    value = xmlGetProp (tree, prof_fields[i].attr_name);

	  buffer_printf (b, "<p> %s: <br>\n", prof_fields[i].description);
	  if (prof_fields[i].flags & PROFILE_BOOLEAN)
	    buffer_printf (b, "<input name=\"%s\" type=checkbox %s> </p>\n",
			   prof_fields[i].attr_name,
			   value ? (strcmp (value, "on") ? "" : " checked") : "");
	  else if (prof_fields[i].flags & PROFILE_TEXTAREA)
	    buffer_printf (b, "<textarea name=\"%s\" cols=%d rows=%d wrap=soft>%s</textarea> </p>\n",
			   prof_fields[i].attr_name,
			   prof_fields[i].size / 1000,
			   prof_fields[i].size % 1000,
			   value ? ap_escape_html (p, value) : "");
	  else
	    buffer_printf (b, "<input name=\"%s\" size=%d value=\"%s\"> </p>\n",
			   prof_fields[i].attr_name, prof_fields[i].size,
			   value ? ap_escape_html (p, value) : "");
	  if (value != NULL)
	    xmlFree (value);
	}
      buffer_puts (b, " <input type=\"submit\" value=\"Update\">\n"
		   "</form>\n");
      return render_footer_send (vr);
    }
  else
    {
      render_header (vr, "Login");
      buffer_puts (b, "<p> Please login if you have an account.\n"
		   "Otherwise, feel free to <a href=\"new.html\">create a new account</a>. </p>\n"
		   "\n"
		   "<form method=\"POST\" action=\"loginsub.html\">\n"
		   " <p> User name: <br/>\n"
		   "  <input name=\"u\" size=\"20\"/> \n"
		   " </p>\n"
		   " <p> Passphrase: <br/>\n"
		   "  <input name=\"pass\" size=\"20\" type=\"password\"/> \n"
		   " </p>\n"
		   " <input type=\"submit\" value=\"Login\"/>\n"
		   "</form>\n"
		   "<p> Note: This site uses cookies to store authentication\n"
		   "information. </p>\n");

      return render_footer_send (vr);
    }
}

static int
acct_newsub_serve (VirguleReq *vr)
{
  request_rec *r = vr->r;
  pool *p = r->pool;
  Db *db = vr->db;
  table *args;
  const char *u, *pass, *pass2;
  char *db_key, *db_key_lc;
  xmlDoc *profile;
  xmlNode *root, *tree;
  int status;
  char *cookie;
  int i;
  const char *date;
  char *u_lc;

  if (!vr->allow_account_creation)
    return send_error_page (
      vr, "Account creation forbidden",
      "No new accounts may be created on this site.\n");

  db_lock_upgrade(vr->lock);
  args = get_args_table (vr);

  if (args == NULL)
    return send_error_page (vr, "Need form data", " This page requires a form submission. If you're not playing around manually with URLs, it suggests there's something wrong with the site.\n");

  u = ap_table_get (args, "u");
  pass = ap_table_get (args, "pass");
  pass2 = ap_table_get (args, "pass2");

#if 0
  buffer_printf (b, "Username: %s\n", u);
  buffer_printf (b, "Password: %s\n", pass);
  buffer_printf (b, "Password 2: %s\n", pass2);
#endif

  if (u == NULL || !u[0])
    return send_error_page (vr, "Specify a username",
			    "You must specify a username.");

  if (strlen (u) > 20)
    return send_error_page (vr, "Username too long",
			    "The username must be 20 characters or less.");

  /* sanity check user name */
  db_key = acct_dbkey (p, u);
  if (db_key == NULL)
    return send_error_page (vr, "Invalid username",
			    "Username must contain only alphanumeric characters.");

  u_lc = ap_pstrdup (p, u);
  ap_str_tolower (u_lc);
  db_key_lc = acct_dbkey (p, u_lc);
  profile = db_xml_get (p, db, db_key_lc);
  if (profile != NULL)
    return send_error_page (vr,
			    "Account already exists",
			    "The account name <tt>%s</tt> already exists.",
			    u);

  if (pass == NULL || pass2 == NULL)
    return send_error_page (vr,
			    "Specify a password",
			    "You must specify a password and enter it twice.");

  if (strcmp (pass, pass2))
    return send_error_page (vr,
			    "Passwords must match",
			    "The passwords must match. Have a cup of coffee and try again.");

  profile = db_xml_doc_new (p);

  root = xmlNewDocNode (profile, NULL, "profile", NULL);
  profile->root = root;

  date = iso_now (p);
  tree = xmlNewChild (root, NULL, "date", date);
  tree = xmlNewChild (root, NULL, "auth", NULL);
  xmlSetProp (tree, "pass", pass);
  cookie = rand_cookie (p);
#if 0
  buffer_printf (b, "Cookie is %s\n", cookie);
#endif
  xmlSetProp (tree, "cookie", cookie);

  tree = xmlNewChild (root, NULL, "info", NULL);
  for (i = 0; prof_fields[i].description; i++)
    {
      const char *val;
      val = ap_table_get (args, prof_fields[i].attr_name);
      if (val)
	  xmlSetProp (tree, prof_fields[i].attr_name, val);
    }

  status = db_xml_put (p, db, db_key, profile);
  if (status)
    return send_error_page (vr,
			    "Error storing account profile",
			    "There was an error storing the account profile. This means there's something wrong with the site.");

  acct_set_cookie (vr, u, cookie, 86400 * 365);

  vr->u = u;

  add_recent (p, db, "recent/acct.xml", u, 50, 0);

  /* store lower case alias if necessary */
  if (! (strcmp (u_lc, u) == 0))
    {
      profile = db_xml_doc_new (p);

      root = xmlNewDocNode (profile, NULL, "profile", NULL);
      profile->root = root;
      tree = xmlNewChild (root, NULL, "alias", NULL);
      xmlSetProp (tree, "link", u);

      status = db_xml_put (p, db, db_key_lc, profile);
    }

  return send_error_page (vr,
			  "Account created",
			  "Account <a href=\"%s/person/%s/\">%s</a> created.\n",
			  vr->prefix, u, u);
}

/* Success: return 1, set *ret1 to username and *ret2 to cookie
 * Failure: return 0, set *ret1 and *ret2 to short and long error messages
 * FIXME: this function's interface is _nasty_
 */
int
acct_login (VirguleReq *vr, const char *u, const char *pass,
	    const char **ret1, const char **ret2)
{
  request_rec *r = vr->r;
  pool *p = r->pool;
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
      db_key = acct_dbkey (p, u);
      if (db_key == NULL)
        {
	  *ret1 = "Invalid username";
	  *ret2 = "Username must contain only alphanumeric characters.";
	  return 0;
        }

      profile = db_xml_get (p, db, db_key);
      if (profile == NULL)
        {
	  *ret1 = "Account does not exist";
	  *ret2 = ap_psprintf (p, "Account <tt>%s</tt> does not exist. Try the <a href=\"new.html\">new account creation</a> page.", u);
	  return 0;
        }
      
#if 0
      buffer_printf (b, "Profile: %s\n", profile->name);
      return buffer_send_response (r, b);
#endif

      root = profile->root;

      tree = xml_find_child (root, "alias");
      if (tree == NULL)
	break;

      u = xml_get_prop (p, tree, "link");
      db_key = acct_dbkey (p, u);
      profile = db_xml_get (p, db, db_key);
    }

  if (i == n_iter_max)
    {
      *ret1 = "Alias loop";
      *ret2 = ap_psprintf (p, "More than %d levels of alias indirection from %s, indicating an alias loop. This is a problem with the server.",
			n_iter_max, u);
      return 0;
    }

  tree = xml_find_child (root, "auth");

  if (tree == NULL)
    {
      *ret1 = "Account is missing auth field";
      *ret2 = ap_psprintf (p, "Account <tt>%s</tt> is missing its auth field. This is a problem with the server.", u);
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

  *ret1 = ap_pstrdup (p, u);
  *ret2 = ap_pstrdup (p, cookie);
  xmlFree (cookie);
  return 1;
}

static int
acct_loginsub_serve (VirguleReq *vr)
{
  request_rec *r = vr->r;
  table *args;
  const char *u, *pass;
  const char *ret1, *ret2;
  const char *cookie;
  
  r->content_type = "text/plain; charset=ISO-8859-1";

  args = get_args_table (vr);

  if (args == NULL)
    return send_error_page (vr, "Need form data", " This page requires a form submission. If you're not playing around manually with URLs, it suggests there's something wrong with the site.\n");

  u = ap_table_get (args, "u");
  pass = ap_table_get (args, "pass");

#if 0
  buffer_printf (b, "Username: %s\n", u);
  buffer_printf (b, "Password: %s\n", pass);
#endif

  if (!acct_login (vr, u, pass, &ret1, &ret2))
      return send_error_page (vr, ret1, ret2);

  u = ret1;
  cookie = ret2;
  
  acct_set_cookie (vr, u, cookie, 86400 * 365);
  
  vr->u = u;

  return send_error_page (vr,
			  "Login ok",
			  "Login to account <tt>%s</tt> ok.\n", u);

}

static int
acct_logout_serve (VirguleReq *vr)
{

  auth_user (vr);

  if (vr->u)
    {
      acct_set_cookie (vr, vr->u, "", -86400);
      return send_error_page (vr,
			      "Logged out",
			      "Logout of account <tt>%s</tt> ok.\n", vr->u);
    }
  else
    return send_error_page (vr,
			    "Already logged out",
			    "You were already logged out.\n");

}

static int
acct_update_serve (VirguleReq *vr)
{
  pool *p = vr->r->pool;
  table *args;

  db_lock_upgrade(vr->lock);
  auth_user (vr);

  args = get_args_table (vr);

  if (vr->u)
    {
      char *db_key;
      xmlDoc *profile;
      xmlNode *tree;
      int i;
      int status;

      db_key = acct_dbkey (p, vr->u);
      profile = db_xml_get (p, vr->db, db_key);

      tree = xml_ensure_child (profile->root, "info");

      for (i = 0; prof_fields[i].description; i++)
	{
	  const char *val;
	  val = ap_table_get (args, prof_fields[i].attr_name);
	  if (val == NULL && prof_fields[i].flags & PROFILE_BOOLEAN)
	    val = "off";
	  if (val)
	    {
#if 0
	      g_print ("Setting field %s to %s\n",
		       prof_fields[i].attr_name, val);
#endif
	      xmlSetProp (tree, prof_fields[i].attr_name, val);
	    }
	}


      status = db_xml_put (p, vr->db, db_key, profile);
      if (status)
	return send_error_page (vr,
				"Error storing account profile",
				"There was an error storing the account profile. This means there's something wrong with the site.");
      ap_table_add (vr->r->headers_out, "refresh",
		    ap_psprintf(p, "0;URL=/person/%s/", vr->u));
      return send_error_page (vr,
			      "Updated",
			      "Updates to account <a href=\"../person/%s/\">%s</a> ok",
			      vr->u, vr->u);
    }
  else
    return send_error_page (vr,
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

static int
acct_person_index_serve (VirguleReq *vr)
{
  Buffer *b = vr->b;
  Db *db = vr->db;
  pool *p = vr->r->pool;
  DbCursor *dbc;
  char *u;
  array_header *list;
  int i;

  auth_user (vr);

  dbc = db_open_dir (db, "acct");
  if (dbc == NULL)
    return send_error_page (vr,
			    "Error reading accounts",
			    "There was an error reading the accounts. This means the server is screwed up.");

  list = ap_make_array (p, 16, sizeof(NodeInfo));

  while ((u = db_read_dir_raw (dbc)) != NULL)
    {
      NodeInfo *ni;
      char *db_key;
      xmlDoc *profile;
      xmlNode *tree;

      db_key = acct_dbkey (p, u);
      profile = db_xml_get (p, db, db_key);
      tree = xml_find_child (profile->root, "info");
      if (tree != NULL)
	{
	  ni = (NodeInfo *)ap_push_array (list);
	  ni->name = u;
	  ni->givenname = xml_get_prop (p, tree, "givenname");
	  ni->surname = xml_get_prop (p, tree, "surname");
	}
      db_xml_free (p, db, profile);
    }
  db_close_dir (dbc);

  qsort (list->elts, list->nelts, sizeof(NodeInfo),
	 node_info_compare);

  render_header (vr, "<x>People</x>");
  for (i = 0; i < list->nelts; i++)
    {
      NodeInfo *ni = &((NodeInfo *)(list->elts))[i];
      const char *u = ni->name;
      char *givenname;
      char *surname;

      givenname = ni->givenname ? nice_text (p, ni->givenname) : "";
      surname = ni->surname ? nice_text (p, ni->surname) : "";

      render_cert_level_begin (vr, u, CERT_STYLE_SMALL);
      buffer_printf (b, "<a href=\"%s/\">%s</a> %s %s, %s\n",
		     u, u, givenname, surname,
		     req_get_tmetric_level (vr, u));
      render_cert_level_end (vr, CERT_STYLE_SMALL);

    }
  if (vr->u)
    buffer_puts (b, "<p> Go to <x>a person</x>'s page to certify them. </p>\n");
  return render_footer_send (vr);
}

static int
acct_person_graph_serve (VirguleReq *vr)
{
  request_rec *r = vr->r;
  pool *p = r->pool;
  Buffer *b = vr->b;
  Db *db = vr->db;
  DbCursor *dbc;
  char *issuer;
  const int threshold = 0;

  r->content_type = "text/plain; charset=ISO-8859-1";
  buffer_printf (b, "digraph G {\n");
  dbc = db_open_dir (db, "acct");
  while ((issuer = db_read_dir_raw (dbc)) != NULL)
    {
      char *db_key;
      xmlDoc *profile;
      xmlNode *tree;
      xmlNode *cert;

      buffer_printf (b, "   /* %s */\n", issuer);

      db_key = acct_dbkey (p, issuer);
      profile = db_xml_get (p, db, db_key);
      tree = xml_find_child (profile->root, "certs");
      if (tree == NULL)
	continue;
      if (cert_level_from_name (vr, req_get_tmetric_level (vr, issuer)) < threshold)
	continue;
      for (cert = tree->childs; cert != NULL; cert = cert->next)
	{
	  if (cert->type == XML_ELEMENT_NODE &&
	      !strcmp (cert->name, "cert"))
	    {
	      char *cert_subj;

	      cert_subj = xml_get_prop (p, cert, "subj");
	      if (cert_subj &&
		  cert_level_from_name (vr, req_get_tmetric_level (vr, cert_subj)) >= threshold)
		{
		  char *cert_level;
		  CertLevel level;
		  const char *colors[] = {"gray", "green", "blue", "violet"};

		  cert_level = xmlGetProp (cert, "level");
		  level = cert_level_from_name (vr, cert_level);
		  xmlFree (cert_level);
		  buffer_printf (b, "   \"%s\" -> \"%s\" [color=\"%s\"];\n",
				 issuer, cert_subj, colors[level]);
		}
	    }
	}
    }
  db_close_dir (dbc);

  buffer_printf (b, "}\n");
  return send_response (vr);
}

static int
acct_person_diary_xml_serve (VirguleReq *vr, char *u)
{
  Buffer *b = vr->b;
  xmlDocPtr doc;
  xmlChar *mem;
  int size;

  doc = xmlNewDoc ("1.0");

  doc->root = xmlNewDocNode (doc, NULL, "tdif", NULL);
  diary_export (vr, doc->root, u);

  xmlDocDumpMemory (doc, &mem, &size);
  buffer_write (b, mem, size);
  xmlFree (mem);
  xmlFreeDoc (doc);
  return send_response (vr);
}

static int
acct_person_diary_rss_serve (VirguleReq *vr, char *u)
{
  Buffer *b = vr->b;
  xmlDocPtr doc;
  xmlChar *mem;
  int size;

  doc = xmlNewDoc ("1.0");

  vr->r->content_type = "text/xml; charset=ISO-8859-1";

  doc->root = xmlNewDocNode (doc, NULL, "rss", NULL);
  xmlSetProp (doc->root, "version", "0.91");
  diary_rss_export (vr, doc->root, u);

  xmlDocDumpMemory (doc, &mem, &size);
  buffer_write (b, mem, size);
  xmlFree (mem);
  xmlFreeDoc (doc);
  return send_response (vr);
}

static int
acct_person_diary_serve (VirguleReq *vr, char *u)
{
  pool *p = vr->r->pool;
  Buffer *b = vr->b;
  char *str;
  table *args;
  int start;

  args = get_args_table (vr);
  if (args == NULL)
    start = -1;
  else
    start = atoi (ap_table_get (args, "start"));

  str = ap_psprintf (p, "Diary for %s", u);

  render_header (vr, str);
  if (start == -1)
    buffer_printf (b, "<p> Recent diary entries for <a href=\"%s/person/%s/\">%s</a>: </p>\n",
		   vr->prefix, u, u);
  else
    buffer_printf (b, "<p> Older diary entries for <a href=\"%s/person/%s/\">%s</a> (starting at number %d): </p>\n",
		   vr->prefix, u, u, start);

  diary_render (vr, u, 10, start);

  return render_footer_send (vr);
}

static int
acct_person_serve (VirguleReq *vr, const char *path)
{
  request_rec *r = vr->r;
  pool *p = r->pool;
  char *q;
  char *u;
  char *db_key;
  xmlDoc *profile, *staff;
  xmlNode *tree;
  Buffer *b = vr->b;
  char *str;
  char *surname, *givenname;
  char *url;
  char *notes;
  int any;
  char *err;
  char *first;

  if (!path[0])
    return acct_person_index_serve (vr);

  if (!strcmp (path, "graph.dot"))
    return acct_person_graph_serve (vr);

  auth_user (vr);

  q = strchr ((char *)path, '/');
  if (q == NULL)
    {
      ap_table_add (r->headers_out, "Location",
		    ap_make_full_path (p, r->uri, ""));
      return REDIRECT;
    }

  u = ap_pstrndup(p, path, q - path);

  if (!strcmp (q + 1, "diary.html"))
    return acct_person_diary_serve (vr, u);

  if (!strcmp (q + 1, "diary.xml"))
    return acct_person_diary_xml_serve (vr, u);

  if (!strcmp (q + 1, "rss.xml"))
    return acct_person_diary_rss_serve (vr, u);

  if (q[1] != '\0')
    return send_error_page (vr,
			    "Extra junk",
			    "Extra junk after <x>person</x>'s name not allowed.");

  db_key = acct_dbkey (p, u);
  if (db_key == NULL)
    {
      return send_error_page (vr, "User name not valid", "The user name doesn't even look valid, much less exist in the database.");
    }

  profile = db_xml_get (p, vr->db, db_key);
  if (profile == NULL)
    return send_error_page (vr,
			    "<x>Person</x> not found",
			    "Account <tt>%s</tt> was not found.", u);

  tree = xml_find_child (profile->root, "alias");
  if (tree != NULL)
    {
      ap_table_add (r->headers_out, "Location",
		    ap_pstrcat (p, vr->prefix, "/person/",
				xml_get_prop (p, tree, "link"), "/", NULL));
      return REDIRECT;
				
    }

  str = ap_psprintf (p, "Personal info for %s", u);
  render_header (vr, str);

  if (strcmp (req_get_tmetric_level (vr, u),
	       cert_level_to_name (vr, CERT_LEVEL_NONE)))
    {
      render_cert_level_begin (vr, u, CERT_STYLE_SMALL);
      buffer_printf (b, "This <x>person</x> is currently certified at %s level.\n", req_get_tmetric_level (vr, u));
      render_cert_level_end (vr, CERT_STYLE_SMALL);
    }

  any = 0;
  tree = xml_find_child (profile->root, "info");
  if (tree)
    {
      givenname = xml_get_prop (p, tree, "givenname");
      surname = xml_get_prop (p, tree, "surname");
      buffer_printf (b, "<p> Name: %s %s</p>\n",
		     givenname ? nice_text(p, givenname) : "",
		     surname ? nice_text(p, surname) : "");

      url = xml_get_prop (p, tree, "url");
      if (url && url[0])
	{
	  char *url2;
	  char *colon;

	  url2 = url;
	  colon = strchr (url, ':');
	  if (!colon || colon[1] != '/' || colon[2] != '/')
	    url2 = ap_pstrcat (p, "http://", url, NULL);
	  buffer_printf (b, "<p> Homepage: <a href=\"%s\">%s</a> </p>\n",
			 url2, nice_text (p, url));
	  any = 1;
	}
      notes = xml_get_prop (p, tree, "notes");
      if (notes && notes[0])
	{
	  buffer_printf (b, "<p> <b>Notes:</b> %s </p>\n", nice_htext (vr, notes, &err));
	  any = 1;
	}
    }
  if (!any)
    buffer_puts (b, "<p> No personal information is available. </p>\n");

  /* Render staff listings */
  first = "<p> This <x>person</x> is: </p>\n"
    "<ul>\n";
  db_key = ap_psprintf (p, "acct/%s/staff-person.xml", u);

  staff = db_xml_get (p, vr->db, db_key);
  if (staff != NULL)
    {
      for (tree = staff->root->childs; tree != NULL; tree = tree->next)
	{
	  char *name;
	  char *type;

	  name = xml_get_prop (p, tree, "name");
	  type = xml_get_prop (p, tree, "type");

	  if (! !strcmp (type, "None"))
	    {
	      buffer_puts (b, first);
	      first = "";
	      buffer_printf (b, "<li>a %s on <x>project</x> %s.\n",
			     nice_text (p, type), render_proj_name (vr, name));
	    }
	}
    }
  if (first[0] == 0)
    buffer_puts (b, "</ul>\n");

  buffer_printf (b, "<p> Recent diary entries for %s:<br />\n", u);
  buffer_printf (b, "<a href=\"rss.xml\"><img src=\"/images/rss.png\" width=36 height=20 border=0 alt=\"RSS\" /></a></p>\n", u);

  diary_render (vr, u, 5, -1);

  /* Browse certifications */
  buffer_puts (b, "<a name=\"certs\">\n");
  tree = xml_find_child (profile->root, "certs");
  if (tree)
    {
      xmlNode *cert;
      int any = 0;
      for (cert = tree->childs; cert != NULL; cert = cert->next)
	if (cert->type == XML_ELEMENT_NODE &&
	    !strcmp (cert->name, "cert"))
	  {
	    char *subject, *level;
	    subject = xmlGetProp (cert, "subj");
	    level = xmlGetProp (cert, "level");
	    if (strcmp (level, cert_level_to_name (vr, 0)))
	      {
		if (!any)
		  {
		    buffer_puts (b, "<p> This <x>person</x> has certified others as follows: </p>\n"
				 "<ul>\n");
		    any = 1;
		  }
	      buffer_printf (b, "<li>%s certified <a href=\"../%s/\">%s</a> as %s\n",
			     u, subject, subject, level);
	      }
	  }
      if (any)
	buffer_puts (b, "</ul>\n");
    }

  tree = xml_find_child (profile->root, "certs-in");
  if (tree)
    {
      xmlNode *cert;
      int any = 0;
      for (cert = tree->childs; cert != NULL; cert = cert->next)
	if (cert->type == XML_ELEMENT_NODE &&
	    !strcmp (cert->name, "cert"))
	  {
	    char *issuer, *level;
	    issuer = xmlGetProp (cert, "issuer");
	    level = xmlGetProp (cert, "level");
	    if (strcmp (level, cert_level_to_name (vr, 0)))
	      {
		if (!any)
		  {
		    buffer_puts (b, "<p> Others have certified this <x>person</x> as follows: </p>\n"
		   "<ul>\n");
		    any = 1;
		  }
		buffer_printf (b, "<li><a href=\"../%s/\">%s</a> certified %s as %s\n",
			       issuer, issuer, u, level);
	      }
	  }
      if (any)
	buffer_puts (b, "</ul>\n");
    }

  /* Certification form; need to be authenticated, and disallow self-cert. */
  if (vr->u == NULL)
    buffer_puts (b, "<p> [ Certification disabled because you're not logged in. ] </p>\n");
#if 0
  /* disable self-cert */
  else if (!strcmp (u, vr->u))
    buffer_puts (b, "<p> [ Certification disabled for yourself. ] </p>\n");
#endif
  else
    {
      int i;
      CertLevel level;

      level = cert_get (vr, vr->u, u);

      buffer_printf (b, "<form method=\"POST\" action=\"%s/acct/certify.html\">\n"
		     "Certify %s as:\n"
		     " <select name=\"level\" value=\"level\">\n", vr->prefix, u);

      for (i = cert_num_levels (vr) - 1; i >= 0; i--)
	buffer_printf (b, "  <option%s> %s\n",
		       level == i ? " selected" : "",
		       cert_level_to_name (vr, i));

      buffer_printf (b, " </select>\n"
		     " <input type=\"submit\" value=\"Certify\">\n"
		     " <input type=\"hidden\" name=\"subject\" value=\"%s\">\n"
		     "</form>\n"
		     "<p> See the <a href=\"%s/certs.html\">Certification</a> overview for more information.</p>\n",
		     u, vr->prefix);

      if (vr->render_diaryratings) 
	rating_diary_form (vr, u);
    }

  return render_footer_send (vr);
}

static int
acct_certify_serve (VirguleReq *vr)
{
  table *args;
  const char *subject;
  const char *level;
  int status;

  db_lock_upgrade(vr->lock);
  auth_user (vr);

  args = get_args_table (vr);

  if (vr->u)
    {
      subject = ap_table_get (args, "subject");
      level = ap_table_get (args, "level");

      status = cert_set (vr, vr->u, subject,
			 cert_level_from_name (vr, level));
      if (status)
	return send_error_page (vr,
				"Error storing certificate",
				"There was an error storing the certificate. This means there's something wrong with the site.");
      ap_table_add (vr->r->headers_out, "refresh",
		    ap_psprintf(vr->r->pool, "0;URL=/person/%s/#certs",
				subject));
      return send_error_page (vr,
			      "Updated",
			      "Certification of <a href=\"../person/%s/\">%s</a> to %s level ok.",
			      subject, subject, level);
    }
  else
    return send_error_page (vr,
			    "Not logged in",
			    "You need to be logged in to certify another <x>person</x>.");
}

int
acct_maint_serve (VirguleReq *vr)
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
  if ((p = match_prefix (vr->uri, "/person/")) != NULL)
    return acct_person_serve (vr, p);
  if (!strcmp (vr->uri, "/acct/certify.html"))
    return acct_certify_serve (vr);
  return DECLINED;
}
