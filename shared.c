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

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <dirent.h>
#include <stdio.h>
#include <errno.h>

#include <glib.h>
#include <glib/gprintf.h>
#include <glib/gstdio.h>
#include <gio/gio.h>

#include "squashfuse.h"
#include <squashfs_fs.h>

#include "elf.h"
#include "getsection.h"

#include <regex.h>

#define FNM_FILE_NAME 2

#define URI_MAX (FILE_MAX * 3 + 8)

char *vendorprefix = "appimagekit";

void set_executable(char *path, gboolean verbose)
{
    if(!g_find_program_in_path ("firejail")){
        int result = chmod(path, 0755); // TODO: Only do this if signature verification passed
        if(result != 0){
            fprintf(stderr, "Could not set %s executable: %s\n", path, strerror(errno));
        } else {
            if(verbose)
                fprintf(stderr, "Set %s executable\n", path);
        }
    }
}

/* Search and replace on a string, this really should be in Glib
 * https://mail.gnome.org/archives/gtk-list/2012-February/msg00005.html */
gchar* replace_str(const gchar *src, const gchar *find, const gchar *replace){
    gchar* retval = g_strdup(src);
    gchar* ptr = NULL;
    ptr = g_strstr_len(retval,-1,find); 
    if (ptr != NULL){
        gchar* after_find = replace_str(ptr+strlen(find),find,replace);
        gchar* before_find = g_strndup(retval,ptr-retval);
        gchar* temp = g_strconcat(before_find,replace,after_find,NULL);
        g_free(retval);
        retval = g_strdup(temp);
        g_free(before_find);
        g_free(temp);
    }   
    return retval;
}

/* Return the md5 hash constructed according to
 * https://specifications.freedesktop.org/thumbnail-spec/thumbnail-spec-latest.html#THUMBSAVE
 * This can be used to identify files that are related to a given AppImage at a given location */
char * get_md5(char *path)
{
    gchar *uri = g_filename_to_uri (path, NULL, NULL);
    char *file;
    GChecksum *checksum;
    checksum = g_checksum_new(G_CHECKSUM_MD5);
    guint8 digest[16];
    gsize digest_len = sizeof (digest);
    g_checksum_update(checksum, (const guchar *) uri, strlen (uri));
    g_checksum_get_digest(checksum, digest, &digest_len);
    g_assert(digest_len == 16);
    return g_checksum_get_string(checksum);
}

/* Return the path of the thumbnail regardless whether it already exists; may be useful because
 * G*_FILE_ATTRIBUTE_THUMBNAIL_PATH only exists if the thumbnail is already there.
 * Check libgnomeui/gnome-thumbnail.h for actually generating thumbnails in the correct
 * sizes at the correct locations automatically; which would draw in a dependency on gdk-pixbuf.
 */
char * get_thumbnail_path(char *path, char *thumbnail_size, gboolean verbose)
{
    gchar *uri = g_filename_to_uri (path, NULL, NULL);
    char *file;
    file = g_strconcat (get_md5(path), ".png", NULL);
    gchar *thumbnail_path = g_build_filename (g_get_user_cache_dir(), "thumbnails", thumbnail_size, file, NULL);
    g_free (file);
    return thumbnail_path;
}

/* Check if a file is an AppImage. Returns the image type if it is, or -1 if it isn't */
int check_appimage_type(char *path, gboolean verbose)
{
    FILE *f;
    char buffer[4];
    if (f = fopen(path, "rt"))
    {
        /* Check magic bytes at offset 8 */
        fseek(f, 8, SEEK_SET);
        fread(buffer, 1, 3, f);
        buffer[4] = 0;
        fclose(f);
        if((buffer[0] == 0x41) && (buffer[1] == 0x49) && (buffer[2] == 0x01)){
            if(verbose)
                fprintf(stderr, "AppImage type 1\n");
            return 1;
        } else if((buffer[0] == 0x41) && (buffer[1] == 0x49) && (buffer[2] == 0x02)){
            if(verbose)
                fprintf(stderr, "AppImage type 2\n");
            return 2;
        } else {
            return -1;
        }
    }
}

