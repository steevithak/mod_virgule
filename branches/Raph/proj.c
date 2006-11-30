/* This file sets up a data model for projects. */

#include "httpd.h"

#include <tree.h>

#include "buffer.h"
#include "db.h"
#include "req.h"
#include "db_xml.h"
#include "style.h"
#include "util.h"
#include "auth.h"
#include "xml_util.h"
#include "db_ops.h"
#include "certs.h"
#include "schema.h"
#include "acct_maint.h"

#include "proj.h"

static void proj_render_replies (VirguleReq *vr, const char *name);

/* Schema for project page. */

static SchemaField rproj_fields[] = {
  { "name", "<x>Project</x> name", "proj/", 20, 0 },
  { "url", "<x>Project</x> homepage URL", NULL, 60, 0 },
  { "fmurl", "<a href=\"http://freshmeat.net/\">Freshmeat</a> URL", NULL, 60, 0 },
  { "license", "License", NULL, 20, 0 },
  { "notes", "Notes", NULL, 60016, SCHEMA_TEXTAREA },
  { NULL }
};

static SchemaField nproj_fields[] = {
  { "name", "Name", "proj/", 20, 0 },
  { "url", "URL", NULL, 60, 0 },
  { "notes", "Notes", NULL, 60016, SCHEMA_TEXTAREA },
  { NULL }
};

static char *staff_rels[] = {
  "None",
  "Helper",
  "Documenter",
  "Contributor",
  "Developer",
  "Lead Developer",
  NULL
};

static SchemaField staff_fields[] = {
  { "name", "<x>Project</x> name", "proj/", 20, 0 },
  { "person", "<x>Person</x>", "acct/", 20, 0 },
  { "type", "Type of relationship", NULL, 20, SCHEMA_SELECT, staff_rels }
};

static DbField staff_db_fields[] = {
  { "name", "proj/", DB_FIELD_INDEX | DB_FIELD_UNIQUE },
  { "person", "acct/", DB_FIELD_INDEX | DB_FIELD_UNIQUE },
  { "type", NULL, 0 }
};

static DbRelation staff_db_rel = {
  "staff",
  sizeof (staff_db_fields) / sizeof (staff_db_fields[0]),
  staff_db_fields,
  0
};

/**
 * proj_url: Get a url for the project name.
 * @proj: The project name.
 *
 * Return value: A url for the project
 **/
static char *
proj_url (VirguleReq *vr, const char *proj)
{
  pool *p = vr->r->pool;
  return ap_pstrcat (p, vr->prefix, "/proj/", escape_uri_arg (p, proj),
		     vr->projstyle == PROJSTYLE_RAPH ? "/" : "/#lastread",
		     NULL);
}

char *
render_proj_name (VirguleReq *vr, const char *proj)
{
  pool *p = vr->r->pool;

  return ap_pstrcat (p, "<a href=\"", proj_url (vr, proj), "\">",
		     nice_text (p, proj),
		     "</a>", NULL);
}

/**
 * proj_ok_to_edit: Determine whether user is ok to edit the project page.
 * @doc: The document for the project.
 *
 * Implements the policy that it is ok to edit if (a) you're certified or
 * (b) it is not locked and you are the creator.
 *
 * Return value: TRUE if ok to edit project.
 **/
static int
proj_ok_to_edit (VirguleReq *vr, xmlDoc *doc)
{
  pool *p = vr->r->pool;
  xmlNode *tree;
  char *creator;
  char *locked;

  auth_user (vr);

  if (vr->u == NULL)
    return 0;

  if (strcmp (req_get_tmetric_level (vr, vr->u),
		  cert_level_to_name (vr, CERT_LEVEL_NONE)))
    return 1;

  tree = xml_find_child (doc->root, "info");

  locked = xml_get_prop (p, tree, "locked");
  if (locked && !strcmp (locked, "yes"))
    return 0;
  creator = xml_get_prop (p, tree, "creator");

  return (!strcmp (vr->u, creator));
}

