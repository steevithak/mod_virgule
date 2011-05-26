#include <ctype.h>
#include <time.h>

#include <apr.h>
#include <apr_strings.h>
#include <apr_file_io.h>
#include <apr_date.h>
#include <apr_sha1.h>
#include <httpd.h>
#include <http_log.h>

#include <libxml/tree.h>
#include <libxml/entities.h>
#include <libxml/HTMLtree.h>
#include <libxml/HTMLparser.h>

#include "private.h"
#include "buffer.h"
#include "db.h"
#include "req.h"
#include "wiki.h"
#include "util.h"
#include "xml_util.h"

static const char UTF8length[256] = {
  1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
  1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
  1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
  1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
  1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
  1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
  2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,
  3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,4,4,4,4,4,4,4,4,5,5,5,5,6,6,6,6
};
				
static const char basis_64[] = 
"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"; 


/**
 * RSR note: This is the UTF8ToHTML function out of libxml2 with a fix that
 * properly parses UTF-8 documents containing characters for which there
 * are no named entity values in HTML (e.g. Chinese characters). Instead,
 * these are simply converted to numerical entity values. A patch was
 * submitted and accepted but we'll have to include the code here if we
 * want to work with libxml2 v2.6.27 and earlier. Once all major distros
 * include a new version we can pull this and start relying on libxml2.
 *
 * UTF8ToHtml:
 * @out:  a pointer to an array of bytes to store the result
 * @outlen:  the length of @out
 * @in:  a pointer to an array of UTF-8 chars
 * @inlen:  the length of @in
 *
 * Take a block of UTF-8 chars in and try to convert it to an ASCII
 * plus HTML entities block of chars out.
 *
 * Returns 0 if success, -2 if the transcoding fails, or -1 otherwise
 * The value of @inlen after return is the number of octets consumed
 *     as the return value is positive, else unpredictable.
 * The value of @outlen after return is the number of octets consumed.
 */
static int
virgule_UTF8ToHtml(unsigned char* out, int *outlen,
              const unsigned char* in, int *inlen) {
    const unsigned char* processed = in;
    const unsigned char* outend;
    const unsigned char* outstart = out;
    const unsigned char* instart = in;
    const unsigned char* inend;
    unsigned int c, d;
    int trailing;

    if ((out == NULL) || (outlen == NULL) || (inlen == NULL)) return(-1);
    if (in == NULL) {
        /*
	 * initialization nothing to do
	 */
	*outlen = 0;
	*inlen = 0;
	return(0);
    }
    inend = in + (*inlen);
    outend = out + (*outlen);
    while (in < inend) {
	d = *in++;
	if      (d < 0x80)  { c= d; trailing= 0; }
	else if (d < 0xC0) {
	    /* trailing byte in leading position */
	    *outlen = out - outstart;
	    *inlen = processed - instart;
	    return(-2);
        } else if (d < 0xE0)  { c= d & 0x1F; trailing= 1; }
        else if (d < 0xF0)  { c= d & 0x0F; trailing= 2; }
        else if (d < 0xF8)  { c= d & 0x07; trailing= 3; }
	else {
	    /* no chance for this in Ascii */
	    *outlen = out - outstart;
	    *inlen = processed - instart;
	    return(-2);
	}

	if (inend - in < trailing) {
	    break;
	} 

	for ( ; trailing; trailing--) {
	    if ((in >= inend) || (((d= *in++) & 0xC0) != 0x80))
		break;
	    c <<= 6;
	    c |= d & 0x3F;
	}

	/* assertion: c is a single UTF-4 value */
	if (c < 0x80) {
	    if (out + 1 >= outend)
		break;
	    *out++ = c;
	} else {
	    int len;
	    const htmlEntityDesc * ent;
	    const char *cp;
	    char nbuf[16];

	    /*
	     * Try to lookup a predefined HTML entity for it
	     */

	    ent = htmlEntityValueLookup(c);
	    if (ent == NULL) {
	      snprintf(nbuf, sizeof(nbuf), "#%u", c);
	      cp = nbuf;
	    }
	    else
	      cp = ent->name;
	    len = strlen(cp);
	    if (out + 2 + len >= outend)
		break;
	    *out++ = '&';
	    memcpy(out, cp, len);
	    out += len;
	    *out++ = ';';
	}
	processed = in;
    }
    *outlen = out - outstart;
    *inlen = processed - instart;
    return(0);
}



static char *
b64enc (apr_pool_t *p, const char *data, int size)
{
  char *result;
  int result_blocks;
  int i;

  result_blocks = ((size + 2) / 3);
  result = apr_pcalloc (p, 4 * result_blocks + 1);
  for (i = 0; i < result_blocks; i++)
    {
      int rem = size - i * 3;
      int d1 = data[i * 3] & 0xff;
      int d2 = rem > 1 ? data[i * 3 + 1] & 0xff : 0;
      int d3 = rem > 2 ? data[i * 3 + 2] & 0xff : 0;
      result[i * 4] = basis_64[(d1 >> 2) & 0x3f];
      result[i * 4 + 1] = basis_64[((d1 & 0x03) << 4) | ((d2 & 0xf0) >> 4)];
      result[i * 4 + 2] = rem > 1 ? basis_64[((d2 & 0x0f) << 2) |
					    ((d3 & 0xc0) >> 6)] : '=';
      result[i * 4 + 3] = rem > 2 ? basis_64[d3 & 0x3f] : '=';
    }
  return result;
}