/* Register a type 1 AppImage in the system */
bool appimage_type1_register_in_system(char *path, gboolean verbose)
{
    fprintf(stderr, "ISO9660 based type 1 AppImage, not yet implemented: %s\n", path);
    set_executable(path, verbose);
}

/* Get filename extension */
static gchar *get_file_extension(const gchar *filename)
{
    gchar **tokens;
    gchar *extension;
    tokens = g_strsplit(filename, ".", 2);
    if (tokens[0] == NULL)
        extension = NULL;
    else
        extension = g_strdup(tokens[1]);
    g_strfreev(tokens);
    return extension;
}

/* Find files in the squashfs matching to the regex pattern. 
 * Returns a newly-allocated NULL-terminated array of strings.
 * Use g_strfreev() to free it. 
 * 
 * The following is done within the sqfs_traverse run for performance reaons:
 * 1.) For found files that are in usr/share/icons, install those icons into the system
 * with a custom name that involves the md5 identifier to tie them to a particular
 * AppImage.
 * 2.) For found files that are in usr/share/mime/packages, install those icons into the system
 * with a custom name that involves the md5 identifier to tie them to a particular
 * AppImage.
 */
gchar **squash_get_matching_files(sqfs *fs, char *pattern, gchar *desktop_icon_value_original, char *md5, gboolean verbose) {
    GPtrArray *array = g_ptr_array_new();
    sqfs_err err = SQFS_OK;
    sqfs_traverse trv;
    if (err = sqfs_traverse_open(&trv, fs, sqfs_inode_root(fs)))
        fprintf(stderr, "sqfs_traverse_open error\n");
    while (sqfs_traverse_next(&trv, &err)) {
        if (!trv.dir_end) {
            int r;
            regex_t regex;
            regmatch_t match[2];
            regcomp(&regex, pattern, REG_ICASE | REG_EXTENDED);
            r = regexec(&regex, trv.path, 2, match, 0);
            regfree(&regex);
            sqfs_inode inode;
            if(sqfs_inode_get(fs, &inode, trv.entry.inode))
                fprintf(stderr, "sqfs_inode_get error\n");
            if(r == 0){
                // We have a match
                if(verbose)
                    fprintf(stderr, "squash_get_matching_files found: %s\n", trv.path);
                g_ptr_array_add(array, g_strdup(trv.path));
                if(verbose)
                    fprintf(stderr, "desktop_icon_value_original: %s\n", desktop_icon_value_original);
                gchar *dest = NULL;
                gchar *dest_dirname;
                gchar *dest_basename;
                if(inode.base.inode_type == SQUASHFS_REG_TYPE) {
                    if(g_str_has_prefix (trv.path, "usr/share/icons/") || g_str_has_prefix (trv.path, "usr/share/pixmaps/") ||(g_str_has_prefix(trv.path, "usr/share/mime/")) && (g_str_has_suffix(trv.path, ".xml"))){
                        dest_dirname = g_path_get_dirname(replace_str(trv.path, "usr/share", g_get_user_data_dir()));          
                        dest_basename = g_strdup_printf("%s_%s_%s", vendorprefix, md5, g_path_get_basename(trv.path));
                        dest = g_build_path("/", dest_dirname, dest_basename, NULL);
                    }
                    /* According to https://specifications.freedesktop.org/icon-theme-spec/icon-theme-spec-latest.html#install_icons
                     * share/pixmaps is ONLY searched in /usr but not in $XDG_DATA_DIRS and hence $HOME and this seems to be true at least in XFCE */
                    if(g_str_has_prefix (trv.path, "usr/share/pixmaps/")){
                        dest = g_build_path("/", g_get_home_dir(), ".icons", dest_basename, NULL);                           
                    }
                    /* Some AppImages only have the icon in their root directory, so we have to get it from there */
                    if((g_str_has_prefix(trv.path, desktop_icon_value_original)) && (! strstr(trv.path, "/")) && ( (g_str_has_suffix(trv.path, ".png")) || (g_str_has_suffix(trv.path, ".xpm")) || (g_str_has_suffix(trv.path, ".svg")) || (g_str_has_suffix(trv.path, ".svgz")))){
                        dest_basename = g_strdup_printf("%s_%s_%s.%s", vendorprefix, md5, desktop_icon_value_original, get_file_extension(trv.path));
                        dest = g_build_path("/", g_get_home_dir(), ".icons", dest_basename, NULL);                           
                    }
                    
                    if(dest){
                        if(verbose)
                            fprintf(stderr, "install: %s\n", dest);
                        if(g_mkdir_with_parents(g_path_get_dirname(dest), 0755))
                            fprintf(stderr, "Could not create directory: %s\n", g_path_get_dirname(dest));
                        
                        // Read the file in chunks
                        off_t bytes_already_read = 0;
                        sqfs_off_t bytes_at_a_time = 64*1024;
                        FILE * f;
                        f = fopen (dest, "w+");
                        if (f == NULL){
                            fprintf(stderr, "fopen error\n");
                            break;
                        }
                        while (bytes_already_read < inode.xtra.reg.file_size)
                        {
                            char buf[bytes_at_a_time];
                            if (sqfs_read_range(fs, &inode, (sqfs_off_t) bytes_already_read, &bytes_at_a_time, buf))
                                fprintf(stderr, "sqfs_read_range error\n");
                            fwrite(buf, 1, bytes_at_a_time, f);
                            bytes_already_read = bytes_already_read + bytes_at_a_time;
                        }
                        fclose(f);
                        chmod (dest, 0644);
                        
                        if(verbose)
                            fprintf(stderr, "Installed: %s\n", dest);
                    }
                }
            }
        }
    }
    g_ptr_array_add(array, NULL);
    if (err)
        fprintf(stderr, "sqfs_traverse_next error\n");
    sqfs_traverse_close(&trv);
    return (gchar **) g_ptr_array_free(array, FALSE);
}

