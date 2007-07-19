#include <apr.h>
#include <apr_strings.h>
#include <httpd.h>

#include <libxml/tree.h>
#include <libxml/parser.h>
#include <libxml/xmlmemory.h>

#include "private.h"
#include "buffer.h"
#include "db.h"
#include "req.h"
#include "auth.h"

#include "xmlrpc.h"
#include "xmlrpc-methods.h"


/* make sure this doesn't clash with any HTTP status codes */
#define XMLRPC_FAULT_SERVED 1000

/* Create <value>s */
static xmlNode *
xmlrpc_value_int (xmlNode *node, long i)
{
  char buf[12];
  apr_snprintf(buf, 12, "%ld", i);

  node = xmlNewChild (node, NULL, (xmlChar *)"value", NULL);
  node = xmlNewChild (node, NULL, (xmlChar *)"int", (xmlChar *)buf);
  return node;
}

static xmlNode *
xmlrpc_value_string (xmlNode *node, const char *s)
{
  node = xmlNewChild (node, NULL, (xmlChar *)"value", NULL);
  node = xmlNewTextChild (node, NULL, (xmlChar *)"string", (xmlChar *)s);
  return node;
}

static xmlNode *
xmlrpc_value_datetime (xmlNode *node, VirguleReq *vr, const char *s)
{
  char *dt, *s_tmp, *y, *m, *d, *rest;

  s_tmp = apr_pstrdup (vr->r->pool, s);
  
  /* Handle YYYY-MM-DD case with no tail */
  rest = s_tmp[10] ? s_tmp + 11 : NULL;

  s_tmp[4] = s_tmp[7] = s_tmp[10] = 0;
  y = s_tmp;
  m = s_tmp + 5;
  d = s_tmp + 8;

  if (rest == NULL)
    dt = apr_psprintf (vr->r->pool, "%s%s%s", y, m, d);
  else
    dt = apr_psprintf (vr->r->pool, "%s%s%sT%s", y, m, d, rest);

  node = xmlNewChild (node, NULL, (xmlChar *)"value", NULL);
  node = xmlNewTextChild (node, NULL, (xmlChar *)"dateTime.iso8601", (xmlChar *)dt);
  return node;
}

static xmlNode *
xmlrpc_value_struct (xmlNode *node)
{
  node = xmlNewChild (node, NULL, (xmlChar *)"value", NULL);
  node = xmlNewChild (node, NULL, (xmlChar *)"struct", NULL);
  return node;
}

static xmlNode *
xmlrpc_struct_add_member (xmlNode *node, const char *name)
{
  node = xmlNewChild (node, NULL, (xmlChar *)"member", NULL);
  xmlNewChild (node, NULL, (xmlChar *)"name", (xmlChar *)name);
  return node;
}


/* Create and send responses */
static xmlNode *
xmlrpc_create_response (VirguleReq *vr)
{
  xmlDoc *r = xmlNewDoc ((xmlChar *)"1.0");
  r->xmlRootNode = xmlNewDocNode (r, NULL, (xmlChar *)"methodResponse", NULL);
  return r->xmlRootNode->doc == r ? r->xmlRootNode : NULL;
}

static int
xmlrpc_send_response (VirguleReq *vr, xmlNode *r)
{
  xmlChar *mem;
  int size;

  xmlDocDumpFormatMemory (r->doc, &mem, &size, 1);
  virgule_buffer_write (vr->b, (char *)mem, size);
  xmlFree (mem);
  xmlFreeDoc (r->doc);

  vr->r->content_type = "text/xml";
  return virgule_send_response (vr);
}


/* Create and send a fault response */
int
virgule_xmlrpc_fault (VirguleReq *vr, int code, const char *fmt, ...)
{
  xmlNode *resp, *str;
  va_list ap;
  char *msg;
  int ret;

  va_start (ap, fmt);
  msg = apr_pvsprintf (vr->r->pool, fmt, ap);
  va_end (ap);
  
  resp = xmlrpc_create_response (vr);

  str = xmlrpc_value_struct (xmlNewChild (resp, NULL, (xmlChar *)"fault", NULL));
  xmlrpc_value_int (xmlrpc_struct_add_member (str, "faultCode"), code);
  xmlrpc_value_string (xmlrpc_struct_add_member (str, "faultString"), msg);

  ret = xmlrpc_send_response (vr, resp);
  if (ret != OK)
    return ret;

  return XMLRPC_FAULT_SERVED;
}


/* Create and send a normal response */
int
virgule_xmlrpc_response (VirguleReq *vr, const char *types, ...)
{
  xmlNode *resp;
  xmlNode *container;
  va_list va;
  int i;
  
  if (!strlen (types))
    return virgule_xmlrpc_fault (vr, 1, "internal error: must return something!");

  resp = xmlrpc_create_response (vr);
  container = xmlNewChild (xmlNewChild (resp, NULL, (xmlChar *)"params", NULL),
                           NULL, (xmlChar *)"param", NULL);

  if (strlen (types) > 1)
    {
      container = xmlNewChild (container, NULL, (xmlChar *)"value", NULL);
      container = xmlNewChild (xmlNewChild (container, NULL, (xmlChar *)"array", NULL),
			       NULL, (xmlChar *)"data", NULL);
    }

  va_start (va, types);
  for (i=0; i<strlen (types); i++)
    {
      switch (types[i])
        {
	case 'i':
          xmlrpc_value_int (container, va_arg (va, int));
          break;

        case 's':
          xmlrpc_value_string (container, va_arg (va, char *));
          break;
          
	case 'd':
	  xmlrpc_value_datetime (container, vr, va_arg (va, char *));
	  break;

        default:
          va_end (va);
          xmlFreeDoc (resp->doc);
          return virgule_xmlrpc_fault (vr, 1, "internal error: unknown type '%c'",
                               types[i]);
	}
    }
  va_end (va);
  
  return xmlrpc_send_response (vr, resp);
}