/**
 * virgule_rand_cookie: Create a new, random cookie.
 * @p: pool in which to allocate.
 *
 * Creates a new, base-64 encoded random cookie. The cookie has 120
 * bits of entropy, which should be enough for even the paranoid, and
 * also aligns nicely in base64.
 *
 * Return value: The random cookie.
 **/
char *
virgule_rand_cookie (apr_pool_t *p)
{
  apr_file_t *fd;
  apr_size_t bytes_read;
  char buf[15];

  if (apr_file_open (&fd, "/dev/random", APR_READ,
      APR_UREAD|APR_UWRITE|APR_GWRITE|APR_WREAD, p) != APR_SUCCESS)
    return NULL;
    
  bytes_read = sizeof(buf);
  apr_file_read (fd, buf, &bytes_read);
  apr_file_close (fd);
  if (bytes_read < sizeof(buf))
    {
      return NULL;
    }
  return b64enc (p, buf, sizeof(buf));
}

const char *
virgule_match_prefix (const char *url, const char *prefix)
{
  int len;
  len = strlen (prefix);
  if (!strncmp (url, prefix, len))
    return url + len;
  else
    return NULL;
}

/**
 * escape_noniso_char: Provide a plausible replacement for non-iso characters.
 * @c: Character to escape.
 *
 * If a character is not an ISO printable, provide a replacement. Otherwise,
 * return NULL.
 *
 * I was very tempted to name this routine "defenestrate_char", as a pun
 * on "escape windows char". Cooler heads prevailed.
 *
 * Reference: http://czyborra.com/charsets/codepages.html#CP1252
 *
 * Return value: Replacement string.
 **/
static const char *
escape_noniso_char (char c)
{
  int u = c & 0xff;

  if ((u >= 0x20 && u <= 0x80) ||
      u >= 0xa0)
    return NULL;
  switch (u)
    {
    case 0x80:
      return "[Euro]";
    case 0x82:
      return ",";
    case 0x83:
      return "f";
    case 0x84:
      return ",,";
    case 0x85:
      return "...";
    case 0x86:
      return "[dagger]";
    case 0x87:
      return "[dbldagger]";
    case 0x88:
      return "^";
    case 0x89:
      return "%0";
    case 0x8A:
      return "S";
    case 0x8B:
      return "&lt;";
    case 0x8C:
      return "OE";
    case 0x8E:
      return "Z";
    case 0x91:
      return "`";
    case 0x92:
      return "'";
    case 0x93:
      return "``";
    case 0x94:
      return "''";
    case 0x95:
      return "*";
    case 0x96:
      return "-";
    case 0x97:
      return "--";
    case 0x98:
      return "~";
    case 0x99:
      return "[TM]";
    case 0x9A:
      return "s";
    case 0x9B:
      return "&gt;";
    case 0x9C:
      return "oe";
    case 0x9E:
      return "z";
    case 0x9F:
      return "Y";
    default:
      return "";
    }
}

static void
nice_text_cat (char *buf, int *p_j, const char *src, int size)
{
  if (buf) memcpy (buf + *p_j, src, size);
  *p_j += size;
}

static int
nice_text_helper (const char *raw, char *buf)
{
  int i;
  int j;
  int nl_state = 0;
  const char *replacement;

  j = 0;
  for (i = 0; raw[i]; i++)
    {
      char c = raw[i];

      if (c == '\n')
	{
	  nice_text_cat (buf, &j, "\n", 1);

	  if (nl_state == 3)
	    nl_state = 1;
	  else if (nl_state == 1)
	    nl_state = 2;
	}
      else if (c != '\r')
	{
	  if (nl_state == 2)
	    nice_text_cat (buf, &j, "<p> ", 4);
	  nl_state = 3;

	  if (c == '&')
	    nice_text_cat (buf, &j, "&amp;", 5);
	  else if (c == '<')
	    nice_text_cat (buf, &j, "&lt;", 4);
	  else if (c == '>')
	    nice_text_cat (buf, &j, "&gt;", 4);
	  else if ((replacement = escape_noniso_char (c)) != NULL)
	    nice_text_cat (buf, &j, replacement, strlen (replacement));
	  else
	    {
	      if (buf) buf[j] = c;
	      j++;
	    }
	}
    }
  return j;
}


/**
 * virgule_nice_UTF8: Convert raw UTF8 into nice HTML. Outlength is probably
 * overkill. It assumes every input character is converted to a 6 byte
 * entity in the output buffer.
 * @raw: Raw UTF8
 *
 * Return value: HTML formatted text
 **/
char *
virgule_nice_utf8 (apr_pool_t *p, const char *utf8)
{
  int inlen = strlen(utf8);
  int outlen = inlen * 6;
  char *out = NULL;
   
  if (utf8 == NULL)
     return NULL;

  out = apr_palloc (p, outlen + 1);
  if(out == NULL)
    return NULL;

  memset(out,0,outlen);
  if(virgule_UTF8ToHtml ((unsigned char *)out,&outlen,(unsigned char *)utf8,&inlen) == 0)
    {
      out[outlen] = 0;
      return out;
    }

  return NULL;

}
 

/**
 * nice_text: Convert raw text into nice HTML.
 * @raw: Raw text.
 *
 * Return value: HTML formatted text.
 **/
char *
virgule_nice_text (apr_pool_t *p, const char *raw)
{
  char *result;
  int size;

  if (raw == NULL)
    return NULL;
  size = nice_text_helper (raw, NULL);
  result = apr_palloc (p, size + 1);
  nice_text_helper (raw, result);
  result[size] = '\0';
  return result;
}


