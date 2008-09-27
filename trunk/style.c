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
#include "db_xml.h"
#include "xml_util.h"
#include "req.h"
#include "certs.h"
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
virgule_render_header (VirguleReq *vr, const char *title)
{
  Buffer *b = vr->b;

  vr->r->content_type = "text/html; charset=UTF-8";

  virgule_buffer_printf (b, "<!DOCTYPE HTML PUBLIC \"-//W3C//DTD HTML 4.01 Transitional//EN\">\n"
                 "<html>\n<head><title>%s</title>\n", title ? title : "");
                 
  virgule_buffer_printf (b, "<link rel=\"stylesheet\" type=\"text/css\" "
                 "href=\"%s/css/global.css\" />\n"
		 "<link rel=\"shortcut icon\" href=\"%s/images/favicon.ico\" />\n",
                 vr->prefix, vr->prefix);

  virgule_buffer_printf (b,"<script language=\"javascript\" type=\"text/javascript\" src=\"%s/css/v.js\"></script>\n",vr->prefix);

  /* check for additional page header contents */
  if (virgule_buffer_size (vr->hb) > 0)
    virgule_buffer_puts (b, virgule_buffer_extract (vr->hb));

  virgule_buffer_puts (b, "</head>\n\n<body>\n");
}


struct _NavOption {
  char *label;
  char *url;
};


/**
 * virgule_render_userstats - renders a table of user statistics
 **/
void
virgule_render_userstats (VirguleReq *vr)
{
  xmlDocPtr stats;
  xmlNodePtr stat;

  stats = virgule_db_xml_get (vr->r->pool, vr->db, "userstats.xml");
  if (stats != NULL)
    {
      virgule_buffer_puts (vr->b, "<table>\n");
      for (stat = stats->xmlRootNode->children; stat != NULL; stat = stat->next)
      {
        if (stat->type != XML_ELEMENT_NODE)
	  continue;
	virgule_buffer_printf (vr->b, "<tr><td>%s</td><td>%s</td></tr>\n", 
	                       (char *)stat->name,
			       virgule_xml_get_string_contents (stat));
      }
      virgule_buffer_puts (vr->b, "</table>\n");
    }
}


/*
 * The enclose option is for backward compatibility with advogato.org only.
 * It can be removed once all pages are template-based.
 */
void
virgule_render_sitemap (VirguleReq *vr, int enclose)
{
  const NavOption **option;
  char *separator = " | ";

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
}

/*
 * virgule_render_footer_send - tack on the necessary page close HTML tags
 * and send the buffer out to the client.
 */
int
virgule_render_footer_send (VirguleReq *vr)
{
  if(vr->priv->google_analytics)
    virgule_buffer_printf (vr->b, "\n<script src=\"http://www.google-analytics.com/urchin.js\" type=\"text/javascript\">\n"
                                  "</script>\n<script type=\"text/javascript\">\n"
				  "_uacct = \"%s\";\nurchinTracker();\n</script>\n",
				  vr->priv->google_analytics);
  virgule_buffer_puts (vr->b, "</body>\n</html>\n");
  return virgule_send_response (vr);
}


/**
 * virgule_send_error_page: Render an error page using the default template.
 * If buffer allocation problems occur, declare an internal error and abort.
 * Error types:
 * 0 Error
 * 1 Info
 * ToDo: We could generate an Apache error log entry before aborting.
 */
int
virgule_send_error_page (VirguleReq *vr, int type, const char *error_short, const char *fmt, ...)
{
  va_list ap;
  char *emsg[] = { "Error", "Info" };
  char *title = apr_psprintf (vr->r->pool, "%s: %s", emsg[type], error_short);

  if (vr->r->status == 404)
    vr->r->status_line = apr_pstrdup (vr->r->pool, "404 Not Found");

  if (virgule_set_temp_buffer (vr) != 0)
    return HTTP_INTERNAL_SERVER_ERROR;

  va_start (ap, fmt);
  virgule_buffer_puts (vr->b, apr_pvsprintf (vr->r->pool, fmt, ap));
  va_end (ap);

  virgule_set_main_buffer (vr);

  return virgule_render_in_template (vr, "/templates/default.xml", "content", title);
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
  if (showtime == 3)
    {
      return apr_psprintf (vr->r->pool, "%d %s %02d", year, months[month], day);
    }
  if (showtime == 2)
    {
      hhmm = apr_pstrndup (vr->r->pool, iso + 11, 8);
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


/**
 * virgule_set_temp_buffer: Allocate a temp buffer, set the translations,
 * and swap it out with the main request buffer. Once swapped, all buffer
 * printing function within mod_virgule will write to the temp buffer. This
 * is normally done to pre-render a portion of content for insertion into a
 * template. The main buffer must be made active again before mod_virgule
 * can render the final page, by calling virgule_set_main_buffer().
 *
 * @vr: The #VirguleReq context.
 *
 * Return value: -1 on error, 0 on success
 **/
int 
virgule_set_temp_buffer (VirguleReq *vr)
{
  Buffer *tmp = NULL;
  
  tmp = virgule_buffer_new (vr->r->pool);
  if (tmp == NULL)
    return -1;

  virgule_buffer_set_translations (tmp, vr->priv->trans);

  vr->tb = vr->b;
  vr->b = tmp;

  return 0;
}


/**
 * virgule_set_main_buffer: Swap the main buffer and temp buffer pointers.
 * @vr: The #VirguleReq context.
 *
 **/
void
virgule_set_main_buffer (VirguleReq *vr)
{
  Buffer *tmp = vr->b;
  vr->b = vr->tb;
  vr->tb = tmp;
}


/**
 * virgule_render_in_template: Load the specified template and replaced the
 * specified XML tag with the contents of the VirguleReq temp buffer. If an
 * optional page title has been provided, it will be used on the rendered
 * page.
 * @vr: The #VirguleReq context.
 *
 **/
int
virgule_render_in_template (VirguleReq *vr, char *tpath, char *tagname, char *title)
{
  char *istr = NULL;
  xmlDocPtr tdoc;
  xmlNodePtr troot;

  if (tpath == NULL)
    return virgule_send_error_page (vr, vERROR, "internal", "virgule_render_in_template() failed: tpath or tagname were invalid");
  
  /* extract the contents of the temp buffer as a string */
  if (vr->tb != NULL)
    istr = virgule_buffer_extract (vr->tb);

  /* load the template */
  tdoc = virgule_db_xml_get (vr->r->pool, vr->db, tpath);
  if (tdoc == NULL)
    return virgule_send_error_page (vr, vERROR, "internal", "virgule_db_xml_get() failed, unable to load template");

  troot = xmlDocGetRootElement (tdoc);

  return virgule_site_render_page (vr, troot, tagname, istr, title);        
}
