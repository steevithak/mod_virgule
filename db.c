/* This is a simple database. This implementation is very simple, just
   a thin layer over the file system. However, the api should support
   a number of other db implementation techniques. */

/* The treatment of "/" within keys is special. If the key "foo" is
   ever used, then the key "foo/bar" may not be used. Thus, keys
   correspond to a hierarchical filesystem in which prefixes (relative
   to "/") may be used as a directory or a leaf key, but not both. */

#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include <apr.h>
#include <apr_strings.h>
#include <httpd.h>

#include "db.h"

struct _Db {
  apr_pool_t *p;
  char *base_pathname;
};

struct _DbCursor {
  Db *db;
  char *dir_pathname;
  DIR *dir;
};

struct _DbLock {
  apr_pool_t *p;
  apr_file_t *fd;
};

Db *
virgule_db_new_filesystem (apr_pool_t *p, const char *base_pathname)
{
  Db *result = (Db *)apr_palloc (p, sizeof (Db));

  result->p = p;
  result->base_pathname = apr_pstrdup (p, base_pathname);

  return result;
}

static const char *
db_mangle_key_component (apr_pool_t *p, const char *comp)
{
  int n, log100;
  int tmp;
  const char *buf;
  char *tail;

  if (comp[0] != '_' || !isdigit (comp[1]))
    return comp;
  n = strtol (comp + 1, &tail, 10);
  log100 = 0;
  for (tmp = n; tmp >= 100; tmp /= 100)
    log100++;
  if (log100 == 0)
    return apr_psprintf (p, "_%02d%s", n, tail);
  buf = apr_psprintf (p, "%02d", n % 100);
  n /= 100;
  for (; n; n /= 100)
      buf = apr_psprintf (p, "%02d/%s", n % 100, buf);
  return apr_psprintf (p, "_%c/%s%s", 'a' + log100 - 1, buf, tail);
}

/**
 * db_mk_filename: Make a filename from a database key.
 * @p: Pool for allocation.
 * @db: The database.
 * @key: The key.
 *
 * Makes a filename from the database key. Basically, this routine
 * does munching of numeric keys according to the following algorithm:
 *
 * For each component (ie, split '/') of the key that matches ^_\d+,
 * interpret the number as decimal n. If it is <100, then it becomes
 * "_%02d", n. Elsif it is <10 000, then it becomes "_a/%02d/%02d/", n
 * / 100, n % 100. Elsif it is <1 000 000, it becomes
 * "_b/%02d/%02d/%02d", etc.
 *
 **/
char *
virgule_db_mk_filename (apr_pool_t *p, Db *db, const char *key)
{
  const char *component;
  const char *buf;
  int first = 1;

  /*
  if (key[0] == '/') key++;

  return ap_make_full_path (p, db->base_pathname, key);
  */
  buf = "";
  for (;;)
    {
      component = ap_getword (p, &key, '/');
      if (component[0] == 0)
	{
	  if (key[0] == 0)
	    break;
	  else
	    continue;
	}

      if (first)
	buf = db_mangle_key_component (p, component);
      else
	buf = apr_pstrcat (p, buf, "/", db_mangle_key_component (p, component),
			  NULL);
      first = 0;
    }

  return ap_make_full_path (p, db->base_pathname, buf);
}

/**
 * db_get_p: Get a record from the database, explicit pool.
 * @p: Pool for allocations.
 * @db: The database.
 * @key: The key.
 * @p_size: Where to store the size of the record.
 *
 * Gets the record named by @key from the database.
 *
 * Return value: The contents of the record, or NULL if not found.
 **/
char *
virgule_db_get_p (apr_pool_t *p, Db *db, const char *key, int *p_size)
{
  char *fn;
  apr_file_t *fd;
  apr_finfo_t finfo;
  apr_status_t status;
  apr_off_t file_size;
  char *result;
  apr_size_t bytes_read;

  if (!key)
    return NULL;
    
  fn = virgule_db_mk_filename (p, db, key);

  status = apr_stat(&finfo, fn, APR_FINFO_MIN, p);
  if (status != APR_SUCCESS)
    return NULL;

  /* Must be regular file; so we're _not_ handling symlinks. */
  if (finfo.filetype != APR_REG)
    return NULL;

  file_size = finfo.size;

  if (apr_file_open(&fd, fn, APR_READ,
	APR_UREAD|APR_UWRITE|APR_GREAD|APR_GWRITE|APR_WREAD, p) != APR_SUCCESS)
    return NULL;

  result = (char *)apr_palloc (p, file_size + 1);
  /* todo: make read resistant to E_INTR. */
  bytes_read = file_size;

  if (apr_file_lock(fd, APR_FLOCK_SHARED) != APR_SUCCESS)
    return NULL;

  apr_file_read(fd, result, &bytes_read);
  apr_file_close(fd);  /* close implicitly unlocks */

  if (bytes_read != file_size)
    return NULL;

  /* Null terminate, in case result is used as string. */
  result[file_size] = 0;

  *p_size = file_size;

  return result;
}

