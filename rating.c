/* Interface for diary ratings. The actual computation is done in eigen.c.
   Note that the eigenvector computation is quite general, but diary ratings
   are specific. */

#include <apr.h>
#include <apr_pools.h>
#include <apr_strings.h>
#include <httpd.h>

#include <libxml/tree.h>

#include "private.h"
#include "buffer.h"
#include "db.h"
#include "req.h"
#include "util.h"
#include "hashtable.h"
#include "db_xml.h"
#include "xml_util.h"
#include "acct_maint.h"
#include "auth.h"
#include "style.h"
#include "eigen.h"
#include "rating.h"

/**
 * Render a form soliciting a diary rating. */
void
virgule_rating_diary_form (VirguleReq *vr, const char *u)
{
  Buffer *b = vr->b;
  int i;

  /* Don't solicit self-rating. */
  if (!strcmp (vr->u, u))
    return;

  /* todo: report rating if present in db */
  virgule_buffer_printf (b, "<form method=\"POST\" action=\"%s/rating/rate_diary.html\">\n"
		 "How interesting is %s's blog, on a 1 to 10 scale?\n"
		 " <select name=\"rating\" value=\"rating\">\n"
		 " <option selected>--\n",
		 vr->prefix, u);
  for (i = 1; i <= 10; i++)
    virgule_buffer_printf (b, " <option> %d\n", i);
  virgule_buffer_printf (b, " </select>\n"
		 " <input type=\"submit\" value = \"Submit\">\n"
		 " <input type=\"hidden\" name=\"subject\" value=\"%s\">\n"
		 "</form>\n", u);
}

static int
rating_rate_diary (VirguleReq *vr)
{
  apr_table_t *args;

  virgule_auth_user (vr);
  args = virgule_get_args_table (vr);

  if (vr->u)
    {
      const char *subject;
      const char *rating_s;
      int rating;
      char *subj;

      subject = apr_table_get (args, "subject");
      rating_s = apr_table_get (args, "rating");
      rating = atoi (rating_s);
      if (rating < 1 || rating > 10)
	return virgule_send_error_page (vr, vERROR, "out of range",
				"Ratings must be from 1 to 10.");
      subj = apr_pstrcat (vr->r->pool, "d/", subject, NULL);
      virgule_eigen_set_local (vr, subj, (double)rating);
      return virgule_send_error_page (vr, vINFO, "Submitted",
			     "Your rating of %s's blog as %d is noted. Thanks.",
			     subject, rating);
    }
  else
    return virgule_send_error_page (vr, vERROR, "forbidden",
			    "You need to be logged in to rate diaries.");
}


/**
 * rating_crank: Updates ratings for the specified user. Called by hitting
 * the URL /rating/crank/username
 * ToDo: Should this be an admin function? (e.g. /admin/rating/crank/user)
 */
static int
rating_crank (VirguleReq *vr, const char *u)
{
  int status;
  const char *reason = virgule_validate_username (vr, u);

  if (reason)
    return virgule_send_error_page (vr, vERROR, "username", reason);

  status = virgule_eigen_crank (vr->r->pool, vr, u);

  if (status)
    return virgule_send_error_page (vr, vERROR, "rating", "Unable to crank rating for node <em>%s</em>.", u);
  else
    return virgule_send_error_page (vr, vINFO, "rating", "Rating cranked for node <em>%s</em>.", u);
}


/**
 * rating_crank_all: This is a complete rewrite of Raph's original. There
 * is an approximate 30% speed improvement from using the presorted tmetric
 * cache instead of looping through every account in the XML acct tree and
 * testing to see if it's an alias or not. 
 * There is room for further improvement. The memory pool allocation in
 * each iteration of the loop is a work around for a memory leak somewhere 
 * in the eigen code.
 **/
static int
rating_crank_all (VirguleReq *vr)
{
  char *tmetric = virgule_req_get_tmetric (vr);
  char *user;
  int i = 0;
  int j, k;
  apr_pool_t *sp;

  while(tmetric[i])
    {
      for(j = 0; tmetric[i + j] && tmetric[i + j] != '\n'; j++); /* EOL */
      for(k = j; k > 0 && tmetric[i + k] != ' '; k--); /* username */
      user = apr_palloc (vr->r->pool, k + 1);
      memcpy (user, tmetric + i, k);
      user[k] = 0;
      i += j;
      if (tmetric[i] == '\n')
        i++;

      apr_pool_create(&sp,vr->r->pool);   
      virgule_eigen_crank (sp, vr, user);
      apr_pool_destroy (sp);
    }

  return virgule_send_error_page (vr, vINFO, "rating", "Ratings cranked for all nodes.");
}


