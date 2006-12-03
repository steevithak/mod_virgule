#include <stdarg.h>
#include <stdio.h>
#define __USE_GNU
#include <string.h>
#undef __USE_GNU

#include "httpd.h"
#include "http_protocol.h"
#include "util_md5.h"

#include "buffer.h"

/* An abstraction for an appendable buffer. This should be reasonably
efficient in both time and memory usage for most purposes. */

typedef struct _BufferChunk BufferChunk;

struct _BufferChunk {
  BufferChunk *next;
  char *buf;
  int size;
  int size_max;
};

struct _Buffer {
  pool *p;
  int total_size;
  BufferChunk *first_chunk;
  BufferChunk *last_chunk;
  AP_MD5_CTX md5_ctx;
  const char **trans;
};

Buffer *
buffer_new (pool *p)
{
  Buffer *result = ap_palloc (p, sizeof(Buffer));
  BufferChunk *chunk = ap_palloc (p, sizeof(BufferChunk));

  result->p = p;
  result->total_size = 0;
  result->first_chunk = chunk;
  result->last_chunk = chunk;
  ap_MD5Init (&result->md5_ctx);
  result->trans = NULL;

  chunk->next = NULL;
  chunk->size = 0;
  chunk->size_max = 256;
  chunk->buf = ap_palloc (p, chunk->size_max);

  return result;
}

void
buffer_set_translations (Buffer *b, const char **translations)
{
  b->trans = translations;
}

static void
real_buffer_write (Buffer *b, const char *data, int size)
{
  BufferChunk *last = b->last_chunk;
  int copy_size;
  BufferChunk *new;
  pool *p;

  ap_MD5Update (&b->md5_ctx, data, size);

  b->total_size += size;

  copy_size = size;
  if (copy_size + last->size > last->size_max)
    copy_size = last->size_max - last->size;

  memcpy (last->buf + last->size, data, copy_size);
  last->size += copy_size;
  if (copy_size == size)
    return;

  /* Allocate a new chunk. */
  p = b->p;
  new = ap_palloc (p, sizeof(BufferChunk));
  /* append new chunk to linked list */
  new->next = NULL;
  last->next = new;
  b->last_chunk = new;

  new->size = size - copy_size;
  new->size_max = 256;
  while (new->size_max < new->size)
    new->size_max <<= 1;
  new->buf = ap_palloc (p, new->size_max);
  memcpy (new->buf, data + copy_size, size - copy_size);
}

static void
trans_buffer_write (Buffer *b, const char *data, int size)
{
  const char **t;

  for (t = b->trans; *t; t+=2) {
    if (strlen (t[0]) != size)
      continue;
    if (memcmp (data, t[0], size))
      continue;
    real_buffer_write (b, t[1], strlen (t[1]));
    return;
  }
  real_buffer_write (b, data, size);
}

void
buffer_write (Buffer *b, const char *data, int size)
{
  if (!b->trans) {
    real_buffer_write (b, data, size);
    return;
  }

  while (size > 0) {
    const char *start, *end;
    int len;

    start = memmem (data, size, "<x>", 3);
    if (!start) {
      real_buffer_write (b, data, size);
      break;
    }

    len = start - data;
    real_buffer_write (b, data, len);
    len += 3;
    size -= len;
    data += len;

    end = memmem (data, size, "</x>", 4);
    if (!end) {
      real_buffer_write (b, " &iexcl;X_ERROR! ", 17);
      real_buffer_write (b, data, size);
      break;
    }

    len = end - data;
    trans_buffer_write (b, data, len);
    len += 4;
    size -= len;
    data += len;
  }
}

void
buffer_printf (Buffer *b, const char *fmt, ...)
{
  /* It might be worth fiddling with this so that it doesn't copy
     quite so much. */

  char *str;
  va_list ap;
  va_start (ap, fmt);
  str = ap_pvsprintf (b->p, fmt, ap);
  va_end (ap);
  buffer_write (b, str, strlen (str));
}

void
buffer_puts (Buffer *b, const char *str)
{
  buffer_write (b, str, strlen (str));
}

void
buffer_append (Buffer *b, const char *str1, ...)
{
  va_list args;
  char *s;

  buffer_puts (b, str1);
  va_start (args, str1);
  while ((s = va_arg (args, char *)) != NULL)
    buffer_puts (b, s);
  va_end (args);
}

/* Send http header and buffer */
int
buffer_send_response (request_rec *r, Buffer *b)
{
  BufferChunk *chunk;
  char *md5, *etag;
  int ret;

  md5 = ap_md5contextTo64 (r->pool, &b->md5_ctx);
  etag = ap_psprintf (r->pool, "\"%s\"", md5);
  ap_table_setn (r->headers_out, "ETag", etag);
  ret = ap_meets_conditions(r);
  if (ret != OK)
      return ret;  

  ap_table_setn (r->headers_out, "Content-MD5", md5);
  ap_set_content_length (r, b->total_size);

  ap_send_http_header (r);
  if (!r->header_only)
    for (chunk = b->first_chunk; chunk != NULL; chunk = chunk->next)
      ap_rwrite (chunk->buf, chunk->size, r);

  return OK;
}

/**
 * buffer_extract: Extract buffer contents to null-terminated string.
 * @b: The buffer.
 *
 * Return value: A null-terminated string corresponding to the buffer.
 **/
char *
buffer_extract (Buffer *b)
{
  char *result;
  BufferChunk *chunk;
  int idx;

  idx = 0;
  result = ap_palloc (b->p, b->total_size + 1);
  for (chunk = b->first_chunk; chunk != NULL; chunk = chunk->next)
    {
      memcpy (result + idx, chunk->buf, chunk->size);
      idx += chunk->size;
    }
  result[idx] = 0;
  return result;
}