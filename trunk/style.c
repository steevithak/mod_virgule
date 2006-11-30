#include <stdarg.h>
#include "httpd.h"

#include "buffer.h"
#include "db.h"
#include "req.h"
#include "site.h"
#include "util.h"

#include "style.h"

void
render_header_raw (VirguleReq *vr, const char *title, const char *head_content)
{
  Buffer *b = vr->b;

  vr->r->content_type = "text/html; charset=UTF-8";

  buffer_printf (b, "<!DOCTYPE HTML PUBLIC \"-//W3C//DTD HTML 4.01 Transitional//EN\">\n"
                 "<html>\n<head><title>%s</title>\n", title);
                 
#ifdef STYLE
  buffer_printf (b, "<link rel=\"stylesheet\" type=\"text/css\" "
                 "href=\"%s/css/global.css\">\n"
		 "<link rel=\"shortcut icon\" href=\"/images/favicon.ico\" />"
                 "<style type=\"text/css\"><!-- "
                 "@import \"%s/css/notns4.css\"; --></style>\n",
                 vr->prefix, vr->prefix);

//  if(!strcmp (title, "robots.net"))
//    buffer_printf (b,"<meta name=\"keywords\" content=\"robot, robots, robotic, robotics, UAV, ROV, AUV, FAQ, cybernetics, android, cyborg, exoskeleton, robot competition, AI\">\n"
    buffer_printf (b,"<script language=\"JavaScript\" type=\"text/javascript\">\n"
		     "<!-- \n"
		     "if (top != self) { top.location = self.location } \n"
		     "//-->\n"
		     "</script>\n");

#endif

  if (head_content)
    buffer_puts (b, head_content);

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
render_header (VirguleReq *vr, const char *title, const char *head_content)
{
  Buffer *b = vr->b;
  
  render_header_raw (vr, title, head_content);
// RSR test code begin
  buffer_puts (b, "<table bgcolor=\"#406690\" border=\"0\" cellpadding=\"4\" cellspacing=\"0\">\n");
  buffer_puts (b, "<tr><td><a href=\"http://robots.net/\"><img src=\"http://img.robots.net/images/logo160.png\" width=\"160\" height=\"49\" border=\"0\">");
  buffer_puts (b, "</a></td><td width=\"100%\" align=\"center\">");
  site_render_banner_ad(vr);
  buffer_puts (b, "</td></tr><tr><td colspan=\"2\" align=\"right\" class=\"sitemap\">");
  render_sitemap(vr,0);
  buffer_puts (b, "</td></tr></table><div class=\"main\" style=\"margin: 2em;\">");
// RSR test code end
  vr->raw = 0;
}


struct _NavOption {
  char *label;
  char *url;
};

void
render_sitemap (VirguleReq *vr, int enclose)
{
  const NavOption **option;
  char *separator = " | ";

  if (vr->sitemap_rendered)
    return;
    
//  if (enclose)
//    buffer_puts (vr->b, "<p align=center>");
#ifdef STYLE
  buffer_puts (vr->b, "<span class=\"sitemap\">&nbsp;[ ");
#endif

  for (option = vr->nav_options; *option; option++)
    {
      if(!*(option+1)) { separator = ""; }
      buffer_printf (vr->b, "<a href=\"%s\">%s</a>%s",
                     (*option)->url,
		     (*option)->label,
		     separator);
    }

#ifdef STYLE
  buffer_puts (vr->b, " ]&nbsp;</span>");
#endif    
//  if (enclose)
//    buffer_puts (vr->b, "</p>");
  vr->sitemap_rendered = 1;
}

void
render_footer (VirguleReq *vr)
{
  Buffer *b = vr->b;

  if (!vr->raw)
    buffer_puts (b, "</div>\n");

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

  render_header (vr, error_short, NULL);
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
render_date (VirguleReq *vr, const char *iso, int showtime)
{
  int year, month, day;
  char *hhmm, *zone;
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
  if (showtime)
    {
      hhmm = ap_pstrndup (vr->r->pool, iso + 11, 5);
      zone = ap_ht_time (vr->r->pool, time (NULL), "%Z", 0);
      return ap_psprintf (vr->r->pool, "%d %s %d at %s %s", 
                          day, months[month], year, hhmm, zone);
    }
  return ap_psprintf (vr->r->pool, "%d %s %d", day, months[month], year);
}