/* Authenticate the user */
int
virgule_xmlrpc_auth_user (VirguleReq *vr, const char *cookie)
{
  virgule_auth_user_with_cookie (vr, cookie);
  if (vr->u == NULL)
    return virgule_xmlrpc_fault (vr, 1, "authentication failure");
  
  return OK;
}


/* Extract the parameters from the request */  
int
virgule_xmlrpc_unmarshal_params (VirguleReq *vr, xmlNode *params,
			         const char *types, ...)
{
  xmlNode *param;
  va_list va;
  int argc;
  int i;

  /* check that the caller supplied the right number of parameters */
  argc = 0;
  if (params)
    {
      if (strcmp ((char *)params->name, "params"))
        return virgule_xmlrpc_fault (vr, 1, "expecting <params>, got <%s>",
                             params->name);  

      for (param = params->children; param; param = param->next)
        {
          if (xmlIsBlankNode (param))
            continue;
          argc++;
        }
      if (argc != strlen (types))
        return virgule_xmlrpc_fault (vr, 1, "expecting %d parameters, got %d",
                             strlen (types), argc);
    }
  else
    {
      if (strlen(types))
        return virgule_xmlrpc_fault (vr, 1, "expecting %d parameters, got 0",
                             strlen (types));
      return OK;
    }

  /* unmarshal the parameters */
  param = params->children;
  va_start (va, types);
  for (i=0; i<argc; i++)
    {
      xmlNode *value;
	
      while (xmlIsBlankNode (param))
        param = param->next;

      value = param->children;
      while (xmlIsBlankNode (value))
        value = value->next;

      value = value->children;
      while (xmlIsBlankNode (value))
        value = value->next;
	
      switch (types[i])
        {
	case 'i':
          if (!strcmp ((char *)value->name, "int") || !strcmp ((char *)value->name, "i4"))
	    {
              char *val;
		
              val = (char *)xmlNodeListGetString (value->doc, value->children, 1);
              *va_arg (va, int *) = atoi (val);
              xmlFree (val);
	    }
          else if (xmlNodeIsText (value))
            {
              char *val;
                
              val = (char *)xmlNodeGetContent (value);
              *va_arg (va, int *) = atoi (val);
              xmlFree (val);
            }
          else
            {
              va_end (va);
              return virgule_xmlrpc_fault (vr, 1,
                                   "param %d: expecting <int>, got <%s>",
                                   i + 1, value->name);
	    }
          break;

        case 's':
          if (!strcmp ((char *)value->name, "string"))
            {
              char *val;
		
              val = (char *)xmlNodeListGetString (value->doc, value->children, 1);
              *va_arg (va, char **) = apr_pstrdup (vr->r->pool, val);
              xmlFree (val);
            }
          else if (xmlNodeIsText (value))
            {
              char *val;
                
              val = (char *)xmlNodeGetContent (value);
              *va_arg (va, char **) = apr_pstrdup (vr->r->pool, val);
              xmlFree (val);
            }  
          else
            {
              va_end (va);
              return virgule_xmlrpc_fault (vr, 1,
                                   "param %d: expecting <string>, got <%s>",
                                   i + 1, value->name);
	    }
    
          break;
          
	default:
          va_end (va);
          return virgule_xmlrpc_fault (vr, 1, "internal error: unknown type '%c'",
                               types[i]);
	}
      param = param->next;
    }
  va_end (va);

  return OK;
}


/* Fob the request off at the appropriate method function */
static int
xmlrpc_unmarshal_request (VirguleReq *vr, xmlNode *xr)
{
  extern xmlrpc_method xmlrpc_method_table[];
  xmlNode *n;
  char *tmp;
  const char *name;
  xmlrpc_method *m;

  /* root element should be a <methodCall> */
  if (strcmp ((char *)xr->name, "methodCall"))
    return virgule_xmlrpc_fault (vr, 1, "expecting <methodCall>, got <%s>", xr->name);
    
  /* first element of methodCall should be a <methodName> */
  n = xr->children;
  while (n && xmlIsBlankNode (n))
    n = n->next;

  if (!n || strcmp ((char *)n->name, "methodName"))
    return virgule_xmlrpc_fault (vr, 1, "expecting <methodName>, got <%s>", n->name);

  tmp = (char *)xmlNodeListGetString (n->doc, n->children, 1);
  name = apr_pstrdup (vr->r->pool, tmp);
  xmlFree (tmp);

  /* second element of methodCall should be a <params> */
  n = n->next;
  while (n && xmlIsBlankNode (n))
    n = n->next;

  /* dispatch the method */
  for (m = xmlrpc_method_table; m->name != NULL; m++)
    {
      if (!strcmp (m->name, name))
        break;
    }
  if (m->name == NULL)
    return virgule_xmlrpc_fault (vr, 1, "%s: method not implemented", name);

  return m->func (vr, n);
}


/* Main handler for requests */
int
virgule_xmlrpc_serve (VirguleReq *vr)
{
  xmlDoc *request = NULL;
  int ret;
  
  if (strcmp (vr->uri, "/XMLRPC"))
    return DECLINED;

  if (vr->r->method_number != M_POST)
    return HTTP_METHOD_NOT_ALLOWED;

  request = xmlParseMemory (vr->args, vr->r->read_length);
  if (request)
    {
      ret = xmlrpc_unmarshal_request (vr, request->xmlRootNode);
      xmlFreeDoc (request);
    }
  else
    {
      ret = virgule_xmlrpc_fault (vr, 1, "unable to parse request");
    }

  if (ret == XMLRPC_FAULT_SERVED)
    return OK;
  
  return ret;
}
