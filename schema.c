/* A mechanism to partially automate generating HTML forms, sucking things
   out of forms, and gluing them to XML. */

#include "httpd.h"

#include <libxml/tree.h>

#include "buffer.h"
#include "xml_util.h"
#include "schema.h"

/**
 * schema_render_input: Render an input field.
 * @sf: The schema description.
 * @tree: a record containing the field as a property.
 *
 * Renders the input field, inserting the value from @tree if present.
 **/
void
schema_render_input (pool *p, Buffer *b, SchemaField *sf, xmlNode *tree)
{
  char *value;

  if (tree == NULL)
    value = NULL;
  else
    value = xml_get_prop (p, tree, sf->name);
  buffer_printf (b, "<p> %s: <br>\n", sf->description);
  if (sf->flags & SCHEMA_TEXTAREA)
    buffer_printf (b, "<textarea name=\"%s\" cols=%d rows=%d wrap=hard>%s</textarea> </p>\n",
		   sf->name,
		   sf->size / 1000,
		   sf->size % 1000,
		   value ? ap_escape_html (p, value) : "");
  else if (sf->flags & SCHEMA_SELECT)
    {
      int i;
      buffer_printf (b, "<select name=\"%s\">\n", sf->name);
      for (i = 0; sf->choices[i] != NULL; i++)
	buffer_printf (b, "<option%s>%s\n",
		       value && !strcmp (value, sf->choices[i]) ? " selected" : "",
		       sf->choices[i]);
      buffer_puts (b, "</select>\n");
    }
  else
    buffer_printf (b, "<input name=\"%s\" size=%d value=\"%s\"> </p>\n",
		   sf->name, sf->size,
		   value ? ap_escape_html (p, value) : "");
}


/**
 * schema_render_inputs: Render multiple input templates.
 * @sf: An array of schema descriptions (last one has NULL name).
 * @fields: A NULL-terminated array of field names.
 *
 **/
void
schema_render_inputs (pool *p, Buffer *b, SchemaField *sf, const char **fields, xmlNode *tree)
{
  int i;
  int j;

  for (i = 0; fields[i] != NULL; i++)
    {
      for (j = 0; sf[j].name != NULL; j++)
	if (!strcmp (fields[i], sf[j].name))
	  {
	    schema_render_input (p, b, &sf[j], tree);
	    break;
	  }
    }
}

void
schema_put_field (pool *p, SchemaField *sf, xmlNode *tree, table *args)
{
  const char *value;

  value = ap_table_get (args, sf->name);
  if (value != NULL)
    xmlSetProp (tree, sf->name, value);
}

/**
 * schema_render_inputs: Render multiple input templates.
 * @sf: An array of schema descriptions (last one has NULL name).
 * @fields: A NULL-terminated array of field names.
 *
 **/
void
schema_put_fields (pool *p, SchemaField *sf, const char **fields, xmlNode *tree, table *args)
{
  int i;
  int j;

  for (i = 0; fields[i] != NULL; i++)
    {
      for (j = 0; sf[j].name != NULL; j++)
	if (!strcmp (fields[i], sf[j].name))
	  {
	    schema_put_field (p, &sf[j], tree, args);
	    break;
	  }
    }
}

