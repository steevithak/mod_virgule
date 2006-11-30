/* A module for posting news stories. */

#include <time.h>

#include "httpd.h"

#include <tree.h>

#include "buffer.h"
#include "db.h"
#include "req.h"
#include "util.h"
#include "style.h"
#include "auth.h"
#include "db_xml.h"
#include "xml_util.h"

#include "post.h"

static int
post_form_serve (VirguleReq *vr)
{
  Buffer *b = vr->b;

  auth_user (vr);

  if (vr->u == NULL)
    return send_error_page (vr, "Not logged in", "You can't post <x>an article</x> because you're not logged in.");

  render_header (vr, "Post a new <x>article</x>");

  buffer_puts (b, "<p> Post a new <x>article</x>. </p>\n"
	       "<form method=\"POST\" action=\"postsubmit.html\">\n"
	       " <p> <x>Article</x> title: <br>\n"
	       " <input type=\"text\" name=\"title\" size=50> </p>\n"
	       " <p> <x>Article</x> lead: <br>\n"
	       " <textarea name=\"lead\" cols=72 rows=4 wrap=soft>"
	       "</textarea> </p>\n"
	       " <p> Body of <x>article</x>: <br>\n"
	       " <textarea name=\"body\" cols=72 rows=16 wrap=soft>"
	       "</textarea> </p>\n"
	       " <p> <input type=\"submit\" value=\"Post\">\n"
	       "</form>\n");

  render_acceptable_html (vr);

  return render_footer_send (vr);
}

static int
post_submit_serve (VirguleReq *vr)
{
  pool *p = vr->r->pool;
  table *args;
  const char *title, *lead, *body;
  const char *date;
  char *key;
  xmlDoc *doc;
  xmlNode *root;
  xmlNode *tree;
  int status;

  db_lock_upgrade(vr->lock);
  auth_user (vr);
  if (vr->u == NULL)
    return send_error_page (vr, "Not logged in", "You can't post <x>an article</x> because you're not logged in.");

  args = get_args_table (vr);
  date = iso_now (p);
  title = nice_text (p, ap_table_get (args, "title"));
  lead = nice_text (p, ap_table_get (args, "lead"));
  body = nice_text (p, ap_table_get (args, "body"));

  if (title[0] == 0)
    return send_error_page (vr, "Need title", "Your <x>article</x> needs a title. Go back and try again.");
  if (lead[0] == 0)
    return send_error_page (vr, "Need lead", "Your <x>article</x> needs a lead. Go back and try again.");

  key = ap_psprintf (p, "articles/_%d/article.xml",
		     db_dir_max (vr->db, "articles") + 1);

  doc = db_xml_doc_new (p);
  root = xmlNewDocNode (doc, NULL, "article", NULL);
  doc->root = root;

  tree = xmlNewChild (root, NULL, "date", date);
  tree = xmlNewChild (root, NULL, "author", vr->u);

  tree = xmlNewChild (root, NULL, "title", NULL);
  xmlAddChild (tree, xmlNewDocText (doc, title));

  tree = xmlNewChild (root, NULL, "lead", NULL);
  xmlAddChild (tree, xmlNewDocText (doc, lead));

  if (body[0])
    {
      tree = xmlNewChild (root, NULL, "body", NULL);
      xmlAddChild (tree, xmlNewDocText (doc, body));
    }

  status = db_xml_put (p, vr->db, key, doc);

  if (status)
    return send_error_page (vr,
			    "Error storing <x>article</x>",
			    "There was an error storing the <x>article</x>. This means there's something wrong with the site.");

  return send_error_page (vr, "<x>Article</x> posted", "Ok, your <x>article</x> was posted. Thanks!");
}

int
post_serve (VirguleReq *vr)
{
  const char *p;
  if ((p = match_prefix (vr->uri, "/post/")) == NULL)
    return DECLINED;

  if (!strcmp (p, "post.html"))
    return post_form_serve (vr);

  if (!strcmp (p, "postsubmit.html"))
    return post_submit_serve (vr);

  return DECLINED;
}
