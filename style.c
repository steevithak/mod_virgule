#include <time.h>
#include <stdarg.h>

#include <apr.h>
#include <apr_strings.h>
#include <httpd.h>

#include <libxml/tree.h>
#include <libxml/parser.h>

#include "private.h"
#include "buffer.h"
#include "db.h"
#include "req.h"
#include "site.h"
#include "util.h"

#include "style.h"


/**
 * dayofweek: Return the day of the week
 * @d: Day = 1 to 31
 * @m: Month = 1 to 12
 * @y: Year = YYYY
 *
 * Based on Mike Keith's alleged most compact method of calculating the day
 * of the week from an arbitrary day, month, and year as described in the
 * Journal of Recreational Mathematics, Vol. 22, No 4, 1990, p. 280
 * Return value: 0 - 6
 **/
static int
dayofweek(int d, int m, int y)
{
    return (d+=m<3?y--:y-2,23*m/9+d+4+y/4-y/100+y/400)%7;
}


void
virgule_render_header_raw (VirguleReq *vr, const char *title, const char *head_content)
{
  Buffer *b = vr->b;

  vr->r->content_type = "text/html; charset=UTF-8";

  virgule_buffer_printf (b, "<!DOCTYPE HTML PUBLIC \"-//W3C//DTD HTML 4.01 Transitional//EN\">\n"
                 "<html>\n<head><title>%s</title>\n", title);
                 
  virgule_buffer_printf (b, "<link rel=\"stylesheet\" type=\"text/css\" "
                 "href=\"%s/css/global.css\">\n"
		 "<link rel=\"shortcut icon\" href=\"/images/favicon.ico\" />"
                 "<style type=\"text/css\"><!-- "
                 "@import \"%s/css/notns4.css\"; --></style>\n",
                 vr->prefix, vr->prefix);

  virgule_buffer_printf (b,"<script language=\"JavaScript\" type=\"text/javascript\">\n"
	         "<!-- \n"
		 "if (top != self) { top.location = self.location } \n"
		 "//-->\n"
		 "</script>\n");

  if (head_content)
    virgule_buffer_puts (b, head_content);

  virgule_buffer_puts (b, "</head>\n\n<body>\n");

  vr->raw = 1;
}

void
virgule_render_header (VirguleReq *vr, const char *title, const char *head_content)
{
  Buffer *b = vr->b;
  
  virgule_render_header_raw (vr, title, head_content);

// begin nasty robots.net kluge - this will have no effect unless the site's
// domain name contains robots.net.
// vr->priv->site_name
if (!strncmp (vr->priv->site_name, "robots.net",10)) {
  virgule_buffer_puts (b, "<table bgcolor=\"#406690\" border=\"0\" cellpadding=\"4\" cellspacing=\"0\">\n");
  virgule_buffer_puts (b, "<tr><td><a href=\"http://robots.net/\"><img src=\"/images/logo160.png\" width=\"160\" height=\"49\" border=\"0\">");
  virgule_buffer_puts (b, "</a></td><td width=\"100%\" align=\"center\">");
  virgule_site_render_banner_ad(vr);
  virgule_buffer_puts (b, "</td></tr><tr><td colspan=\"2\" align=\"right\" class=\"sitemap\">");
  virgule_render_sitemap(vr,0);
  virgule_buffer_puts (b, "</td></tr></table>");
  virgule_buffer_puts (b, "<div class=\"main\" style=\"margin: 2em;\">");
}
else {
  virgule_buffer_puts (b, "<div class=\"main\" style=\"margin: 2em;\">");
  virgule_buffer_printf (b, "<h1>%s</h1>", title);
}
// end nasty robots.net kluge

  vr->raw = 0;
}


struct _NavOption {
  char *label;
  char *url;
};


/*
 * The enclose option is for backward compatibility with advogato.org only
 */
void
virgule_render_sitemap (VirguleReq *vr, int enclose)
{
  const NavOption **option;
  char *separator = " | ";

  if (vr->sitemap_rendered)
    return;
    
  if (enclose)
    virgule_buffer_puts (vr->b, "<p align=center>");

  virgule_buffer_puts (vr->b, "<span class=\"sitemap\">&nbsp;[ ");
  for (option = vr->priv->nav_options; *option; option++)
    {
      if(!*(option+1)) { separator = ""; }
      if(strcmp(vr->uri,(*option)->url))
        virgule_buffer_printf (vr->b, "<a href=\"%s\">%s</a>%s",
                               (*option)->url,(*option)->label,separator);
      else
        virgule_buffer_printf (vr->b, "%s%s",(*option)->label,separator);
    }
  virgule_buffer_puts (vr->b, " ]&nbsp;</span>");

  if (enclose)
    virgule_buffer_puts (vr->b, "</p>");

  vr->sitemap_rendered = 1;
}

/*
 * Note that if render_sitemap has previously been called on this page, the
 * call in render_footer_send will have no effect. Only one sitemap may be
 * rendered on each page.
 */
int
virgule_render_footer_send (VirguleReq *vr)
{
  if (!vr->raw)
    virgule_buffer_puts (vr->b, "</div>\n");

  virgule_render_sitemap (vr, 1);  /* needed for advogato.org compatibility */  
  virgule_buffer_puts (vr->b, "</body>\n</html>\n");
  return virgule_send_response (vr);
}

int
virgule_send_error_page (VirguleReq *vr, const char *error_short,
		 const char *fmt, ...)
{
  Buffer *b = vr->b;
  va_list ap;

  virgule_render_header (vr, error_short, NULL);
  virgule_buffer_puts (b, "<p> ");
  va_start (ap, fmt);
  virgule_buffer_puts (b, apr_pvsprintf (vr->r->pool, fmt, ap));
  va_end (ap);
  virgule_buffer_puts (b, "</p>\n");
  return virgule_render_footer_send (vr);
}

/**
 * render_date: Render date nicely.
 * @vr: The #VirguleReq context.
 * @iso: The date in ISO format (YYYY-MM-DD)
 * @showtime: 0 = date only, 1 = date + time, 2 = date + time (RFC 822)
 *
 * Return value: Nicely formatted date string.
 **/
char *
virgule_render_date (VirguleReq *vr, const char *iso, int showtime)
{
  int year, month, day;
  char *hhmm;
  const char *months[] = {
    "Nilember",
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
  };
  const char *days[] = {
    "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
  };

  if (strlen (iso) < 10 || iso[4] != '-' || iso[7] != '-')
    return "--error--";
  year = atoi (iso);
  month = atoi (iso + 5);
  day = atoi (iso + 8);
  if (showtime == 2)
    {
      hhmm = apr_pstrndup (vr->r->pool, iso + 11, 5);
      return apr_psprintf (vr->r->pool, "%s, %d %s %d %s %s", 
                          days[dayofweek(day,month,year)],
			  day, months[month], year, hhmm, "GMT");
    }
  if (showtime == 1)
    {
      hhmm = apr_pstrndup (vr->r->pool, iso + 11, 5);
      return apr_psprintf (vr->r->pool, "%d %s %d at %s %s", 
                          day, months[month], year, hhmm, "UTC");
    }
  return apr_psprintf (vr->r->pool, "%d %s %d", day, months[month], year);
}