static int
proj_new_serve (VirguleReq *vr)
{
  
/* This should really be in XML. But two things:

     First, it's dependent on being authenticated (but not necessarily
     certified).

     Second, all these forms should be schema based and mostly automated.

     But for the time being, the path of least resistance is to write
     a pile of C. So here goes.
*/
  pool *p = vr->r->pool;
  Buffer *b = vr->b;
  const char *rfields[] = { "name", "url", "fmurl", "license", "notes", NULL };
  const char *nfields[] = { "name", "url", "notes", NULL };
  const char **fields = vr->projstyle == PROJSTYLE_RAPH ? rfields : nfields;
  SchemaField *proj_fields = vr->projstyle == PROJSTYLE_RAPH ? rproj_fields :
    							       nproj_fields;

  auth_user (vr);

  if (vr->u == NULL)
    return send_error_page (vr, "Not logged in", "You can't create a new <x>project</x> because you're not logged in.");

  render_header (vr, "Create new <x>project</x>");

  buffer_puts (b, "<p> Create a new <x>project</x>: </p>\n"
	       "<form method=\"POST\" action=\"newsub.html\">\n");

  schema_render_inputs (p, b, proj_fields, fields, NULL);


  buffer_puts (b, " <p> <input type=\"submit\" value=\"Create\">\n"
	       "</form>\n");

  render_acceptable_html (vr);

  return render_footer_send (vr);
  
}

static int
proj_newsub_serve (VirguleReq *vr)
{
  pool *p = vr->r->pool;
  Db *db = vr->db;
  table *args;
  const char *date;
  const char *name, *url, *fmurl, *license, *notes;
  char *db_key;
  xmlDoc *doc;
  xmlNode *root, *tree;
  int status;

  db_lock_upgrade(vr->lock);
  auth_user (vr);

  if (vr->u == NULL)
    return send_error_page (vr, "Not logged in", "You can't create a new <x>project</x> because you're not logged in.");

  args = get_args_table (vr);

  name = ap_table_get (args, "name");
  url = ap_table_get (args, "url");
  fmurl = ap_table_get (args, "fmurl");
  license = ap_table_get (args, "license");
  notes = ap_table_get (args, "notes");
  date = iso_now (p);

  if (!name[0])
    return send_error_page (vr, "Specify <x>a project</x> name",
			    "You must specify <x>a project</x> name.");

  if (strlen (name) > 50)
    return send_error_page (vr, "<x>Project</x> name too long",
			    "The <x>project</x> name must be 50 characters or less.");

  if (name[0] == '.')
    return send_error_page (vr, "<x>Project</x> name can't begin with dot",
			    "The <x>project</x> can't begin with a dot, sorry.");

  if (strchr (name, '/'))
    return send_error_page (vr, "<x>Project</x> name can't have slash",
			    "The <x>project</x> can't have a slash, sorry.");

  db_key = ap_psprintf (p, "proj/%s/info.xml", name);
  doc = db_xml_get (p, db, db_key);
  if (doc != NULL)
    return send_error_page (vr,
			    "<x>Project</x> already exists",
			    "The <x>project</x> name <tt>%s</tt> already exists.",
			    render_proj_name (vr, name));

  doc = db_xml_doc_new (p);

  root = xmlNewDocNode (doc, NULL, "info", NULL);
  doc->root = root;

  tree = xmlNewChild (root, NULL, "cdate", date);
  tree = xmlNewChild (root, NULL, "info", NULL);
  xmlSetProp (tree, "url", url);
  xmlSetProp (tree, "fmurl", fmurl);
  xmlSetProp (tree, "license", license);
  xmlSetProp (tree, "notes", notes);
  xmlSetProp (tree, "creator", vr->u);
  if (req_ok_to_post (vr))
    xmlSetProp (tree, "locked", "yes");

  status = db_xml_put (p, db, db_key, doc);
  if (status)
    return send_error_page (vr,
			    "Error storing <x>project</x>",
			    "There was an error storing the <x>project</x>. This means there's something wrong with the site.");

  add_recent (p, db, "recent/proj-c.xml", name, 50, 0);
  add_recent (p, db, "recent/proj-m.xml", name,
	      vr->projstyle == PROJSTYLE_RAPH ? 50 : -1, 0);

  return send_error_page (vr,
			  "<x>Project</x> created",
			  "<x>Project</x> %s created.\n",
			  render_proj_name (vr, name));
}

