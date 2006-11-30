/* The mod_virgule XML-RPC API in five easy steps:
 *
 *  1. the method should call virgule_xmlrpc_unmarshal_params() even if it
 *     doesn't have any parameters.
 *  2. (optionally) call virgule_xmlrpc_auth_user()
 *  3. a) on success the method should call virgule_xmlrpc_response()
 *     b) on failure the method should call virgule_xmlrpc_fault()
 *  4. add your method to method_table[] so it can be called.
 *  5. add your method to sample_db/site/xmlrpc.xml
 */

#include <ctype.h>

#include <apr.h>
#include <apr_strings.h>
#include <httpd.h>

#include <libxml/tree.h>

#include "private.h"
#include "buffer.h"
#include "db.h"
#include "req.h"
#include "util.h"
#include "acct_maint.h"
#include "aggregator.h"
#include "hashtable.h"
#include "eigen.h"
#include "diary.h"
#include "db_xml.h"
#include "xml_util.h"
#include "auth.h"

#include "xmlrpc.h"
#include "xmlrpc-methods.h"


/* Authentication
 */
static int
authenticate (VirguleReq *vr, xmlNode *params)
{
  char *user, *pass;
  const char *ret1, *ret2;
  char *id_cookie;
  int ret;
  
  ret = virgule_xmlrpc_unmarshal_params (vr, params, "ss", &user, &pass);
  if (ret != OK)
    return ret;

  ret = virgule_acct_login (vr, user, pass, &ret1, &ret2);
  if (ret == 0)
    return virgule_xmlrpc_fault (vr, 1, ret1);

  id_cookie = apr_pstrcat (vr->r->pool, ret1, ":", ret2, NULL);
  return virgule_xmlrpc_response (vr, "s", id_cookie);
}

static int
check_cookie (VirguleReq *vr, xmlNode *params)
{
  char *cookie;
  int ret;
  
  ret = virgule_xmlrpc_unmarshal_params (vr, params, "s", &cookie);
  if (ret != OK)
    return ret;
    
  virgule_auth_user_with_cookie (vr, cookie);
  return virgule_xmlrpc_response (vr, "i", vr->u == NULL ? 0 : 1);
}  


/* Diary methods
 */
static int
diary_len (VirguleReq *vr, xmlNode *params)
{
  char *user;
  const char *key;
  int ret;
  
  ret = virgule_xmlrpc_unmarshal_params (vr, params, "s", &user);
  if (ret != OK)
    return ret;

  key = apr_psprintf (vr->r->pool, "acct/%s/diary", user);
  return virgule_xmlrpc_response (vr, "i", virgule_db_dir_max (vr->db, key) + 1);
}

static int
diary_get (VirguleReq *vr, xmlNode *params)
{
  char *user;
  int index;
  const char *key;
  xmlDoc *entry;
  int ret;

  ret = virgule_xmlrpc_unmarshal_params (vr, params, "si", &user, &index);
  if (ret != OK)
    return ret;

  key = apr_psprintf (vr->r->pool, "acct/%s/diary/_%d", user, index);
  entry = virgule_db_xml_get (vr->r->pool, vr->db, key);
  if (entry == NULL)
    return virgule_xmlrpc_fault (vr, 1, "entry %d not found", index);

  return virgule_xmlrpc_response (vr, "s", virgule_xml_get_string_contents (entry->xmlRootNode));
}

static int
diary_get_dates (VirguleReq *vr, xmlNode *params)
{
  char *user;
  int index;
  const char *key;
  xmlDoc *entry;
  xmlNode *create_el;
  xmlNode *update_el;
  int ret;

  ret = virgule_xmlrpc_unmarshal_params (vr, params, "si", &user, &index);
  if (ret != OK)
    return ret;

  key = apr_psprintf (vr->r->pool, "acct/%s/diary/_%d", user, index);
  entry = virgule_db_xml_get (vr->r->pool, vr->db, key);
  if (entry == NULL)
    return virgule_xmlrpc_fault (vr, 1, "entry %d not found", index);

  create_el = virgule_xml_find_child (entry->xmlRootNode, "date");
  if (create_el == NULL)
    return virgule_xmlrpc_fault (vr, 1, "date broken in %d", index);

  update_el = virgule_xml_find_child (entry->xmlRootNode, "update");
  if (update_el == NULL)
    update_el = create_el;

  return virgule_xmlrpc_response (vr, "dd", virgule_xml_get_string_contents (create_el),
			  virgule_xml_get_string_contents (update_el));
}

