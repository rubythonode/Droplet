/*
 * Copyright (C) 2010 SCALITY SA. All rights reserved.
 * http://www.scality.com
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 * Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 *
 * Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY SCALITY SA ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL SCALITY SA OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * The views and conclusions contained in the software and documentation
 * are those of the authors and should not be interpreted as representing
 * official policies, either expressed or implied, of SCALITY SA.
 *
 * https://github.com/scality/Droplet
 */
#include "dropletp.h"

//#define DPRINTF(fmt,...) fprintf(stderr, fmt, ##__VA_ARGS__)
#define DPRINTF(fmt,...)

//#define DEBUG

/**
 * this function doesn't modify str
 *
 * @return -1 on failure, 0 if OK
 */
static int
http_parse_reply(char *str,
                 struct dpl_http_reply *reply)
{
  char	*p, *p2;
  char ver_buf[9];
  int ver_len;
  char code_buf[4];
  int code_len;

  if (!(p = index(str, ' ')))
    return -1;

  ver_len = p - str;

  if (ver_len > 8)
    return -1;

  bcopy(str, ver_buf, ver_len);
  ver_buf[ver_len] = 0;

  if (!strcmp(ver_buf, "HTTP/1.0"))
    reply->version = DPL_HTTP_VERSION_1_0;
  else if (!strcmp(ver_buf, "HTTP/1.1"))
    reply->version = DPL_HTTP_VERSION_1_1;
  else
    return -1;

  p++;

  if (!(p2 = index(p, ' ')))
    return -1;

  code_len = p2 - p;

  if (code_len > 3)
    return -1;

  bcopy(p, code_buf, code_len);
  code_buf[code_len] = 0;

  reply->code = atoi(code_buf);

  p2++;

  if (!(p = index(p2, '\r')))
    return -1;

  reply->descr_start = p2;
  reply->descr_end = p;

  return 0;
}

/**
 * reset state machine
 *
 * @param conn
 */
static void
read_line_init(dpl_conn_t *conn)
{
  conn->block_size = 512;
  conn->max_blocks = 200; //max chunks
  conn->read_buf_pos = 1;
  conn->cc = 1;
  conn->eof = 0;
}

/**
 * This function reads the file descriptor and bufferize input until a
 * newline is encountered. It returns NULL and sets status if read()
 * or malloc() fail or either if no newline was encountered (in this
 * case remaining data is discarded). On success it returns a newly
 * allocated string. Caller is responsible for freeing it.
 *
 * @return NULL on problem
 */
static char *
read_line(dpl_conn_t *conn)
{
  int		size, line_pos, found_nl, ret;
  char		*line, *tmp;

  DPRINTF("read_line cc=%ld read_buf_pos=%d\n", conn->cc, conn->read_buf_pos);

  errno = 0;

  if (conn->eof)
    {
      /*
       * EOF was previously encountered
       */
      conn->status = DPL_EINVAL;
      return NULL;
    }

  conn->status = DPL_SUCCESS;

  size = 1;

  /*
   * alloc a new line
   */
  if ((line = malloc(conn->block_size * size + 1)) == NULL)
    {
      conn->status = DPL_ENOMEM;
      return NULL;
    }

  line_pos = 0;
  found_nl = 0;

  while (conn->cc && !found_nl)
    {
      /*
       * copies buf from the current pos into the line until
       * buf is totally read
       * or the line is full
       * or we found a newline
       */
      while (conn->read_buf_pos < conn->cc &&
	     line_pos < conn->block_size * size &&
	     conn->read_buf[conn->read_buf_pos] != '\n')
        {
          //DPRINTF("%c\n", conn->read_buf[conn->read_buf_pos]);

          line[line_pos++] = conn->read_buf[conn->read_buf_pos++];
        }

      if (conn->read_buf_pos == conn->cc)
	{
	  /*
	   * current buf is totally read: read a new buf
	   */

          DPL_TRACE(conn->ctx, DPL_TRACE_IO, "read conn=%p https=%d size=%ld", conn, conn->ctx->use_https, conn->read_buf_size);

          if (0 == conn->ctx->use_https)
            {
              struct pollfd fds;

            retry:
              memset(&fds, 0, sizeof (fds));
              fds.fd = conn->fd;
              fds.events = POLLIN;

              ret = poll(&fds, 1, conn->ctx->read_timeout*1000);
              if (-1 == ret)
                {
                  if (errno == EINTR)
                    goto retry;
                  free(line);
                  conn->status = DPL_ESYS;
                  return NULL;
                }

              if (0 == ret)
                {
                  free(line);
                  conn->status = DPL_ETIMEOUT;
                  return NULL;
                }
              else if (!(fds.revents & POLLIN))
                {
                  free(line);
                  conn->status = DPL_ESYS;
                  return NULL;
                }

              conn->cc = read(conn->fd, conn->read_buf, conn->read_buf_size);
              if (conn->cc == -1)
                {
                  free(line);
                  conn->status = DPL_EIO;
                  return NULL;
                }
            }
          else
            {
              conn->cc = SSL_read(conn->ssl, conn->read_buf, conn->read_buf_size);
              if (conn->cc <= 0)
                {
                  free(line);
                  conn->status = DPL_EIO;
                  return NULL;
                }
            }

          DPL_TRACE(conn->ctx, DPL_TRACE_IO, "read conn=%p https=%d cc=%ld", conn, conn->ctx->use_https, conn->cc);

	  if (conn->cc == 0)
            conn->eof = 1;

          if (conn->ctx->trace_level & DPL_TRACE_BUF)
            dpl_dump_simple(conn->read_buf, conn->cc);

	  conn->read_buf_pos = 0;

	}
      else if (line_pos >= (conn->block_size * size))
	{
          DPRINTF("not enough mem line_pos=%d\n", line_pos);

	  if (size == conn->max_blocks)
	    {
	      /*
	       * we didn't find a newline within limit
	       */
	      free(line);
	      conn->status = DPL_ELIMIT;
	      return NULL;
	    }

	  /*
	   * line is not large enough: realloc line
	   */

	  size++;
          DPRINTF("reallocing %d chunks\n", size);
	  if ((tmp = realloc(line, conn->block_size * size + 1)) == NULL)
	    {
	      free(line);
	      conn->status = DPL_ENOMEM;
	      return NULL;
	    }
	  line = tmp;
	}
      else
	{
          DPRINTF("found nl\n");

          //found newline. skip it
	  found_nl = 1;
	  conn->read_buf_pos++;
	}
    }

  line[line_pos] = 0;

  return line;
}

