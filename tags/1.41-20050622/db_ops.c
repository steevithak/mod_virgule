/* This file contains a number of useful operations based on the db
   layer below, such as keeping a list of recent updates, and so
   on. As I develop advogato, it's likely that I'll put in schemas for
   relations, ontology, indexing, and some other things. */

#include <string.h>

#include <apr.h>
#include <apr_strings.h>
#include <httpd.h>

#include <libxml/tree.h>

#include "buffer.h"
#include "db.h"
#include "req.h"
#include "db_xml.h"
#include "xml_util.h"
#include "util.h"

#include "db_ops.h"

/* careful: val better not have any xml metacharacters */
int
add_recent (apr_pool_t *p, Db *db, const char *key, const char *val, int n_max, int dup)
{
  xmlDoc *doc;
  xmlNode *root, *tree;
  int n;
  const char *date;

  if (val == NULL || !strcmp (val, ""))
    return -1;

  doc = db_xml_get (p, db, key);
  if (doc == NULL)
    {
      doc = db_xml_doc_new (p);
      root = xmlNewDocNode (doc, NULL, "recent", NULL);
      doc->xmlRootNode = root;
    }
  else
    root = doc->xmlRootNode;

  tree = xmlNewTextChild (root, NULL, "item", val);
  date = iso_now (p);
  xmlSetProp (tree, "date", date);

  n = 0;
  for (tree = root->last; tree != NULL; tree = tree->prev)
    {
      if ((!dup && n > 0 && !strcmp (val, xml_get_string_contents (tree))) ||
	  n == n_max)
	{
	  xmlUnlinkNode (tree);
	  xmlFreeNode (tree);
	  break;
	}
      n++;
    }

  return db_xml_put (p, db, key, doc);
}

/**
 * db_relation_match: Match unique parts of fields.
 * Return value: TRUE if they match.
 **/
static int
db_relation_match (apr_pool_t *p, xmlNode *n1, xmlNode *n2,
		   const DbRelation *rel, int i)
{
  int j;

  for (j = 0; j < rel->n_fields; j++)
    {
      if (j != i && (rel->fields[j].flags & DB_FIELD_UNIQUE))
	{
	  /* get props, return 0 if they don't match */
	  char *v1, *v2;
	  char *prop;

	  prop = rel->fields[j].name;
	  v1 = xml_get_prop (p, n1, prop);
	  v2 = xml_get_prop (p, n2, prop);
	  if (v1 == NULL) v1 = "";
	  if (v2 == NULL) v2 = "";
	  if (strcmp (v1, v2))
	    return 0;
	}
    }
  return 1;
}

static int
db_relation_put_field (apr_pool_t *p, Db *db, const DbRelation *rel,
		       const char **values, int i)
{
  char *db_key;
  xmlDoc *doc;
  xmlNode *root;
  xmlNode *tree = NULL;
  xmlNode *child, *next;
  char *relname;
  int j;

  db_key = apr_pstrcat (p, rel->fields[i].prefix, values[i],
		       "/", rel->name, "-", rel->fields[i].name, ".xml", NULL);
  doc = db_xml_get (p, db, db_key);
  if (doc == NULL)
    {
      doc = db_xml_doc_new (p);
      relname = apr_pstrcat (p, rel->name, "-", rel->fields[i].name, NULL);
      root = xmlNewDocNode (doc, NULL, relname, NULL);
      doc->xmlRootNode = root;
    }
  else
    root = doc->xmlRootNode;

  tree = xmlNewChild (root, NULL, "rel", NULL);
  for (j = 0; j < rel->n_fields; j++)
    {
      if (i != j)
	{
	  xmlSetProp (tree, rel->fields[j].name, values[j]);
	}
    }

  /* uniqueness checking */
  for (child = root->children; child != NULL && tree != NULL; child = next)
    {

      next = child->next;
      if (child != tree &&
	  db_relation_match (p, child, tree, rel, i))
	{
	  xmlUnlinkNode (child);
	  xmlFreeNode (child);
	}
    }

  return db_xml_put (p, db, db_key, doc);
}

/**
 * db_relation_put: Put a relation in the database.
 * @db: The database.
 * @rel: The relation.
 * @values: A list of values.
 *
 * Puts the relation in the database.
 *
 * Whether the relation overwrites another relation is based on whether
 * all fields with the UNIQUE flag match.
 *
 * Return value: 0 on sucess.
 **/
int
db_relation_put (apr_pool_t *p, Db *db, const DbRelation *rel, const char **values)
{
  int i;
  int status = 0;

  for (i = 0; i < rel->n_fields; i++)
    {
      DbField *field = &rel->fields[i];
      if (field->flags & DB_FIELD_INDEX)
	{
	  status = db_relation_put_field (p, db, rel, values, i);
	  if (status)
	    break;
	}
    }
  return status;
}

/*
int
db_relation_get (apr_pool_t *p, Db *db, const DbRelation *rel, char **values)
{
}
*/