static int
proj_index_serve (VirguleReq *vr)
{
  pool *p = vr->r->pool;
  Buffer *b = vr->b;
  Db *db = vr->db;
  DbCursor *dbc;
  char *proj;

  auth_user (vr);

  render_header (vr, "<x>Project</x> index");
  if (vr->projstyle == PROJSTYLE_RAPH)
    buffer_puts (b, "<ul>\n");
  dbc = db_open_dir (db, "proj");

  while ((proj = db_read_dir_raw (dbc)) != NULL)
    {
      if (vr->projstyle == PROJSTYLE_RAPH)
	{
	  buffer_printf (b, "<li>%s\n", render_proj_name (vr, proj));
	}
      else /* vr->projstyle == PROJSTYLE_NICK */
	{
	  char *db_key = ap_psprintf (p, "proj/%s/info.xml", proj);
	  char *creator;
	  xmlDoc *proj_doc;
	  xmlNode *proj_tree;

	  proj_doc = db_xml_get (p, vr->db, db_key);
	  if (proj_doc == NULL)
	    /* the project doesn't exist, so skip it */
	    continue;

	  proj_tree = xml_find_child (proj_doc->root, "info");
	  if (proj_tree != NULL)
	    creator = xml_get_prop (p, proj_tree, "creator");
	  else
	    {
	      /* No creator?  Skip it. */
	      db_xml_free (p, vr->db, proj_doc);
	      continue;
	    }

	  render_cert_level_begin (vr, creator, CERT_STYLE_SMALL);
	  buffer_printf (b, "%s", render_proj_name (vr, proj));
#if 0 /* I don't like how this looks, but you might */
	  buffer_printf (vr->b, " - <a href=\"%s/person/%s/\" style=\"text-decoration: none\">%s</a>\n",
			 vr->prefix, creator, creator);
#endif
	  render_cert_level_text (vr, creator);
	  render_cert_level_end (vr, CERT_STYLE_SMALL);
	  db_xml_free (p, vr->db, proj_doc);
	}
    }
  if (vr->projstyle == PROJSTYLE_RAPH)
    buffer_puts (b, "</ul>\n");

  if (vr->u != NULL)
    buffer_puts (b, "<p> <a href=\"new.html\">Create</a> a new <x>project</x>...</p>\n");
  db_close_dir (dbc);

  return render_footer_send (vr);
}