static void
nice_person_link (VirguleReq *vr, xmlNode *n)
{
    apr_pool_t *p = vr->r->pool;
    char *tmp = "";
    char *name = virgule_xml_get_string_contents (n);
    if(name != NULL)
    {
	tmp =  apr_psprintf (p, "%s/person/%s", vr->prefix, ap_os_escape_path (p, name, 1));
	xmlNodeSetName (n, (xmlChar *)"a");
	xmlSetProp (n, (xmlChar *)"href", (xmlChar *)(tmp == NULL ? "" : tmp));
    }
}


static void
nice_proj_link (VirguleReq *vr, xmlNode *n)
{
    apr_pool_t *p = vr->r->pool;
    char *tmp = "";
    char *name = virgule_xml_get_string_contents (n);
    if(name != NULL)
    {
	tmp =  apr_psprintf (p, "%s/proj/%s", vr->prefix, ap_os_escape_path (p, name, 1));
        xmlNodeSetName (n, (xmlChar *)"a");
	xmlSetProp (n, (xmlChar *)"href", (xmlChar *)(tmp == NULL ? "" : tmp));
    }
}


/**
 * The passed string will be modified to make it a legal CSS1 class name.
 * CSS1 class names MUST start with an [A-Za-z]. Subsequent characters MUST
 * be [A-Za-z0-9] or '-'.  All illegal characters must be escaped using a
 * prefix of '\' (e.g. 2foo becomes \2foo and foo_bar becomes foo\_bar).
 */
char *
virgule_force_legal_css_name (VirguleReq *vr, const char *name)
{
  int inlen = strlen(name);
  int outlen = inlen * 3;
  int i = 0;
  int o = 0;
  char *cssname = NULL;
   
  if (name == NULL)
     return NULL;

  cssname = apr_palloc (vr->r->pool, outlen + 1);
  if(cssname == NULL)
    return NULL;

  memset(cssname,0,outlen);

  if (!isalpha(name[0]))
    {
      cssname[o++] = '\\';
      cssname[o++] = name[0];
    }
  else
    cssname[o++] = name[0];

  for (i = 1; name[i]; i++,o++)
    {
      if (name[i] == ' ')
        {
	  cssname[o++] = '\\';
	  cssname[o++] = '2';
	  cssname[o] = '0';
	}
      else if (!isalnum(name[i]) && !name[i] != '-')
        {
          cssname[o++] = '\\';
	  cssname[o] = name[i];
        }
      else
        cssname[o] = name[i];
    }
    
  cssname[o] = 0;
  return cssname;
}


struct _Topic {
  char *desc;
  char *url;
};


/**
 * add_topic - Allocates a Topic structures during loading
 * of the site configuration. This information must survive across
 * multiple requests so it uses the thread private pool.
 */
const Topic *
virgule_add_topic (VirguleReq *vr, const char *desc, const char *url)
{
  Topic *topic;
  topic = apr_palloc (vr->priv->pool, sizeof(Topic));
  topic->desc = apr_pstrdup (vr->priv->pool, desc);
  topic->url = apr_pstrdup (vr->priv->pool, url);
  return topic;
}



struct _NavOption {
  char *label;
  char *url;
};

/**
 * add_nav_option - Allocates a NavOption structures during loading
 * of the site configuration. This information must survive across
 * multiple requests so it uses the thread private pool.
 */
const NavOption *
virgule_add_nav_option (VirguleReq *vr, const char *label, const char *url)
{
  NavOption *option;
  option = apr_palloc (vr->priv->pool, sizeof(NavOption));
  option->label = apr_pstrdup (vr->priv->pool, label);
  option->url = apr_pstrdup (vr->priv->pool, url);
  return option;
}


struct _AllowedTag {
  char *tagname;
  int empty;
  char **allowed_attributes;
  void (*handler) (VirguleReq *vr, xmlNode *n);
};

static AllowedTag special_allowed_tags[] = {
  { "person", 0, NULL, nice_person_link },
//  { "proj", 0, NULL, nice_proj_link },
  { "project", 0, NULL, nice_proj_link },
  { "wiki", 0, NULL, virgule_wiki_link },
};


/**
 * add_allowed_tag - Allocates an AllowedTag structures during loading
 * of the site configuration. This information must survive across
 * multiple requests so it uses the thread private pool.
 */
const AllowedTag *
virgule_add_allowed_tag (VirguleReq *vr, const char *tagname, int can_be_empty,
		char **allowed_attributes)
{
  AllowedTag *tag;
  int i, n = sizeof (special_allowed_tags) / sizeof (special_allowed_tags[0]);

  /* special tags are handled specially */
  for (i = 0; i < n; i++)
      if (!strcmp(special_allowed_tags[i].tagname, tagname))
	return &special_allowed_tags[i];

  tag = apr_palloc (vr->priv->pool, sizeof(AllowedTag));
  tag->tagname = apr_pstrdup  (vr->priv->pool, tagname);
  tag->empty = can_be_empty;
  tag->allowed_attributes = allowed_attributes;
  tag->handler = NULL;

  return tag;
}

int
virgule_render_acceptable_html (VirguleReq *vr)
{
  const AllowedTag **tag;

  virgule_buffer_printf (vr->b, "<p> The following <a href=\"%s/html.html\">HTML</a> "
		 "is accepted: ", vr->prefix);

  for (tag = vr->priv->allowed_tags; *tag; tag++)
    virgule_buffer_printf (vr->b, "&lt;%s&gt; ", (*tag)->tagname);

  virgule_buffer_puts (vr->b, "</p>\n");

  return 0;
}


/**
 * virgule_user_is_special: Test whether or not the current user, if any, is
 * on the list of special (admin) users. Returns TRUE if the user is special.
 */
