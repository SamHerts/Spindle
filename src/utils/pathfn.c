/*
This file is part of Spindle.  For copyright information see the COPYRIGHT 
file in the top level directory, or at 
https://github.com/hpc/Spindle/blob/master/COPYRIGHT

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU Lesser General Public License (as published by the Free Software
Foundation) version 2.1 dated February 1999.  This program is distributed in the
hope that it will be useful, but WITHOUT ANY WARRANTY; without even the IMPLIED
WARRANTY OF MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the terms 
and conditions of the GNU Lesser General Public License for more details.  You should 
have received a copy of the GNU Lesser General Public License along with this 
program; if not, write to the Free Software Foundation, Inc., 59 Temple
Place, Suite 330, Boston, MA 02111-1307 USA
*/

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

#include "ldcs_api.h"

int addCWDToDir(pid_t pid, char *dir, int result_size)
{
   int cwd_len, dir_len, result;
   char cwd[MAX_PATH_LEN+1];
   char cwd_loc[64];
   
   if (dir[0] == '/')
      return 0;

   memset(cwd, 0, sizeof(cwd));
   snprintf(cwd_loc, sizeof(cwd_loc)-1, "/proc/%d/cwd", pid);
   result = readlink(cwd_loc, cwd, sizeof(cwd)-1);
   if (result == -1) {
      int error = errno;
      err_printf("Could not read CWD from %s: %s\n", cwd_loc, strerror(error));
   }
   cwd[MAX_PATH_LEN] = '\0';
   
   cwd_len = strlen(cwd);
   dir_len = strlen(dir);
   if (!cwd_len)
      return 0;
   if (cwd[cwd_len-1] == '/')
      cwd_len = cwd_len - 1;
   if (dir_len + cwd_len + 1 >= result_size)
      return -1;

   if (dir[0] == '\0') {
      strncpy(dir, cwd, cwd_len);
      dir[cwd_len] = '\0';
      return 0;
   }

   char *dir2 = strdup( dir );
   snprintf( dir, result_size, "%s/%s", cwd, dir2 );
   free(dir2);
   return 0;
}

int parseFilenameNoAlloc(const char *name, char *file, char *dir, int result_size)
{
   char *last_slash = strrchr(name, '/');
   int size;
   if (last_slash) {
      strncpy(file, last_slash+1, result_size);
      file[result_size-1] = '\0';
      size = last_slash - name;
      if (size >= result_size)
         size = result_size-1;
      strncpy(dir, name, size);
      if (size == 0 && last_slash == name) {
         dir[0] = '/';
         size = 1;
      }
      dir[size] = '\0';
         
   }
   else {
      strncpy(file, name, result_size);
      file[result_size-1] = '\0';
      dir[0] = '\0';
   }
   return 0;
}

/* Remove '.', '..', '//' from directory strings to normalize name.  We don't
   use the glibc functions that do this because they might access disk, which 
   isn't appropriate for Spindle.  Note that this means we can't resolve symlinks
   to a normalized name here. */
int reducePath(char *dir)
{
   int slash_begin = 0, slash_end, i, tmpdir_loc = 0;
   int dir_len = strlen(dir);
   char tmpdir[MAX_PATH_LEN+1];

   if (strcmp(dir, "/") == 0)
      return 0;
   
   while (dir[slash_begin]) {
      slash_end = slash_begin+1;
      while (dir[slash_end] != '\0' && dir[slash_end] != '/') slash_end++;

      if (slash_end == slash_begin + 1) {
         /* / case.  Do nothing, we will just advance past the directory */
      }
      else if (dir[slash_begin+1] == '.' && slash_end == slash_begin + 2) {
         /* /./ case.  Do nothing, we will just advance past the directory */
      }
      else if (dir[slash_begin+1] == '.' && dir[slash_begin+2] == '.' && slash_end == slash_begin + 3) {
         /* /../ case.  Back up tmpdir one directory */
         if (tmpdir_loc == 0) {
            return -1;
         }
         while (tmpdir[tmpdir_loc] != '/' && tmpdir_loc != 0)
            tmpdir_loc--;
         tmpdir[tmpdir_loc] = '\0';
      }
      else {
         /* Normal directory.  Copy from dir to dir */
         for (i = slash_begin; i != slash_end; i++, tmpdir_loc++) {
            tmpdir[tmpdir_loc] = dir[i];
         }
         tmpdir[tmpdir_loc] = '\0';
      }
      slash_begin = slash_end;
   }
   strncpy(dir, tmpdir, dir_len);
   return 0;
}