static int
proj_proj_serve (VirguleReq *vr, const char *path)
{
  Buffer *b = vr->b;
  request_rec *r = vr->r;
  pool *p = r->pool;
  char *q;
  char *name;
  char *db_key;
  char *title;
  xmlDoc *doc, *staff;
  xmlNode *tree;
  char *err;
  char *cdate;
  const char *fields[] = { "type", NULL };
  char *first;
  xmlNode *myrel;
  char *proj_dir;
  int n_replies;

  auth_user (vr);

  q = strchr ((char *)path, '/');
  if (q == NULL)
    {
      ap_table_add (r->headers_out, "Location",
		    ap_make_full_path (r->pool, r->uri, ""));
      return REDIRECT;
    }

  if (q[1] != '\0')
    return send_error_page (vr,
			    "Extra junk",
			    "Extra junk after <x>project</x> name not allowed.");

  name = ap_pstrndup (p, path, q - path);

  db_key = ap_psprintf (p, "proj/%s/info.xml", name);

  doc = db_xml_get (p, vr->db, db_key);
  if (doc == NULL)
    return send_error_page (vr,
			    "<x>Project</x> not found",
			    "<x>Project</x> <tt>%s</tt> was not found.",
			    nice_text (p, name));

  if (vr->projstyle == PROJSTYLE_RAPH)
    {
      title = ap_psprintf (p, "<x>Project</x> info for %s", nice_text (p, name));
      render_header (vr, title);
    }
  else /* vr->projstyle == PROJSTYLE_NICK */
    {
      title = ap_psprintf(p, "%s", nice_text (p, name));
      render_header_raw (vr, title);
    }

  cdate = xml_find_child_string (doc->root, "cdate", "--no date--");

  tree = xml_find_child (doc->root, "info");
  if (tree != NULL)
    {
      char *url;
      char *fmurl;
      char *notes;
      char *creator;
      char *license;
      char *lastmodby;
      char *mdate;

      creator = xml_get_prop (p, tree, "creator");
      lastmodby = xml_get_prop (p, tree, "lastmodby");

      if (vr->projstyle == PROJSTYLE_NICK)
	{
	  render_cert_level_begin (vr, creator, CERT_STYLE_LARGE);
	  buffer_puts (b, title);
	  render_cert_level_end (vr, CERT_STYLE_LARGE);
	}

      buffer_printf (b, "<p> Page created %s by <a href=\"%s/person/%s/\">%s</a>",
		     render_date (vr, cdate), vr->prefix, creator, creator);
      render_cert_level_text (vr, creator);

      if (lastmodby != NULL)
	{
	  mdate = xml_get_prop (p, tree, "mdate");
	  buffer_printf (b, ", last modified %s by <a href=\"%s/person/%s/\">%s</a>",
			 render_date (vr, mdate), vr->prefix,
			 lastmodby, lastmodby);
	  render_cert_level_text (vr, lastmodby);
	}

      if (proj_ok_to_edit (vr, doc))
	buffer_printf (b, " (<a href=\"../edit.html?proj=%s\">Edit...</a>)",
		       escape_uri_arg (p, name));
      buffer_puts (b, "</p>\n");

      url = xml_get_prop (p, tree, "url");
      if (url && url[0])
	{
	  buffer_puts (b, render_url (p,
					vr->projstyle == PROJSTYLE_RAPH ?
					" Homepage: " :
					" URL: ",
					url));
	}
      fmurl = xml_get_prop (p, tree, "fmurl");
      if (fmurl && fmurl[0])
	{
	  buffer_puts (b,
			 render_url (p, " Freshmeat page: ", fmurl));
	}

      notes = xml_get_prop (p, tree, "notes");
      if (notes && notes[0])
	buffer_printf (b, "<p>  %s </p>\n", nice_htext (vr, notes, &err));

      license = xml_get_prop (p, tree, "license");
      if (license && license[0])
	buffer_printf (b, "<p> License: %s </p>\n", nice_text (p, license));

      if (vr->projstyle == PROJSTYLE_RAPH)
	{
	  /* Render staff listings */
	  myrel = NULL;
	  first = "<p> This <x>project</x> has the following developers: </p>\n"
	    "<ul>\n";
	  db_key = ap_psprintf (p, "proj/%s/staff-name.xml", name);

	  staff = db_xml_get (p, vr->db, db_key);
	  if (staff != NULL)
	    {
	      for (tree = staff->root->childs; tree != NULL; tree = tree->next)
		{
		  char *person;
		  char *type;

		  person = xml_get_prop (p, tree, "person");
		  type = xml_get_prop (p, tree, "type");
		  if (vr->u != NULL && !strcmp (person, vr->u))
		    myrel = tree;
		  if (! !strcmp (type, "None"))
		    {
		      buffer_puts (b, first);
		      first = "";
		      buffer_printf (b, "<li><a href=\"%s/person/%s/\">%s</a> is a %s.\n",
				     vr->prefix, person, person, nice_text (p, type));
		    }
		}
	    }
	  if (first[0] == 0)
	    buffer_puts (b, "</ul>\n");

	  if (proj_ok_to_edit (vr, doc))
	    {
	      buffer_printf (b, "<p> I have the following relation to %s: </p>\n"
			     "<form method=\"POST\" action=\"../relsub.html\">\n"
			     "<input type=\"hidden\" name=\"name\" value=\"%s\">\n",
			     nice_text (p, name), escape_html_attr (p, name));
	      schema_render_inputs (p, b, staff_fields, fields, myrel);
	      buffer_puts (b, "<input type=\"submit\" value=\"Update\"/>\n"
			   "</form>\n");
	    }
	}
      else /* vr->projstyle == PROJSTYLE_NICK */
	{
	  /* Render replies */
	  proj_dir = ap_psprintf (vr->r->pool, "proj/%s", name);
	  n_replies = db_dir_max (vr->db, proj_dir) + 1;

	  proj_render_replies (vr, name);
	  if(vr->u != NULL)
	    buffer_puts (b,"<hr><div class=\"next\"><a class=\"next\" href=\"/proj/nextnew.html\"><b>Next&raquo;</b></a></div> \n");

	  if (req_ok_to_post (vr))
	    {
	      buffer_printf (b, "<p> Post a reply: %s. </p>\n"
		"<form method=\"POST\" action=\"/proj/replysubmit.html\">\n"
		" <p> Reply title: <br>\n"
		" <input type=\"text\" name=\"title\" size=50> </p>\n"
		" <p> Body of reply: <br>\n"
		" <textarea name=\"body\" cols=72 rows=16 wrap=soft>"
		"</textarea> </p>\n"
		" <input type=\"hidden\" name=\"name\" value=\"%s\">\n"
		" <p> <input type=\"submit\" name=post value=\"Post\">\n"
		" <input type=\"submit\" name=preview value=\"Preview\">\n"
		"</form>\n", nice_text (p, name), escape_html_attr (p, name));

	      render_acceptable_html (vr);
	    }
	}
    }

  return render_footer_send (vr);
}

