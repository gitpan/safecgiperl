/*
 * Copyright (C) Malcolm Beattie 1995
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <pwd.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <errno.h>
#include <syslog.h>

#define MIN_USER_UID 100
#define MAX_USER_UID 30000
#define CGI_PREFIX "/cgi/bin/"
#define CGIPERL_PATH "/usr/local/bin/cgiperl"

#define CPU_LIMIT	30
#define DATA_LIMIT	(64*1024*1024)
#define CORE_LIMIT	0
#define RSS_LIMIT	(16*1024*1024)
#define CGI_NICE	2

char *usage = "URL must look like .../cgi-bin/safeperl/username/filename/...";
char *env_info = "PATH_INFO=";

void die(char *str)
{
    printf("Content-type: text/html\n\n");
    printf("<HEAD><TITLE>500 %s</TITLE></HEAD>\n", str);
    printf("<BODY><H2>Error code 500</H2>\n%s\n</BODY>\n", str);
    exit(1);
}

char *clone(char *str, size_t len)
{
    char *newstr = (char *) malloc(len + 1);
    
    if (!newstr)
	die("Out of memory");
    strncpy(newstr, str, len);
    newstr[len] = '\0';
    return newstr;
}

char *unescape(char *str)
{
    char buffer[3], *s, *t;
    char c;
    
    if (!str)
	return 0;
    
    s = t = str;
    while (*s)
    {
	if (s[0] == '%' && s[1] && isxdigit(s[1]) && s[2] && isxdigit(s[2]))
	{
	    buffer[0] = s[1];
	    buffer[1] = s[2];
	    buffer[3] = '\0';
	    c = (char) strtol(buffer, NULL, 16);
	    if (!c)
		die("escaped NUL not allowed");
	    *t++ = c;
	    s += 3;
	}
	else
	    *t++ = *s++;
    }
    *t = '\0';
    return str;
}	

void limit(int resource, int lim)
{
    struct rlimit rl;
    
    /*
     * Set soft and hard limits to lim. We don't mind failing if
     * we're already below the limit. EPERM indicates this case.
     */
    rl.rlim_cur = rl.rlim_max = lim;
    if (setrlimit(resource, &rl) < 0 && errno != EPERM)
	die("setrlimit failed unexpectedly");
}

int main(int argc, char **argv)
{
    char *path_info, *cp, *username, *filename, *extra, *program;
    struct passwd *pw;

    openlog("safeperl", LOG_PID, LOG_USER);

    /* 
     * The format of an URL passed to us should be
     * .../cgi-bin/safeperl/username/filename/...
     * The file we then look for is ~username/cgi/bin/filename
     * (unless CGI_PREFIX has been defined as something else).
     * Since this program is safeperl, the stuff from /username
     * onwards gets passed to us in PATH_INFO.
     */
    path_info = getenv("PATH_INFO");
    if (!path_info)
	die("PATH_INFO not set");
    if (path_info[0] != '/')
	die(usage);

    /* Parse the username */
    username = path_info + 1;
    cp = strchr(username, '/');
    if (!cp)
	die(usage);
    username = unescape(clone(username, cp - username));

    /* Parse the filename */
    filename = cp + 1;
    cp = strchr(filename, '/');
    if (!cp)
    {
	filename = unescape(clone(filename, strlen(filename)));
	extra = env_info;
    }
    else
    {
	filename = unescape(clone(filename, cp - filename));
	/* Parse the remaining stuff */
	extra = clone(env_info, strlen(env_info) + strlen(cp + 1));
	strcat(extra, cp + 1);	/* guaranteed room */
    }
    
    if (strchr(filename, '/'))
	die("safeperl filename may not have escaped slashes embedded");

    /* Put the altered PATH_INFO into the environment */
    if (putenv(extra))
	die("Failed to alter PATH_INFO in environment");

    /* Get and check the username requested */
    pw = getpwnam(username);
    if (!pw || pw->pw_uid < MIN_USER_UID || pw->pw_uid > MAX_USER_UID)
	die("No such username");

    /* Make room for string /homedirectory/public_html/cgi-bin/filename */
    program = clone(pw->pw_dir,
		    strlen(pw->pw_dir) + strlen(CGI_PREFIX)+ strlen(filename));
    strcat(program, CGI_PREFIX);	/* guaranteed room */
    strcat(program, filename);		/* guaranteed room */
    
    syslog(LOG_INFO, "Running cgiperl %s as user %s", program, pw->pw_name);
    closelog();

    /* Now become the user */
    if (setgid(pw->pw_gid) < 0)
	die("setgid failed");
    if (initgroups(pw->pw_name, pw->pw_gid))
	die("initgroups failed");
    if (setuid(pw->pw_uid) < 0)
	die("setuid failed");
    if (chdir(pw->pw_dir) < 0)
	die("failed to chdir to home directory");
    
    /* Ensure low resource limits */
    limit(RLIMIT_CPU, CPU_LIMIT);
    limit(RLIMIT_DATA, DATA_LIMIT);
    limit(RLIMIT_CORE, CORE_LIMIT);
    limit(RLIMIT_RSS, RSS_LIMIT);
    (void) setpriority(PRIO_PROCESS, 0, CGI_NICE);

    /*
     * Off we go. We do not make any attempt to close file
     * descriptors or do similar clean-up stuff. Looking after
     * that is the responsibility of cgiperl.
     */
    execl(CGIPERL_PATH, CGIPERL_PATH, program, (char *) 0);
    die("Failed to execl cgiperl");
    /* NOTREACHED */
}
