#include <apr.h>
#include <apr_strings.h>
#include <httpd.h>
#include <http_log.h>

#include "private.h"
#include "buffer.h"
#include "db.h"
#include "apache_util.h"
#include "req.h"
#include "certs.h"
#include "auth.h"
#include "tmetric.h"
#include "util.h"

/* Send http header and buffer. Probably redunant at this point? */
int
virgule_send_response (VirguleReq *vr)
{
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
 * The trust metric cache is loaded into a sub pool of the Apache thread
 * private memory pool. This allows us to preserve the trust data across
 * hits once it's loaded. A stat is done on each tmetric request and, if
 * a newer cache is available, the current one is dumped, the sub pool is
 * destroyed and a reload occurs. The sub pool also gets destroyed if the
 * entire thread private pool gets destroyed, which occurs (rarely) when
 * the mod_virgule config file has changed and needs to be reloaded.
 *
 * Return value: The trust metric results.
 **/
char *
virgule_req_get_tmetric (VirguleReq *vr)
{
  apr_finfo_t finfo;

  /* Stat the trust metric cache file */
  apr_stat(&finfo, ap_make_full_path (vr->r->pool, vr->priv->base_path, "tmetric/default"),
           APR_FINFO_MIN, vr->r->pool);

  /* Load the trust metric cache and reset timestamp */
  if (vr->priv->tmetric == NULL || finfo.mtime != vr->priv->tm_mtime)
    {
      /* free existing memory if needed */
      if(vr->priv->tmetric != NULL)
        {
          apr_pool_destroy (vr->priv->tm_pool);
	  vr->priv->tm_mtime = 0L;
	}

      /* allocate a sub pool and load the tmetric cache */
      apr_pool_create(&vr->priv->tm_pool, vr->priv->pool);
      vr->priv->tmetric = virgule_tmetric_get (vr);
      vr->priv->tm_mtime = finfo.mtime;
    }

  return vr->priv->tmetric;
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
  char *user = NULL;
  char *tmetric = NULL;
  int i, j;

  if (u == NULL || *u == 0)
    return virgule_cert_level_to_name (vr, CERT_LEVEL_NONE);

  user = ap_escape_uri(vr->r->pool,u);

  tmetric = virgule_req_get_tmetric (vr);
  
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

  if(virgule_user_is_special(vr, vr->u))
    return 1;

  if(vr->priv->article_post_by_editors_only && *vr->priv->editors)
    {
      const char **s;
      for (s = vr->priv->editors; *s; s++)
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

  if(virgule_user_is_special(vr, vr->u))
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

  if(virgule_user_is_special(vr, vr->u))
    return 1;

  return 0;
}


/**
 * virgule_req_ok_to_syndicate_blog: Determine if it's ok for the user to 
 * setup blog syndication.
 * @vr: The #VirguleReq context.
 *
 * Checks the users trust certification against the minimum cert level set
 * in the config.xml file.
 *
 * Return value: TRUE if it's ok for the user to syndicate.
 **/
int
virgule_req_ok_to_syndicate_blog (VirguleReq *vr)
{
  virgule_auth_user (vr);

  if (vr->u == NULL)
    return 0;

  if (virgule_cert_level_from_name(vr, virgule_req_get_tmetric_level (vr, vr->u)) >= vr->priv->level_blogsyndicate)
    return 1;

  if(virgule_user_is_special(vr, vr->u))
    return 1;

  return 0;
}