static int
proj_next_new_serve (VirguleReq *vr)
{
  pool *p = vr->r->pool;
  xmlDoc *doc;
  xmlNode *root, *tree;

  doc = db_xml_get (p, vr->db, "recent/proj-m.xml");
  if (doc == NULL)
    return send_error_page (vr,
			    "Modification log not found",
			    "The modification log was not found.");
  root = doc->root;
  for (tree = root->childs; tree != NULL; tree = tree->next)
    {
      char *name = xml_get_string_contents (tree);
      char *date = xml_get_prop (p, tree, "date");
      char *lastread_date = acct_get_lastread_date (vr, "proj", name);
  
      if (lastread_date != NULL)
	if (strcmp (date, lastread_date) > 0)
	  {
	    char *db_key = ap_psprintf (p, "proj/%s/info.xml", name);
	    xmlDoc *testdoc = db_xml_get (p, vr->db, db_key);
	    if (testdoc == NULL)
	      continue;
	    else 
	      db_xml_free (p, vr->db, testdoc);

	    ap_table_add (vr->r->headers_out, "refresh", 
			  ap_psprintf(p, "0;URL=%s",
				      proj_url(vr, name))); 
	    return send_error_page (vr, "Next room", 
				    "The next room is %s.",
				    render_proj_name(vr, name), 
				    name);
	  }
    }
  db_xml_free (p, vr->db, doc);
  ap_table_add (vr->r->headers_out, "refresh", "0;URL=/");
  return send_error_page (vr, "Next room", "There are no more rooms with new messages.");
}

static int
proj_edit_serve (VirguleReq *vr)
{
  Buffer *b = vr->b;
  pool *p = vr->r->pool;
  table *args;
  const char *name;
  const char *fields[] = { "url", "fmurl", "license", "notes", NULL };
  char *db_key;
  xmlDoc *doc;
  xmlNode *tree;
  SchemaField *proj_fields = vr->projstyle == PROJSTYLE_RAPH ? rproj_fields :
    							       nproj_fields;

  args = get_args_table (vr);
  name = ap_table_get (args, "proj");

  db_key = ap_psprintf (p, "proj/%s/info.xml", name);

  doc = db_xml_get (p, vr->db, db_key);
  if (doc == NULL)
    return send_error_page (vr,
			    "<x>Project</x> not found",
			    "<x>Project</x> <tt>%s</tt> was not found.", name);

  if (!proj_ok_to_edit (vr, doc))
    return send_error_page (vr, "Not authorized",
			    "You are not authorized to edit <x>project</x> %s. You have to either be certified to %s level or higher, or be the creator of the <x>project</x> and not have anyone else edit the page before you.", name, cert_level_to_name (vr, 1));

  render_header (vr, "Edit <x>project</x>");

  buffer_printf (b, "<p> Edit <x>project</x> for %s: </p>\n"
	       "<form method=\"POST\" action=\"editsub.html\">\n"
		 "<input type=\"hidden\" name=\"name\" value=\"%s\">\n",
		 nice_text (p, name), escape_html_attr(p, name));

  tree = xml_find_child (doc->root, "info");

  schema_render_inputs (p, b, proj_fields, fields, tree);

  buffer_puts (b, " <input type=\"submit\" value=\"Update\"/>\n"
		   "</form>\n");

  render_acceptable_html (vr);

  return render_footer_send (vr);
}

static int
proj_editsub_serve (VirguleReq *vr)
{
  pool *p = vr->r->pool;
  Db *db = vr->db;
  table *args;
  const char *date;
  char *db_key;
  xmlDoc *doc;
  xmlNode *tree;
  int status;
  const char *fields[] = { "url", "fmurl", "license", "notes", NULL };
  const char *name;
  SchemaField *proj_fields = vr->projstyle == PROJSTYLE_RAPH ? rproj_fields :
    							       nproj_fields;

  db_lock_upgrade(vr->lock);
  args = get_args_table (vr);
  name = ap_table_get (args, "name");

  db_key = ap_psprintf (p, "proj/%s/info.xml", name);

  doc = db_xml_get (p, vr->db, db_key);
  if (doc == NULL)
    return send_error_page (vr,
			    "<x>Project</x> not found",
			    "<x>Project</x> <tt>%s</tt> was not found.", name);

  if (!proj_ok_to_edit (vr, doc))
    return send_error_page (vr, "Not authorized",
			    "You are not authorized to edit <x>project</x> %s. You have to either be certified to %s level or higher, or be the creator of the <x>project</x> and not have anyone else edit the page before you.", name, cert_level_to_name (vr, 1));
 
  tree = xml_find_child (doc->root, "info");
  date = iso_now (p);

  schema_put_fields (p, proj_fields, fields, tree, args);

  xmlSetProp (tree, "mdate", date);
  xmlSetProp (tree, "lastmodby", vr->u);

  if (req_ok_to_post (vr))
    xmlSetProp (tree, "locked", "yes");

  status = db_xml_put (p, db, db_key, doc);
  if (status)
    return send_error_page (vr,
			    "Error storing <x>project</x>",
			    "There was an error storing the <x>project</x>. This means there's something wrong with the site.");

  add_recent (p, db, "recent/proj-m.xml", name,
	      vr->projstyle == PROJSTYLE_RAPH ? 50 : -1, 0);

  return send_error_page (vr,
			  "<x>Project</x> updated",
			  "<x>Project</x> %s updated.\n",
			  render_proj_name (vr, name));
}

