#include <ctype.h>
#include <time.h>

#include <apr.h>
#include <apr_strings.h>
#include <apr_file_io.h>
#include <apr_date.h>
#include <httpd.h>

#include <libxml/entities.h>
#include <libxml/HTMLparser.h>

#include "private.h"
#include "buffer.h"
#include "db.h"
#include "req.h"
#include "wiki.h"
#include "util.h"

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
 * virgule_nice_UTF8: Convert raw UTF8 into nice HTML.
 * @raw: Raw UTF8
 *
 * Return value: HTML formatted text
 **/
char *
virgule_nice_utf8 (apr_pool_t *p, const unsigned char *raw)
{
  int inlen = xmlStrlen(raw);
  int outlen = inlen * 4;
  char *out = NULL;
   
  if (raw == NULL)
     return NULL;

  out = apr_palloc (p, outlen + 1);
  if(out == NULL)
    return NULL;
//  memset(out,0,outlen);
//  if(UTF8ToHtml (out,&outlen,raw,&inlen) == 0)
  if(htmlEncodeEntities ((xmlChar *)out,&outlen,raw,&inlen,0) == 0)
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

static char *
nice_person_link (VirguleReq *vr, const char *name)
{
  apr_pool_t *p = vr->r->pool;
  int i;

  for (i = 0; name[i]; i++)
    if (!isalnum (name[i]))
      return apr_psprintf (p, "&lt;person&gt;%s&lt;/person&gt;", name);
  return apr_psprintf (p, "<a href=\"%s/person/%s/\">%s</a>",
		      vr->prefix, ap_os_escape_path(p, name, 1), name);
}

static char *
nice_proj_link (VirguleReq *vr, const char *proj)
{
  apr_pool_t *p = vr->r->pool;

  if (strchr (proj, '/') || proj[0] == '.' || strlen (proj) > 30)
    return apr_psprintf (p, "&lt;proj&gt;%s&lt;/proj&gt;", proj);
  return apr_psprintf (p, "<a href=\"%s/proj/%s/\">%s</a>",
                      vr->prefix, ap_os_escape_path (p, proj, 1), proj);
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
  char *(*handler) (VirguleReq *vr, const char *str);
};

static AllowedTag special_allowed_tags[] = {
  { "person", 0, nice_person_link },
  { "proj", 0, nice_proj_link },
  { "project", 0, nice_proj_link },
  { "wiki", 0, virgule_wiki_link }
};


/**
 * add_allowed_tag - Allocates an AllowedTag structures during loading
 * of the site configuration. This information must survive across
 * multiple requests so it uses the thread private pool.
 */
const AllowedTag *
virgule_add_allowed_tag (VirguleReq *vr, const char *tagname, int can_be_empty)
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
 * match_tag: Match the html source against the tag.
 * @src: Pointer to html source, right after the '<'.
 * @tag: Tag name, in lower case.
 *
 * Return value: pointer to next token after tag if matched, NULL
 * otherwise.
 **/
static const char *
match_tag (const char *src, const char *tag)
{
  int i;
  char c;

  for (i = 0; isalpha (c = src[i]); i++)
    if (tolower (c) != tag[i])
      return NULL;
  if (tag[i] != 0)
    return NULL;
  while (isspace (src[i])) i++;
  return src + i;
}

static const char *
find_end_tag (const char *str, const char *tag, const char **after)
{
  int i;

  for (i = 0; str[i]; i++)
    {
      if (str[i] == '<')
	{
	  const char *ptr;

	  if (str[i + 1] != '/')
	    return NULL;

	  /* Allow </> close tag syntax. */
	  if (str[i + 2] == '>')
	    ptr = str + i + 2;
	  else
	    ptr = match_tag (str + i + 2, tag);

	  if (ptr == NULL) return NULL;
	  if (ptr[0] != '>')
	    return NULL;
	  *after = ptr + 1;
	  return str + i;
	}
    }
  return NULL;
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
        }
      if(strncasecmp(tmp1,"</a>",4)==0)
        tmp1+=4;
      *tmp2 = *tmp1;
      tmp1++;
      tmp2++;
    }
  *tmp2=0;

  return clean;
}


/**
 * nice_htext: Convert raw html'ish text into nice HTML.
 * @raw: Raw text.
 * @p_error: Where error message is to be stored.
 *
 * Return value: HTML formatted text.
 **/