int
virgule_user_is_special (VirguleReq *vr, const char *user)
{
  const char **u = NULL;
  
  if (user)
    {
      for (u = vr->priv->special_users; *u; u++)
        if (!strcmp (user, *u))
          break;

      if (*u)
        return TRUE;
    }
    
  return FALSE;
}


/**
 * virgule_add_nofollow: Add a nofollow relationship attribute
 * to any anchor tags found in the string.
 */
char *
virgule_add_nofollow (VirguleReq *vr, const char *raw)
{
  const char *rel = " rel=\"nofollow\"";
  const char *c = NULL;
  char *out = NULL;
  char *tmp = NULL;
  int i = 0;
  
  /* count the anchor tags */
  for(c=raw;*c;c++)
    {
      if(*c == '<' && (*(c+1) == 'a' || *(c+1) == 'A'))
        {
	  i++;
	  c++;
	}
    }

  if(i == 0)
    return (char *)raw;

  /* allocate a new buffer or fail silently */
  out = apr_palloc (vr->priv->pool, (apr_size_t)((i*15)+strlen(raw)+1));
  if(out == NULL)
    return (char *)raw;

  /* add the nofollow relations */
  for(tmp=out,c=raw;*c;c++)
    {
      if(*c == '<' && (*(c+1) == 'a' || *(c+1) == 'A'))
        {
	  while(*c && *c!='>')
	    *tmp++ = *c++;
	  if(*c == '>')
	    {
	      memcpy(tmp,rel,15);
	      tmp+=15;
	    }
	  *tmp++ = *c;
	}
      else
        *tmp++ = *c;
    }
  *tmp=0;
    
  return out;
}


/**
 * virgule_strip_a: Remove any anchor tags found in the string
 */
char *
virgule_strip_a (VirguleReq *vr, const char *raw)
{
  /* strip anchor tags */
  char *clean = apr_pstrdup (vr->r->pool, raw);
  char *tmp1 = (char *)raw;
  char *tmp2 = clean;
  while(*tmp1!=0)
    {
      if(strncasecmp(tmp1,"<a",2)==0)
        {
	  while(*tmp1!=0&&*tmp1!='>')
	    tmp1++;
	  if(*tmp1!=0)
            tmp1++;
	  continue;
        }
      if(strncasecmp(tmp1,"</a>",4)==0)
        {
          tmp1+=4;
	  continue;
	}
      *tmp2 = *tmp1;
      tmp1++;
      tmp2++;
    }
  *tmp2=0;

  return clean;
}


/**
 * virgule_format_content: Applies appropriate type of content conversion
 * for rendering the specified type of data.
 *
 **/
char *
virgule_format_content (VirguleReq *vr, char *raw, int format_type)
{

// debug use only
//ap_log_rerror(APLOG_MARK, APLOG_CRIT, APR_SUCCESS, vr->r,"format type [%d]",format_type);

    switch (format_type)
    {
	case 0:
	    return virgule_normalize_html (vr, raw, NULL);
	case 1:
	    return raw;
	case 2:
	default:
	    return raw;
    }
    return raw;
}

 
/**
 * virgule_xmlSetTreeNs:
 * @tree: the top element
 * @ns: the namespace
 *
 * update all nodes under the tree to point to the namespace
 */
void
virgule_xmlSetTreeNs (xmlNodePtr tree, xmlNsPtr ns) {
    xmlAttrPtr prop;
    
    if (tree == NULL)
	return;
    if (tree->ns != ns) {
	if (tree->type == XML_ELEMENT_NODE) {
	    prop = tree->properties;
	    while (prop != NULL) {
		prop->ns = ns;
		virgule_xmlSetListNs(prop->children, ns);
		prop = prop->next;
	    }
	}
	if (tree->children != NULL)
	    virgule_xmlSetListNs(tree->children, ns);
	tree->ns = ns;
    }
}


/**
 * virgule_xmlSetListNs:
 * @list: the first element
 * @ns: the namespace
 *
 * update all nodes in the list to point to the namespace
 */
void
virgule_xmlSetListNs(xmlNodePtr list, xmlNsPtr ns) {
    xmlNodePtr cur;
    
    if (list == NULL)
	return;
    cur = list;
    while (cur != NULL) {
	if (cur->ns != ns)
	    virgule_xmlSetTreeNs (cur, ns);
	cur = cur->next;
    }
}


/**
 * nice_element: Walks the attribute list for a single HTML element tag
 * stripping out any properties not on the allowed attributes list.
 * Also checks for relative img src attributes and prepends the baseurl.
 *
 **/
 
// debug
// ap_log_rerror(APLOG_MARK, APLOG_CRIT, APR_SUCCESS, vr->r,"img src after fixup [%s]",(char *)np->children->content);

void
nice_element (VirguleReq *vr, const AllowedTag *tag, xmlNodePtr n, const char *baseurl)
{
    xmlAttrPtr np = n->properties;
    while (np != NULL)
    {
	xmlAttrPtr npnext = np->next;
	/* Allowed attribute check */
	if (tag->allowed_attributes)
	{
	    char **att = NULL;
	    for (att = tag->allowed_attributes; *att; att++)
	    {
		if(strcasecmp((char *)np->name,*att)==0)
		    break;
	    }
	    if (*att == NULL)
	    {
		xmlAttrPtr tmp = np;
		np = np->next;
		xmlRemoveProp (tmp);
		continue;
	    }
	}
	/* BaseURL fixup */
	if (baseurl != NULL && strcasecmp(tag->tagname, "img")==0 && strcasecmp((char *)np->name,"src")==0)
	{
	    char *absurl = NULL;
	    if(strncasecmp((char *)np->children->content,"http://",7)!=0) {
		absurl = apr_pstrcat(vr->r->pool, baseurl, (char *)np->children->content, NULL);
		xmlSetProp(n, (xmlChar *)"src", (xmlChar *)absurl);
	    }
	}
	np = npnext;
    }
}