/* Loads a desktop file from squashfs into an empty GKeyFile structure.
 * FIXME: Use sqfs_lookup_path() instead of g_key_file_load_from_squash()
 * should help for performance. Please submit a pull request if you can
 * get it to work.
 */
gboolean g_key_file_load_from_squash(sqfs *fs, char *path, GKeyFile *key_file_structure, gboolean verbose) {
    sqfs_traverse trv;
    sqfs_err err = SQFS_OK;
    gboolean success;
    if (err = sqfs_traverse_open(&trv, fs, sqfs_inode_root(fs)))
        fprintf(stderr, "sqfs_traverse_open error\n");
    while (sqfs_traverse_next(&trv, &err)) {
        if (!trv.dir_end) {
            if (strcmp(path, trv.path) == 0){
                sqfs_inode inode;
                if (sqfs_inode_get(fs, &inode, trv.entry.inode))
                    fprintf(stderr, "sqfs_inode_get error\n");
                if (inode.base.inode_type == SQUASHFS_REG_TYPE){
                    off_t bytes_already_read = 0;
                    sqfs_off_t max_bytes_to_read = 256*1024;
                    char buf[max_bytes_to_read];
                    if (sqfs_read_range(fs, &inode, (sqfs_off_t) bytes_already_read, &max_bytes_to_read, buf))
                        fprintf(stderr, "sqfs_read_range error\n");
                    // fwrite(buf, 1, max_bytes_to_read, stdout);
                    success = g_key_file_load_from_data (key_file_structure, buf, max_bytes_to_read, G_KEY_FILE_KEEP_COMMENTS | G_KEY_FILE_KEEP_TRANSLATIONS, NULL);
                } else {
                    fprintf(stderr, "TODO: Implement inode.base.inode_type %i\n", inode.base.inode_type);
                }
                break;
            }
        }
    }
    
    if (err)
        fprintf(stderr, "sqfs_traverse_next error\n");
    sqfs_traverse_close(&trv);
    
    return success;
}