char *
virgule_nice_htext (VirguleReq *vr, const char *raw, char **p_error)
{
  apr_pool_t *p = vr->r->pool;
  Buffer *b = virgule_buffer_new (p);
  apr_array_header_t *tag_stack;
  int i, end;
  char c;
  int nl_state = 0;
  int in_quote = 0;

  *p_error = NULL;
#if 0
  /* revert to old nicetext behavior */
  return virgule_nice_text (p, raw);
#endif
  tag_stack = apr_array_make (p, 16, sizeof (char *));
  for (i = 0; raw[i]; i = end)
    {
      for (end = i;
	   (c = raw[end]) &&
	     c != '\n' && c != '&' && c != '<' && c != '>' && !(c & 0x80);
	   end++);
      if (end > i + 1 || raw[i] != '\r')
	{
	  if (nl_state == 2)
	    virgule_buffer_puts (b, "<p> ");
	  nl_state = 3;
	}
      if (end > i)
	virgule_buffer_write (b, raw + i, end - i);
      i = end;
      if (c == '&')
	{
	  end++;
	  if (raw[end] == '#')
	    {
	      /* numeric entity */
	      if (isdigit (raw[end + 1]))
		{
		  /* decimal character reference */
		  end += 2;
		  while (isdigit (raw[end])) end++;
		}
#if 0
	      /* apparently, the &#x123; syntax is not valid HTML,
		 even though it is XML. */
	      else if (raw[end + 1] == 'x')
		{
		  /* hexadecimal character reference */
		  if (isxdigit (raw[end + 2]))
		    {
		      end += 3;
		      while (isxdigit (raw[end])) end++;
		    }
		}
#endif
	    }
	  else
	    {
	      /* entity reference */
	      while (isalpha (raw[end])) end++;
	    }
	  if (end > i + 1 && raw[end] == ';')
	    {	
	      end++;
	      virgule_buffer_write (b, raw + i, end - i);
	      continue;
	    }
	  end = i + 1;
	  virgule_buffer_puts (b, "&amp;");
	}
      else if (c == '<')
	{
	  const AllowedTag **tag;
	  const char *tail = NULL; /* to avoid uninitialized warning */
	  end++;
	  if (raw[end] == '/')
	    {
	      char *tos;
	      int tos_idx;

	      /* just skip closing tag for empty elements */
	      for (tag = vr->priv->allowed_tags; *tag; tag++)
		if ((*tag)->empty &&
		    (tail = match_tag (raw + end + 1, (*tag)->tagname)) != NULL)
		    break;

	      if (*tag)
		{
		  end = tail - raw + 1;
		  continue;
		}

	      /* pop tag stack if closing tag matches */
	      tos_idx = tag_stack->nelts - 1;
	      if (tos_idx >= 0)
		{
		  tos = ((char **)(tag_stack->elts))[tos_idx];

		  /* Allow </> syntax to close tags. */
		  if (raw[end + 1] == '>')
		    tail = raw + end + 1;
		  else
		    tail = match_tag (raw + end + 1, tos);

		  if (tail != NULL && *tail == '>')
		    {
		      virgule_buffer_printf (b, "</%s>", tos);
		      tag_stack->nelts--;
		      end = tail - raw + 1;
		      continue;
		    }
		}
	      while(*tag)
		tag++;
	    }
	  else
	    {
	      for (tag = vr->priv->allowed_tags; *tag; tag++)
		{
		  tail = match_tag (raw + end, (*tag)->tagname);
		  if (tail != NULL)
		    break;
		}
	    }
	  if (*tag)
	    {
	      /* todo: handle quotes */
	      while ((c = raw[end]) && c != '>')
		{
		  if (raw[end] == '"')
		    in_quote = !in_quote;
		  end++;
		}
	      if (c == '>')
		{

		  end++;
		  if ((*tag)->handler != NULL)
		    {
		      char *body;
		      int body_size;
		      const char *body_end;
		      const char *after;

		      body_end = find_end_tag (raw + end,
					       (*tag)->tagname,
					       &after);
		      if (body_end != NULL)
			{
			  body_size = body_end - (raw + end);
			  body = apr_palloc (p, body_size + 1);
			  memcpy (body, raw + end, body_size);
			  body[body_size] = 0;
#if 1
			  virgule_buffer_puts (b, (*tag)->handler (vr, body));
#else
			  virgule_buffer_printf (b, "[body = %s, %d]", body, body_size);
#endif
			  end = after - raw;
			  continue;
			}

		      else
			{
#if 0
			  virgule_buffer_printf (b, "[body_end = NULL]");
#endif
			}
		    }
		  else
		    {
		      if (in_quote)
			{
			  virgule_buffer_write (b, raw + i, end - i - 2);
			  virgule_buffer_puts (b, "\">");
			  *p_error = "Unterminated quote in tag";
			}
		      else
			virgule_buffer_write (b, raw + i, end - i);
		      if (!(*tag)->empty)
			{
			  char **p_stack;
			  
			  p_stack = (char **)apr_array_push (tag_stack);
			  *p_stack = (*tag)->tagname;
			}
		      continue;
		    }
		}
	      end = i + 1;
	    }
	  /* tag not matched, escape the html */
	  virgule_buffer_puts (b, "&lt;");
	}
      else if (c == '>')
	{
	  end++;
	  virgule_buffer_puts (b, "&gt;");
	}
      else if (c == '\n')
	{
	  end++;
	  virgule_buffer_puts (b, "\n");

	  if (nl_state == 3)
	    nl_state = 1;
	  else if (nl_state == 1)
	    nl_state = 2;
	}
      else if (c != 0)
	{
	  const char *replacement;
	  end++;
	  replacement = escape_noniso_char (c);
	  if (replacement == NULL)
	    virgule_buffer_write (b, raw + i, end - i);
	  else
	    virgule_buffer_puts (b, replacement);
	}
    }

  /* close all open tags */
  while (tag_stack->nelts)
    {
      int tos_idx;
      char *tos;

      tos_idx = --tag_stack->nelts;
      tos = ((char **)(tag_stack->elts))[tos_idx];
      virgule_buffer_printf (b, "</%s>", tos);
      if (*p_error == NULL)
	*p_error = apr_psprintf (p, "Unclosed tag %s", tos);
    }

  return virgule_buffer_extract (b);
}