/**
 * read http reply
 *
 * TODO: use a callback instead of expect_data
 *
 * @param conn
 * @param expect_data // always set to 1, expect for HEAD-like methods!
 * @param header_func
 * @param buffer_func
 * @param cb_arg
 *
 * @return dpl_status
 */
dpl_status_t
dpl_read_http_reply_buffered(dpl_conn_t *conn,
                             int expect_data,
                             dpl_header_func_t header_func,
                             dpl_buffer_func_t buffer_func,
                             void *cb_arg)
{
  int ret, ret2;
  struct dpl_http_reply http_reply;
  char *line = NULL;
  size_t chunk_len = 0;
  ssize_t chunk_remain = 0;
  ssize_t chunk_off = 0;
  int chunked = 0;
#define MODE_REPLY  0
#define MODE_HEADER  1
#define MODE_CHUNK   2
#define MODE_CHUNKED 3
  int mode;

  DPRINTF("read_http_reply fd=%d flags=0x%x\n", conn->fd, flags);

  http_reply.code = 0;

  read_line_init(conn);

  mode = MODE_REPLY;
  ret = DPL_SUCCESS;

  while (1)
    {
      if (MODE_CHUNK == mode)
        {
          DPRINTF("chunk_len=%ld chunk_off=%ld\n", chunk_len, chunk_off);

          if (chunk_off < chunk_len)
            {
              chunk_remain = chunk_len - chunk_off;

              DPL_TRACE(conn->ctx, DPL_TRACE_IO, "read conn=%p https=%d size=%ld (remain %ld)", conn, conn->ctx->use_https, conn->read_buf_size, chunk_remain);

              if (0 == conn->ctx->use_https)
                {
                  struct pollfd fds;

                  int recvfl = 0;

                retry:
                  memset(&fds, 0, sizeof (fds));
                  fds.fd = conn->fd;
                  fds.events = POLLIN;

                  ret2 = poll(&fds, 1, conn->ctx->read_timeout*1000);
                  if (-1 == ret2)
                    {
                      if (errno == EINTR)
                        goto retry;
                      DPLERR(1, "poll");
                      ret = DPL_FAILURE;
                      goto end;
                    }

                  if (0 == ret2)
                    {
                      DPLERR(0, "read timeout");
                      ret = DPL_FAILURE;
                      goto end;
                    }
                  else if (!(fds.revents & POLLIN))
                    {
                      DPLERR(0, "socket error");
                      ret = DPL_FAILURE;
                      goto end;
                    }

                  recvfl = (chunk_remain >= conn->read_buf_size) ? MSG_WAITALL : 0;

                  conn->cc = recv(conn->fd, conn->read_buf, conn->read_buf_size, recvfl);
                  if (-1 == conn->cc)
                    {
                      DPLERR(1, "recv");
                      ret = DPL_FAILURE;
                      goto end;
                    }
                }
              else
                {
                  conn->cc = SSL_read(conn->ssl, conn->read_buf, conn->read_buf_size);
                  if (conn->cc <= 0)
                    {
                      DPLERR(1, "SSL_read");
                      ret = DPL_FAILURE;
                      goto end;
                    }
                }

              DPL_TRACE(conn->ctx, DPL_TRACE_IO, "read conn=%p https=%d cc=%ld", conn, conn->ctx->use_https, conn->cc);

              if (0 == conn->cc)
                {
                  DPRINTF("no more data to read\n");
                  break ;
                }

              if (conn->ctx->trace_level & DPL_TRACE_BUF)
                dpl_dump_simple(conn->read_buf, conn->cc);

              chunk_remain = MIN(conn->cc, chunk_len - chunk_off);
              ret2 = buffer_func(cb_arg, conn->read_buf, chunk_remain);
              if (DPL_SUCCESS != ret2)
                {
                  DPLERR(0, "buffer_func");
                  ret = DPL_FAILURE;
                  goto end;
                }
              conn->read_buf_pos = chunk_remain;
              chunk_off += chunk_remain;

              continue ;
            }

          DPL_TRACE(conn->ctx, DPL_TRACE_HTTP, "conn=%p chunk done", conn);

          if (1 == chunked)
            {
              mode = MODE_HEADER; //skip crlf
              continue ;
            }
          else
            {
              ret = DPL_SUCCESS;
              break ;
            }
        }
      else
        {
          line = read_line(conn);
          if (NULL == line)
            {
              DPLERR(0, "read line: %s", dpl_status_str(conn->status));
              ret = DPL_FAILURE;
              goto end;
            }

          switch (mode)
            {
            case MODE_REPLY:

              if (http_parse_reply(line, &http_reply) != 0)
                {
                  DPLERR(0, "bad http reply: %.*s...", 100, line);
                  ret = DPL_FAILURE;
                  goto end;
                }

              DPL_TRACE(conn->ctx, DPL_TRACE_HTTP, "conn=%p http_status=%d", conn, http_reply.code);

              if (!(DPL_HTTP_CODE_CONTINUE == http_reply.code ||
                    DPL_HTTP_CODE_OK == http_reply.code ||
                    DPL_HTTP_CODE_CREATED == http_reply.code ||
                    DPL_HTTP_CODE_NO_CONTENT == http_reply.code ||
                    DPL_HTTP_CODE_PARTIAL_CONTENT == http_reply.code))
                {
                  DPLERR(0, "request failed %d", http_reply.code);

                  switch (http_reply.code)
                    {
                    case DPL_HTTP_CODE_NOT_FOUND:
                      ret = DPL_ENOENT;
                      break;
                    case DPL_HTTP_CODE_CONFLICT:
                      ret = DPL_EEXIST;
                      break;
                    default:
                      ret = DPL_FAILURE;
                      break;
                    }

                  goto end;
                }

              mode = MODE_HEADER;

              break ;

            case MODE_HEADER:

              if (line[0] == '\r')
                {
                  if (1 == chunked)
                    {
                      mode = MODE_CHUNKED;
                    }
                  else
                    {
                      //one big chunk
                      mode = MODE_CHUNK;
                      chunk_off = 0;
                      if (conn->read_buf_pos < conn->cc)
                        {
                          chunk_remain = MIN(conn->cc - conn->read_buf_pos, chunk_len);
                          ret2 = buffer_func(cb_arg, conn->read_buf + conn->read_buf_pos, chunk_remain);
                          if (DPL_SUCCESS != ret2)
                            {
                              DPLERR(0, "buffer_func");
                              ret = DPL_FAILURE;
                              goto end;
                            }
                          conn->read_buf_pos += chunk_remain;
                          chunk_off += chunk_remain;
                        }
                    }

                  break ;
                }

              //headers
              {
                char *p, *p2;

                p = index(line, ':');
                if (NULL == p)
                  {
                    DPLERR(0, "bad header: %.*s...", 100, line);
                    break ;
                  }
                *p++ = 0;

                //skip ws
                for (;*p;p++)
                  if (!isspace(*p))
                    break ;

                //remove '\r'
                p2 = index(p, '\r');
                if (NULL != p2)
                  *p2 = 0;

                DPL_TRACE(conn->ctx, DPL_TRACE_HTTP, "conn=%p header='%s' value='%s'", conn, line, p);

                if (expect_data && !strcasecmp(line, "Content-Length"))
                  {
                    chunk_len = atoi(p);
                  }
                else if (!strcasecmp(line, "Transfer-Encoding"))
                  {
                    if (!strcasecmp(p, "chunked"))
                      chunked = 1;
                  }
                else
                  {
                    ret2 = header_func(cb_arg, line, p);
                    if (DPL_SUCCESS != ret2)
                      {
                        DPLERR(0, "header_func");
                        ret = DPL_FAILURE;
                        goto end;
                      }
                  }
                break ;
              }

            case MODE_CHUNKED:

              chunk_len = strtoul(line, NULL, 16);

              DPL_TRACE(conn->ctx, DPL_TRACE_IO, "chunk_len=%d", chunk_len);

              if (0 == chunk_len)
                {
                  DPRINTF("data done\n");
                  ret = DPL_SUCCESS;
                  goto end;
                }

              mode = MODE_CHUNK;
              chunk_off = 0;
              if (conn->read_buf_pos < conn->cc)
                {
                  chunk_remain = MIN(conn->cc - conn->read_buf_pos, chunk_len);
                  ret2 = buffer_func(cb_arg, conn->read_buf + conn->read_buf_pos, chunk_remain);
                  if (DPL_SUCCESS != ret2)
                    {
                      DPLERR(0, "buffer_func");
                      ret = DPL_FAILURE;
                      goto end;
                    }
                  conn->read_buf_pos += chunk_remain;
                  chunk_off += chunk_remain;
                }
              break ;

            default:
              assert(0);
            }

          free(line);
          line = NULL;
        }
    }

 end:

  if (NULL != line)
    free(line);

  return ret;
}

