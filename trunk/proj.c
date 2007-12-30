/* This file sets up a data model for projects. */

#include <ctype.h>

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
  { "name", "<x>Project</x> name", "proj/", 25, 0 },
  { "url", "<x>Project</x> homepage URL", NULL, 60, 0 },
  { "fmurl", "<a href=\"http://freshmeat.net/\">Freshmeat</a> URL", NULL, 60, 0 },
  { "license", "License", NULL, 60, 0 },
  { "notes", "Notes (provide as much detail as you'd like!)", NULL, 60016, SCHEMA_TEXTAREA },
  { NULL }
};

static SchemaField sproj_fields[] = {
  { "name", "<x>Project</x> name", "proj/", 25, 0 },
  { "url", "<x>Project</x> homepage URL", NULL, 60, 0 },
  { "fmurl", "<a href=\"http://freshmeat.net/\">Freshmeat</a> URL", NULL, 60, 0 },
  { "desc", "Brief Description (e.g. Software, PCM board, Sensor array, etc.)", NULL, 60, 0 },
  { "notes", "Notes (provide as much detail as you'd like!)", NULL, 60016, SCHEMA_TEXTAREA },
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
  apr_pool_t *p = vr->r->pool;
  return apr_pstrcat (p, vr->prefix, "/proj/", virgule_escape_uri_arg (p, proj),
		     vr->priv->projstyle != PROJSTYLE_NICK ? "/" : "/#lastread",
		     NULL);
}

