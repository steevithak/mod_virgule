#include <apr.h>
#include <apr_strings.h>
#include <httpd.h>

#include <libxml/tree.h>
#include <libxml/entities.h>
#include <libxml/xmlmemory.h>

#include "private.h"
#include "buffer.h"
#include "db.h"
#include "req.h"
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
virgule_xml_find_child (xmlNode *n, const char *tag)
{
  xmlNode *child;

  if (n == NULL)
    return NULL;
  for (child = n->children; child != NULL; child = child->next)
    if (!strcmp ((char *)child->name, tag))
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
virgule_xml_ensure_child (xmlNode *n, const char *tag)
{
  xmlNode *child;

  for (child = n->children; child != NULL; child = child->next)
    if (!strcmp ((char *)child->name, tag))
      return child;
  return xmlNewChild (n, NULL, (xmlChar *)tag, NULL);
}


char *
virgule_xml_get_string_contents (xmlNode *n)
{
  if(n == NULL)
    return NULL;

  xmlNode *child = n->children;

  while (child && child->type != XML_TEXT_NODE && child->type != XML_CDATA_SECTION_NODE)
    child = child->next;

  if (child)
    return (char *)child->content;
  else
    return NULL;
}


/**
 * virgule_xml_del_string_contents - remove any text or CDATA child nodes
 * while leaving other nodes intact.
 **/
void
virgule_xml_del_string_contents (xmlNode *n)
{
  xmlNode *child = n->children;
  xmlNode *tmp;

  while (child)
    {
      if(child->type == XML_TEXT_NODE || child->type == XML_CDATA_SECTION_NODE)
      {
        tmp = child;
        child = tmp->next;
        xmlUnlinkNode(tmp);
        xmlFreeNode(tmp);
      }
      else
        child = child->next;
    }
}




/* xmlGetProp with Apache-friendly allocation */
char *
virgule_xml_get_prop (apr_pool_t *p, xmlNodePtr node, const xmlChar *name)
{
  char *value;
  char *result;

  value = (char *)xmlGetProp (node, name);
  if (value == NULL)
    return NULL;
  result = apr_pstrdup (p, value);
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
virgule_xml_find_child_string (xmlNode *n, const char *tag, char *def)
{
  xmlNode *child;

  child = virgule_xml_find_child (n, tag);
  if (child == NULL)
    return def;
  else
    return virgule_xml_get_string_contents (child);
}