/**
 * virgule_normalize_html_node: Normalizes a node of an HTML tree,
 * recursively Walking the tree if needed.
 *
 **/
void
virgule_normalize_html_node (VirguleReq *vr, xmlNodePtr a_node, const char *baseurl)
{
    xmlNodePtr cur_node = NULL;
    for (cur_node = a_node; cur_node; cur_node = cur_node->next) {

	if(cur_node->type == XML_ELEMENT_NODE) {
	    const AllowedTag **tag = NULL;

	    /* Look up tag in the allowed tag list */
	    for (tag = vr->priv->allowed_tags; *tag; tag++)
	    {
		if(strcasecmp((*tag)->tagname, (char *)cur_node->name)==0)
		{
		    /* if it has a handler, run it */
		    if((*tag)->handler != NULL)
			(*tag)->handler(vr, cur_node);
		
		    /* if it has properties, clean 'em up */
		    if(cur_node->properties != NULL)
			nice_element (vr, *tag, cur_node, baseurl);

		    break;
		}
	    
		/* If our tag wasn't found, rip it out */
		if (*tag == NULL)
		{
//ap_log_rerror(APLOG_MARK, APLOG_CRIT, APR_SUCCESS, vr->r,"disallowed tag [%s]",(char *)cur_node->name);
		    if (cur_node->children != NULL)
			xmlReplaceNode (cur_node, cur_node->children);
		    xmlFreeNode (cur_node);
// will this keep going past a removed node?
// we might need to patch cur_node->previous into cur_node after the removal
		}

// create handler for danger tags that allow youtube and vimeo
// can this be done through configurable regex somehow?

	        // look this tag up in the disallowed list (XSS and stuff)
		// if found and handler exists, run handler
		// if found but no handler exists, remove this tag (continue)
	    }
	}
	virgule_normalize_html_node(vr,cur_node->children,baseurl);
    }
}


/**
 * virgule_normalize_html_tree: Walks an XML tree of HTML tags and 
 * removes any undesired or dangerous markup that could lead to XSS
 * threats. Dumps tree to a text buffer and returns a pointer.
 * NOTE: This function does not free the XML tree; make sure the 
 * calling function free the tree (or xmldoc) when finished!
 *
 **/
char *
virgule_normalize_html_tree (VirguleReq *vr, xmlNodePtr tree, const char *baseurl)
{
    char *nicehtml = NULL;
    xmlNsPtr ns = NULL;
    xmlNodePtr out_n;
    xmlBufferPtr buf = NULL;

    if (tree == NULL || tree->children == NULL)
	return NULL;

    /* walk tree */
    virgule_normalize_html_node(vr,tree->children,baseurl);

    /* Create the XHTML namespace pointer and assign to all nodes. */
    /* Should be done after normalization to avoid need to set the */ 
    /* ns on every new node added during normalizing process */
    ns = xmlNewNs (tree, (xmlChar *)"http://www.w3.org/TR/xhtml1/DTD/xhtml1-transitional.dtd", NULL);
    virgule_xmlSetTreeNs (tree, ns);

    /* create an output buffer and dump tree to it */
    buf = xmlBufferCreate();
    for (out_n = tree->children; out_n != NULL; out_n = out_n->next)
	xmlNodeDump (buf, tree->doc, out_n, 0, 1);

    /* Free memory and return the cleaned HTML */
    nicehtml = apr_pstrdup (vr->priv->pool, (char *)(buf->content));

    xmlBufferFree (buf);

    return nicehtml;
}


/**
 * virgule_normalize_html: Converts any HTML/XML like tag soup into 
 * reasonably legal HTML. XSS threats and illegal tags are stripped.
 * Since many functions utilizing the return value segfault on NULL
 * values, it's important to return an empty string in case of failure!
 *
 **/

#if !defined(HTML_PARSE_RECOVER)
    #define HTML_PARSE_RECOVER 0
#endif
#if !defined(HTML_PARSE_COMPACT)
    #define HTML_PARSE_COMPACT 0
#endif

char *
virgule_normalize_html (VirguleReq *vr, const char *raw, const char *baseurl)
{
    htmlDocPtr hdoc = NULL;
    xmlDtdPtr dtd = NULL;
    xmlNodePtr root_n, cur_n;
    char *nicehtml = NULL;
    int size = strlen(raw);
    char *empty = "";

    if(!raw || size == 0)
	return empty;

    /* parse HTML in the least strict mode possible */
    hdoc = htmlReadMemory (raw, size, NULL, "utf-8",
		HTML_PARSE_RECOVER | HTML_PARSE_NOERROR |
		HTML_PARSE_NOWARNING | HTML_PARSE_NOBLANKS |
		HTML_PARSE_NONET | HTML_PARSE_COMPACT);

    if(hdoc == NULL)
	return empty;

    /* get pointer to the <html> node */    
    root_n = xmlDocGetRootElement (hdoc);
    if(root_n == NULL || root_n->children == NULL) 
	return empty;

    /* set cur_n to the <body> node */
    for (cur_n = root_n->children; cur_n != NULL; cur_n = cur_n->next)
	if((cur_n->type == XML_ELEMENT_NODE) && (strcmp((char *)cur_n->name,"body") == 0))
	    break;

    if(cur_n == NULL)
	return empty;

    /* Create the XHTML DTD and assign to the root node */
    dtd = xmlNewDtd (hdoc, (xmlChar *)"html", (xmlChar *)"-//W3C/DTD XHTML 1.0 Transitional//EN", (xmlChar *)"http://www.w3.org/TR/xhtml1/DTD/xhtml1-transitional.dtd");
    xmlDocSetRootElement (hdoc, (xmlNodePtr)dtd);

    nicehtml = virgule_normalize_html_tree (vr, cur_n, baseurl);

    xmlFreeDoc (hdoc);
    return (nicehtml == NULL ? empty : nicehtml);
}