static int
proj_relsub_serve (VirguleReq *vr)
{
  pool *p = vr->r->pool;
  Db *db = vr->db;
  table *args;
  char *db_key;
  xmlDoc *doc;
  const char *name;
  const char *type;
  const char *values[3];

  args = get_args_table (vr);
  name = ap_table_get (args, "name");

  db_key = ap_psprintf (p, "proj/%s/info.xml", name);

  doc = db_xml_get (p, db, db_key);
  if (doc == NULL)
    return send_error_page (vr,
			    "<x>Project</x> not found",
			    "<x>Project</x> <tt>%s</tt> was not found.", name);

  if (!proj_ok_to_edit (vr, doc))
    return send_error_page (vr, "Not authorized",
			    "You are not authorized to edit <x>project</x> %s. You have to either be certified to %s level or higher, or be the creator of the <x>project</x> and not have anyone else edit the page before you.", name, cert_level_to_name (vr, 1));
 
  type = ap_table_get (args, "type");

  /* some this stuffing should probably be done by schema */
  values[0] = name;
  values[1] = vr->u;
  values[2] = type;

  db_relation_put (p, db, &staff_db_rel, values);

  return send_error_page (vr, "Relationship updated",
			  "The update of the relationship between <x>person</x> %s and <x>project</x> %s (type %s) is completed. Have a great day!",
			  vr->u, render_proj_name (vr, name), type);
}

static void
proj_render_reply (VirguleReq *vr, const char *name, int reply_num)
{
  pool *p = vr->r->pool;
  Buffer *b = vr->b;
  char *key;
  xmlDoc *doc;

  if (reply_num < 0)
    return;

  key = ap_psprintf (p, "proj/%s/_%d/reply.xml", name, reply_num);

  doc = db_xml_get (p, vr->db, key);
  if (doc != NULL)
    {
      xmlNode *root = doc->root;
      char *author;
      char *title;
      char *body;
      char *date;

      author = xml_find_child_string (root, "author", "(no author)");
      title = xml_find_child_string (root, "title", "(no title)");
      body = xml_find_child_string (root, "body", "(no body)");
      date = xml_find_child_string (root, "date", "(no date)");

      render_cert_level_begin (vr, author, CERT_STYLE_MEDIUM);
      buffer_printf (b, "<a name=\"%u\"><b>%s</b></a>, posted %s by <a href=\"%s/person/%s/\">%s</a> <a href=\"#%u\" style=\"text-decoration: none\">&raquo;</a>\n",
		     reply_num, title, render_date (vr, date), vr->prefix, author, author, reply_num);
      render_cert_level_text (vr, author);
      render_cert_level_end (vr, CERT_STYLE_MEDIUM);
      buffer_printf (b, "<blockquote>\n%s\n</blockquote>\n", body);
    }
  else
    {
      buffer_printf (b, "<p> Error reading <x>project</x> %s.\n", name);
    }
  db_xml_free (p, vr->db, doc);
}

static void
proj_render_replies (VirguleReq *vr, const char *name)
{
  int lastread, num_old, start;
  char *base;
  int n_art;
  int i;

  base = ap_psprintf (vr->r->pool, "proj/%s", name);
  n_art = db_dir_max (vr->db, base) + 1;
  lastread = acct_get_lastread (vr, "proj", name);
  num_old = acct_get_num_old (vr);
  start = lastread - num_old;

  if (start < 0)
    start = 0;
#if 0
  buffer_printf (vr->b, "<p> Rendering %d replies. </p>\n", n_art);
  buffer_printf (vr->b, "<!-- lastread is set to %d -->", lastread);
#endif
  if (n_art > 0)
    {
      buffer_puts (vr->b, "<hr>\n");
      for (i = start; i < n_art; i++)
	{
	  if (i == lastread)
	    buffer_puts (vr->b, "<a name=\"lastread\">");
	  proj_render_reply (vr, name, i);
	}
    }
  acct_set_lastread(vr, "proj", name, n_art - 1);
}

