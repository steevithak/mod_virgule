#include <apr.h>
#include <apr_strings.h>
#include <httpd.h>

#include "private.h"
#include "buffer.h"
#include "db.h"
#include "apache_util.h"
#include "req.h"
#include "certs.h"
#include "auth.h"
#include "tmetric.h"

/* Send http header and buffer. Also releases the lock. */
int
virgule_send_response (VirguleReq *vr)
{
  if (vr->lock)
    virgule_db_unlock (vr->lock);
  return virgule_buffer_send_response (vr->r, vr->b);
}

/**
 * get_args_table: Get the uri args in table form.
 * @vr: The request record.
 *
 * Gets the uri args, returning them in apache table form.
 *
 * Return value: The table containing the args, or NULL if error.
 **/
apr_table_t *
virgule_get_args_table (VirguleReq *vr)
{
  apr_pool_t *p = vr->r->pool;
  char *args;
  apr_table_t *result;
  const char *arg_p, *entry;
  char *key, *val;

  args = vr->args;
  if (args == NULL)
    return NULL;

  /* todo: check content-type for POSTed args */

  result = apr_table_make (p, 8);
  arg_p = args;
  while (arg_p[0] != '\0' && (entry = ap_getword (p, &arg_p, '&')))
    {
      key = ap_getword (p, &entry, '=');
      val = apr_pstrdup (p, entry);
      ap_unescape_url (key);
      virgule_unescape_url_info (val);
      apr_table_merge (result, key, val);
    }
  return result;
}

/**
 * req_get_tmetric: Get the trust metric results.
 * @vr: The #VirguleReq context.
 *
 * Gets the trust metric results in simple username->level ap table
 * form. Note: ap tables are (as of the time of writing) stored
 * linearly and not in a tree or hash table format. Thus, if you're
 * doing a lot of queries, performance could kinda suck.
 *
 * Currently, this only bothers with the default trust root, but that
 * could easily change. In that case, this routine will suck trust
 * root preference info out of the user's profile.
 *
 * Return value: The trust metric results.
 **/
char *
virgule_req_get_tmetric (VirguleReq *vr)
{
  char *result;
  if (vr->tmetric)
    return vr->tmetric;

  result = virgule_tmetric_get (vr);
  vr->tmetric = result;
  return result;
}

/**
 * req_get_tmetric_level: Get the trust metric level of a user.
 * @vr: The #VirguleReq context.
 * @u: The account name.
 *
 * Gets the certification level of @u according to the default trust
 * metric.
 *
 * Return value: The certification level, as a string.
 **/
const char *
virgule_req_get_tmetric_level (VirguleReq *vr, const char *u)
{
  char *result;
  char *user = ap_escape_uri(vr->r->pool,u);
  char *tmetric = virgule_req_get_tmetric (vr);
  int i, j;

  if (tmetric == NULL)
    return virgule_cert_level_to_name (vr, CERT_LEVEL_NONE);

  /* This is a hackish linear scan through the tmetric table.
     At some point, we may want to use a real binary tree. */
  for (i = 0; tmetric[i];)
    {
      for (j = 0; user[j]; j++)
	if (tmetric[i++] != user[j])
	  break;
      if (user[j] == 0 && tmetric[i++] == ' ')
	{
	  /* found */
	  for (j = 0; tmetric[i + j] && tmetric[i + j] != '\n'; j++);
	  result = apr_palloc (vr->r->pool, j + 1);
	  memcpy (result, tmetric + i, j);
	  result[j] = 0;
	  return result;
	}
      else
	{
	  /* skip to next line */
	  while (tmetric[i] && tmetric[i] != '\n')
	    i++;
	  if (tmetric[i] == '\n')
	    i++;
	}
    }

  return virgule_cert_level_to_name (vr, CERT_LEVEL_NONE);
}

/**
 * req_ok_to_post: Determine if it's ok for the user to post.
 * @vr: The #VirguleReq context.
 *
 * Checks the users trust certification against the minimum cert level set
 * in the config.xml file.
 *
 * Return value: TRUE if it's ok for the user to post.
 **/
int
virgule_req_ok_to_post (VirguleReq *vr)
{
  virgule_auth_user (vr);

  if (vr->u == NULL)
    return 0;

  if(vr->priv->article_post_by_seeds_only && *vr->priv->seeds)
    {
      const char **s;
      for (s = vr->priv->seeds; *s; s++)
        if(strcmp(vr->u,*s) == 0)
	  return 1;
    }

  else if(virgule_cert_level_from_name(vr, virgule_req_get_tmetric_level (vr, vr->u)) >= vr->priv->level_articlepost)
    return 1;

  return 0;
}

/**
 * req_ok_to_reply: Determine if it's ok for the user to reply to posts.
 * @vr: The #VirguleReq context.
 *
 * Checks the users trust certification against the minimum cert level set
 * in the config.xml file.
 *
 * Return value: TRUE if it's ok for the user to reply to posts.
 **/
int
virgule_req_ok_to_reply (VirguleReq *vr)
{
  virgule_auth_user (vr);

  if (vr->u == NULL)
    return 0;

  if (virgule_cert_level_from_name(vr, virgule_req_get_tmetric_level (vr, vr->u)) >= vr->priv->level_articlereply)
    return 1;

  return 0;
}

/**
 * req_ok_to_create_project: Determine if it's ok for the user to create a
 * new project.
 * @vr: The #VirguleReq context.
 *
 * Checks the users trust certification against the minimum cert level set
 * in the config.xml file.
 *
 * Return value: TRUE if it's ok for the user to create projects.
 **/
int
virgule_req_ok_to_create_project (VirguleReq *vr)
{
  virgule_auth_user (vr);

  if (vr->u == NULL)
    return 0;

  if (virgule_cert_level_from_name(vr, virgule_req_get_tmetric_level (vr, vr->u)) >= vr->priv->level_projectcreate)
    return 1;

  return 0;
}