/**
 * virgule_time_t_to_iso: Converts a Unix time_t value to string in an ISO
 * format ("YYYY-MM-DD hh:mm:ss"). Returned time is UTC (GMT) time zone.
 * For a time_t value of 0 or -1, the current time is returned.
 *
 * ToDo: It would probably be wise to use either the full ISO 8661 or 
 * RFC-3339 formats. This would require conversion of all the existing 
 * dates in the XML database, however.
 **/
char *
virgule_time_t_to_iso (VirguleReq *vr, time_t t)
{
  apr_time_t time = 0;

  if(t < 1)
    time = apr_time_now();
  else 
    apr_time_ansi_put (&time, t);
  
  return ap_ht_time (vr->r->pool, time, "%Y-%m-%d %H:%M:%S", 1);
}


/**
 * iso_now: Current date/time in iso format.
 * Return value: UTC (GMT) Time in "YY-MM-DD hh:mm:ss" format.
 **/
char *
virgule_iso_now (apr_pool_t *p)
{
  return ap_ht_time (p, (apr_time_t) (time (NULL)) * 1000000,
                     "%Y-%m-%d %H:%M:%S", 1);
}


/**
 * virgule_rfc822_to_time_t: Translates an RFC-822 encoded time string into
 * a Unix time_t value. We rely on the APR function apr_date_parse_rfc to
 * do the dirtywork. It claims to parse true RFC-822 strings as well as
 * nine common variants that are known to occur in the wild. 
 **/
time_t
virgule_rfc822_to_time_t (VirguleReq *vr, const char *time_string)
{
  time_t t;
  struct tm tm;
  apr_time_t at;
  
  if(time_string == NULL)
    return -1;

  at = apr_date_parse_rfc (time_string);

  t = apr_time_sec(at);
  
  /* If date is so hosed up the APR lib can't figure out... */
  if(t <= 0)
    {
      /* check for RFC822 with 4 digit year, no seconds, assume UTC */
      memset(&tm, 0, sizeof(struct tm));
      strptime(time_string, "%a, %d %b %Y %R %z", &tm);
      t = timegm(&tm);      
    }
  
  return t;
}



/**
 * virgule_rfc3339_to_time_t: Translates an RFC-3339 encoded time string
 * into a Unix time_t value. RFC-3339 is equivalent to the ISO 8601 time
 * encoding format. This format is used in Atom (RFC-4278) XML files.
 *
 * Both mktime and timegm ignore the timezone in the tm struct, so we have
 * to adjust if needed, based on the offset in the encoded string.
 **/
time_t
virgule_rfc3339_to_time_t (VirguleReq *vr, const char *time_string)
{
  const char *c;
  int mm = 0;
  int hh = 0;
  int n = 0;
  time_t t;
  struct tm tm;

  if(time_string == NULL)
    return -1;

  memset(&tm, 0, sizeof(struct tm));
  strptime(time_string, "%FT%T%z", &tm);
  t = timegm(&tm);

  /* Explicit UTC designation */
  c = time_string + strlen(time_string);
  if (*c == 'Z' || *c == 'z')
    return t;

  /* Explicit UTC offset designation */
  c = time_string + strlen(time_string) - 6;
  if (*c == '-' || *c == '+')
    {
      if (*c == '-')
        n = -1;
      else
        n = 1;
      hh = atoi (c+1);
      mm = atoi (c+4);
    }

  return t - (((mm * 60) + (hh * 60 * 60)) * n);
}


/**
 * virgule_virgule_to_time_t: Translates an old-style mod_virgule encoded
 * time string into a Unix time_t value. This could probably replace the
 * older function, virgule_iso_to_time_t(). Unlike the older function,
 * this would should return a valid time_t value under all conditions. It
 * is assumed that all times in the XML data stored are UTC (GMT).
 **/
time_t
virgule_virgule_to_time_t (VirguleReq *vr, const char *time_string)
{
  time_t t;
  struct tm tm;

  if(time_string == NULL)
    return -1;

  memset(&tm, 0, sizeof(struct tm));
  strptime(time_string, "%F %T", &tm);
  t = timegm(&tm);

  return t;
}


/**
 * iso_to_time_t: Compute Unix time from ISO date string.
 * @iso: String in ISO format (usually from iso_now).
 *
 * Return value: Unix time, roughly number of seconds since 1970-01-01.
 *
 * Steve's notes:
 * This code returns the approximate time but will not return the exact
 * time under all conditions. If an exact match is needed when converting
 * a time string from the XML data store, the virgule_virgule_to_time_t()
 * function should be used instead. This function is deprecated and will
 * be totally removed eventually.
 **/