/**
 * virgule_time_t_to_iso: Converts a Unix time_t value to string in an ISO
 * format ("YYYY-MM-DD hh:mm:ss"). Returned time will be in the local time
 * zone. For a time_t value of 0 or -1, the current time is returned.
 *
 * ToDo: It would probably be wise to add a timezone and use either the
 * full ISO 8661 or RFC-3339 formats. This would require conversion of all
 * the existing dates in the XML database, however.
 **/
char *
virgule_time_t_to_iso (VirguleReq *vr, time_t t)
{
  apr_time_t time = 0;

  if(t < 1)
    time = apr_time_now();
  else 
    apr_time_ansi_put (&time, t);
  
  return ap_ht_time (vr->r->pool, time, "%Y-%m-%d %H:%M:%S", 0);
}


/**
 * iso_now: Current date/time in iso format.
 * Return value: Time in "YY-MM-DD hh:mm:ss" format.
 *
 * Note: we should at least be adding a timezone.
 * 
 * Steve's notes:
 * Need to investigate whether or not ap_ht_time compensates for the local
 * time zone.
 **/
char *
virgule_iso_now (apr_pool_t *p)
{
  return ap_ht_time (p, (apr_time_t) (time (NULL)) * 1000000,
                     "%Y-%m-%d %H:%M:%S", 0);
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
  apr_time_t at;
  
  if(time_string == NULL)
    return -1;

  at = apr_date_parse_rfc (time_string);

  return apr_time_sec(at);
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
 * iso_to_time_t: Compute Unix time from ISO date string.
 * @iso: String in ISO format (usually from iso_now).
 *
 * Return value: Unix time, roughly number of seconds since 1970-01-01.
 *
 * Note: we should recognize and parse timezone code also.
 *
 * Steve's notes:
 * This code is currently broken in that it assumes the ISO time zone is UTC.
 * The resulting time_t value will be off by the amount of local time offset.
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
		     ap_os_escape_path (p, url2, 1), virgule_nice_text (p, url));
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
 * in_input_valid: Checks a null-terminated char string to see if there
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
