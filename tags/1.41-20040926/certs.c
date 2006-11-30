#include <string.h>
#include "httpd.h"

#include <libxml/tree.h>
#include <libxml/xmlmemory.h>

#include "buffer.h"
#include "db.h"
#include "req.h"
#include "db_xml.h"
#include "xml_util.h"
#include "acct_maint.h"
#include "style.h"

#include "certs.h"

int
cert_num_levels (VirguleReq *vr)
{
  int count;

  for (count = 0;; count++)
    if (!vr->cert_level_names[count])
      break;

  return count;
}

CertLevel
cert_level_from_name (VirguleReq *vr, const char *name)
{
  int i;

  for (i = 0;; i++) {
    if (!vr->cert_level_names[i])
      break;
    if (!strcmp (name, vr->cert_level_names[i]))
      return i;
  }
  return CERT_LEVEL_NONE;
}

const char *
cert_level_to_name (VirguleReq *vr, CertLevel level)
{
  if (level >= 0 && level < cert_num_levels (vr))
    return vr->cert_level_names[level];
  return "None";
}

CertLevel
cert_get (VirguleReq *vr, const char *issuer, const char *subject)
{
  pool *p = vr->r->pool;
  Db *db = vr->db;
  char *db_key;
  xmlDoc *profile;
  xmlNode *tree;
  xmlNode *cert;
  CertLevel result = CERT_LEVEL_NONE;

  db_key = acct_dbkey (p, issuer);
  profile = db_xml_get (p, db, db_key);
  tree = xml_find_child (profile->xmlRootNode, "certs");
  if (tree == NULL)
    return CERT_LEVEL_NONE;
  for (cert = tree->children; cert != NULL; cert = cert->next)
    {
      if (cert->type == XML_ELEMENT_NODE &&
	  !strcmp (cert->name, "cert"))
	{
	  char *cert_subj;

	  cert_subj = xmlGetProp (cert, "subj");
	  if (cert_subj)
	    {
	      if (!strcmp (cert_subj, subject))
		{
		  char *cert_level;

		  cert_level = xmlGetProp (cert, "level");
		  result = cert_level_from_name (vr, cert_level);
		  xmlFree (cert_level);
		  xmlFree (cert_subj);
		  break;
		}
	      xmlFree (cert_subj);
	    }
	}
    }
  db_xml_free (p, db, profile);
  return result;
}

int
cert_set (VirguleReq *vr, const char *issuer, const char *subject, CertLevel level)
{
  pool *p = vr->r->pool;
  Db *db = vr->db;
  char *db_key;
  xmlDoc *profile;
  xmlNode *tree;
  xmlNode *cert;
  int status;

  /* update subject first because it's more likely not to exist. */
  db_key = acct_dbkey (p, subject);
  profile = db_xml_get (p, db, db_key);
  if (profile == NULL)
    return -1;

  tree = xml_ensure_child (profile->xmlRootNode, "certs-in");

  for (cert = tree->children; cert != NULL; cert = cert->next)
    {
      if (cert->type == XML_ELEMENT_NODE &&
	  !strcmp (cert->name, "cert"))
	{
	  char *cert_issuer;

	  cert_issuer = xmlGetProp (cert, "issuer");
	  if (cert_issuer)
	    {
	      if (!strcmp (cert_issuer, issuer))
		{
		  xmlFree (cert_issuer);
		  break;
		}
	      xmlFree (cert_issuer);
	    }
	}
    }
  if (cert == NULL)
    {
      cert = xmlNewChild (tree, NULL, "cert", NULL);
      xmlSetProp (cert, "issuer", issuer);
    }

  if (level == CERT_LEVEL_NONE)
    {
      xmlUnlinkNode(cert);
      xmlFreeNode(cert);
    }
  else
    xmlSetProp (cert, "level", cert_level_to_name (vr, level));

  status = db_xml_put (p, db, db_key, profile);
  db_xml_free (p, db, profile);

  /* then, update issuer */
  db_key = acct_dbkey (p, issuer);
  profile = db_xml_get (p, db, db_key);
  if (profile == NULL)
    return -1;
  tree = xml_ensure_child (profile->xmlRootNode, "certs");

  for (cert = tree->children; cert != NULL; cert = cert->next)
    {
      if (cert->type == XML_ELEMENT_NODE &&
	  !strcmp (cert->name, "cert"))
	{
	  char *cert_subj;

	  cert_subj = xmlGetProp (cert, "subj");
	  if (cert_subj)
	    {
	      if (!strcmp (cert_subj, subject))
		{
		  xmlFree (cert_subj);
		  break;
		}
	      xmlFree (cert_subj);
	    }
	}
    }
  if (cert == NULL)
    {
      cert = xmlNewChild (tree, NULL, "cert", NULL);
      xmlSetProp (cert, "subj", subject);
    }

  if (level == CERT_LEVEL_NONE)
    {
      xmlUnlinkNode(cert);
      xmlFreeNode(cert);
    }
  else
    xmlSetProp (cert, "level", cert_level_to_name (vr, level));

  status = db_xml_put (p, db, db_key, profile);
  db_xml_free (p, db, profile);

  return status;
}

/**
 * render_cert_level_begin: Begin rendering cert level.
 * @vr: The #VirguleReq context.
 * @user: The username.
 *
 * Renders the beginning of the cert level, currently by beginning a table
 * with the corresponding background color. Rendering goes to vr->b.
 **/
void
render_cert_level_begin (VirguleReq *vr, const char *user, CertStyle cs)
{
  Buffer *b = vr->b;
  const char *level;
  CertLevel cert_level;
  const char **u;

  for (u = vr->special_users; *u; u++)
    if (!strcmp (user, *u))
      break;

  if (*u)
    cert_level = cert_num_levels (vr);
  else
    {
      level = req_get_tmetric_level (vr, user);
      cert_level = cert_level_from_name (vr, level);
    }
  buffer_printf (b, "<%s class=\"level%d\">", cs, cert_level);
  render_table_open (vr);
}

/**
 * render_cert_level_end: End rendering cert level.
 * @vr: The #VirguleReq context.
 * @level: The cert level, as a string.
 *
 * Closes the html opened by render_cert_level_begin().
 **/
void
render_cert_level_end (VirguleReq *vr, CertStyle cs)
{
  Buffer *b = vr->b;

  render_table_close (vr);
  buffer_printf (b, "</%s>\n", cs);
}

void
render_cert_level_text (VirguleReq *vr, const char *user)
{
  Buffer *b = vr->b;
  const char *level;

  level = req_get_tmetric_level (vr, user);
  buffer_printf (b, " <span style=\"display: none\">(%s)</span>", level);
}