time_t
virgule_iso_to_time_t (const char *iso)
{
  int year, month, day;
  int monthday[] = { 0, 31, 61, 92, 122, 153,
		     184, 214, 245, 275, 306, 337 }; /* march-relative */
  time_t result;

  if (iso == NULL || strlen (iso) < 10 || iso[4] != '-' || iso[7] != '-')
    return 0;
  year = atoi (iso);
  month = atoi (iso + 5);
  day = atoi (iso + 8);
  if (month < 1 || month > 12) return 0;
  if (month < 3)
    {
      month += 12;
      year -= 1;
    }
  day += year * 365 + year/4 - year/100 + year/400;
  day += monthday[month - 3];
  result = (day - 719469) * 86400;
  if (strlen (iso) >= 19 &&
      iso[10] == ' ' && iso[13] == ':' && iso[16] == ':')
    {
      int hr, min, sec;
      hr = atoi (iso + 11);
      min = atoi (iso + 14);
      sec = atoi (iso + 17);
      result += (hr * 60 + min) * 60 + sec;
    }
  return result;
}


/**
 * str_subst: Simple search-and-replace string substitution.
 * @p: The pool.
 * @str: Original string.
 * @pattern: Pattern to search for.
 * @repl: Replacement string.
 *
 * Replaces all occurrences of @pattern in @str with @repl.
 *
 * Return value: The resulting string.
 **/
char *
virgule_str_subst (apr_pool_t *p, const char *str, const char *pattern, const char *repl)
{
  int size, idx;
  int i, j;
  int repl_len;
  char *result;

  if (pattern[0] == 0)
    return NULL;

  repl_len = strlen (repl);

  size = 0;
  for (i = 0; str[i]; i++)
    {
      for (j = 0; pattern[j]; j++)
	if (str[i + j] != pattern[j])
	  break;
      if (pattern[j] == 0)
	{
	  size += repl_len;
	  i += j - 1;
	}
      else
	size++;
    }
  result = apr_palloc (p, size + 1);

  idx = 0;
  for (i = 0; str[i]; i++)
    {
      for (j = 0; pattern[j]; j++)
	if (str[i + j] != pattern[j])
	  break;
      if (pattern[j] == 0)
	{
	  memcpy (result + idx, repl, repl_len);
	  idx += repl_len;
	  i += j - 1;
	}
      else
	result[idx++] = str[i];
    }
  result[idx] = 0;
  return result;
}

/**
 * escape_uri_arg: Escape string in form suitable for URI argument.
 * @p: The pool.
 * @str: The original string.
 *
 * The same as ap_os_escape_path except that & ' + are also escaped.
 *
 * Return value: the escaped string.
 **/
char *
virgule_escape_uri_arg (apr_pool_t *p, const char *str)
{
  char *tmp = ap_os_escape_path (p, str, 1);
  tmp = virgule_str_subst (p, tmp, "&", "%26");
  tmp = virgule_str_subst (p, tmp, "'", "%27");
  return virgule_str_subst (p, tmp, "+", "%2b");
}

static int
escape_attr_helper (const char *raw, char *buf)
{
  int i;
  int j;

  j = 0;
  for (i = 0; raw[i]; i++)
    {
      char c = raw[i];

      if (c == '&')
	nice_text_cat (buf, &j, "&amp;", 5);
      else if (c == '<')
	nice_text_cat (buf, &j, "&lt;", 4);
      else if (c == '>')
	nice_text_cat (buf, &j, "&gt;", 4);
      else if (c == '"')
	nice_text_cat (buf, &j, "&quot;", 4);
      else
	{
	  if (buf) buf[j] = c;
	  j++;
	}
    }
  return j;
}

/**
 * escape_html_attr: Escape string intended for use as HTML attribute value.
 * @p: The pool.
 * @raw: The original raw string.
 *
 * The same as ap_os_escape_path except that + and ' are also escaped.
 *
 * Return value: the escaped string.
 **/
char *
virgule_escape_html_attr (apr_pool_t *p, const char *raw)
{
  char *result;
  int size;

  if (raw == NULL)
    return NULL;
  size = escape_attr_helper (raw, NULL);
  result = apr_palloc (p, size + 1);
  escape_attr_helper (raw, result);
  result[size] = '\0';
  return result;
}


/**
 * render_url: Render URL with a href link.
 * @p: The pool.
 * @url: The URL
 *
 * Handles URL's without initial http://. The result should be safe
 * for any argument.
 *
 * Return value: the link.
 **/
char *
virgule_render_url (apr_pool_t *p, const char *prefix, const char *url)
{
  const char *url2;
  char *colon;

  url2 = url;
  colon = strchr (url, ':');
  if (!colon || colon[1] != '/' || colon[2] != '/')
    url2 = apr_pstrcat (p, "http://", url, NULL);
  return apr_psprintf (p, "<p>%s<a href=\"%s\">%s</a> </p>\n",
		     prefix,
		     ap_escape_html (p, url2), virgule_nice_text (p, url));
//		     ap_os_escape_path (p, url2, 1), virgule_nice_text (p, url));
}


/**
 * is_legal_XML: Validates character as a legal XML character 
 * based on the W3C XML 1.0 Recommendation:
 * http://www.w3.org/TR/1998/REC-xml-19980210
 *
 * Return value: TRUE if legal XML char, FALSE otherwise
 **/
static int
is_legal_XML(unsigned char *c)
{
  if(*c <= 0x08) return FALSE;
  if(*c == 0x0b) return FALSE;
  if(*c == 0x0c) return FALSE;
  if(*c >= 0x0e && *c <= 0x1f) return FALSE;
  return TRUE;
}


/**
 * is_legal_UTF8:  Validation of a byte sequence as a legal
 * UTF-8 sequence. This function based on code by Unicode, Inc.
 *
 * Return value: TRUE if sequence is valid UTF-8, FALSE otherwise
 **/