/**
 * db_get: Get a record from the database.
 * @db: The database.
 * @key: The key.
 * @p_size: Where to store the size of the record.
 *
 * Gets the record named by @key from the database.
 *
 * Return value: The contents of the record, or NULL if not found.
 **/
char *
virgule_db_get (Db *db, const char *key, int *p_size)
{
  return virgule_db_get_p (db->p, db, key, p_size);
}

/* Ensure that the directory exists, return 1 on success. */
static int
db_ensure_dir (Db *db, const char *fn)
{
  apr_pool_t *p = db->p;
  struct stat stat_buf;
  int status;
  char *parent_dir;

  /* Check for existence of parent dir. */
  parent_dir = ap_make_dirstr_parent (p, fn);

  status = stat (parent_dir, &stat_buf);
  if (status < 0)
    {
      /* Ok, need to do some directory creating. */
      int n_dirs = ap_count_dirs (parent_dir);
      char *prefix;
      int i;

      prefix = (char *)apr_palloc (p, strlen (parent_dir) + 1);
      for (i = 1; i <= n_dirs; i++)
	{
	  ap_make_dirstr_prefix (prefix, parent_dir, i);
	  status = stat (prefix, &stat_buf);
	  if (status < 0)
	    break;
	  else
	    {
	      if (!S_ISDIR (stat_buf.st_mode))
		return 0;
	    }
	}
      for (; i <= n_dirs; i++)
	{
	  ap_make_dirstr_prefix (prefix, parent_dir, i);
	  if (mkdir (prefix, 0775))
	    return 0;
	}
    }
  else
    {
      if (!S_ISDIR (stat_buf.st_mode))
	return 0;
    }

  return 1;
}

/**
 * db_put_p: Put a record in the database, explicit pool.
 * @p: Pool for allocations.
 * @db: The database.
 * @key: The key.
 * @val: The value to put.
 * @size: The size of @val.
 *
 * Puts a key in the database. Creates any directories, as needed.
 *
 * Return value: 0 on success.
 **/
int
virgule_db_put_p (apr_pool_t *p, Db *db, const char *key, const char *val, int size)
{
  char *fn;
  apr_file_t *fd;
  apr_size_t bytes_written;

  fn = virgule_db_mk_filename (p, db, key);

  if (!db_ensure_dir (db, fn))
    return -1;

  if (apr_file_open(&fd, fn, APR_READ|APR_WRITE|APR_CREATE|APR_TRUNCATE,
	APR_UREAD|APR_UWRITE|APR_GREAD|APR_GWRITE|APR_WREAD, p) != APR_SUCCESS)
    return -1;

  if (apr_file_lock(fd, APR_FLOCK_EXCLUSIVE) != APR_SUCCESS)
    return -1;

  /* todo: make write resistant to E_INTR. */
  bytes_written = size;
  apr_file_write(fd, val, &bytes_written);
  apr_file_close(fd); /* close implicitly unlocks */

  if (bytes_written != size)
    return -1;

  return 0;
}


/**
 * db_put: Put a record in the database.
 * @db: The database.
 * @key: The key.
 * @val: The value to put.
 * @size: The size of @val.
 *
 * Puts a key in the database. Creates any directories, as needed.
 *
 * Return value: 0 on success.
 **/
int
virgule_db_put (Db *db, const char *key, const char *val, int size)
{
  return virgule_db_put_p (db->p, db, key, val, size);
}

/**
 * db_del: Delete a record from the database.
 * @db: The database.
 * @key: The key.
 *
 * Deletes key from the database. If the directory containing the key is
 # empty after deletion of the key, the directory is also removed. 
 *
 * Return value: 0 on success.
 **/
int
virgule_db_del (Db *db, const char *key)
{
  int status;
  char *path,*fn,*n;

  fn = virgule_db_mk_filename (db->p, db, key);

  status = apr_file_remove(fn, db->p);

  path = apr_pstrdup (db->p, fn);
  n = strrchr(path,'/');
  if(n != NULL) {
    *n = 0;
    apr_dir_remove(path, db->p);
  }
  
  return status;
}


/**
 * db_is_dir: Determine whether a key is a directory.
 * @db: The database.
 * @key: The key.
 *
 * Determines whether a key is a directory.
 *
 * Return value: true if the key is a directory.
 **/
