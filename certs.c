#include <string.h>

#include <apr.h>
#include <apr_strings.h>
#include <httpd.h>

#include <libxml/tree.h>
#include <libxml/xmlmemory.h>

#include "private.h"
#include "buffer.h"
#include "db.h"
#include "req.h"
#include "db_xml.h"
#include "xml_util.h"
#include "acct_maint.h"
#include "style.h"
#include "util.h"

#include "certs.h"

int
virgule_cert_num_levels (VirguleReq *vr)
{
  int count;

  for (count = 0;; count++)
    if (!vr->priv->cert_level_names[count])
      break;

  return count;
}

CertLevel
virgule_cert_level_from_name (VirguleReq *vr, const char *name)
{
  int i;

  for (i = 0;; i++) {
    if (!vr->priv->cert_level_names[i])
      break;
    if (!strcmp (name, vr->priv->cert_level_names[i]))
      return i;
  }
  return CERT_LEVEL_NONE;
}

const char *
virgule_cert_level_to_name (VirguleReq *vr, CertLevel level)
{
  if (level >= 0 && level < virgule_cert_num_levels (vr))
    return vr->priv->cert_level_names[level];
  return "None";
}


/**
 * virgule_cert_verify_outbound - Checks the specified outbound certification
 * for symmetry with an inbound cert record within the subject's profile.
 * Unlike other cert functions, existing certification dates are preserved.
 * 
 * Returns:
 * 0 No action needed, certs are symmetric
 * 1 Cert levels asymmetric, normalized to issuer level 
 * 2 Subject cert missing, created from issuer cert
 * 3 Subject profile does not exist
 **/
int
virgule_cert_verify_outbound (VirguleReq *vr, apr_pool_t *p, const char *issuer, const char *subject, const char *level, const char *date)
{
  xmlDoc *profile;
  xmlNode *certs, *cert;
  int rc = 0;
  char *sname;
  char *slevel = NULL;
  char *db_key = apr_pstrcat (p, "acct/", subject, "/profile.xml", NULL);

  profile = virgule_db_xml_get (p, vr->db, db_key);
  if (profile == NULL)
    return 3;
  
  certs = virgule_xml_ensure_child (profile->xmlRootNode, "certs-in");
  if (certs != NULL)
    {
      for (cert = certs->children; cert != NULL; cert = cert->next)
        {
          if (cert->type != XML_ELEMENT_NODE || xmlStrcmp (cert->name, (xmlChar *)"cert"))
	    continue;

	  sname = (char *)xmlGetProp (cert, (xmlChar *)"issuer");
	  if (sname)
	    {
	      if (!strcmp (sname, issuer))
	        {
		  slevel = (char *)xmlGetProp (cert, (xmlChar *)"level");
		  break;
		}
	      xmlFree (sname);
	    }
        }
    }

  if(slevel != NULL)
    {
      /* subject inbound cert exists and matches issuer cert! */
      if (!strcmp (slevel, level))
        {
	  xmlFree(slevel);
          return 0;
	}

      /* subject inbound cert exists but is incorrect */
      xmlFree(slevel);
      rc = 1;
    }
  else
    {
      /* subject inbound cert is missing, create new cert */
      cert = xmlNewChild (certs, NULL, (xmlChar *)"cert", NULL);
      rc = 2;
    }

  /* correct the cert */
  xmlSetProp (cert, (xmlChar *)"issuer", (xmlChar *)issuer);
  xmlSetProp (cert, (xmlChar *)"level", (xmlChar *)level);
  if(date != NULL)
    xmlSetProp (cert, (xmlChar *)"date", (xmlChar *)date);

  virgule_db_xml_put (p, vr->db, db_key, profile);
  virgule_db_xml_free (p, profile);

  return rc;
}


/**
 * virgule_cert_verify_inbound - Checks the specified inbound certification
 * for symmetry with an outbound cert record within the issuer's profile.
 * Unlike other cert functions, existing certification dates are preserved.
 * Note: error numbers are shared with virgule_cert_verify_outbound.
 * 
 * Returns:
 * 0 No action needed, certs are symmetric
 * 4 Issuer cert missing, created from subject cert
 * 5 Issuer cert exists but levels don't match
 * 6 Issuer profile does not exist
 **/