static int
is_legal_UTF8(unsigned char *source, char length)
{
  unsigned char a;
  unsigned char *srcptr = source+length;
  switch (length) {
    default: return FALSE;
    /* Everything else falls through when "true"... */
    case 4: if ((a = (*--srcptr)) < 0x80 || a > 0xBF) return FALSE;
    case 3: if ((a = (*--srcptr)) < 0x80 || a > 0xBF) return FALSE;
    case 2: if ((a = (*--srcptr)) > 0xBF) return FALSE;
            switch (*source) {
              /* no fall-through in this inner switch */
              case 0xE0: if (a < 0xA0) return FALSE; break;
              case 0xF0: if (a < 0x90) return FALSE; break;
              case 0xF4: if (a > 0x8F) return FALSE; break;
              default:   if (a < 0x80) return FALSE;
            }
    case 1: if (*source >= 0x80 && *source < 0xC2) return FALSE;
            if (*source > 0xF4) return FALSE;
  }
  return TRUE;
}


/**
 * is_input_valid: Checks a null-terminated char string to see if there
 * are any invalid UTF-8 byte sequences or illegal XML characters.
 *
 * Returns: TRUE if string is valid, FALSE otherwise
 **/
int
virgule_is_input_valid(const char *val)
{
  unsigned char *c;
  c = (unsigned char *)val;

return TRUE;
  
  while(c) {
    if(!is_legal_XML(c)) return FALSE;
    if(!is_legal_UTF8(c,UTF8length[*c])) return FALSE;
    c+= UTF8length[*c];
  }
  return TRUE;
}


/**
 * virgule_sha1 - return an SHA-1 hash of the input data in the form of
 * a string of 20 hexadecimal values (40 characters)
 */
char *
virgule_sha1(apr_pool_t *p, const char *input)
{
  int i;
  apr_sha1_ctx_t context;
  apr_byte_t digest[APR_SHA1_DIGESTSIZE];
  char *result = apr_pcalloc (p, 2 * APR_SHA1_DIGESTSIZE + 1);

  if(input == NULL)
    return NULL;

  apr_sha1_init (&context);
  apr_sha1_update (&context, input, strlen(input));
  apr_sha1_final (digest, &context);
  
  for (i = 0; i < APR_SHA1_DIGESTSIZE; i++)
    sprintf (result + (i*2), "%02x", digest[i]);

  return result;
}


/**
 * virgule_decode_textarea: Prepare textarea data for saving
 * @p: APR memory pool pointer.
 * @raw: The original raw string.
 *
 * This attempts to handle common line feed issues with text areas by
 * preserving certain combinations of line feeds as <br>s while removing
 * other combinations of line feeds.
 *
 * In theory, what we want to do is:
 * convert \r\n to <br/> when saving editor data as HTML
 * convert <br/> to \n when loading HTML into the editor
 *
 * Return value: textarea data ready for saving.
 **/
char *
virgule_decode_textarea (apr_pool_t *p, const char *raw)
{
    char *out = NULL;

    if(raw == NULL)
	return NULL;
    
    out = virgule_str_subst(p, (char *)raw, ">\r\n<","><");
    out = virgule_str_subst(p, out, "\r\n<p>","<p>");
    out = virgule_str_subst(p, out, "\r\n</p>","</p>");
    out = virgule_str_subst(p, out, "<p>\r\n","<p>");
    out = virgule_str_subst(p, out, "</p>\r\n","</p>");
    out = virgule_str_subst(p, out, "\r\n", "<br/>\n");
    
    return out;
}


/**
 * virgule_encode_textarea: Prepare textarea data for rendering
 * @p: APR memory pool pointer.
 * @raw: The original raw string.
 *
 * This attempts to handle common line feed issues with text areas by
 * inserting linefeeds that will make the display more readable.
 *
 * In theory, what we want to do is:
 * convert \r\n to <br/> when saving editor data as HTML
 * convert <br/> to \n when loading HTML into the editor
 *
 * Return value: textarea data ready for display.
 **/
char *
virgule_encode_textarea (apr_pool_t *p, const char *raw)
{
    char *out = NULL;
    
    if(raw == NULL)
	return NULL;
    
    out = virgule_str_subst(p, (char *)raw, "<br>\n", "\r\n");
    out = virgule_str_subst(p, out, "<br/>\n", "\r\n");
    
    return out;
}


/**
 * virgule_strsub - Return a newly allocated copy of str with any 
 * occurances of string o(ld) replaced with string n(ew).
 * duh - I wrote this and then realized Raph had already written
 * essentially the same thing. For now I'm using his but maybe it
 * would make sense to see if one or the other of these is more
 * efficient or faster?
 */
/*
char *
virgule_strsub(apr_pool_t *pool, const char *str, const char *o, const char *n)
{
    int cnt = 0;
    int strl = strlen(str);
    int ol = strlen(o);
    int nl = strlen(n);
    int bl = 0;
    int i = 0;

    char *out = NULL;
    char *op = NULL;
    char *p = NULL;
    char *end = (char *)str + strl;
*/
    /* count occurances of old in string */
/*    for (p = (char *)str; p < end; p++)
	if (strncmp (p, o, ol) == 0)
	    cnt++;
*/
    /* calculate buffer size and allocate */
/*    bl = strl - (ol * cnt) + (nl * cnt) + 1;
    out = apr_palloc (pool, bl);
*/        
    /* make replacements during copy */
/*    for (p = (char *)str, op = out; p < end;)
    {
	if (strncmp (p, o, ol) == 0)
	{
	    for (i = 0; i < nl; i++)
		*(op+i) = *(n+i);
	    p+=ol;
	    op+=nl;
	}
	else *op++ = *p++;
    }
    *op = 0;
        
    return out;
}
*/

