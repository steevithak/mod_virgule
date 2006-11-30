#include "httpd.h"

#include "buffer.h"
#include "db.h"
#include "apache_util.h"
#include "req.h"
#include "certs.h"
#include "auth.h"
#include "tmetric.h"

/* Send http header and buffer. Also releases the lock. */
int
send_response (VirguleReq *vr)
{
  if (vr->lock)
    db_unlock (vr->lock);
  return buffer_send_response (vr->r, vr->b);
}

/**
 * get_args_table: Get the uri args in table form.
 * @vr: The request record.
 *
 * Gets the uri args, returning them in apache table form.
 *
 * Return value: The table containing the args, or NULL if error.
 **/
table *
get_args_table (VirguleReq *vr)
{
  pool *p = vr->r->pool;
  char *args;
  table *result;
  const char *arg_p, *entry;
  char *key, *val;

  args = vr->args;
  if (args == NULL)
    return NULL;

  /* todo: check content-type for POSTed args */

  result = ap_make_table (p, 8);
  arg_p = args;
  while (arg_p[0] != '\0' && (entry = ap_getword (p, &arg_p, '&')))
    {
      key = ap_getword (p, &entry, '=');
      val = ap_pstrdup (p, entry);
      ap_unescape_url (key);
      unescape_url_info (val);
      ap_table_merge (result, key, val);
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
req_get_tmetric (VirguleReq *vr)
{
  char *result;
  if (vr->tmetric)
    return vr->tmetric;

  result = tmetric_get (vr);
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
req_get_tmetric_level (VirguleReq *vr, const char *u)
{
  char *result;
  char *tmetric = req_get_tmetric (vr);
  int i, j;

  if (tmetric == NULL)
    return cert_level_to_name (vr, CERT_LEVEL_NONE);

  /* This is a hackish linear scan through the tmetric table.
     At some point, we may want to use a real binary tree. */
  for (i = 0; tmetric[i];)
    {
      for (j = 0; u[j]; j++)
	if (tmetric[i++] != u[j])
	  break;
      if (u[j] == 0 && tmetric[i++] == ' ')
	{
	  /* found */
	  for (j = 0; tmetric[i + j] && tmetric[i + j] != '\n'; j++);
	  result = ap_palloc (vr->r->pool, j + 1);
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

  return cert_level_to_name (vr, CERT_LEVEL_NONE);
}

/**
 * req_ok_to_post: Determine if it's ok for the user to post.
 * @vr: The #VirguleReq context.
 *
 * Currently, this implements the policy that it's ok to post if the
 * user comes out with a non-zero trust metric certification.
 *
 * Return value: TRUE if it's ok for the user to post.
 **/
int
req_ok_to_post (VirguleReq *vr)
{
  auth_user (vr);

  if (vr->u == NULL)
    return 0;
  return (strcmp (req_get_tmetric_level (vr, vr->u),
		  cert_level_to_name (vr, CERT_LEVEL_NONE)));
}
