/*
 * Droplets, high performance cloud storage client library
 * Copyright (C) 2010 Scality http://github.com/scality/Droplets
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *  
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "dropletsp.h"

//#define DPRINTF(fmt,...) fprintf(stderr, fmt, ##__VA_ARGS__)
#define DPRINTF(fmt,...)

dpl_status_t
dpl_vdir_lookup(dpl_ctx_t *ctx,
                char *bucket,
                dpl_ino_t parent_ino,
                const char *obj_name,
                dpl_ino_t *obj_inop,
                dpl_ftype_t *obj_typep)
{
  int ret, ret2;
  dpl_vec_t *files = NULL;
  dpl_vec_t *directories = NULL;
  int i;
  dpl_ino_t obj_ino;
  dpl_ftype_t obj_type;

  memset(&obj_ino, 0, sizeof (obj_ino));

  DPL_TRACE(ctx, DPL_TRACE_VDIR, "lookup bucket=%s parent_ino=%s obj_name=%s", bucket, parent_ino.key, obj_name);

  if (!strcmp(obj_name, "."))
    {
      if (NULL != obj_inop)
        *obj_inop = parent_ino;
      
      if (NULL != obj_typep)
        *obj_typep = DPL_FTYPE_DIR;
      
      ret = DPL_SUCCESS;
      goto end;
    }
  
  //AWS do not like "" as a prefix
  ret2 = dpl_list_bucket(ctx, bucket, !strcmp(parent_ino.key, "") ? NULL : parent_ino.key, "/", &files, &directories);
  if (DPL_SUCCESS != ret2)
    {
      DPLERR(0, "list_bucket failed %s:%s", bucket, parent_ino.key);
      ret = DPL_FAILURE;
      goto end;
    }
  
  for (i = 0;i < files->n_items;i++)
    {
      dpl_object_t *obj = (dpl_object_t *) files->array[i];

      if (!strcmp(obj->key, obj_name))
        {
          int key_len;
          
          key_len = strlen(obj->key);
          if (key_len >= DPL_MAXNAMLEN)
            {
              DPLERR(0, "key is too long");
              ret = DPL_FAILURE;
              goto end;
            }
          memcpy(obj_ino.key, obj->key, key_len);
          if ('/' == obj->key[key_len-1])
            obj_type = DPL_FTYPE_DIR;
          else
            obj_type = DPL_FTYPE_REG;
          
          if (NULL != obj_inop)
            *obj_inop = obj_ino;

          if (NULL != obj_typep)
            *obj_typep = obj_type;

          ret = DPL_SUCCESS;
          goto end;
        }
    }

  for (i = 0;i < directories->n_items;i++)
    {
      dpl_common_prefix_t *prefix = (dpl_common_prefix_t *) directories->array[i];
      int key_len;

      key_len = strlen(prefix->prefix);

      if (!strncmp(prefix->prefix, obj_name, key_len-1))
        {
          if (key_len >= DPL_MAXNAMLEN)
            {
              DPLERR(0, "key is too long");
              ret = DPL_FAILURE;
              goto end;
            }
          memcpy(obj_ino.key, prefix->prefix, key_len);
          obj_type = DPL_FTYPE_DIR;
          
          if (NULL != obj_inop)
            *obj_inop = obj_ino;

          if (NULL != obj_typep)
            *obj_typep = obj_type;

          ret = DPL_SUCCESS;
          goto end;
        }
    }

  ret = DPL_ENOENT;

 end:

  if (NULL != files)
    dpl_vec_objects_free(files);

  if (NULL != directories)
    dpl_vec_common_prefixes_free(directories);

  return ret;
}

dpl_status_t
dpl_vdir_mknod()
{
  return DPL_FAILURE;
}

dpl_status_t
dpl_vdir_mkdir(dpl_ctx_t *ctx,
               char *bucket,
               dpl_ino_t parent_ino,
               const char *obj_name)
{
  int ret, ret2;
  char resource[DPL_MAXPATHLEN];

  DPL_TRACE(ctx, DPL_TRACE_VDIR, "mkdir bucket=%s parent_ino=%s name=%s", bucket, parent_ino.key, obj_name);

  snprintf(resource, sizeof (resource), "%s/", obj_name); //XXX

  ret2 = dpl_put(ctx, bucket, resource, NULL, NULL, DPL_CANNED_ACL_PRIVATE, NULL, 0);
  if (DPL_SUCCESS != ret2)
    {
      ret = DPL_FAILURE;
      goto end;
    }

  ret = DPL_SUCCESS;

 end:

  return ret;
}

dpl_status_t
dpl_vdir_unlink()
{
  return DPL_FAILURE;
}

dpl_status_t
dpl_vdir_rmdir()
{
  return DPL_FAILURE;
}

dpl_status_t
dpl_vdir_rename()
{
  return DPL_FAILURE;
}

dpl_status_t
dpl_vdir_opendir(dpl_ctx_t *ctx,
                 char *bucket,
                 dpl_ino_t ino,
                 void **dir_hdlp)
{
  dpl_dir_t *dir;
  int ret, ret2;

  DPL_TRACE(ctx, DPL_TRACE_VDIR, "opendir bucket=%s ino=%s", bucket, ino.key);

  dir = malloc(sizeof (*dir));
  if (NULL == dir)
    {
      ret = DPL_FAILURE;
      goto end;
    }

  memset(dir, 0, sizeof (*dir));

  dir->ctx = ctx;

  //AWS prefers NULL for listing the root dir
  ret2 = dpl_list_bucket(ctx, bucket, !strcmp(ino.key, "") ? NULL : ino.key, "/", &dir->files, &dir->directories);
  if (DPL_SUCCESS != ret2)
    {
      DPLERR(0, "list_bucket failed %s:%s", bucket, ino.key);
      ret = DPL_FAILURE;
      goto end;
    }

  printf("%s:%s n_files=%d n_dirs=%d\n", bucket, ino.key, dir->files->n_items, dir->directories->n_items);
  
  if (NULL != dir_hdlp)
    *dir_hdlp = dir;

  DPL_TRACE(dir->ctx, DPL_TRACE_VDIR, "dir_hdl=%p", dir);

  ret = DPL_SUCCESS;

 end:

  if (DPL_SUCCESS != ret)
    {
      if (NULL != dir->files)
        dpl_vec_objects_free(dir->files);

      if (NULL != dir->directories)
        dpl_vec_common_prefixes_free(dir->directories);

      if (NULL != dir)
        free(dir);
    }

  return ret;
}

dpl_status_t
dpl_vdir_readdir(void *dir_hdl,
                 dpl_dirent_t *dirent)
{
  dpl_dir_t *dir = (dpl_dir_t *) dir_hdl;
  int key_len;

  DPL_TRACE(dir->ctx, DPL_TRACE_VDIR, "readdir dir_hdl=%p files_cursor=%d directories_cursor=%d", dir_hdl, dir->files_cursor, dir->directories_cursor);

  memset(dirent, 0, sizeof (*dirent));

  if (dir->files_cursor >= dir->files->n_items)
    {
      if (dir->directories_cursor >= dir->directories->n_items)
        {
          DPLERR(0, "beyond cursors");
          return DPL_FAILURE;
        }
      else
        {
          dpl_common_prefix_t *prefix;

          prefix = (dpl_common_prefix_t *) dir->directories->array[dir->directories_cursor];
          
          key_len = strlen(prefix->prefix);
          if (key_len >= DPL_MAXNAMLEN)
            {
              DPLERR(0, "key is too long");
              return DPL_FAILURE;
            }
          memcpy(dirent->ino.key, prefix->prefix, key_len);
          dirent->type = DPL_FTYPE_DIR;
          
          dir->directories_cursor++;

          return DPL_SUCCESS;
        }
    }
  else
    {
      dpl_object_t *obj;

      obj = (dpl_object_t *) dir->files->array[dir->files_cursor];
      
      key_len = strlen(obj->key);
      if (key_len >= DPL_MAXNAMLEN)
        {
          DPLERR(0, "key is too long");
          return DPL_FAILURE;
        }
      memcpy(dirent->ino.key, obj->key, key_len);
      if ('/' == obj->key[key_len-1])
        dirent->type = DPL_FTYPE_DIR;
      else
        dirent->type = DPL_FTYPE_REG;
      
      dirent->last_modified = obj->last_modified;
      dirent->size = obj->size;
      
      dir->files_cursor++;
      
      return DPL_SUCCESS;
    }
}

int
dpl_vdir_eof(void *dir_hdl)
{
  dpl_dir_t *dir = (dpl_dir_t *) dir_hdl;

  return dir->files_cursor == dir->files->n_items &&
    dir->directories_cursor == dir->directories->n_items;
}

void
dpl_vdir_closedir(void *dir_hdl)
{
  dpl_dir_t *dir = (dpl_dir_t *) dir_hdl;

  DPL_TRACE(dir->ctx, DPL_TRACE_VDIR, "closedir dir_hdl=%p", dir_hdl);
}

dpl_status_t
dpl_vdir_iname(dpl_ctx_t *ctx,
               char *bucket,
               dpl_ino_t ino,
               char *path,
               u_int path_len)
{
  DPL_TRACE(ctx, DPL_TRACE_VDIR, "iname bucket=%s ino=%s", bucket, ino.key);

  return DPL_FAILURE;
}

dpl_status_t
dpl_vdir_namei(dpl_ctx_t *ctx,
               char *path,
               char *bucket,
               dpl_ino_t ino,
               dpl_ino_t *parent_inop,
               dpl_ino_t *obj_inop,
               dpl_ftype_t *obj_typep)
{
  char *p1, *p2;
  char name[DPL_MAXNAMLEN];
  int namelen;
  int ret;
  dpl_ino_t parent_ino, obj_ino;
  dpl_ftype_t obj_type;

  DPL_TRACE(ctx, DPL_TRACE_VDIR, "namei path=%s bucket=%s ino=%s", path, bucket, ino.key);

  p1 = path;

  if (!strcmp(p1, "/"))
    {
      if (NULL != parent_inop)
        *parent_inop = DPL_ROOT_INO;
      if (NULL != obj_inop)
        *obj_inop = DPL_ROOT_INO;
      if (NULL != obj_typep)
        *obj_typep = DPL_FTYPE_DIR;
      return DPL_SUCCESS;
    }

  //absolute path
  if ('/' == p1[0])
    {
      parent_ino = DPL_ROOT_INO;
      p1++;
    }
  else
    {
      parent_ino = ino;
    }

  while (1)
    {
      p2 = index(p1, '/');
      if (NULL == p2)
        {
          namelen = strlen(p1);
        }
      else
        {
          p2++;
          namelen = p2 - p1 - 1;
        }

      if (namelen >= DPL_MAXNAMLEN)
        return DPL_ENAMETOOLONG;
      
      memcpy(name, p1, namelen);
      name[namelen] = 0;
      
      DPRINTF("lookup '%s'\n", name);
      
      if (!strcmp(name, ""))
        {
          obj_ino = parent_ino;
          obj_type = DPL_FTYPE_DIR;
        }
      else
        {
          ret = dpl_vdir_lookup(ctx, bucket, parent_ino, name, &obj_ino, &obj_type);
          if (DPL_SUCCESS != ret)
            return ret;
        }
      
      DPRINTF("p2='%s'\n", p2);

      if (NULL == p2)
        {
          if (NULL != parent_inop)
            *parent_inop = parent_ino;
          if (NULL != obj_inop)
            *obj_inop = obj_ino;
          if (NULL != obj_typep)
            *obj_typep = obj_type;

          return DPL_SUCCESS;
        }
      else
        {
          if (DPL_FTYPE_DIR != obj_type)
            return DPL_ENOTDIR;
          
          parent_ino = obj_ino;
          p1 = p2;

          DPRINTF("remain '%s'\n", p1);
        }
    }
  
  return DPL_FAILURE;
}
