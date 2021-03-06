/**************************************************************************
 * 
 * Copyright (c) 2004-16 Simon Peter
 * 
 * All Rights Reserved.
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 * 
 **************************************************************************/

#ident "AppImage by Simon Peter, http://appimage.org/"

/*
 * Optional daempon to watch directories for AppImages 
 * and register/unregister them with the system
 * 
 * TODO (feel free to send pull requests):
 * - Switch to https://developer.gnome.org/gio/stable/GFileMonitor.html (but with subdirectories)
 *   which would drop the dependency on libinotifytools.so.0
 * - Add and remove subdirectories on the fly at runtime - 
 *   see https://github.com/paragone/configure-via-inotify/blob/master/inotify/src/inotifywatch.c
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <dirent.h>

#include <inotifytools/inotifytools.h>
#include <inotifytools/inotify.h>

#include <glib.h>
#include <glib/gprintf.h>

#include "shared.c"

#include <pthread.h>

static gboolean verbose = FALSE;
gchar **remaining_args = NULL;

static GOptionEntry entries[] =
{
    { "verbose", 'v', 0, G_OPTION_ARG_NONE, &verbose, "Be verbose", NULL },
    { G_OPTION_REMAINING, 0, 0, G_OPTION_ARG_FILENAME_ARRAY, &remaining_args, NULL },
    { NULL }
};

#define EXCLUDE_CHUNK 1024
#define WR_EVENTS (IN_CLOSE_WRITE | IN_MOVED_FROM | IN_MOVED_TO | IN_DELETE | IN_DELETE_SELF | IN_MOVE_SELF)

/* Run the actual work in treads; 
 * pthread allows to pass only one argument to the thread function, 
 * hence we use a struct as the argument in which the real arguments are */
struct arg_struct {
    char* path;
    gboolean verbose;
};

void *thread_appimage_register_in_system(void *arguments)
{
    struct arg_struct *args = arguments;
    appimage_register_in_system(args->path, args->verbose);
    pthread_exit(NULL);
}

void *thread_appimage_unregister_in_system(void *arguments)
{
    struct arg_struct *args = arguments;
    appimage_unregister_in_system(args->path, args->verbose);
    pthread_exit(NULL);
}

/* Recursively process the files in this directory and its subdirectories,
 * http://stackoverflow.com/questions/8436841/how-to-recursively-list-directories-in-c-on-linux
 */
void initially_register(const char *name, int level)
{
    DIR *dir;
    struct dirent *entry;
    
    if (!(dir = opendir(name)))
        fprintf(stderr, "opendir error\n");
    if (!(entry = readdir(dir)))
        fprintf(stderr, "readdir error\n");
    
    do {
        if (entry->d_type == DT_DIR) {
            char path[1024];
            int len = snprintf(path, sizeof(path)-1, "%s/%s", name, entry->d_name);
            path[len] = 0;
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
                continue;
            initially_register(path, level + 1);
        }
        else {
            int ret;
            gchar *absolute_path = g_build_path(G_DIR_SEPARATOR_S, name, entry->d_name, NULL);
            if(g_file_test(absolute_path, G_FILE_TEST_IS_REGULAR)){
                pthread_t some_thread;
                struct arg_struct args;
                args.path = absolute_path;
                args.verbose = verbose;
                ret = pthread_create(&some_thread, NULL, thread_appimage_register_in_system, &args);
                if (!ret) {
                    pthread_join(some_thread, NULL);
                }
            }
        }
    } while (entry = readdir(dir));
    closedir(dir);
}

int add_dir_to_watch(char *directory)
{
    if (g_file_test (directory, G_FILE_TEST_IS_DIR)){
        if(!inotifytools_watch_recursively(directory, WR_EVENTS) ) {
            fprintf(stderr, "%s\n", strerror(inotifytools_error()));
            exit(1);
            
        }
        initially_register(directory, 0);
    }
}

void handle_event(struct inotify_event *event)
{
    int ret;
    gchar *absolute_path = g_build_path(G_DIR_SEPARATOR_S, inotifytools_filename_from_wd(event->wd), event->name, NULL);
    
    if(event->mask & IN_CLOSE_WRITE | event->mask & IN_MOVED_TO){
        if(g_file_test(absolute_path, G_FILE_TEST_IS_REGULAR)){
            pthread_t some_thread;
            struct arg_struct args;
            args.path = absolute_path;
            args.verbose = verbose;
            ret = pthread_create(&some_thread, NULL, thread_appimage_register_in_system, &args);
            if (!ret) {
                pthread_join(some_thread, NULL);
            }
        }
    }
    
    if(event->mask & IN_MOVED_FROM | event->mask & IN_DELETE){
        pthread_t some_thread;
        struct arg_struct args;
        args.path = absolute_path;
        args.verbose = verbose;
        ret = pthread_create(&some_thread, NULL, thread_appimage_unregister_in_system, &args);
        if (!ret) {
            pthread_join(some_thread, NULL);
        }
    }
    
    /* Too many FS events were received, some event notifications were potentially lost */
    if (event->mask & IN_Q_OVERFLOW){
        printf ("Warning: AN OVERFLOW EVENT OCCURRED\n");
    }
    
    if(event->mask & IN_IGNORED){
        printf ("Warning: AN IN_IGNORED EVENT OCCURRED\n");
    }
    
}

int main(int argc, char ** argv) {
    
    GError *error = NULL;
    GOptionContext *context;
    
    context = g_option_context_new ("");
    g_option_context_add_main_entries (context, entries, NULL);
    // g_option_context_add_group (context, gtk_get_option_group (TRUE));
    if (!g_option_context_parse (context, &argc, &argv, &error))
    {
        g_print("option parsing failed: %s\n", error->message);
        exit (1);
    }
    
    if ( !inotifytools_initialize()){
        fprintf(stderr, "inotifytools_initialize error\n");
        exit(1);
    }
    
    add_dir_to_watch(g_build_filename(g_get_home_dir(), "/Downloads", NULL));
    add_dir_to_watch(g_build_filename(g_get_home_dir(), "/bin", NULL));
    add_dir_to_watch(g_build_filename("/Applications", NULL));
    add_dir_to_watch(g_build_filename("/isodevice/Applications", NULL));
    add_dir_to_watch(g_build_filename("/opt", NULL));
    add_dir_to_watch(g_build_filename("/usr/local/bin", NULL));
    
    struct inotify_event * event = inotifytools_next_event(-1);
    while (event) {
        if(verbose){
            inotifytools_printf(event, "%w%f %e\n");
        }
        fflush(stdout);        
        handle_event(event);
        fflush(stdout);        
        event = inotifytools_next_event(-1);
    }
}