char *concatStrings(const char *str1, int str1_len, const char *str2, int str2_len) {
   char *buffer = NULL;
   unsigned cur_size = str1_len + str2_len + 1;

   buffer = (char *) malloc(cur_size);
   strncpy(buffer, str1, str1_len);
   if (str2)
      strncpy(buffer+str1_len, str2, str2_len);
   buffer[str1_len+str2_len] = '\0';

   return buffer;
}

static int addCWD(pid_t pid, const char *dir, char *target, int result_size)
{
   char cwd_loc[64];
   int result;
   
   if (dir[0] == '/')
      return 0;
   
   snprintf(cwd_loc, sizeof(cwd_loc)-1, "/proc/%d/cwd", pid);
   result = readlink(cwd_loc, target, result_size-1);
   if (result == -1) {
      int error = errno;
      err_printf("Could not read CWD from %s: %s\n", cwd_loc, strerror(error));
      return -1;
   }
   return 0;
}

int parseFilenameNoAlloc2(const char *name, char *file, char *dir, int result_size, pid_t pid)
{
   int result;
   int dir_end, is_absolute, cur, last_slash;
   int component_start, component_end, component_size;   
   char path_component[MAX_PATH_LEN];
   
   memset(file, 0, result_size);
   memset(dir, 0, result_size);

   is_absolute = name[0] == '/';

   /* Add CWD to 'dir' if we're a relative path */
   if (!is_absolute) {
      result = addCWD(pid, name, dir, result_size);
      if (result == -1) {
         err_printf("Aborting path parsing of %s\n", name);
         return -1;
      }
      dir_end = strlen(dir);
      if (dir_end > 1 && dir[dir_end-1] != '/') {
         dir[dir_end] = '/';
         dir_end++;
      }
   }
   else {
      dir[0] = '/';
      dir_end = 1;
   }

   /* Go through each path component in 'name' and add it to 'dir'. Resolve ./ and ////
      path components as we do so. 
      Do not resolve .. path components. That needs readlink access to do correctly.
   */
   cur = 0;
   while (name[cur] != '\0') {
      while (name[cur] == '/') cur++;
      component_start = cur;
      if (name[component_start] == '\0')
         break;
      while (name[cur] != '/' && name[cur] != '\0') cur++;
      component_end = cur;
      component_size = component_end - component_start;
      strncpy(path_component, name+component_start, component_size);
      path_component[component_size] = '\0';

      if (strcmp(path_component, ".") == 0) {
         //Nothing needed
      }
      /*
        else if (strcmp(path_component, "..") == 0) {
         while (dir_end > 1 && dir[dir_end-1] == '/') dir_end--;
         while (dir_end > 1 && dir[dir_end-1] != '/') dir_end--;         
         dir[dir_end] = '\0';
      }
      */
      else if (path_component[0] != '\0') {
         strncpy(dir+dir_end, path_component, component_size);
         dir_end += component_size;
         dir[dir_end] = '/';
         dir_end++;
         dir[dir_end] = '\0';
      }
   }
   /* Remove the trailing slash we always added to the end of dir. */
   if (dir_end > 1 && dir[dir_end-1] == '/')
      dir[dir_end-1] = '\0';
   
   /* Copy the final component out of dir and to file */
   last_slash = dir_end;
   while (last_slash > 0 && dir[last_slash] != '/') last_slash--;
   strncpy(file, dir+last_slash+1, result_size);

   /* Truncate that final slash that seperated the dir and file 
      (unless it's a root file system file/dir like "/bin" */
   if (last_slash > 0) 
      dir[last_slash] = '\0';
   else
      dir[1] = '\0';

   return 0;
}

