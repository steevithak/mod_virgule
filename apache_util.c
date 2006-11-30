#include <apr.h>
#include <httpd.h>
#include <http_protocol.h>
#include <http_main.h>

#include "apache_util.h"

static char *
read_post_data (request_rec *r)
{
  int rc;
  char *result;

  rc = ap_setup_client_block (r, REQUEST_CHUNKED_ERROR);
  if (rc != OK)
    return NULL;
  if (ap_should_client_block (r))
    {
      char buf[HUGE_STRING_LEN];
      int rsize, bytes_read, rpos=0;
      long length = r->remaining;
      result = apr_pcalloc (r->pool, length + 1);

      while (rpos < length)
	{
	  if (rpos + sizeof(buf) > length)
	    rsize = length - rpos;
	  else
	    rsize = sizeof(buf);
	  bytes_read = ap_get_client_block (r, buf, rsize);
	  if (bytes_read <= 0)
	    break;
	  memcpy (result + rpos, buf, bytes_read);
	  rpos += bytes_read;
	}
      return result;
    }
  return NULL;
}

/**
 * get_args: Get the uri args.
 * @r: The request record.
 *
 * Gets the uri args, whether by GET or POST method.
 *
 * Return value: The args, or NULL if error.
 **/
char *
virgule_get_args (request_rec *r)
{
  if (r->method_number == M_GET)
    return r->args;
  else if (r->method_number == M_POST)
    return read_post_data (r);
  else
    return NULL;
}

/**
 * unescape_url_info: Unescape URI arguments.
 * @s: String to unescape.
 *
 * Does the same as ap_unescape_url, but also converts '+' to ' ', so
 * it's most appropriate for unescaping url arguments. Leaves the
 * unescaped string in-place in @s.
 **/
void
virgule_unescape_url_info (char *s)
{
  int i, len;

  len = strlen (s);
  for (i = 0; i < len; i++)
    if (s[i] == '+')
      s[i] = ' ';
  ap_unescape_url (s);
}