static int
diary_set (VirguleReq *vr, xmlNode *params)
{
  char *cookie, *entry;
  const char *user;
  int index, max;
  const char *key;
  char *error;
  int ret;

  ret = virgule_xmlrpc_unmarshal_params (vr, params, "sis", &cookie, &index, &entry);
  if (ret != OK)
    return ret;
  ret = virgule_xmlrpc_auth_user (vr, cookie);
  if (ret != OK)
    return ret;
  user = vr->u;

  key = apr_psprintf (vr->r->pool, "acct/%s/diary", user);
  max = virgule_db_dir_max (vr->db, key) + 1;
  if (index == -1)
    index = max;
  if (index < 0 || index > max)
    return virgule_xmlrpc_fault (vr, 1, "invalid entry key %d", index);
  key = apr_psprintf (vr->r->pool, "acct/%s/diary/_%d", user, index);
  
  entry = virgule_nice_htext (vr, entry, &error);
  if (error)
    return virgule_xmlrpc_fault (vr, 1, "%s", error);
  ret = virgule_diary_store_entry (vr, key, entry);
  if (ret)
    return virgule_xmlrpc_fault (vr, 1, "error storing diary entry");

  return virgule_xmlrpc_response (vr, "i", 1);
}


static int
user_exists (VirguleReq *vr, xmlNode *params)
{
  char *user, *reason;
  char *db_key;
  xmlDoc *profile;
  int ret, exists;

  ret = virgule_xmlrpc_unmarshal_params (vr, params, "s", &user);
  if (ret != OK)
    return ret;

  reason = virgule_validate_username (vr, user);
  if (reason) 
    exists = 0;
  else {
    db_key = virgule_acct_dbkey (vr, user);
    profile = virgule_db_xml_get (vr->r->pool, vr->db, db_key);
    exists = profile ? 1 : 0;
  }

  return virgule_xmlrpc_response (vr, "i", exists);
}


static int
cert_get (VirguleReq *vr, xmlNode *params)
{
  char *user, *reason;
  const char *level;
  int ret;

  ret = virgule_xmlrpc_unmarshal_params (vr, params, "s", &user);
  if (ret != OK)
    return ret;

  reason = virgule_validate_username (vr, user);
  if (reason)
    return virgule_xmlrpc_fault (vr, 1, "invalid username: %s", user);

  level = virgule_req_get_tmetric_level (vr, user);
     
  return virgule_xmlrpc_response (vr, "s", level);
}


/* Simple functions to test the stuff in xmlrpc.c
 */

static int
test_guess (VirguleReq *vr, xmlNode *params)
{
  int ret;
  
  ret = virgule_xmlrpc_unmarshal_params (vr, params, XMLRPC_NO_PARAMS);
  if (ret != OK)
    return ret;
  
  return virgule_xmlrpc_response (vr, "si", "You guessed", 42);
}

static int
test_square (VirguleReq *vr, xmlNode *params)
{
  int x;
  int ret;
  
  ret = virgule_xmlrpc_unmarshal_params (vr, params, "i", &x);
  if (ret != OK)
    return ret;
  
  return virgule_xmlrpc_response (vr, "i", x*x);
}

static int
test_sumprod (VirguleReq *vr, xmlNode *params)
{
  int x, y;
  int ret;

  ret = virgule_xmlrpc_unmarshal_params (vr, params, "ii", &x, &y);
  if (ret != OK)
    return ret;
 
  return virgule_xmlrpc_response (vr, "ii", x+y, x*y);
}

static int
test_strlen (VirguleReq *vr, xmlNode *params)
{
  char *s;
  int ret;

  ret = virgule_xmlrpc_unmarshal_params (vr, params, "s", &s);
  if (ret != OK)
    return ret;
  
  return virgule_xmlrpc_response (vr, "i", strlen (s));
}

static int
test_capitalize (VirguleReq *vr, xmlNode *params)
{
  char *s;
  int i;
  int ret;

  ret = virgule_xmlrpc_unmarshal_params (vr, params, "s", &s);
  if (ret != OK)
    return ret;

  for (i=0; i<strlen(s); i++)
    s[i] = toupper (s[i]);
  
  return virgule_xmlrpc_response (vr, "s", s);
}


/* Method table
 */
xmlrpc_method xmlrpc_method_table[] = {
  { "authenticate",    authenticate    },
  { "checkcookie",     check_cookie    },
  { "diary.len",       diary_len       },
  { "diary.get",       diary_get       },
  { "diary.getDates",  diary_get_dates },
  { "diary.set",       diary_set       },
  { "user.exists",     user_exists     },
  { "cert.get",        cert_get        },
  { "test.guess",      test_guess      },
  { "test.square",     test_square     },
  { "test.sumprod",    test_sumprod    },
  { "test.strlen",     test_strlen     },
  { "test.capitalize", test_capitalize },
  { NULL, NULL }
};
