#include <httpd.h>

#include <tree.h>
#include <entities.h>
#include <xmlmemory.h>

#include "xml_util.h"

/**
 * xml_find_child: Find child with given tagname.
 * @n: The parent node,
 * @tag: The tagname.
 *
 * Finds the first child of @n with tagname @tag.
 *
 * Return value: the child found, or NULL if none found.
 **/
xmlNode *
xml_find_child (xmlNode *n, const char *tag)
{
  xmlNode *child;

  if (n == NULL)
    return NULL;
  for (child = n->childs; child != NULL; child = child->next)
    if (!strcmp (child->name, tag))
      return child;
  return NULL;
}

/**
 * xml_ensure_child: Find child with given tagname, create if needed.
 * @n: The parent node,
 * @tag: The tagname.
 *
 * Finds the first child of @n with tagname @tag. If the child does
 * not exist, creates a new child.
 *
 * Return value: the child found.
 **/
xmlNode *
xml_ensure_child (xmlNode *n, const char *tag)
{
  xmlNode *child;

  for (child = n->childs; child != NULL; child = child->next)
    if (!strcmp (child->name, tag))
      return child;
  return xmlNewChild (n, NULL, tag, NULL);
}

char *
xml_get_string_contents (xmlNode *n)
{
  xmlNode *child = n->childs;

  while (child && child->type != XML_TEXT_NODE)
    child = child->next;

  if (child)
    return child->content;
  else
    return NULL;
}

/* xmlGetProp with Apache-friendly allocation */
char *
xml_get_prop (pool *p, xmlNodePtr node, const xmlChar *name)
{
  char *value;
  char *result;

  value = xmlGetProp (node, name);
  if (value == NULL)
    return NULL;
  result = ap_pstrdup (p, value);
  xmlFree (value);
  return result;
}

/**
 * xml_find_child_string: Find child with given tagname, in string form.
 * @n: The parent node.
 * @tag: The tagname.
 * @def: A default string.
 *
 * Finds the first child of @n with tagname @tag, extracting a string.
 *
 * Return value: the string, or @def if not found.
 **/
char *
xml_find_child_string (xmlNode *n, const char *tag, char *def)
{
  xmlNode *child;

  child = xml_find_child (n, tag);
  if (child == NULL)
    return def;
  else
    return xml_get_string_contents (child);
}