static int
proj_update_all_pointers_serve (VirguleReq *vr)
{
  pool *p = vr->r->pool;
  Db *db = vr->db;
  DbCursor *dbc;
  char *proj;

  auth_user (vr);

  dbc = db_open_dir (db, "proj");

  while ((proj = db_read_dir_raw (dbc)) != NULL)
    {
	char *base;
	int n_reply;

	base = ap_psprintf (p, "proj/%s", proj);
	n_reply = db_dir_max (db, base);
	acct_set_lastread(vr, "proj", proj, n_reply -1);
    }
  ap_table_add (vr->r->headers_out, "refresh", "0;URL=/");
  return send_error_page (vr, "Updated", "Your pointers have all been updated.");
}



static int
proj_reply_submit_serve (VirguleReq *vr)
{
  pool *p = vr->r->pool;
  Buffer *b = vr->b;
  table *args;
  const char *name, *title, *body;
  const char *date;
  char *key_base, *key;

  xmlDoc *doc;
  xmlNode *root;
  xmlNode *tree;
  int status;
  char *str; 
  char *body_error;
  char *nice_title;
  char *nice_body;


  args = get_args_table (vr);
  if (args == NULL)
    return send_error_page (vr, "Need form data", "This page requires a form submission. If you're not playing around manually with URLs, it suggests there's something wrong with the site.");

  /* XXX */
  name = ap_table_get (args, "name");
  title = ap_table_get (args, "title");
  body = ap_table_get (args, "body");

  if (name == NULL)
    return send_error_page (vr, "Need <x>project</x> name", "This page requires an <x>project</x> name. If you're not playing around manually with URLs, it suggests there's something wrong with the site.");

  key_base = ap_psprintf (p, "proj/%s", name);

  db_lock_upgrade (vr->lock);
  auth_user (vr);
  if (vr->u == NULL)
    return send_error_page (vr, "Not logged in", "You can't post because you're not logged in.");

  if (!req_ok_to_post (vr))
    return send_error_page (vr, "Not certified", "You can't post because you're not certified. Please see the <a href=\"%s/certs.html\">certification overview</a> for more details.", vr->prefix);

  date = iso_now (p);

  if (title == NULL || title[0] == 0)
    return send_error_page (vr, "Need title", "Your reply needs a title. Go back and try again.");
  if (body == NULL || body[0] == 0)
    return send_error_page (vr, "Need body", "Your reply needs a body. Go back and try again.");


  nice_title = nice_text (p, title);
  nice_body = body == NULL ? NULL : nice_htext (vr, body, &body_error);

  if (ap_table_get (get_args_table (vr), "preview"))
    {
      render_header (vr, "Reply preview");
      render_cert_level_begin (vr, vr->u, CERT_STYLE_MEDIUM);
      buffer_puts (b, nice_title);
      render_cert_level_end (vr, CERT_STYLE_MEDIUM);
      buffer_printf (b, "<p> %s </p>\n", nice_body);
      buffer_puts (b, "<hr>\n");
      buffer_printf (b, "<p> Edit your reply: </p>\n"
		     "<form method=\"POST\" action=\"replysubmit.html\">\n"
		     " <p> Reply title: <br>\n"
		     " <input type=\"text\" name=\"title\" value=\"%s\" size=50> </p>\n"
		     " <p> Body of reply: <br>\n"
		     " <textarea name=\"body\" cols=72 rows=16 wrap=soft>%s"
		     "</textarea> </p>\n"
		     " <input type=\"hidden\" name=\"name\" value=\"%s\">\n"
		     " <p> <input type=\"submit\" name=post value=\"Post\">\n"
		     " <input type=\"submit\" name=preview value=\"Preview\">\n"
		     "</form>\n",
		     escape_html_attr (p, title),
		     ap_escape_html (p, body),
		     escape_html_attr (p, name));
	  
      render_acceptable_html (vr);
      return render_footer_send (vr);
    } 

  key = ap_psprintf (p, "%s/_%d%s",
		     key_base,
		     db_dir_max (vr->db, key_base) + 1,
		     "/reply.xml");

  doc = db_xml_doc_new (p);
  root = xmlNewDocNode (doc, NULL, "article", NULL);
  doc->root = root;

  tree = xmlNewChild (root, NULL, "date", date);
  tree = xmlNewChild (root, NULL, "author", vr->u);

  tree = xmlNewChild (root, NULL, "title", NULL);
  xmlAddChild (tree, xmlNewDocText (doc, nice_title));

  if (body != NULL && body[0])
    {
      tree = xmlNewChild (root, NULL, "body", NULL);
      xmlAddChild (tree, xmlNewDocText (doc, nice_body));
    }


  status = db_xml_put (p, vr->db, key, doc);

  if (status)
    return send_error_page (vr,
			    "Error storing reply",
			    "There was an error storing the reply. This means there's something wrong with the site.");

  /* update the info page */
  add_recent (p, vr->db, "recent/proj-m.xml", name, -1, 0);

  if (status)
    send_error_page (vr,
		     "Error storing reply",
		     "There was an error storing the modification time. This means there's something wrong with the site.");


  /* I prefer to see what I've done
     str = ap_psprintf (p, "/proj/%s", escape_uri_arg(p, name));
     return proj_proj_serve (vr, str);
  */

  ap_table_add (vr->r->headers_out, "refresh", ap_psprintf(p, "0;URL=%s", proj_url(vr, name)));
  str = ap_psprintf (p, "Ok, your <a href=\"%s\">reply was posted</a>. Thanks!", proj_url(vr, name));
  return send_error_page (vr, "Posted", str);

}