int
dpl_connection_close(dpl_ctx_t *ctx,
                     dpl_dict_t *headers_returned)
{
  dpl_var_t *var;
  int ret;

  if (NULL == headers_returned)
    return 1; //assume close

  ret = dpl_dict_get_lowered(headers_returned, "Connection", &var);
  if (DPL_SUCCESS != ret)
    {
      //some servers does not send explicit connection information and does not support keep alive
      if (!ctx->keep_alive)
        return -1;

      return 0;
    }

  if (NULL != var)
    {
      if (!strcasecmp(var->value, "close"))
        return 1;
    }

  return 0;
}

/*
 * convenience function
 */
struct httreply_conven
{
  char *data_buf;
  u_int data_len;
  dpl_dict_t *headers;
};

static dpl_status_t
cb_httpreply_header(void *cb_arg,
                    char *header,
                    char *value)
{
  struct httreply_conven *hc = (struct httreply_conven *) cb_arg;
  int ret;

  if (NULL == hc->headers)
    {
      hc->headers = dpl_dict_new(13);
      if (NULL == hc->headers)
        return DPL_ENOMEM;
    }

  ret = dpl_dict_add(hc->headers, header, value, 1);
  if (DPL_SUCCESS != ret)
    return DPL_ENOMEM;

  return DPL_SUCCESS;
}

