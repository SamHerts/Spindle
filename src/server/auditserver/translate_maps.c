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

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>

#include "ldcs_audit_server_filemngt.h"
#include "global_name.h"
#include "spindle_debug.h"

static int open_proc_maps(int pid)
{
   int fd;
   char filename[MAX_NAME_LEN+1];
   int error;
   snprintf(filename, MAX_NAME_LEN, "/proc/%d/maps", pid);
   filename[MAX_NAME_LEN] = '\0';
   fd = open(filename, O_RDONLY);
   if (fd == -1) {
      error = errno;
      err_printf("Error opening /proc/%d/maps for translation: %s\n", pid, strerror(error));
      return -1;
   }
   return fd;
}

static int open_replacement_proc_maps(char *spindle_dir, int pid, char *output_file, int output_file_size)
{
   int fd;
   int error;
   static int uniqnum = 0;

   snprintf(output_file, output_file_size, "%s/spindle_proc_maps_%d_%d", spindle_dir, pid, uniqnum++);
   fd = open(output_file, O_WRONLY | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR);
   if (fd == -1) {
      error = errno;
      err_printf("Unable to open replacement proc maps for process %d in %s: %s\n",
                 pid, spindle_dir, strerror(error));
      return -1;
   }
   output_file[output_file_size-1] = '\0';
   return fd;
}

#define BUFFER_SIZE 4096
struct buffer_info_t {
   char *buffer;
   int buffer_pos;
   int buffer_size;
   int max_buffer_size;
};

static int read_into_buffer(int fd, struct buffer_info_t *buf) {
   int result, error;

   do {
      result = read(fd, buf->buffer, buf->max_buffer_size);
   } while (result == -1 && errno == EINTR);
   if (result == -1) {
      error = errno;
      err_printf("Error reading from from /proc/maps for interception: %s\n", strerror(error));
      return -1;
   }
   if (result == 0)
      return 0;
   buf->buffer_size = result;
   buf->buffer_pos = 0;
   return result;
}
   
static int read_one_line(int fd, char *line, int *linelen, int maxlinelen, struct buffer_info_t *buf) {
   int pos = 0;
   char c;
   int result;

   *linelen = 0;
   
   for (;;) {
      if (buf->buffer_size == 0 || buf->buffer_pos == buf->buffer_size) {
         result = read_into_buffer(fd, buf);
         if (result == -1)
            return -1;
         if (result == 0)
            return 0;
      }
      c = buf->buffer[buf->buffer_pos];
      buf->buffer_pos++;
      if (c == '\n') {
         if (pos < maxlinelen)
            line[pos] = '\0';
         break;
      }
      if (pos < maxlinelen) {
         line[pos] = c;
         *linelen = pos+1;
      }
      pos++;
   }
   line[maxlinelen-1] = '\0';
   return 0;
}

static int write_line(int fd, char *line, int linelen)
{
   int pos = 0, result, error;
   do {
      result = write(fd, line + pos, linelen - pos);
      if (result == -1 && errno == EINTR)
         continue;
      if (result == -1) {
         error = errno;
         err_printf("Error writing to new proc maps: %s\n", strerror(error));
         return -1;
      }
      pos += result;
   } while (pos < linelen);
   return 0;
}

static void add_newline(char *line, int *linelen, int maxlen)
{
   if (*linelen < maxlen-2) {
      line[*linelen] = '\n';
      *linelen = *linelen + 1;
      line[*linelen] = '\0';
   }
   else {
      line[maxlen-2] = '\n';
      line[maxlen-1] = '\0';
      *linelen = maxlen - 1;
   }
}

static int translate_line(char *spindle_dir, char *line, int *linelen, int maxlen)
{
   char *p;
   char *newname;

   char *lastpart = strrchr(spindle_dir, '/');
   if (!lastpart)
      lastpart = spindle_dir;
   if (*(lastpart+1) == '\0') {
      lastpart--;
      while (lastpart != spindle_dir && *lastpart != '/') lastpart--;
   }

   p = strstr(line, lastpart);
   if (!p) {
      return 0;
   }

   while (p != line && *(p-1) != ' ')
      p--;

   newname = lookup_global_name(p);
   if (!newname) {
      return 0;
   }
   debug_printf3("Translating path '%s' to '%s' in maps file\n", p, newname);
   strncpy(p, newname, maxlen - (p - line) - 1);
   *linelen = strlen(line);

   return 0;
}

#define MAX_PROC_MAPS_LINESZ 8192
int translate_proc_pid_maps(char *spindle_dir, int pid, char *output_file, int output_file_size)
{
   int fd, newfd;
   int result;
   struct buffer_info_t buf;
   char working_buf[BUFFER_SIZE];
   char line[MAX_PROC_MAPS_LINESZ];
   int linelen;


   fd = open_proc_maps(pid);
   if (fd == -1) {
      return -1;
   }

   newfd = open_replacement_proc_maps(spindle_dir, pid, output_file, output_file_size);
   if (newfd == -1) {
      close(fd);
      return -1;
   }

   buf.buffer = working_buf;
   buf.buffer_pos = 0;
   buf.buffer_size = 0;
   buf.max_buffer_size = sizeof(working_buf);

   for (;;) {
      result = read_one_line(fd, line, &linelen, sizeof(line), &buf);
      if (result == -1)
         break;
      
      if (linelen <= 1)
         break;
      translate_line(spindle_dir, line, &linelen, sizeof(line));
      add_newline(line, &linelen, sizeof(line));
      result = write_line(newfd, line, linelen);
      if (result == -1)
         break;
   }
   close(fd);
   close(newfd);
   return result;
}

