/* Try to authenticate the user from the cookie */

#include <ctype.h>

#include <apr.h>
#include <apr_strings.h>
#include <httpd.h>

#include <libxml/tree.h>

#include "private.h"
#include "buffer.h"
#include "db.h"
#include "db_xml.h"
#include "req.h"
#include "acct_maint.h"
#include "xml_util.h"
#include "auth.h"

void
virgule_auth_user_with_cookie (VirguleReq *vr, const char *id_cookie)
{
  request_rec *r = vr->r;
  Db *db = vr->db;
  apr_pool_t *p = r->pool;
  char *u;
  char *db_key;
  xmlDoc *profile;
  xmlNode *root, *tree;
  char *stored_cookie;

  u = ap_getword (p, &id_cookie, ':');
  db_key = virgule_acct_dbkey (vr, u);
  if (db_key == NULL)
    /* cookie is invalid */
    return;

  profile = virgule_db_xml_get (p, db, db_key);
  if (profile == NULL)
    /* account doesn't exist */
    return;
  root = profile->xmlRootNode;

  tree = virgule_xml_find_child (root, "auth");
  if (tree == NULL)
    return;

  stored_cookie = (char *)xmlGetProp (tree, (xmlChar *)"cookie");

  if (strcmp (id_cookie, stored_cookie))
    /* cookie doesn't match */
    return;
  vr->u = u;
  virgule_acct_touch(vr,u);

  /* store the username where it will be logged */
  if (!vr->r->user)
    {
      char *lu, *s, *d;
      lu = apr_pstrdup (p, u);
      for (s = d = lu; *s; s++)
        if (isgraph(*s))
	  *d++ = *s;
	else
	  *d++ = '_';
      *d = 0;
      vr->r->user = lu;
    }
}

void
virgule_auth_user (VirguleReq *vr)
{
  const char *cookie, *val;
  char *key;
  const char *id_cookie;

  if (vr->u != NULL)
    /* already authenticated */
    return;

  cookie = apr_table_get (vr->r->headers_in, "Cookie");
  if (cookie == NULL)
    return;

  id_cookie = NULL;
  while (*cookie) {
    key = ap_getword (vr->r->pool, &cookie, '=');
    val = ap_getword (vr->r->pool, &cookie, ';');
    if (*cookie == ' ') cookie++;
    if (!strcmp (key, "id"))
      {
	id_cookie = val;
	break;
      }
  }
  if (id_cookie == NULL)
    return;

  virgule_auth_user_with_cookie (vr, id_cookie);
}