int
virgule_db_is_dir (Db *db, const char *key)
{
  char *fn;
  struct stat stat_buf;
  int status;

  fn = virgule_db_mk_filename (db->p, db, key);

  /* Check for existence of parent dir. */
  status = stat (fn, &stat_buf);
  if (status < 0)
    return 0;
  return S_ISDIR (stat_buf.st_mode);
}

/**
 * db_open_dir: Open a directory cursor.
 * @db: The database.
 * @key: The key of the directory.
 *
 * Note: a limitation of the current implementation is that numeric
 * keys are not properly reported, in other words they come back in
 * the file system format, not the "database key" format. It's not too
 * hard to fix, but right now we don't seem to need it.
 *
 * Return value: The cursor.
 **/
DbCursor *
virgule_db_open_dir (Db *db, const char *key)
{
  DIR *dir;
  DbCursor *result;
  char *fn;

  fn = virgule_db_mk_filename (db->p, db, key);
  dir = opendir (fn);
  if (dir == NULL)
    return NULL;

  result = (DbCursor *)apr_palloc (db->p, sizeof (DbCursor));

  result->db = db;
  result->dir_pathname = apr_pstrdup (db->p, key);
  result->dir = dir;

  return result;
}

/**
 * db_read_dir: Traverse a directory cursor.
 * @dbc: The database cursor.
 *
 * Finds the next child of the directory cursor.
 *
 * Return value: The database key of the child, or NULL if no more.
 **/
char *
virgule_db_read_dir (DbCursor *dbc)
{
  struct dirent *de;

  de = readdir (dbc->dir);
  if (de == NULL)
    return NULL;
  return ap_make_full_path (dbc->db->p, dbc->dir_pathname, de->d_name);
}

/**
 * db_read_dir_raw: Traverse a directory cursor.
 * @dbc: The database cursor.
 *
 * Finds the next child of the directory cursor.
 *
 * Return value: The relative database key of the child, or NULL if no more.
 **/
char *
virgule_db_read_dir_raw (DbCursor *dbc)
{
  struct dirent *de;

  do
    {
      de = readdir (dbc->dir);
      if (de == NULL)
	return NULL;
    }
  while (de->d_name[0] == '.');
  return apr_pstrdup (dbc->db->p, de->d_name);
}

int
virgule_db_close_dir (DbCursor *dbc)
{
  return closedir (dbc->dir);
}

/**
 * db_dir_max_in_level: Find maximum key in filesystem level.
 * @db: The database.
 * @dir: The open directory.
 * @top: TRUE if toplevel.
 *
 * Find the maximum key. If @top, then look for _\d\d, or _[a-d]. In
 * the latter case, return -2..-5, respectively. If not @top, then look
 * for \d\d. On error or no integer keys, return -1.
 *
 * Return value: maximum key.
 **/
static int
db_dir_max_in_level (const char *fn, int top)
{
  DIR *dir;
  struct dirent *de;
  int result = -1;
  int val;
  
  dir = opendir (fn);
  if (dir == NULL)
    return -1;
  while (1)
    {
      de = readdir (dir);
      if (de == NULL)
	break;
      if (top && de->d_name[0] == '_')
	{
	  if (de->d_name[1] >= 'a' && de->d_name[1] <= 'd')
	    {
	      val = -2 - (de->d_name[1] - 'a');
	      if (val < result)
		result = val;
	    }
	  else if (isdigit (de->d_name[1]) &&
		   isdigit (de->d_name[2]))
	    {
	      val = (de->d_name[1] - '0') * 10 + de->d_name[2] - '0';
	      if (result >= -1 && val > result)
		result = val;
	    }
	}
      else if (!top && isdigit (de->d_name[0]) && isdigit (de->d_name[1]))
	{
	  val = (de->d_name[0] - '0') * 10 + de->d_name[1] - '0';
	  if (val > result)
	    result = val;
	}
    }
  closedir (dir);
  return result;
}

/** 
 * db_dir_max: Find maximum integer key in directory.
 * @db: The database.
 * @key: The key of the directory.
 *
 * Return value: the maximum integer key, or -1 if none.
 **/
int
virgule_db_dir_max (Db *db, const char *key)
{
  apr_pool_t *p = db->p;
  char *fn;
  int result;
  int level_max;
  int n_levels;
  int i;

  fn = virgule_db_mk_filename (p, db, key);

  level_max = db_dir_max_in_level (fn, 1);
  if (level_max >= -1)
    return level_max;
  n_levels = -level_max;
  fn = apr_psprintf (p, "%s/_%c", fn, n_levels + 'a' - 2);
  result = 0;
  for (i = 0; i < n_levels; i++)
    {
      level_max = db_dir_max_in_level (fn, 0);
      if (level_max < 0)
	return -1; /* error! */
      result = (result * 100) + level_max;
      fn = apr_psprintf (p, "%s/%02d", fn, level_max);
    }
  return result;
}

