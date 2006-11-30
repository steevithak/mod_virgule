#include <stdarg.h>
#include "httpd.h"

#include "buffer.h"
#include "db.h"
#include "req.h"

#include "style.h"

#define STYLE

void
render_header_raw (VirguleReq *vr, const char *title)
{
  Buffer *b = vr->b;

  vr->r->content_type = "text/html; charset=ISO-8859-1";

  buffer_printf (b, "<!DOCTYPE HTML PUBLIC \"-//W3C//DTD HTML 4.01 Transitional//EN\">\n"
                 "<html>\n<head><title>%s</title>\n", title);
                 
#ifdef STYLE
  buffer_printf (b, "<link rel=\"stylesheet\" type=\"text/css\" "
                 "href=\"%s/css/global.css\">\n"
                 "<style type=\"text/css\"><!-- "
                 "@import \"%s/css/notns4.css\"; --></style>\n",
                 vr->prefix, vr->prefix);
#endif

  buffer_puts (b, "</head>\n\n");

#ifdef STYLE
  buffer_puts (b, "<body>\n");
#endif
#ifndef STYLE
  buffer_puts (b, "<body bgcolor=white>\n"
               "<font face=\"lucida,helvetica,sans-serif\">\n");
#endif

  vr->raw = 1;
}

void
render_header (VirguleReq *vr, const char *title)
{
  Buffer *b = vr->b;
  
  render_header_raw (vr, title);
  buffer_printf (b, "<blockquote>\n"
		 "<h1>%s</h1>\n", title);
  vr->raw = 0;
}

static void
render_site_link (VirguleReq *vr, const char *url, const char *text)
{
  Buffer *b = vr->b;

  /* todo: fix up links when prefix != '' */
  if (!strcmp (url, vr->uri))
    buffer_puts (b, text);
  else
    buffer_printf (b, "<a href=\"%s%s\">%s</a>", vr->prefix, url, text);
}

void
render_sitemap (VirguleReq *vr, int enclose)
{
  Buffer *b = vr->b;

  if (vr->sitemap_rendered)
    return;
  if (enclose)
    buffer_puts (b, "<p align=center>");
  buffer_puts (b, "[ ");
  render_site_link (vr, "/", "Home");
  buffer_puts (b, "  |\n");
  render_site_link (vr, "/article/", "<x>Articles</x>");
  buffer_puts (b, "  |\n");
  render_site_link (vr, "/acct/", "Account");
  buffer_puts (b, "  |\n");
  render_site_link (vr, "/person/", "<x>People</x>");
  buffer_puts (b, "  |\n");
  render_site_link (vr, "/proj/", "<x>Projects</x>");
  buffer_puts (b, " ]\n");
  if (enclose)
    buffer_puts (b, "</p>");
  vr->sitemap_rendered = 1;
}

void
render_footer (VirguleReq *vr)
{
  Buffer *b = vr->b;

  if (!vr->raw)
    buffer_puts (b, "</blockquote>\n");
  render_sitemap (vr, 1);
  buffer_puts (b,
#ifndef STYLE
	       "</font>\n"
#endif
	       "</body>\n"
	       "</html>\n");
}

int
render_table_open (VirguleReq *vr)
{
#ifndef STYLE
  buffer_puts (vr->b, "<font face=\"lucida,helvetica,sans-serif\">\n");
#endif
  return 0;
}

int
render_table_close (VirguleReq *vr)
{
#ifndef STYLE
  buffer_puts (vr->b, "</font>\n");
#endif
  return 0;
}

int
render_footer_send (VirguleReq *vr)
{
  render_footer (vr);
  return send_response (vr);
}

int
send_error_page (VirguleReq *vr, const char *error_short,
		 const char *fmt, ...)
{
  Buffer *b = vr->b;
  va_list ap;

  render_header (vr, error_short);
  buffer_puts (b, "<p> ");
  va_start (ap, fmt);
  buffer_puts (b, ap_pvsprintf (vr->r->pool, fmt, ap));
  va_end (ap);
  buffer_puts (b, " </p>\n");
  return render_footer_send (vr);
}

/**
 * render_date: Render date nicely.
 * @vr: The #VirguleReq context.
 * @iso: The date in ISO format (YYYY-MM-DD)
 *
 * Currently just renders date as "11 Nov 1999", but this maybe 
 * Return value: Nicely formatted date string.
 **/
char *
render_date (VirguleReq *vr, const char *iso)
{
  int year, month, day;
  const char *months[] = {
    "Nilember",
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
  };

  if (strlen (iso) < 10 || iso[4] != '-' || iso[7] != '-')
    return "--error--";
  year = atoi (iso);
  month = atoi (iso + 5);
  day = atoi (iso + 8);
  return ap_psprintf (vr->r->pool, "%d %s %d", day, months[month], year);
}

