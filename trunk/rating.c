/* Interface for diary ratings. The actual computation is done in eigen.c.
   Note that the eigenvector computation is quite general, but diary ratings
   are specific. */

#include "httpd.h"

#include <libxml/tree.h>

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
rating_diary_form (VirguleReq *vr, const char *u)
{
  Buffer *b = vr->b;
  int i;

  /* Don't solicit self-rating. */
  if (!strcmp (vr->u, u))
    return;

  /* todo: report rating if present in db */
  buffer_printf (b, "<form method=\"POST\" action=\"%s/rating/rate_diary.html\">\n"
		 "How interesting is %s's diary, on a 1 to 10 scale?\n"
		 " <select name=\"rating\" value=\"rating\">\n"
		 " <option selected>--\n",
		 vr->prefix, u);
  for (i = 1; i <= 10; i++)
    buffer_printf (b, " <option> %d\n", i);
  buffer_printf (b, " </select>\n"
		 " <input type=\"submit\" value = \"Submit\">\n"
		 " <input type=\"hidden\" name=\"subject\" value=\"%s\">\n"
		 "</form>\n", u);
}

static int
rating_rate_diary (VirguleReq *vr)
{
  table *args;

  db_lock_upgrade (vr->lock);
  auth_user (vr);

  args = get_args_table (vr);

  if (vr->u)
    {
      const char *subject;
      const char *rating_s;
      int rating;
      char *subj;

      subject = ap_table_get (args, "subject");
      rating_s = ap_table_get (args, "rating");
      rating = atoi (rating_s);
      if (rating < 1 || rating > 10)
	return send_error_page (vr, "Rating out of range",
				"Ratings must be from 1 to 10.");
      subj = ap_pstrcat (vr->r->pool, "d/", subject, NULL);
      eigen_set_local (vr, subj, (double)rating);
      return send_error_page (vr, "Submitted",
			     "Your rating of %s's diary page as %d is noted. Thanks.",
			     subject, rating);
    }
  else
    return send_error_page (vr, "Not logged in",
			    "You need to be logged in to rate diaries.");
}

static int
rating_crank (VirguleReq *vr, const char *u)
{
  int status;
  const char *reason = validate_username (u);

  if (reason)
    return send_error_page (vr, "Username error", reason);

  render_header (vr, "Crank", NULL);
  buffer_printf (vr->b, "<p>Cranking node %s.</p>\n", u);
  status = eigen_crank (vr->r->pool, vr, u);
  if (status)
    buffer_printf (vr->b, "<p>Error.</p>\n");
  return render_footer_send (vr);
}

static int
rating_crank_all (VirguleReq *vr)
{
  pool *p = vr->r->pool;
  DbCursor *dbc;
  char *u;

  render_header (vr, "Cranking all nodes", NULL);
  dbc = db_open_dir (vr->db, "acct");
  while ((u = db_read_dir_raw (dbc)) != NULL)
    {
      pool *sp = ap_make_sub_pool (p);
      char *dbkey = acct_dbkey (sp, u);
      xmlDoc *profile = db_xml_get (sp, vr->db, dbkey);
      xmlNode *tree = xml_find_child (profile->xmlRootNode, "info");

      if (tree != NULL)
	{
#if 0
	  buffer_printf (vr->b, "<p>Cranking node %s.</p>\n", u);
#endif
	  eigen_crank (sp, vr, u);
	}
      ap_destroy_pool (sp);
    }
  return render_footer_send (vr);
}

static int
rating_report (VirguleReq *vr, const char *u)
{
  const char *reason = validate_username (u);

  if (reason)
    return send_error_page (vr, "Username error", reason);

  render_header (vr, "Report", NULL);
  buffer_printf (vr->b, "<p>Reporting node %s.</p>\n", u);
  eigen_report (vr, u);
  return render_footer_send (vr);
}

int
rating_serve (VirguleReq *vr)
{
  const char *tail;

  if (!strcmp (vr->uri, "/rating/rate_diary.html"))
    return rating_rate_diary (vr);
  if (!strcmp (vr->uri, "/rating/crank.html"))
    return rating_crank_all (vr);
  if ((tail = match_prefix (vr->uri, "/rating/crank/")) != NULL)
    return rating_crank (vr, tail);
  if ((tail = match_prefix (vr->uri, "/rating/report/")) != NULL)
    return rating_report (vr, tail);
  return DECLINED;
}