static int
proj_reply_form_serve (VirguleReq *vr)
{
  pool *p = vr->r->pool;
  Buffer *b = vr->b;
  table *args;
  const char *name;
  char *key;
  xmlDoc *doc;

  auth_user (vr);

  if (vr->u == NULL)
    return send_error_page (vr, "Not logged in", "You can't post a reply because you're not logged in.");

  if (!req_ok_to_post (vr))
    return send_error_page (vr, "Not certified", "You can't post because you're not certified. Please see the <a href=\"%s/certs.html\">certification overview</a> for more details.", vr->prefix);

  args = get_args_table (vr);
  name = ap_table_get (args, "name");
  if (name == NULL)
    return send_error_page (vr, "Need <x>project</x> name", "Need name of <x>project</x> to reply to.");


  key = ap_psprintf (p, "proj/%s/info.xml", name);
  doc = db_xml_get (p, vr->db, key);
  if (doc == NULL)
    return send_error_page (vr, "<x>project</x> not found", "<x>project</x> %s not found.", name);

  render_header (vr, "Post a reply");

  buffer_printf (b, "<p> Post a reply to <x>project</x>: %s. </p>\n"
		 "<form method=\"POST\" action=\"/proj/replysubmit.html\">\n"
		 " <p> Reply title: <br>\n"
		 " <input type=\"text\" name=\"title\" size=50> </p>\n"
		 " <p> Body of reply: <br>\n"
		 " <textarea name=\"body\" cols=72 rows=16 wrap=soft>"
		 "</textarea> </p>\n"
		 " <input type=\"hidden\" name=\"name\" value=\"%s\">\n"
		 " <p> <input type=\"submit\" name=post value=\"Post\">\n"
		 " <input type=\"submit\" name=preview value=\"Preview\">\n"
		 "</form>\n",
		 render_proj_name (vr, name), escape_html_attr (p, name));

  render_acceptable_html (vr);

  return render_footer_send (vr);
}

int
proj_serve (VirguleReq *vr)
{
  const char *p;
  if ((p = match_prefix (vr->uri, "/proj/")) == NULL)
    return DECLINED;

  if (!strcmp (p, "new.html"))
    return proj_new_serve (vr);

  if (!strcmp (p, "newsub.html"))
    return proj_newsub_serve (vr);

  if (vr->projstyle == PROJSTYLE_NICK && !strcmp (p, "reply.html"))
    return proj_reply_form_serve (vr);
 
  if (!strcmp (p, "edit.html"))
    return proj_edit_serve (vr);
 
  if (!strcmp (p, "editsub.html"))
    return proj_editsub_serve (vr);

  if (!strcmp (p, "relsub.html"))
    return proj_relsub_serve (vr);

  if (vr->projstyle == PROJSTYLE_NICK && !strcmp (p, "replysubmit.html"))
      return proj_reply_submit_serve(vr);

  if (vr->projstyle == PROJSTYLE_NICK && !strcmp (p, "nextnew.html"))
      return proj_next_new_serve (vr);

  if (vr->projstyle == PROJSTYLE_NICK && !strcmp (p, "updatepointers.html"))
      return proj_update_all_pointers_serve (vr);

 if (!strcmp (p, ""))
    return proj_index_serve (vr);

  return proj_proj_serve (vr, p);
}
