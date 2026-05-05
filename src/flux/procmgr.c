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
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include "procmgr.h"
#include "spindle_launch.h"

static int create_pidfile(const char *name, const char *session_dir, pid_t pid);
static pid_t read_pidfile(const char *name, const char *session_dir);
static int clean_pidfile(const char *name, const char *session_dir);

static const char *local_name;
static const char *local_session_dir;
static int ready_pipe[2] = { -1, -1 };
#define PIPE_RD 0
#define PIPE_WR 1

int start_service(const char *name, service_cb_t cb, const char *session_dir, int argc, char *argv[])
{
   int result, error;
   pid_t pid;

   pid = fork();
   if (pid == -1) {
      error = errno;
      fprintf(stderr, "Error forking process for spindle service %s: %s\n", name, strerror(error));
      return -1;
   }
   else if (pid == 0) {
      local_name = name;
      local_session_dir = session_dir;
      result = cb(argc, argv);
      exit(result);
   }
   else {
      return create_pidfile(name, session_dir, pid);
   }
}

int stop_service(const char *name, const char *session_dir)
{
   pid_t pid;

   pid = read_pidfile(name, session_dir);
   if (pid == -1)
      return -1;
   return kill(pid, SIGTERM);
}

int clean_service()
{
   return clean_pidfile(local_name, local_session_dir);
}

int is_active_service(const char *name, const char *session_dir)
{
   char path[4096];
   pid_t pid = -1;
   snprintf(path, sizeof(path), "%s/%s.pid", session_dir, name);
   path[sizeof(path)-1] = '\0';

   FILE *f = fopen(path, "r");
   if (!f) {
      spindle_debug_printf(3, "Determined that service %s is not running\n", name);
      return 0;
   }
   fscanf(f, "%d", &pid);
   fclose(f);
   spindle_debug_printf(2, "Service %s is running on pid %d\n", name, pid);
   return 1;
}

int init_readymsg()
{
   int result, error;
   result = pipe(ready_pipe);
   if (result == -1) {
      error = errno;
      fprintf(stderr, "Could not create ready pipe for spindle session: %s\n", strerror(error));
      return -1;
   }
   return 0;
}

int ping_readymsg(int had_error)
{
   int result, error;
   char v;

   v = had_error ? '0' : '1';
   
   result = write(ready_pipe[PIPE_WR], &v, 1);
   if (result == -1) {
      error = errno;
      fprintf(stderr, "Could not ping ready pipe for spindle session: %s\n", strerror(error));
      return -1;
   }
   return 0;
}

void clean_readymsg()
{
   if (ready_pipe[PIPE_WR] != -1)
      close(ready_pipe[PIPE_WR]);
   if (ready_pipe[PIPE_RD] != -1)
      close(ready_pipe[PIPE_RD]);
}

int waitfor_readymsg(int timeout_seconds, int *had_error, int *had_timeout)
{
   int result, error;
   fd_set readfds;
   struct timeval tv;
   int nfds;
   char v = 0;

   *had_error = 0;
   *had_timeout = 0;
   
   tv.tv_sec = timeout_seconds;
   tv.tv_usec = 0;

   for (;;) {
      FD_ZERO(&readfds);
      FD_SET(ready_pipe[PIPE_RD], &readfds);
      nfds = ready_pipe[PIPE_RD] + 1;

      result = select(nfds, &readfds, NULL, NULL, &tv);      
      if (result == -1 && errno == EINTR) {
         continue;
      }
      else if (result == -1) {
         error = errno;
         fprintf(stderr, "Could not block for ready servers in spindle session: %s\n", strerror(error));
         return -1;
      }
      else if (result == 0 && (tv.tv_sec || tv.tv_usec)) {
         continue;
      }
      else if (result == 0) {
         *had_timeout = 1;
         return -1;
      }
      else if (FD_ISSET(ready_pipe[PIPE_RD], &readfds)) {
         result = read(ready_pipe[PIPE_RD], &v, 1);
         if (result == -1) {
            error = errno;
            fprintf(stderr, "Could not read from ready pipe in spindle session: %s\n", strerror(error));
            return -1;
         }
         if (v == '0') {
            *had_error = 1;
            return -1;
         }
         else if (v == '1') {
            return 0;
         }
         else {
            fprintf(stderr, "Unexpected value from ready pipe blocker in spindle session\n");
            return -1;
         }
      }
      else {
         fprintf(stderr, "Unexpected return from ready pipe blocker in spindle session\n");
         return -1;
      }
   }
}

static int create_pidfile(const char *name, const char *session_dir, pid_t pid)
{
   char session_path[4096];
   int error;
   FILE *f;
   
   snprintf(session_path, sizeof(session_path), "%s/%s.pid", session_dir, name);
   session_path[sizeof(session_path)-1] = '\0';
   f = fopen(session_path, "w");
   if (!f) {
      error = errno;
      fprintf(stderr, "Error creating pid file for spindle service at %s: %s\n", session_path, strerror(error));
      return -1;
   }
   fprintf(f, "%d\n", pid);
   fclose(f);
   return 0;   
}

static int read_pidfile(const char *name, const char *session_dir)
{
   char session_path[4096];
   int error;
   FILE *f;
   pid_t pid = -1;
   
   snprintf(session_path, sizeof(session_path), "%s/%s.pid", session_dir, name);
   session_path[sizeof(session_path)-1] = '\0';
   f = fopen(session_path, "r");
   if (!f) {
      error = errno;
      fprintf(stderr, "Error reading pid file for spindle service at %s: %s\n", session_path, strerror(error));
      return -1;
   }
   fscanf(f, "%d\n", &pid);
   fclose(f);
   return pid;
}

static int clean_pidfile(const char *name, const char *session_dir)
{
  char session_path[4096];
  snprintf(session_path, sizeof(session_path), "%s/%s.pid", session_dir, name);
  session_path[sizeof(session_path)-1] = '\0';
  return unlink(session_path);
}