/* Write a modified desktop file to disk that points to the AppImage */
void write_edited_desktop_file(GKeyFile *key_file_structure, char* appimage_path, gchar* desktop_filename, char *md5, gboolean verbose){
    if(!g_key_file_has_key(key_file_structure, G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_EXEC, NULL)){
        fprintf(stderr, "Desktop file has no Exec key\n");
        return;
    }
    g_key_file_set_value(key_file_structure, G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_EXEC, appimage_path);
    gchar *tryexec_path = replace_str(appimage_path," ", "\\ "); // TryExec does not support blanks
    g_key_file_set_value(key_file_structure, G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_TRY_EXEC, appimage_path);

    /* If firejail is on the $PATH, then use it to run AppImages */
    if(g_find_program_in_path ("firejail")){
        char *firejail_exec;
        firejail_exec = g_strdup_printf("firejail --env=DESKTOPINTEGRATION=appimaged --noprofile --appimage '%s'", appimage_path);
        g_key_file_set_value(key_file_structure, G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_EXEC, firejail_exec);

        gchar *firejail_profile_group = "Desktop Action FirejailProfile";
        gchar *firejail_profile_exec = g_strdup_printf("firejail --env=DESKTOPINTEGRATION=appimaged --private --appimage '%s'", appimage_path);
        gchar *firejail_tryexec = "firejail";
        g_key_file_set_value(key_file_structure, firejail_profile_group, G_KEY_FILE_DESKTOP_KEY_NAME, "Run without sandbox profile");
        g_key_file_set_value(key_file_structure, firejail_profile_group, G_KEY_FILE_DESKTOP_KEY_EXEC, firejail_profile_exec);
        g_key_file_set_value(key_file_structure, firejail_profile_group, G_KEY_FILE_DESKTOP_KEY_TRY_EXEC, firejail_tryexec);
        g_key_file_set_value(key_file_structure, G_KEY_FILE_DESKTOP_GROUP, "Actions", "FirejailProfile;");
            
    }
    
    /* Add AppImageUpdate desktop action
     * https://specifications.freedesktop.org/desktop-entry-spec/latest/ar01s10.html 
     * This will only work if AppImageUpdate is on the user's $PATH.
     * TODO: we could have it call this appimaged instance instead of AppImageUpdate and let it
     * figure out how to update the AppImage */
    if(g_find_program_in_path ("AppImageUpdate")){
        unsigned long offset = 0;
        unsigned long length = 0;
        get_elf_section_offset_and_lenghth(appimage_path, ".upd_info", &offset, &length);
        fprintf(stderr, ".upd_info offset: %lu\n", offset);
        fprintf(stderr, ".upd_info length: %lu\n", length);
        if(length != 1024)
            fprintf(stderr, "WARNING: .upd_info length is %lu rather than 1024, this might be a bug in the AppImage\n", length);
        FILE *binary;
        char buffer[4];
        if (binary = fopen(appimage_path, "rt"))
        {
            /* Check whether the first three bytes at the offset are not NULL */
            fseek(binary, offset, SEEK_SET);
            fread(buffer, 1, 3, binary);
            buffer[4] = 0;
            fclose(binary);
            if((buffer[0] != 0x00) && (buffer[1] != 0x00) && (buffer[2] != 0x00)){
                gchar *appimageupdate_group = "Desktop Action AppImageUpdate";
                gchar *appimageupdate_exec = g_strdup_printf("%s %s", "AppImageUpdate", appimage_path);
                gchar *appimageupdate_tryexec = "AppImageUpdate";
                g_key_file_set_value(key_file_structure, appimageupdate_group, G_KEY_FILE_DESKTOP_KEY_NAME, "Update");
                g_key_file_set_value(key_file_structure, appimageupdate_group, G_KEY_FILE_DESKTOP_KEY_EXEC, appimageupdate_exec);
                g_key_file_set_value(key_file_structure, appimageupdate_group, G_KEY_FILE_DESKTOP_KEY_TRY_EXEC, appimageupdate_tryexec);
                g_key_file_set_value(key_file_structure, G_KEY_FILE_DESKTOP_GROUP, "Actions", "AppImageUpdate;");
            }
        }
    }
    
    gchar *icon_with_md5 = g_strdup_printf("%s_%s_%s", vendorprefix, md5, g_path_get_basename(g_key_file_get_value(key_file_structure, "Desktop Entry", "Icon", NULL)));
    g_key_file_set_value(key_file_structure, "Desktop Entry", "Icon", icon_with_md5);
    /* At compile time, inject VERSION_NUMBER like this:
     * cc ... -DVERSION_NUMBER=\"$(git describe --tags --always --abbrev=7)\" -..
     */
    gchar *generated_by = g_strdup_printf("Generated by appimaged %s", VERSION_NUMBER);
    g_key_file_set_value(key_file_structure, "Desktop Entry", "X-AppImage-Comment", generated_by);
    g_key_file_set_value(key_file_structure, "Desktop Entry", "X-AppImage-Identifier", md5);
    fprintf(stderr, "Installing desktop file\n");
    if(verbose)
        fprintf(stderr, "%s", g_key_file_to_data(key_file_structure, NULL, NULL));
    
    /* https://specifications.freedesktop.org/menu-spec/menu-spec-latest.html#paths says:
     * 
     * $XDG_DATA_DIRS/applications/
     * When two desktop entries have the same name, the one appearing earlier in the path is used.
     * 
     * --
     * 
     * https://developer.gnome.org/integration-guide/stable/desktop-files.html.en says:
     * 
     * Place this file in the /usr/share/applications directory so that it is accessible
     * by everyone, or in ~/.local/share/applications if you only wish to make it accessible
     * to a single user. Which is used should depend on whether your application is
     * installed systemwide or into a user's home directory. GNOME monitors these directories
     * for changes, so simply copying the file to the right location is enough to register it
     * with the desktop.
     * 
     * Note that the ~/.local/share/applications location is not monitored by versions of GNOME
     * prior to version 2.10 or on Fedora Core Linux, prior to version 2.8.
     * These versions of GNOME follow the now-deprecated vfolder standard,
     * and so desktop files must be installed to ~/.gnome2/vfolders/applications.
     * This location is not supported by GNOME 2.8 on Fedora Core nor on upstream GNOME 2.10
     * so for maximum compatibility with deployed desktops, put the file in both locations.
     * 
     * Note that the KDE Desktop requires one to run kbuildsycoca to force a refresh of the menus.
     * 
     * --
     * 
     * https://specifications.freedesktop.org/menu-spec/menu-spec-latest.html says:
     * 
     * To prevent that a desktop entry from one party inadvertently cancels out
     * the desktop entry from another party because both happen to get the same
     * desktop-file id it is recommended that providers of desktop-files ensure
     * that all desktop-file ids start with a vendor prefix.
     * A vendor prefix consists of [a-zA-Z] and is terminated with a dash ("-").
     * For example, to ensure that GNOME applications start with a vendor prefix of "gnome-",
     * it could either add "gnome-" to all the desktop files it installs
     * in datadir/applications/ or it could install desktop files in a
     * datadir/applications/gnome subdirectory.
     * 
     * --
     * 
     * https://specifications.freedesktop.org/desktop-entry-spec/latest/ape.html says:
     * The desktop file ID is the identifier of an installed desktop entry file.
     * 
     * To determine the ID of a desktop file, make its full path relative
     * to the $XDG_DATA_DIRS component in which the desktop file is installed,
     * remove the "applications/" prefix, and turn '/' into '-'.
     * For example /usr/share/applications/foo/bar.desktop has the desktop file ID
     * foo-bar.desktop.
     * If multiple files have the same desktop file ID, the first one in the
     * $XDG_DATA_DIRS precedence order is used.
     * For example, if $XDG_DATA_DIRS contains the default paths
     * /usr/local/share:/usr/share, then /usr/local/share/applications/org.foo.bar.desktop
     * and /usr/share/applications/org.foo.bar.desktop both have the same desktop file ID
     * org.foo.bar.desktop, but only the first one will be used.
     * 
     * --
     * 
     * https://specifications.freedesktop.org/desktop-entry-spec/latest/ar01s07.html says:
     * 
     * The application must name its desktop file in accordance with the naming
     * recommendations in the introduction section (e.g. the filename must be like
     * org.example.FooViewer.desktop). The application must have a D-Bus service
     * activatable at the well-known name that is equal to the desktop file name
     * with the .desktop portion removed (for our example, org.example.FooViewer).
     * 
     * --
     * 
     * Can it really be that no one thought about having multiple versions of the same
     * application installed? What are we supposed to do if we want
     * a) have desktop files installed by appimaged not interfere with desktop files
     *    provided by the system, i.e., if an application is installed in the system
     *    and the user also installs the AppImage, then both should be available to the user
     * b) both should be D-Bus activatable
     * c) the one installed by appimaged should have an AppImage vendor prefix to make
     *    it easy to distinguish it from system- or upstream-provided ones
     */
    
    /* FIXME: The following is most likely not correct; see the comments above.
     * Open a GitHub issue or send a pull request if you would like to propose asolution. */
    /* TODO: Check for consistency of the id with the AppStream file, if it exists in the AppImage */
    gchar *partial_path = g_strdup_printf("applications/appimagekit_%s-%s", md5, desktop_filename);
    gchar *destination = g_build_filename(g_get_user_data_dir(), partial_path, NULL);
    if(verbose)
        fprintf(stderr, "install: %s\n", destination);
    if(g_mkdir_with_parents(g_path_get_dirname(destination), 0755))
        fprintf(stderr, "Could not create directory: %s\n", g_path_get_dirname(destination));    
    
    
    // g_key_file_save_to_file(key_file_structure, destination, NULL);
    // g_key_file_save_to_file is too new, only since 2.40
    /* Write config file on disk */
    gsize length;
    gchar *buf;
    GIOChannel *file;
    buf = g_key_file_to_data(key_file_structure, &length, NULL);
    file = g_io_channel_new_file(destination, "w", NULL);
    g_io_channel_write_chars(file, buf, length, NULL, NULL);
    g_io_channel_shutdown(file, TRUE, NULL);
    
    
    /* GNOME shows the icon and name on the desktop file only if it is executable */
    chmod(destination, 0755);
}