/**
 * rating_report: Render a rating report for the specified user node. Called
 * by hitting /rating/report/username
 */
static int
rating_report (VirguleReq *vr, const char *u)
{
  const char *reason = virgule_validate_username (vr, u);

  if (reason)
    return virgule_send_error_page (vr, vERROR, "username", reason);

  if (virgule_set_temp_buffer (vr) != 0)
    return HTTP_INTERNAL_SERVER_ERROR;

  virgule_buffer_printf (vr->b, "<p>Reporting node <em>%s</em>.</p>\n", u);
  virgule_eigen_report (vr, u);

  virgule_set_main_buffer (vr);
  return virgule_render_in_template (vr, "/templates/default.xml", "content", "Rating Report");
}


/**
 * rating_clean - Remove rating files for accounts that no longer exist. Read
 * rating files for the remaining good accounts and remove any ratings of 
 * nonexistent users. Because Raph's hash implementation does not provide a
 * way of removing a key from the hash, it's necesary to create a new hash 
 * for each rating table, leaving out the keys that should be deleted. At 
 * some point, Raph's hash implementation should be replaced with the one in
 * the Apache APR and this code can be simplified.
 */
static int
rating_clean (VirguleReq *vr)
{
  apr_finfo_t finfo;
  apr_status_t status;
  DbCursor *dbc;
  char *u, *eigenkey, *profilekey, *profile;
  
  if (virgule_set_temp_buffer (vr) != 0)
    return HTTP_INTERNAL_SERVER_ERROR;
    
  virgule_buffer_puts (vr->b, "<h2>Analyzing local eigen vector cache</h2>\n");
  
  dbc = virgule_db_open_dir (vr->db, "eigen/local");
  while ((u = virgule_db_read_dir_raw (dbc)) != NULL)
    {
      eigenkey = apr_pstrcat (vr->r->pool, "eigen/local/", u, NULL);
      profilekey = apr_pstrcat (vr->r->pool, "acct/", u, "/profile.xml", NULL);
      profile = virgule_db_mk_filename (vr->r->pool, vr->db, profilekey);    
      status = apr_stat (&finfo, profile, APR_FINFO_MIN, vr->r->pool);
      if (status == APR_SUCCESS)
        {
          const char *key;
          void *val;
	  HashTable *elt1, *elt2;
	  HashTableIter *iter;
	  elt1 = virgule_eigen_local_load (vr->r->pool, vr, eigenkey);
	  elt2 = virgule_hash_table_new (vr->r->pool);
          for (iter = virgule_hash_table_iter (vr->r->pool, elt1);
              virgule_hash_table_iter_get (iter, &key, &val);
              virgule_hash_table_iter_next (iter))
	    {
              profilekey = apr_pstrcat (vr->r->pool, "acct/", key+2, "/profile.xml", NULL);
              profile = virgule_db_mk_filename (vr->r->pool, vr->db, profilekey);
              status = apr_stat (&finfo, profile, APR_FINFO_MIN, vr->r->pool);
              if (status != APR_SUCCESS)
	        {
                  virgule_buffer_printf (vr->b, "Removed rating of nonexistent user: %s by user: %s<br/>\n", key+2, u);
                }
	      else 
	        {
		  virgule_hash_table_set (vr->r->pool, elt2, key, val);
		}
	    }
	  virgule_eigen_local_store (vr, elt2, eigenkey);
	}
      else
        {
	  virgule_buffer_printf (vr->b, "Deleted eigen cache for nonexistent user: %s<br/>\n", eigenkey);
          virgule_db_del (vr->db, eigenkey);
        }
    }
    
  virgule_buffer_puts (vr->b, "<p>Eigen cache cleanup is complete!</p>\n");
  virgule_set_main_buffer (vr);
  return virgule_render_in_template (vr, "/templates/default.xml", "content", "Diary Rating Maintenance");
}


int
virgule_rating_serve (VirguleReq *vr)
{
  const char *tail;

  if (!strcmp (vr->uri, "/rating/rate_diary.html"))
    return rating_rate_diary (vr);
  if (!strcmp (vr->uri, "/admin/crank-diaryratings.html"))
    return rating_crank_all (vr);
  if (!strcmp (vr->uri, "/admin/clean-diaryratings.html"))
    return rating_clean (vr);
  if ((tail = virgule_match_prefix (vr->uri, "/rating/crank/")) != NULL)
    return rating_crank (vr, tail);
  if ((tail = virgule_match_prefix (vr->uri, "/rating/report/")) != NULL)
    return rating_report (vr, tail);
  return DECLINED;
}
