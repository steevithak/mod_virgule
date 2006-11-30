/**
 * private.h
 * 
 * Data structure used for mod_virgule thread private data
 */
 
/*
 Copyright (c) 2005 by R. Steven Rainwater <steve@ncc.com>
 
 This program is free software; you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation; either version 2 of the License, or
 (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

*/

typedef struct _Topic Topic;
typedef struct _NavOption NavOption;
typedef struct _AllowedTag AllowedTag;
typedef struct virgule_private virgule_private_t;

struct virgule_private {
  apr_pool_t        *pool;         /* Thread private memory pool */
  apr_time_t	     mtime;        /* Time of last CFG modification */
  unsigned long      count;        /* Thread request counter */
  const char        *site_name;
  const char        *base_uri;
  const char	    *admin_email;
  int                recentlog_as_posted;
  const char       **cert_level_names;
  const char       **seeds;
  const int         *caps;
  const char       **special_users;
  const char       **trans;
  int                render_diaryratings;
  int                allow_account_creation;
  int		     allow_account_extendedcharset;
  int		     use_article_title_links;
  int		     use_article_topics;
  int		     article_post_by_seeds_only;
  int	             level_articlepost;
  int		     level_articlereply;
  int		     level_projectcreate;
  const Topic	   **topics;
  const NavOption  **nav_options;
  const AllowedTag **allowed_tags;
  enum {
    PROJSTYLE_RAPH,
    PROJSTYLE_NICK,
    PROJSTYLE_STEVE
  }               projstyle;
};