char *
virgule_render_proj_name (VirguleReq *vr, const char *proj)
{
  apr_pool_t *p = vr->r->pool;

  return apr_pstrcat (p, "<a href=\"", proj_url (vr, proj), "\">",
		     virgule_nice_text (p, proj),
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
  apr_pool_t *p = vr->r->pool;
  xmlNode *tree;
  char *creator;
  char *locked;

  virgule_auth_user (vr);

  if (vr->u == NULL)
    return 0;

  if (strcmp (virgule_req_get_tmetric_level (vr, vr->u),
		  virgule_cert_level_to_name (vr, CERT_LEVEL_NONE)))
    return 1;

  tree = virgule_xml_find_child (doc->xmlRootNode, "info");

  locked = virgule_xml_get_prop (p, tree, (xmlChar *)"locked");
  if (locked && !strcmp (locked, "yes"))
    return 0;
  creator = virgule_xml_get_prop (p, tree, (xmlChar *)"creator");

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
  apr_pool_t *p = vr->r->pool;

  const char *rfields[] = { "name", "url", "fmurl", "license", "notes", NULL };
  const char *sfields[] = { "name", "url", "fmurl", "desc", "notes", NULL};  
  const char *nfields[] = { "name", "url", "notes", NULL };

  const char **fields = rfields;
  if(vr->priv->projstyle == PROJSTYLE_NICK) 
    fields = nfields;
  else if(vr->priv->projstyle == PROJSTYLE_STEVE)
    fields = sfields;

  SchemaField *proj_fields = rproj_fields;
  if(vr->priv->projstyle == PROJSTYLE_NICK) 
    proj_fields = nproj_fields;
  else if(vr->priv->projstyle == PROJSTYLE_STEVE)
    proj_fields = sproj_fields;


  virgule_auth_user (vr);

  if (vr->u == NULL)
    return virgule_send_error_page (vr, vERROR, "forbidden", "You can't create a new <x>project</x> because you're not logged in.");

  if (!virgule_req_ok_to_create_project (vr))
    return virgule_send_error_page (vr, vERROR, "forbidden", "You need a higher certification level to create a <x>project</x>.");

  if (virgule_set_temp_buffer (vr) != 0)
    return HTTP_INTERNAL_SERVER_ERROR;

  virgule_buffer_puts (vr->b, "<p> Create a new <x>project</x>: </p>\n"
	       "<form method=\"POST\" action=\"newsub.html\" accept-charset=\"UTF-8\">\n");

  virgule_schema_render_inputs (p, vr->b, proj_fields, fields, NULL);


  virgule_buffer_puts (vr->b, " <p> <input type=\"submit\" value=\"Create\">\n"
	       "</form>\n");

  virgule_render_acceptable_html (vr);

  virgule_set_main_buffer (vr);
  return virgule_render_in_template (vr, "/templates/default.xml", "content", "Create new <x>project</x>");
}

static int
proj_newsub_serve (VirguleReq *vr)
{
  apr_pool_t *p = vr->r->pool;
  Db *db = vr->db;
  apr_table_t *args;
  const char *date;
  const char *name, *url, *fmurl, *desc, *license, *notes;
  char *db_key;
  xmlDoc *doc;
  xmlNode *root, *tree;
  int status;

  virgule_auth_user (vr);

  if (vr->u == NULL)
    return virgule_send_error_page (vr, vERROR, "forbidden", "You can't create a new <x>project</x> because you're not logged in.");

  if (!virgule_req_ok_to_create_project (vr))
    return virgule_send_error_page (vr, vERROR, "forbidden", "You need a higher certification level to create a <x>project</x>.");

  args = virgule_get_args_table (vr);

  name = apr_table_get (args, "name");
  url = apr_table_get (args, "url");
  fmurl = apr_table_get (args, "fmurl");
  desc = apr_table_get (args, "desc");
  license = apr_table_get (args, "license");
  notes = apr_table_get (args, "notes");
  date = virgule_iso_now (p);

  if (!name[0])
    return virgule_send_error_page (vr, vERROR, "form data",
			    "You must specify <x>a project</x> name.");

  if (strlen (name) > 25)
    return virgule_send_error_page (vr, vERROR, "form data",
			    "The <x>project</x> name must be 25 characters or less.");

  if (name[0] == '.')
    return virgule_send_error_page (vr, vERROR, "form data",
			    "The <x>project</x> can't begin with a dot, sorry.");

  if (strchr (name, '/'))
    return virgule_send_error_page (vr, vERROR, "form data",
			    "The <x>project</x> can't have a slash, sorry.");

  db_key = apr_psprintf (p, "proj/%s/info.xml", name);
  doc = virgule_db_xml_get (p, db, db_key);
  if (doc != NULL)
    return virgule_send_error_page (vr, vERROR,
			    "database",
			    "The <x>project</x> name <em>%s</em> already exists.",
			    name);

  doc = virgule_db_xml_doc_new (p);

  root = xmlNewDocNode (doc, NULL, (xmlChar *)"info", NULL);
  doc->xmlRootNode = root;

  tree = xmlNewChild (root, NULL, (xmlChar *)"cdate", (xmlChar *)date);
  tree = xmlNewChild (root, NULL, (xmlChar *)"info", NULL);
  xmlSetProp (tree, (xmlChar *)"url", (xmlChar *)url);
  xmlSetProp (tree, (xmlChar *)"fmurl", (xmlChar *)fmurl);
  xmlSetProp (tree, (xmlChar *)"desc", (xmlChar *)desc);
  xmlSetProp (tree, (xmlChar *)"notes", (xmlChar *)notes);
  xmlSetProp (tree, (xmlChar *)"license", (xmlChar *)license);
  xmlSetProp (tree, (xmlChar *)"creator", (xmlChar *)vr->u);
  if (virgule_req_ok_to_create_project (vr))
    xmlSetProp (tree, (xmlChar *)"locked", (xmlChar *)"yes");

  status = virgule_db_xml_put (p, db, db_key, doc);
  if (status)
    return virgule_send_error_page (vr, vERROR,
			    "database",
			    "There was an error storing the <x>project</x>. This means there's something wrong with the site.");

  virgule_add_recent (p, db, "recent/proj-c.xml", name, 50, 0);
  virgule_add_recent (p, db, "recent/proj-m.xml", name,
              vr->priv->projstyle == PROJSTYLE_RAPH ? 50 : -1, 0);

  return virgule_send_error_page (vr, vINFO,
			  "<x>Project</x> created",
			  "<x>Project</x> %s created.\n",
			  virgule_render_proj_name (vr, name));
}

/* Compares two project names. Used by qsort in proj_index_serve */
static int
proj_index_sort (const void *n1, const void *n2)
{
  const char *proj1 = *(char **)n1;
  const char *proj2 = *(char **)n2;
  int i;

  for(i = 0; proj1[i] && proj2[i]; i++)
    {
      int c1,c2;
      c1 = tolower (proj1[i]);
      c2 = tolower (proj2[i]);
      if (c1 != c2) return c1 - c2;
    }
  return proj1[i];
}

/* Renders the project index page which consists of a list of all existing
   projects, each a link to the associated project page */
static int
proj_index_serve (VirguleReq *vr)
{
  Buffer *b;
  Db *db = vr->db;
  DbCursor *dbc;
  char *proj;
  int i;
  apr_pool_t *p = vr->r->pool;
  apr_array_header_t *list;

  virgule_auth_user (vr);

  if (virgule_set_temp_buffer (vr) != 0)
    return HTTP_INTERNAL_SERVER_ERROR;
  b = vr->b;

  if (vr->priv->projstyle == PROJSTYLE_RAPH)
    virgule_buffer_puts (b, "<ul>\n");
  dbc = virgule_db_open_dir (db, "proj");
  
  if (dbc == NULL)
    return virgule_send_error_page (vr, vERROR, "database",
			    "There was a problem reading the <x>project</x> list due to an internal server error.");

  list = apr_array_make (p, 16, sizeof(char *));
  
  while ((proj = virgule_db_read_dir_raw (dbc)) != NULL)
    *(char **)apr_array_push (list) = proj;
    
  qsort(list->elts, list->nelts, sizeof(char *), proj_index_sort);
  
  for(i = 0; i < list->nelts; i++)
    {
      if (vr->priv->projstyle != PROJSTYLE_NICK)
        {
          virgule_buffer_printf (b, "<li>%s\n", virgule_render_proj_name (vr, ((char **)list->elts)[i]));
        }
      else /* vr->priv->projstyle == PROJSTYLE_NICK */
        {
          char *db_key = apr_psprintf (p, "proj/%s/info.xml", ((char **)list->elts)[i]);
          char *creator;
	  xmlDoc *proj_doc;
	  xmlNode *proj_tree;

	  proj_doc = virgule_db_xml_get (p, vr->db, db_key);
	  if (proj_doc == NULL)
	    /* the project doesn't exist, so skip it */
	    continue;

	  proj_tree = virgule_xml_find_child (proj_doc->xmlRootNode, "info");
	  if (proj_tree != NULL)
	    creator = virgule_xml_get_prop (p, proj_tree, (xmlChar *)"creator");
	  else
	    {
	      /* No creator?  Skip it. */
	      virgule_db_xml_free (p, proj_doc);
	      continue;
	    }

	  virgule_render_cert_level_begin (vr, creator, CERT_STYLE_SMALL);
	  virgule_buffer_printf (b, "%s", virgule_render_proj_name (vr, ((char **)list->elts)[i]));
#if 0 /* I don't like how this looks, but you might */
	  virgule_buffer_printf (vr->b, " - <a href=\"%s/person/%s/\" style=\"text-decoration: none\">%s</a>\n",
		       vr->prefix, creator, creator);
#endif
	  virgule_render_cert_level_text (vr, creator);
	  virgule_render_cert_level_end (vr, CERT_STYLE_SMALL);
	  virgule_db_xml_free (p, proj_doc);
        }
    }
    
  if (vr->priv->projstyle != PROJSTYLE_NICK)
    virgule_buffer_puts (b, "</ul>\n");

  if (vr->u != NULL)
    virgule_buffer_puts (b, "<p> <a href=\"new.html\">Create</a> a new <x>project</x>...</p>\n");
  virgule_db_close_dir (dbc);

  virgule_set_main_buffer (vr);
  
  return virgule_render_in_template (vr, "/templates/proj-list.xml", "projects", "<x>Project</x> index");
}

static int
proj_proj_serve (VirguleReq *vr, const char *path)
{
  Buffer *b;
  request_rec *r = vr->r;
  apr_pool_t *p = r->pool;
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

  virgule_auth_user (vr);

  q = strchr ((char *)path, '/');
  if (q == NULL)
    {
      apr_table_add (r->headers_out, "Location",
		    ap_make_full_path (r->pool, r->uri, ""));
      return HTTP_MOVED_PERMANENTLY;
    }

  if (q[1] != '\0')
    return virgule_send_error_page (vr, vERROR,
			    "form data",
			    "Invalid <x>project</x> URL.");

  name = apr_pstrndup (p, path, q - path);

  db_key = apr_psprintf (p, "proj/%s/info.xml", name);

  doc = virgule_db_xml_get (p, vr->db, db_key);
  if (doc == NULL)
    {
      vr->r->status = 404;
      return virgule_send_error_page (vr, vERROR, "database",
		    "<x>Project</x> <em>%s</em> was not found.",
		    virgule_nice_text (p, name));
    }
    
  if (vr->priv->projstyle != PROJSTYLE_NICK)
    title = apr_psprintf (p, "<x>Project</x> info for %s", virgule_nice_text (p, name));
  else /* vr->priv->projstyle == PROJSTYLE_NICK */
    title = apr_psprintf(p, "%s", virgule_nice_text (p, name));

  if (virgule_set_temp_buffer (vr) != 0)
    return HTTP_INTERNAL_SERVER_ERROR;
  b = vr->b;

  cdate = virgule_xml_find_child_string (doc->xmlRootNode, "cdate", "--no date--");

  tree = virgule_xml_find_child (doc->xmlRootNode, "info");
  if (tree != NULL)
    {
      char *url;
      char *fmurl;
      char *notes;
      char *creator;
      char *desc;
      char *license;
      char *lastmodby;
      char *mdate;
      char *share = NULL;

      creator = virgule_xml_get_prop (p, tree, (xmlChar *)"creator");
      lastmodby = virgule_xml_get_prop (p, tree, (xmlChar *)"lastmodby");

      if (vr->priv->projstyle == PROJSTYLE_NICK)
	{
	  virgule_render_cert_level_begin (vr, creator, CERT_STYLE_LARGE);
	  virgule_buffer_puts (b, title);
	  virgule_render_cert_level_end (vr, CERT_STYLE_LARGE);
	}
      else
        {
	  char *bmurl = apr_psprintf (p, "%s/proj/%s/", vr->priv->base_uri, ap_escape_uri(p,name));
	  char *bmtitle = ap_escape_uri (p, ap_escape_uri(p,name));
	  share = apr_psprintf(p,"<a href=\"javascript:void(0)\" onclick=\"sbm(event,'%s','%s')\">"
				 "<img src=\"/images/share.png\" height=\"16\" width=\"16\" border=\"none\" alt=\"Share This\" title=\"Share This\" /></a> ",
				 bmurl, virgule_str_subst (vr->r->pool, bmtitle, "'", "%27"));
	}

      virgule_buffer_printf (b, "<p>%sCreated %s by <a href=\"%s/person/%s/\">%s</a>",
			     share, 
			     virgule_render_date (vr, cdate, 1), 
			     vr->prefix, 
			     ap_escape_uri(vr->r->pool, creator), 
			     creator);

      if (lastmodby != NULL)
	{
	  mdate = virgule_xml_get_prop (p, tree, (xmlChar *)"mdate");
	  virgule_buffer_printf (b, ", last modified %s by <a href=\"%s/person/%s/\">%s</a>",
			 virgule_render_date (vr, mdate, 1), vr->prefix,
			 ap_escape_uri(vr->r->pool, lastmodby), lastmodby);
	}

      if (proj_ok_to_edit (vr, doc))
	virgule_buffer_printf (b, " (<a href=\"../edit.html?proj=%s\">Edit...</a>)",
		       virgule_escape_uri_arg (p, name));
      virgule_buffer_puts (b, ".</p\n");

      url = virgule_xml_get_prop (p, tree, (xmlChar *)"url");
      if (url && url[0])
	{
	  virgule_buffer_printf (b, virgule_render_url (p,
					vr->priv->projstyle != PROJSTYLE_NICK ?
					" Homepage: " :
					" URL: ",
					url));
	}
      fmurl = virgule_xml_get_prop (p, tree, (xmlChar *)"fmurl");
      if (fmurl && fmurl[0])
	{
	  virgule_buffer_printf (b,
			 virgule_render_url (p, " Freshmeat page: ", fmurl));
	}

      notes = virgule_xml_get_prop (p, tree, (xmlChar *)"notes");
      if (notes && notes[0])
	virgule_buffer_printf (b, "<p> <b>Notes:</b> %s </p>\n", virgule_nice_htext (vr, notes, &err));

      license = virgule_xml_get_prop (p, tree, (xmlChar *)"license");
      if (license && license[0])
	virgule_buffer_printf (b, "<p> License: %s </p>\n", virgule_nice_text (p, license));

      desc = virgule_xml_get_prop (p, tree, (xmlChar *)"desc");
      if (desc && desc[0])
	virgule_buffer_printf (b, "<p> Description: %s </p>\n", virgule_nice_text (p, desc));

      if (vr->priv->projstyle != PROJSTYLE_NICK)
	{
	  /* Render staff listings */
	  myrel = NULL;
	  first = "<p> This <x>project</x> has the following developers: </p>\n"
	    "<ul>\n";
	  db_key = apr_psprintf (p, "proj/%s/staff-name.xml", name);

	  staff = virgule_db_xml_get (p, vr->db, db_key);
	  if (staff != NULL)
	    {
	      for (tree = staff->xmlRootNode->children; tree != NULL; tree = tree->next)
		{
		  char *person;
		  char *type;

		  person = virgule_xml_get_prop (p, tree, (xmlChar *)"person");
		  type = virgule_xml_get_prop (p, tree, (xmlChar *)"type");
		  if (vr->u != NULL && !strcmp (person, vr->u))
		    myrel = tree;
		  if (! !strcmp (type, "None"))
		    {
		      virgule_buffer_puts (b, first);
		      first = "";
		      virgule_buffer_printf (b, "<li><a href=\"%s/person/%s/\">%s</a> is a %s.\n",
				     vr->prefix, ap_escape_uri(p, person), person, virgule_nice_text (p, type));
		    }
		}
	    }
	  if (first[0] == 0)
	    virgule_buffer_puts (b, "</ul>\n");

	  if (proj_ok_to_edit (vr, doc))
	    {
	      virgule_buffer_printf (b, "<p> I have the following relation to %s: </p>\n"
			     "<form method=\"POST\" action=\"../relsub.html\">\n"
			     "<input type=\"hidden\" name=\"name\" value=\"%s\">\n",
			     virgule_nice_text (p, name), virgule_escape_html_attr (p, name));
	      virgule_schema_render_inputs (p, b, staff_fields, fields, myrel);
	      virgule_buffer_puts (b, "<input type=\"submit\" value=\"Update\"/>\n"
			   "</form>\n");
	    }
	}
      else /* vr->priv->projstyle == PROJSTYLE_NICK */
	{
	  /* Render replies */
	  proj_dir = apr_psprintf (vr->r->pool, "proj/%s", name);
	  n_replies = virgule_db_dir_max (vr->db, proj_dir) + 1;

	  proj_render_replies (vr, name);
	  if(vr->u != NULL)
	    virgule_buffer_puts (b,"<hr><div class=\"next\"><a class=\"next\" href=\"/proj/nextnew.html\"><b>Next&raquo;</b></a></div> \n");

	  if (virgule_req_ok_to_post (vr))
	    {
	      virgule_buffer_printf (b, "<p> Post a reply: %s. </p>\n"
		"<form method=\"POST\" action=\"/proj/replysubmit.html\" accept-charset=\"UTF-8\">\n"
		" <p> Reply title: <br>\n"
		" <input type=\"text\" name=\"title\" size=50> </p>\n"
		" <p> Body of reply: <br>\n"
		" <textarea name=\"body\" cols=72 rows=16 wrap=soft>"
		"</textarea> </p>\n"
		" <input type=\"hidden\" name=\"name\" value=\"%s\">\n"
		" <p> <input type=\"submit\" name=post value=\"Post\">\n"
		" <input type=\"submit\" name=preview value=\"Preview\">\n"
		"</form>\n", virgule_nice_text (p, name), virgule_escape_html_attr (p, name));

	      virgule_render_acceptable_html (vr);
	    }
	}
    }

  virgule_set_main_buffer (vr);
  
  return virgule_render_in_template (vr, "/templates/proj-desc.xml", "project", title);
}

static int
proj_next_new_serve (VirguleReq *vr)
{
  apr_pool_t *p = vr->r->pool;
  xmlDoc *doc;
  xmlNode *root, *tree;

  doc = virgule_db_xml_get (p, vr->db, "recent/proj-m.xml");
  if (doc == NULL)
    return virgule_send_error_page (vr, vERROR, "database",
			    "The modification log was not found.");
  root = doc->xmlRootNode;
  for (tree = root->children; tree != NULL; tree = tree->next)
    {
      char *name = virgule_xml_get_string_contents (tree);
      char *date = virgule_xml_get_prop (p, tree, (xmlChar *)"date");
      char *lastread_date = virgule_acct_get_lastread_date (vr, "proj", name);
  
      if (lastread_date != NULL)
	if (strcmp (date, lastread_date) > 0)
	  {
	    char *db_key = apr_psprintf (p, "proj/%s/info.xml", name);
	    xmlDoc *testdoc = virgule_db_xml_get (p, vr->db, db_key);
	    if (testdoc == NULL)
	      continue;
	    else 
	      virgule_db_xml_free (p, testdoc);

	    apr_table_add (vr->r->headers_out, "refresh", 
			  apr_psprintf(p, "0;URL=%s",
				      proj_url(vr, name))); 
	    return virgule_send_error_page (vr, vINFO, "next room",
				    "The next room is %s.",
				    virgule_render_proj_name(vr, name), 
				    name);
	  }
    }
  virgule_db_xml_free (p, doc);
  apr_table_add (vr->r->headers_out, "refresh", "0;URL=/");
  return virgule_send_error_page (vr, vINFO, "Next room", "There are no more rooms with new messages.");
}

static int
proj_edit_serve (VirguleReq *vr)
{
  apr_pool_t *p = vr->r->pool;
  apr_table_t *args;
  const char *name;
  const char *fields[] = { "url", "fmurl", "desc", "license", "notes", NULL };
  char *db_key;
  xmlDoc *doc;
  xmlNode *tree;

  SchemaField *proj_fields = rproj_fields;
  if(vr->priv->projstyle == PROJSTYLE_NICK) 
    proj_fields = nproj_fields;
  else if(vr->priv->projstyle == PROJSTYLE_STEVE)
    proj_fields = sproj_fields;

  args = virgule_get_args_table (vr);
  name = apr_table_get (args, "proj");

  db_key = apr_psprintf (p, "proj/%s/info.xml", name);

  doc = virgule_db_xml_get (p, vr->db, db_key);
  if (doc == NULL)
    return virgule_send_error_page (vr, vERROR, "database",
			    "<x>Project</x> <em>%s</em> was not found.", name);

  if (!proj_ok_to_edit (vr, doc))
    return virgule_send_error_page (vr, vERROR, "forbidden",
			    "You are not authorized to edit <x>project</x> %s. You have to either be certified to %s level or higher, or be the creator of the <x>project</x> and not have anyone else edit the page before you.", name, virgule_cert_level_to_name (vr, 1));

  if (virgule_set_temp_buffer (vr) != 0)
    return HTTP_INTERNAL_SERVER_ERROR;

  virgule_buffer_printf (vr->b, "<p> Edit <x>project</x> for %s: </p>\n"
	       "<form method=\"POST\" action=\"editsub.html\">\n"
		 "<input type=\"hidden\" name=\"name\" value=\"%s\">\n",
		 virgule_nice_text (p, name), virgule_escape_html_attr(p, name));

  tree = virgule_xml_find_child (doc->xmlRootNode, "info");

  virgule_schema_render_inputs (p, vr->b, proj_fields, fields, tree);

  virgule_buffer_puts (vr->b, "<input type=\"submit\" value=\"Update\"/>\n</form>\n");

  virgule_render_acceptable_html (vr);

  virgule_set_main_buffer (vr);

  return virgule_render_in_template (vr, "/templates/default.xml", "content", "Edit <x>project</x>");
}

static int
proj_editsub_serve (VirguleReq *vr)
{
  apr_pool_t *p = vr->r->pool;
  Db *db = vr->db;
  apr_table_t *args;
  const char *date;
  char *db_key;
  xmlDoc *doc;
  xmlNode *tree;
  int status;
  const char *fields[] = { "url", "fmurl", "desc", "license", "notes", NULL };
  const char *name;
  SchemaField *proj_fields = rproj_fields;
  if(vr->priv->projstyle == PROJSTYLE_NICK) 
    proj_fields = nproj_fields;
  else if(vr->priv->projstyle == PROJSTYLE_STEVE)
    proj_fields = sproj_fields;
    
  args = virgule_get_args_table (vr);
  name = apr_table_get (args, "name");

  db_key = apr_psprintf (p, "proj/%s/info.xml", name);

  doc = virgule_db_xml_get (p, vr->db, db_key);
  if (doc == NULL)
    return virgule_send_error_page (vr, vERROR,
			    "database",
			    "<x>Project</x> <em>%s</em> was not found.", name);

  if (!proj_ok_to_edit (vr, doc))
    return virgule_send_error_page (vr, vERROR, "forbidden",
			    "You are not authorized to edit <x>project</x> %s. You have to either be certified to %s level or higher, or be the creator of the <x>project</x> and not have anyone else edit the page before you.", name, virgule_cert_level_to_name (vr, 1));
 
  tree = virgule_xml_find_child (doc->xmlRootNode, "info");
  date = virgule_iso_now (p);

  virgule_schema_put_fields (p, proj_fields, fields, tree, args);

  xmlSetProp (tree, (xmlChar *)"mdate", (xmlChar *)date);
  xmlSetProp (tree, (xmlChar *)"lastmodby", (xmlChar *)vr->u);

  if (virgule_req_ok_to_create_project (vr))
    xmlSetProp (tree, (xmlChar *)"locked", (xmlChar *)"yes");

  status = virgule_db_xml_put (p, db, db_key, doc);
  if (status)
    return virgule_send_error_page (vr, vERROR, "forbidden",
			    "There was an error storing the <x>project</x>. This means there's something wrong with the site.");

  virgule_add_recent (p, db, "recent/proj-m.xml", name, 
	      vr->priv->projstyle != PROJSTYLE_NICK ? 50 : -1, 0);

  return virgule_send_error_page (vr, vINFO, "<x>Project</x> Updated",
			  "<x>Project</x> %s updated.\n",
			  virgule_render_proj_name (vr, name));
}

static int
proj_relsub_serve (VirguleReq *vr)
{
  apr_pool_t *p = vr->r->pool;
  Db *db = vr->db;
  apr_table_t *args;
  char *db_key;
  xmlDoc *doc;
  const char *name;
  const char *type;

  args = virgule_get_args_table (vr);
  name = apr_table_get (args, "name");

  db_key = apr_psprintf (p, "proj/%s/info.xml", name);

  doc = virgule_db_xml_get (p, db, db_key);
  if (doc == NULL)
    return virgule_send_error_page (vr, vERROR, "database",
			    "<x>Project</x> <em>%s</em> was not found.", name);

  if (!proj_ok_to_edit (vr, doc))
    return virgule_send_error_page (vr, vERROR, "forbidden",
			    "You are not authorized to edit <x>project</x> %s. You have to either be certified to %s level or higher, or be the creator of the <x>project</x> and not have anyone else edit the page before you.", name, virgule_cert_level_to_name (vr, 1));
 
  type = apr_table_get (args, "type");

  virgule_proj_set_relation (vr, name, vr->u, type);

  return virgule_send_error_page (vr, vINFO, "Relationship updated",
			  "The update of the relationship between <x>person</x> %s and <x>project</x> %s (type %s) is completed. Have a great day!",
			  vr->u, virgule_render_proj_name (vr, name), type);
}

static void
proj_render_reply (VirguleReq *vr, const char *name, int reply_num)
{
  apr_pool_t *p = vr->r->pool;
  Buffer *b = vr->b;
  char *key;
  xmlDoc *doc;

  if (reply_num < 0)
    return;

  key = apr_psprintf (p, "proj/%s/_%d/reply.xml", name, reply_num);

  doc = virgule_db_xml_get (p, vr->db, key);
  if (doc != NULL)
    {
      xmlNode *root = doc->xmlRootNode;
      char *author;
      char *title;
      char *body;
      char *date;

      author = virgule_xml_find_child_string (root, "author", "(no author)");
      title = virgule_xml_find_child_string (root, "title", "(no title)");
      body = virgule_xml_find_child_string (root, "body", "(no body)");
      date = virgule_xml_find_child_string (root, "date", "(no date)");

      virgule_render_cert_level_begin (vr, author, CERT_STYLE_MEDIUM);
      virgule_buffer_printf (b, "<a name=\"%u\"><b>%s</b></a>, posted %s by <a href=\"%s/person/%s/\">%s</a> <a href=\"#%u\" style=\"text-decoration: none\">&raquo;</a>\n",
		     reply_num, title, virgule_render_date (vr, date, 1), vr->prefix, author, author, reply_num);		     
      virgule_render_cert_level_text (vr, author);
      virgule_render_cert_level_end (vr, CERT_STYLE_MEDIUM);
      virgule_buffer_printf (b, "<blockquote>\n%s\n</blockquote>\n", body);
    }
  else
    {
      virgule_buffer_printf (b, "<p> Error reading <x>project</x> %s.\n", name);
    }
  virgule_db_xml_free (p, doc);
}

static void
proj_render_replies (VirguleReq *vr, const char *name)
{
  int lastread, num_old, start;
  char *base;
  int n_art;
  int i;

  base = apr_psprintf (vr->r->pool, "proj/%s", name);
  n_art = virgule_db_dir_max (vr->db, base) + 1;
  lastread = virgule_acct_get_lastread (vr, "proj", name);
  num_old = virgule_acct_get_num_old (vr);
  start = lastread - num_old;

  if (start < 0)
    start = 0;
#if 0
  virgule_buffer_printf (vr->b, "<p> Rendering %d replies. </p>\n", n_art);
  virgule_buffer_printf (vr->b, "<!-- lastread is set to %d -->", lastread);
#endif
  if (n_art > 0)
    {
      virgule_buffer_puts (vr->b, "<hr>\n");
      for (i = start; i < n_art; i++)
	{
	  if (i == lastread)
	    virgule_buffer_puts (vr->b, "<a name=\"lastread\">");
	  proj_render_reply (vr, name, i);
	}
    }
  virgule_acct_set_lastread(vr, "proj", name, n_art - 1);
}

static int
proj_update_all_pointers_serve (VirguleReq *vr)
{
  apr_pool_t *p = vr->r->pool;
  Db *db = vr->db;
  DbCursor *dbc;
  char *proj;

  virgule_auth_user (vr);

  dbc = virgule_db_open_dir (db, "proj");

  while ((proj = virgule_db_read_dir_raw (dbc)) != NULL)
    {
	char *base;
	int n_reply;

	base = apr_psprintf (p, "proj/%s", proj);
	n_reply = virgule_db_dir_max (db, base);
	virgule_acct_set_lastread(vr, "proj", proj, n_reply -1);
    }
  apr_table_add (vr->r->headers_out, "refresh", "0;URL=/");
  return virgule_send_error_page (vr, vINFO, "Updated", "Your pointers have all been updated.");
}


/**
 * proj_reply_submit_serve - this appears to be used only in the NICK style
 * project configuration which turns the project area into a sort of 
 * citadel style room-based discussion forum. This is not used in standard
 * in standard configurations.
 */
static int
proj_reply_submit_serve (VirguleReq *vr)
{
  apr_pool_t *p = vr->r->pool;
  apr_table_t *args;
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


  args = virgule_get_args_table (vr);
  if (args == NULL)
    return virgule_send_error_page (vr, vERROR, "form data", "Invalid form submission.");

  /* XXX */
  name = apr_table_get (args, "name");
  title = apr_table_get (args, "title");
  body = apr_table_get (args, "body");

  if (name == NULL)
    return virgule_send_error_page (vr, vERROR, "form data", "A valid <x>project</x> name is required.");

  key_base = apr_psprintf (p, "proj/%s", name);

  virgule_auth_user (vr);
  if (vr->u == NULL)
    return virgule_send_error_page (vr, vERROR, "forbidden", "You can't post because you're not logged in.");

  if (!virgule_req_ok_to_post (vr))
    return virgule_send_error_page (vr, vERROR, "forbidden", "You can't post because you're not certified. Please see the <a href=\"%s/certs.html\">certification overview</a> for more details.", vr->prefix);

  date = virgule_iso_now (p);

  if (title == NULL || title[0] == 0)
    return virgule_send_error_page (vr, vERROR, "form data", "Your reply needs a title. Go back and try again.");
  if (body == NULL || body[0] == 0)
    return virgule_send_error_page (vr, vERROR, "form data", "Your reply needs a body. Go back and try again.");


  nice_title = virgule_nice_text (p, title);
  nice_body = body == NULL ? NULL : virgule_nice_htext (vr, body, &body_error);

  if (apr_table_get (virgule_get_args_table (vr), "preview"))
    {
      if (virgule_set_temp_buffer (vr) != 0)
        return HTTP_INTERNAL_SERVER_ERROR;
	
      virgule_render_cert_level_begin (vr, vr->u, CERT_STYLE_MEDIUM);
      virgule_buffer_puts (vr->b, nice_title);
      virgule_render_cert_level_end (vr, CERT_STYLE_MEDIUM);
      virgule_buffer_printf (vr->b, "<p> %s </p>\n", nice_body);
      virgule_buffer_puts (vr->b, "<hr>\n");
      virgule_buffer_printf (vr->b, "<p> Edit your reply: </p>\n"
		     "<form method=\"POST\" action=\"replysubmit.html\" accept-charset=\"UTF-8\">\n"
		     "<p> Reply title: <br>\n"
		     "<input type=\"text\" name=\"title\" value=\"%s\" size=50></p>\n"
		     "<p> Body of reply: <br>\n"
		     "<textarea name=\"body\" cols=72 rows=16 wrap=soft>%s"
		     "</textarea> </p>\n"
		     "<input type=\"hidden\" name=\"name\" value=\"%s\">\n"
		     "<p> <input type=\"submit\" name=post value=\"Post\">\n"
		     "<input type=\"submit\" name=preview value=\"Preview\">\n"
		     "</form>\n",
		     virgule_escape_html_attr (p, title),
		     ap_escape_html (p, body),
		     virgule_escape_html_attr (p, name));
	  
      virgule_render_acceptable_html (vr);
      virgule_set_main_buffer (vr);
      return virgule_render_in_template (vr, "/templates/default.xml", "content", "Reply preview");
    } 

  key = apr_psprintf (p, "%s/_%d%s",
		     key_base,
		     virgule_db_dir_max (vr->db, key_base) + 1,
		     "/reply.xml");

  doc = virgule_db_xml_doc_new (p);
  root = xmlNewDocNode (doc, NULL, (xmlChar *)"article", NULL);
  doc->xmlRootNode = root;

  tree = xmlNewChild (root, NULL, (xmlChar *)"date", (xmlChar *)date);
  tree = xmlNewChild (root, NULL, (xmlChar *)"author", (xmlChar *)vr->u);

  tree = xmlNewChild (root, NULL, (xmlChar *)"title", NULL);
  xmlAddChild (tree, xmlNewDocText (doc, (xmlChar *)nice_title));

  if (body != NULL && body[0])
    {
      tree = xmlNewChild (root, NULL, (xmlChar *)"body", NULL);
      xmlAddChild (tree, xmlNewDocText (doc, (xmlChar *)nice_body));
    }


  status = virgule_db_xml_put (p, vr->db, key, doc);

  if (status)
    return virgule_send_error_page (vr, vERROR, "database",
			    "There was an error storing the reply. This means there's something wrong with the site.");

  /* update the info page */
  virgule_add_recent (p, vr->db, "recent/proj-m.xml", name, -1, 0);

  if (status)
    virgule_send_error_page (vr, vERROR, "database",
		     "There was an error storing the modification time. This means there's something wrong with the site.");


  /* I prefer to see what I've done
     str = apr_psprintf (p, "/proj/%s", virgule_escape_uri_arg(p, name));
     return proj_proj_serve (vr, str);
  */

  apr_table_add (vr->r->headers_out, "refresh", apr_psprintf(p, "0;URL=%s", proj_url(vr, name)));
  str = apr_psprintf (p, "Ok, your <a href=\"%s\">reply was posted</a>. Thanks!", proj_url(vr, name));
  return virgule_send_error_page (vr, vINFO, "Posted", str);

}


/**
 * proj_reply_form_serve - this appears to be used only in the NICK style
 * project configuration which turns the project area into a sort of 
 * citadel style room-based discussion forum. This is not used in standard
 * in standard configurations.
 */
static int
proj_reply_form_serve (VirguleReq *vr)
{
  apr_pool_t *p = vr->r->pool;
  Buffer *b = vr->b;
  apr_table_t *args;
  const char *name;
  char *key;
  xmlDoc *doc;

  virgule_auth_user (vr);

  if (vr->u == NULL)
    return virgule_send_error_page (vr, vERROR, "forbidden", "You can't post a reply because you're not logged in.");

  if (!virgule_req_ok_to_post (vr))
    return virgule_send_error_page (vr, vERROR, "forbidden", "You can't post because you're not certified. Please see the <a href=\"%s/certs.html\">certification overview</a> for more details.", vr->prefix);

  args = virgule_get_args_table (vr);
  name = apr_table_get (args, "name");
  if (name == NULL)
    return virgule_send_error_page (vr, vERROR, "form data", "Need name of <x>project</x> to reply to.");


  key = apr_psprintf (p, "proj/%s/info.xml", name);
  doc = virgule_db_xml_get (p, vr->db, key);
  if (doc == NULL)
    return virgule_send_error_page (vr, vERROR, "database", "<x>project</x> %s not found.", name);

  if (virgule_set_temp_buffer (vr) != 0)
    return HTTP_INTERNAL_SERVER_ERROR;

  virgule_buffer_printf (b, "<p> Post a reply to <x>project</x>: %s. </p>\n"
		 "<form method=\"POST\" action=\"/proj/replysubmit.html\" accept-charset=\"UTF-8\">\n"
		 " <p> Reply title: <br>\n"
		 " <input type=\"text\" name=\"title\" size=50> </p>\n"
		 " <p> Body of reply: <br>\n"
		 " <textarea name=\"body\" cols=72 rows=16 wrap=soft>"
		 "</textarea> </p>\n"
		 " <input type=\"hidden\" name=\"name\" value=\"%s\">\n"
		 " <p> <input type=\"submit\" name=post value=\"Post\">\n"
		 " <input type=\"submit\" name=preview value=\"Preview\">\n"
		 "</form>\n",
		 virgule_render_proj_name (vr, name),
		 virgule_escape_html_attr (p, name));

  virgule_render_acceptable_html (vr);

  virgule_set_main_buffer (vr);
  
  return virgule_render_in_template (vr, "/templates/default.xml", "content", "Post a reply");
}

/**
 * proj_set_relation: set or remove the users relation to a project
 * name: name of project
 * u: user
 * type: type of relationship of this user to the project
 **/
void
virgule_proj_set_relation (VirguleReq *vr, const char *name, const char *u, const char *type)
{
  const char *values[3];

  values[0] = name;
  values[1] = u;
  values[2] = type;

  if (strcmp(type,"None") == 0)
    {
      char *db_key;
      char *user;
      xmlDoc *staff;
      xmlNode *tree;

      db_key = apr_psprintf (vr->r->pool, "proj/%s/staff-name.xml", name);
      staff = virgule_db_xml_get (vr->r->pool, vr->db, db_key);
      if (staff != NULL)
        {
          for (tree = staff->xmlRootNode->children; tree != NULL; tree = tree->next)
	    {
	      user = (char *)xmlGetProp (tree, (xmlChar *)"person");
	      if (strcmp (u, user) == 0)
	        {
                  xmlUnlinkNode (tree);
	          xmlFreeNode (tree);
		  xmlFree (user);
		  break;
	        }
              xmlFree (user);
            }
	}
      virgule_db_xml_put (vr->r->pool, vr->db, db_key, staff);
      virgule_db_xml_free (vr->r->pool, staff);
    }
    virgule_db_relation_put (vr->r->pool, vr->db, &staff_db_rel, values);
}


int
virgule_proj_serve (VirguleReq *vr)
{
  const char *p;
  if ((p = virgule_match_prefix (vr->uri, "/proj/")) == NULL)
    return DECLINED;

  if (!strcmp (p, "new.html"))
    return proj_new_serve (vr);

  if (!strcmp (p, "newsub.html"))
    return proj_newsub_serve (vr);

  if (vr->priv->projstyle == PROJSTYLE_NICK && !strcmp (p, "reply.html"))
    return proj_reply_form_serve (vr);
    
  if (!strcmp (p, "edit.html"))
    return proj_edit_serve (vr);
 
  if (!strcmp (p, "editsub.html"))
    return proj_editsub_serve (vr);

  if (!strcmp (p, "relsub.html"))
    return proj_relsub_serve (vr);

  if (vr->priv->projstyle == PROJSTYLE_NICK && !strcmp (p, "replysubmit.html"))
      return proj_reply_submit_serve(vr);

  if (vr->priv->projstyle == PROJSTYLE_NICK && !strcmp (p, "nextnew.html"))
      return proj_next_new_serve (vr);

  if (vr->priv->projstyle == PROJSTYLE_NICK && !strcmp (p, "updatepointers.html"))
      return proj_update_all_pointers_serve (vr);

 if (!strcmp (p, ""))
    return proj_index_serve (vr);

  return proj_proj_serve (vr, p);
}