int
virgule_cert_verify_inbound (VirguleReq *vr, apr_pool_t *p, const char *subject, const char *issuer, const char *level, const char *date)
{
  xmlDoc *profile;
  xmlNode *certs, *cert;
  int rc = 0;
  char *sname;
  char *slevel = NULL;
  char *db_key = apr_pstrcat (p, "acct/", issuer, "/profile.xml", NULL);

  profile = virgule_db_xml_get (p, vr->db, db_key);
  if (profile == NULL)
    return 6;
  
  certs = virgule_xml_ensure_child (profile->xmlRootNode, "certs");
  if (certs != NULL)
    {
      for (cert = certs->children; cert != NULL; cert = cert->next)
        {
          if (cert->type != XML_ELEMENT_NODE || xmlStrcmp (cert->name, (xmlChar *)"cert"))
	    continue;

	  sname = (char *)xmlGetProp (cert, (xmlChar *)"subj");
	  if (sname)
	    {
	      if (!strcmp (sname, subject))
	        {
		  slevel = (char *)xmlGetProp (cert, (xmlChar *)"level");
		  break;
		}
	      xmlFree (sname);
	    }
        }
    }
  
  if(slevel != NULL)
    {
      /* issuer outbound cert exists and matches subject cert! */
      if (!strcmp (slevel, level))
        rc = 0;
      /* issuer outbound cert exists but level is incorrect */
      else
        rc = 5;
      xmlFree(slevel);
    }
  else
    {
      /* subject inbound cert is missing, create new cert */
      cert = xmlNewChild (certs, NULL, (xmlChar *)"cert", NULL);
      xmlSetProp (cert, (xmlChar *)"subj", (xmlChar *)subject);
      xmlSetProp (cert, (xmlChar *)"level", (xmlChar *)level);
      if(date != NULL)
        xmlSetProp (cert, (xmlChar *)"date", (xmlChar *)date);

      virgule_db_xml_put (p, vr->db, db_key, profile);
      virgule_db_xml_free (p, profile);
      rc = 4;
    }

  return rc;
}


CertLevel
virgule_cert_get (VirguleReq *vr, const char *issuer, const char *subject)
{
  apr_pool_t *p = vr->r->pool;
  Db *db = vr->db;
  char *db_key;
  xmlDoc *profile;
  xmlNode *tree;
  xmlNode *cert;
  CertLevel result = CERT_LEVEL_NONE;

  db_key = virgule_acct_dbkey (vr, issuer);
  profile = virgule_db_xml_get (p, db, db_key);
  tree = virgule_xml_find_child (profile->xmlRootNode, "certs");
  if (tree == NULL)
    return CERT_LEVEL_NONE;
  for (cert = tree->children; cert != NULL; cert = cert->next)
    {
      if (cert->type == XML_ELEMENT_NODE &&
	  !xmlStrcmp (cert->name, (xmlChar *)"cert"))
	{
	  char *cert_subj;

	  cert_subj = (char *)xmlGetProp (cert, (xmlChar *)"subj");
	  if (cert_subj)
	    {
	      if (!strcmp (cert_subj, subject))
		{
		  char *cert_level;

		  cert_level = (char *)xmlGetProp (cert, (xmlChar *)"level");
		  result = virgule_cert_level_from_name (vr, cert_level);
		  xmlFree (cert_level);
		  xmlFree (cert_subj);
		  break;
		}
	      xmlFree (cert_subj);
	    }
	}
    }
  virgule_db_xml_free (p, profile);
  return result;
}


/**
 * Adds a certification of level to subject from issuer
 */