/* Register a type 2 AppImage in the system */
bool appimage_type2_register_in_system(char *path, gboolean verbose)
{
    long unsigned int fs_offset; // The offset at which a squashfs image is expected
    char *md5 = get_md5(path);
    GKeyFile *key_file_structure = g_key_file_new(); // A structure that will hold the information from the desktop file
    gchar *desktop_icon_value_original = "iDoNotMatchARegex"; // FIXME: otherwise the regex does weird stuff in the first run
    if(verbose)
        fprintf(stderr, "md5 of URI RFC 2396: %s\n", md5);
    fs_offset = get_elf_size(path);
    if(verbose)
        fprintf(stderr, "fs_offset: %lu\n", fs_offset);
    sqfs_err err = SQFS_OK;
    sqfs_traverse trv;
    sqfs fs;
    if (err = sqfs_open_image(&fs, path, fs_offset)){
        fprintf(stderr, "sqfs_open_image error: %s\n", path);
        return FALSE;
    } else {
        if(verbose)
            fprintf(stderr, "sqfs_open_image: %s\n", path);
    }
    
    /* TOOO: Change so that only one run of squash_get_matching_files is needed in total,
     * this should hopefully improve performance */
    
    /* Get desktop file(s) in the root directory of the AppImage */
    gchar **str_array = squash_get_matching_files(&fs, "(^[^/]*?.desktop$)", desktop_icon_value_original, md5, verbose); // Only in root dir
    // gchar **str_array = squash_get_matching_files(&fs, "(^.*?.desktop$)", md5, verbose); // Not only there
    /* Work trough the NULL-terminated array of strings */
    for (int i=0; str_array[i]; ++i) {
        const char *ch = str_array[i]; 
        fprintf(stderr, "Got root desktop: %s\n", str_array[i]);
        gboolean success = g_key_file_load_from_squash(&fs, str_array[i], key_file_structure, verbose);
        if(success){
            gchar *desktop_filename = g_path_get_basename(str_array[i]);
            
            desktop_icon_value_original = g_strdup_printf("%s", g_key_file_get_value(key_file_structure, "Desktop Entry", "Icon", NULL));
            if(verbose)
                fprintf(stderr, "desktop_icon_value_original: %s\n", desktop_icon_value_original);
            write_edited_desktop_file(key_file_structure, path, desktop_filename, md5, verbose);
        }
        g_key_file_free(key_file_structure);
        
    }
    /* Free the NULL-terminated array of strings and its contents */
    g_strfreev(str_array);
    
    /* Get relevant  file(s) */
    gchar **str_array2 = squash_get_matching_files(&fs, "(^usr/share/(icons|pixmaps)/.*.(png|svg|svgz|xpm)$|^.DirIcon$|^usr/share/mime/packages/.*.xml$|^usr/share/appdata/.*metainfo.xml$|^[^/]*?.(png|svg|svgz|xpm)$)", desktop_icon_value_original, md5, verbose);
    
    /* Free the NULL-terminated array of strings and its contents */
    g_strfreev(str_array2);
    
    /* The above also gets AppStream metainfo file(s), TODO: Check if the id matches and do something with them*/
    
    set_executable(path, verbose);
    
    return TRUE;
}