static dpl_status_t
cb_httpreply_buffer(void *cb_arg,
                    char *buf,
                    u_int len)
{
  struct httreply_conven *hc = (struct httreply_conven *) cb_arg;

  if (NULL == hc->data_buf)
    {
      hc->data_buf = malloc(len);
      if (NULL == hc->data_buf)
        return DPL_ENOMEM;

      memcpy(hc->data_buf, buf, len);
      hc->data_len = len;
    }
  else
    {
      char *nptr;

      nptr = realloc(hc->data_buf, hc->data_len + len);
      if (NULL == nptr)
        return DPL_ENOMEM;

      hc->data_buf = nptr;
      memcpy(hc->data_buf + hc->data_len, buf, len);
      hc->data_len += len;
    }

  return DPL_SUCCESS;
}

/**
 * read http reply simple version
 *
 * @param fd
 * @param expect_data
 * @param data_bufp caller must free it
 * @param data_lenp
 * @param headersp caller must free it
 *
 * @return dpl_status
 */
dpl_status_t
dpl_read_http_reply(dpl_conn_t *conn,
                    int expect_data,
                    char **data_bufp,
                    unsigned int *data_lenp,
                    dpl_dict_t **headersp)
{
  int ret, ret2;
  struct httreply_conven hc;

  memset(&hc, 0, sizeof (hc));

  ret2 = dpl_read_http_reply_buffered(conn, expect_data, cb_httpreply_header, cb_httpreply_buffer, &hc);
  if (DPL_SUCCESS != ret2)
    {
      ret = ret2;
      goto end;
    }

  ret = DPL_SUCCESS;

 end:

  if (0 == ret)
    {
      if (NULL != data_bufp)
        {
          *data_bufp = hc.data_buf;
          hc.data_buf = NULL; //consumed
        }

      if (NULL != data_lenp)
        *data_lenp = hc.data_len;

      if (NULL != headersp)
        {
          *headersp = hc.headers;
          hc.headers = NULL; //consumed
        }
    }

  //if not consumed
  if (NULL != hc.data_buf)
    free(hc.data_buf);

  if (NULL != hc.headers)
    dpl_dict_free(hc.headers);

  return ret;
}
