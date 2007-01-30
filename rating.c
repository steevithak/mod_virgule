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

  virgule_db_lock_upgrade (vr->lock);
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
	return virgule_send_error_page (vr, "Rating out of range",
				"Ratings must be from 1 to 10.");
      subj = apr_pstrcat (vr->r->pool, "d/", subject, NULL);
      virgule_eigen_set_local (vr, subj, (double)rating);
      return virgule_send_error_page (vr, "Submitted",
			     "Your rating of %s's blog as %d is noted. Thanks.",
			     subject, rating);
    }
  else
    return virgule_send_error_page (vr, "Not logged in",
			    "You need to be logged in to rate diaries.");
}

static int
rating_crank (VirguleReq *vr, const char *u)
{
  int status;
  const char *reason = virgule_validate_username (vr, u);

  if (reason)
    return virgule_send_error_page (vr, "Username error", reason);

  virgule_render_header (vr, "Crank", NULL);
  virgule_buffer_printf (vr->b, "<p>Cranking node %s.</p>\n", u);
  status = virgule_eigen_crank (vr->r->pool, vr, u);
  if (status)
    virgule_buffer_printf (vr->b, "<p>Error.</p>\n");
  return virgule_render_footer_send (vr);
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

  virgule_render_header (vr, "Cranking all nodes", NULL);

  
  while(tmetric[i])
    {

      /* Release lock so we don't hog the system during the update */
      virgule_db_unlock (vr->lock);
      apr_sleep(1);

      for(j = 0; tmetric[i + j] && tmetric[i + j] != '\n'; j++); /* EOL */
      for(k = j; k > 0 && tmetric[i + k] != ' '; k--); /* username */
      user = apr_palloc (vr->r->pool, k + 1);
      memcpy (user, tmetric + i, k);
      user[k] = 0;
      i += j;
      if (tmetric[i] == '\n')
        i++;
#if 0
virgule_buffer_printf (vr->b, "<p>Cranking node %s.</p>\n", user);
#endif
      apr_pool_create(&sp,vr->r->pool);
   
      /* re-establish XML file system lock */
      vr->lock = virgule_db_lock (vr->db);
      
      virgule_eigen_crank (sp, vr, user);
      apr_pool_destroy (sp);
    }


  return virgule_render_footer_send (vr);
}


static int
rating_report (VirguleReq *vr, const char *u)
{
  const char *reason = virgule_validate_username (vr, u);

  if (reason)
    return virgule_send_error_page (vr, "Username error", reason);

  virgule_render_header (vr, "Report", NULL);
  virgule_buffer_printf (vr->b, "<p>Reporting node %s.</p>\n", u);
  virgule_eigen_report (vr, u);
  return virgule_render_footer_send (vr);
}

int
virgule_rating_serve (VirguleReq *vr)
{
  const char *tail;

  if (!strcmp (vr->uri, "/rating/rate_diary.html"))
    return rating_rate_diary (vr);
  if (!strcmp (vr->uri, "/admin/crank-diaryratings.html"))
    return rating_crank_all (vr);
  if ((tail = virgule_match_prefix (vr->uri, "/rating/crank/")) != NULL)
    return rating_crank (vr, tail);
  if ((tail = virgule_match_prefix (vr->uri, "/rating/report/")) != NULL)
    return rating_report (vr, tail);
  return DECLINED;
}