/* Register an AppImage in the system */
int appimage_register_in_system(char *path, gboolean verbose)
{
    if((g_str_has_suffix(path, ".part")) || (g_str_has_suffix(path, ".tmp")) || (g_str_has_suffix(path, ".download")) || (g_str_has_suffix(path, ".zs-old")) || (g_str_has_suffix(path, ".~")))
        return 0;
    int type = check_appimage_type(path, verbose);
    if(type == 1 || type == 2){
        fprintf(stderr, "\n");
        fprintf(stderr, "-> REGISTER %s\n", path);
    }
    /* TODO: Generate thumbnails.
     * Generating proper thumbnails involves more than just copying images out of the AppImage,
     * including checking if the thumbnail already exists and if it's valid 
     * and writing attributes into the thumbnail, see
     * https://specifications.freedesktop.org/thumbnail-spec/thumbnail-spec-latest.html#CREATION */
    if(verbose)
        fprintf(stderr, "get_thumbnail_path: %s\n", get_thumbnail_path(path, "normal", verbose));
    
    if(type == 1){
        appimage_type1_register_in_system(path, verbose);
    }
    
    if(type == 2){
        appimage_type2_register_in_system(path, verbose);
    }
    
    return 0;
}

/* Delete the thumbnail for a given file and size if it exists */
void delete_thumbnail(char *path, char *size, gboolean verbose)
{
    gchar *thumbnail_path = get_thumbnail_path(path, size, verbose);
    if(verbose)
        fprintf(stderr, "get_thumbnail_path: %s\n", thumbnail_path);
    if(g_file_test(thumbnail_path, G_FILE_TEST_IS_REGULAR)){
        g_unlink(thumbnail_path);
        if(verbose)
            fprintf(stderr, "deleted: %s\n", thumbnail_path);
    }
}

