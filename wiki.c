/* Rendering of InterWiki links. */
#include <ctype.h>

#include <apr.h>
#include <apr_strings.h>
#include <httpd.h>

#include "private.h"
#include "buffer.h"
#include "db.h"
#include "req.h"
#include "wiki.h"

/* Note: the intermap file lives at http://usemod.com/intermap.txt
   and should be installed in the database under data/intermap.txt */
static const char *
wiki_get_intermap (VirguleReq *vr)
{
  const char *result;
  int intermap_size;

  result = apr_table_get (vr->render_data, "wiki_intermap");
  if (result != NULL)
    return result;

  result = virgule_db_get (vr->db, "data/intermap.txt", &intermap_size);
  if (result == NULL)
    result = "";

  apr_table_set (vr->render_data, "wiki_intermap", result);

  return result;
}

static char *
wiki_lookup_intermap (VirguleReq *vr, const char *wikiname)
{
  apr_pool_t *p = vr->r->pool;
  const char *intermap = wiki_get_intermap (vr);
  int i, j;
  int end;
  int next;
  char *result;

  for (i = 0; intermap[i]; i = next)
    {
      for (end = i; intermap[end] && intermap[end] != '\n'; end++);
      next = end;
      if (intermap[next] == '\n') next++;
      for (j = 0; wikiname[j] && i + j < end; j++)
	if (wikiname[j] != intermap[i + j])
	  break;
      if (!wikiname[j] && i + j < end && intermap[i + j] == ' ')
	{
	  int val_ix = i + j + 1;
	  result = apr_palloc (p, end - val_ix + 1);
	  memcpy (result, intermap + val_ix, end - val_ix);
	  result[end - val_ix] = '\0';
	  return result;
	}
    }
  return NULL;
}

char *
virgule_wiki_link (VirguleReq *vr, const char *link)
{
  apr_pool_t *p = vr->r->pool;
  int i;
  char c;
  int colon = -1;
  const char *tail;
  char *wikiname;
  char *wiki_loc;

  for (i = 0; (c = link[i]) != 0; i++)
    {
      if (c == ':' && colon == -1)
	colon = i;
      else if (!isalnum (c) && c != ':' && c != '/' &&
	       c != '.' && c != '?' && c != '=')
	return apr_psprintf (p, "&lt;wiki&gt;%s&lt;/wiki&gt;", link);
    }
  tail = link + colon + 1;
  if (colon < 0)
    wikiname = "Wiki";
  else
    {
      wikiname = apr_palloc (p, colon + 1);
      memcpy (wikiname, link, colon);
      wikiname[colon] = '\0';
    }
  wiki_loc = wiki_lookup_intermap (vr, wikiname);
  if (wiki_loc == NULL)
    return apr_psprintf (p, "&lt;wiki&gt;%s<a href=\"http://usemod.com/intermap.txt\">?</a>:%s&lt;/wiki&gt;",
			wikiname, tail);
  return apr_psprintf (p, "<a href=\"%s%s\">%s</a>",
		      wiki_loc, tail, link);
}