int
virgule_cert_set (VirguleReq *vr, const char *issuer, const char *subject, CertLevel level)
{
  apr_pool_t *p = vr->r->pool;
  Db *db = vr->db;
  char *db_key;
  xmlDoc *profile;
  xmlNode *tree;
  xmlNode *cert;
  int status;

  /* update subject first because it's more likely not to exist. */
  db_key = virgule_acct_dbkey (vr, subject);
  profile = virgule_db_xml_get (p, db, db_key);
  if (profile == NULL)
    return -1;

  tree = virgule_xml_ensure_child (profile->xmlRootNode, "certs-in");

  for (cert = tree->children; cert != NULL; cert = cert->next)
    {
      if (cert->type == XML_ELEMENT_NODE &&
	  !xmlStrcmp (cert->name, (xmlChar *)"cert"))
	{
	  char *cert_issuer;

	  cert_issuer = (char *)xmlGetProp (cert, (xmlChar *)"issuer");
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
      cert = xmlNewChild (tree, NULL, (xmlChar *)"cert", NULL);
      xmlSetProp (cert, (xmlChar *)"issuer", (xmlChar *)issuer);
    }

  if (level == CERT_LEVEL_NONE)
    {
      xmlUnlinkNode(cert);
      xmlFreeNode(cert);
    }
  else
    {
      xmlSetProp (cert, (xmlChar *)"level", (xmlChar *)virgule_cert_level_to_name (vr, level));
      xmlSetProp (cert, (xmlChar *)"date", (xmlChar *)virgule_iso_now(vr->r->pool));
    }

  status = virgule_db_xml_put (p, db, db_key, profile);
  virgule_db_xml_free (p, profile);

  /* then, update issuer */
  db_key = virgule_acct_dbkey (vr, issuer);
  profile = virgule_db_xml_get (p, db, db_key);
  if (profile == NULL)
    return -1;
  tree = virgule_xml_ensure_child (profile->xmlRootNode, "certs");

  for (cert = tree->children; cert != NULL; cert = cert->next)
    {
      if (cert->type == XML_ELEMENT_NODE &&
	  !xmlStrcmp (cert->name, (xmlChar *)"cert"))
	{
	  char *cert_subj;

	  cert_subj = (char *)xmlGetProp (cert, (xmlChar *)"subj");
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
      cert = xmlNewChild (tree, NULL, (xmlChar *)"cert", NULL);
      xmlSetProp (cert, (xmlChar *)"subj", (xmlChar *)subject);
    }

  if (level == CERT_LEVEL_NONE)
    {
      xmlUnlinkNode(cert);
      xmlFreeNode(cert);
    }
  else
    {
      xmlSetProp (cert, (xmlChar *)"level", (xmlChar *)virgule_cert_level_to_name (vr, level));
      xmlSetProp (cert, (xmlChar *)"date", (xmlChar *)virgule_iso_now(vr->r->pool));
    }
    
  status = virgule_db_xml_put (p, db, db_key, profile);
  virgule_db_xml_free (p, profile);

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
CertLevel
virgule_render_cert_level_begin (VirguleReq *vr, const char *user, CertStyle cs)
{
  const char *level;
  CertLevel cert_level;

  if (virgule_user_is_special(vr,user))
    cert_level = virgule_cert_num_levels (vr);
  else
    {
      level = virgule_req_get_tmetric_level (vr, user);
      cert_level = virgule_cert_level_from_name (vr, level);
    }
  virgule_buffer_printf (vr->b, "<%s class=\"level%d\">", cs, cert_level);
  return cert_level;
}

/**
 * render_cert_level_end: End rendering cert level.
 * @vr: The #VirguleReq context.
 * @level: The cert level, as a string.
 *
 * Closes the html opened by render_cert_level_begin().
 **/
void
virgule_render_cert_level_end (VirguleReq *vr, CertStyle cs)
{
  virgule_buffer_printf (vr->b, "</%s>\n", cs);
}

void
virgule_render_cert_level_text (VirguleReq *vr, const char *user)
{
  Buffer *b = vr->b;
  const char *level;

  level = virgule_req_get_tmetric_level (vr, user);
  virgule_buffer_printf (b, " <span style=\"display: none\">(%s)</span>", level);
}