/* Recursively delete files in path and subdirectories that contain the given md5
 */
void unregister_using_md5_id(const char *name, int level, char* md5, gboolean verbose)
{
    DIR *dir;
    struct dirent *entry;
    
    if (!(dir = opendir(name)))
        return;
    if (!(entry = readdir(dir)))
        return;
    
    do {
        if (entry->d_type == DT_DIR) {
            char path[1024];
            int len = snprintf(path, sizeof(path)-1, "%s/%s", name, entry->d_name);
            path[len] = 0;
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
                continue;
            unregister_using_md5_id(path, level + 1, md5, verbose);
        }
        
        else if(strstr(entry->d_name, g_strdup_printf("%s_%s", vendorprefix, md5))) {
            gchar *path_to_be_deleted = g_strdup_printf("%s/%s", name, entry->d_name);
            if(g_file_test(path_to_be_deleted, G_FILE_TEST_IS_REGULAR)){
                g_unlink(path_to_be_deleted);
                if(verbose)
                    fprintf(stderr, "deleted: %s\n", path_to_be_deleted);
            }
        }
    } while (entry = readdir(dir));
    closedir(dir);
}


/* Unregister an AppImage in the system */
int appimage_unregister_in_system(char *path, gboolean verbose)
{
    char *md5 = get_md5(path);
    
    /* The file is already gone by now, so we can't determine its type anymore */
    fprintf(stderr, "\n");
    fprintf(stderr, "-> UNREGISTER %s\n", path);
    /* Could use gnome_desktop_thumbnail_factory_lookup instead of the next line */
    
    /* Delete the thumbnails if they exist */
    delete_thumbnail(path, "normal", verbose); // 128x128
    delete_thumbnail(path, "large", verbose); // 256x256
    
    unregister_using_md5_id(g_get_user_data_dir(), 0, md5, verbose);
    
    return 0;
}
