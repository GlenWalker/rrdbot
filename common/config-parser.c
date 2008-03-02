/*
 * Copyright (c) 2005, Stefan Walter
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *     * Redistributions of source code must retain the above
 *       copyright notice, this list of conditions and the
 *       following disclaimer.
 *     * Redistributions in binary form must reproduce the
 *       above copyright notice, this list of conditions and
 *       the following disclaimer in the documentation and/or
 *       other materials provided with the distribution.
 *     * The names of contributors to this software may not be
 *       used to endorse or promote products derived from this
 *       software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 *
 *
 * CONTRIBUTORS
 *  Stef Walter <stef@memberwebs.com>
 */

#include "usuals.h"

#include "config-parser.h"

#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include <syslog.h>
#include <stdarg.h>
#include <dirent.h>
#include <sys/stat.h>

#include <bsnmp/asn1.h>
#include <bsnmp/snmp.h>

static void
errmsg(const char* filename, void* data, const char* msg, ...)
{
    #define MAX_MSGLEN  1024
    char buf[MAX_MSGLEN];
    va_list ap;

    va_start(ap, msg);
    vsnprintf(buf, MAX_MSGLEN, msg, ap);
    buf[MAX_MSGLEN - 1] = 0;
    cfg_error(filename, buf, data);
    va_end(ap);
}

/* -----------------------------------------------------------------------------
 * CONFIG PARSER
 */

static char*
read_config_file(const char** configfile, void* data)
{
    char* config = NULL;
    char* newfilename;
    FILE* f = NULL;
    long len;
    int flen;

    ASSERT(configfile);

    f = fopen(*configfile, "r");
    if(f == NULL)
    {
        errmsg(*configfile, data, "couldn't open config file: %s", *configfile);
        return NULL;
    }

    /* Figure out size */
    if(fseek(f, 0, SEEK_END) == -1 || (len = ftell(f)) == -1 || fseek(f, 0, SEEK_SET) == -1)
    {
        errmsg(*configfile, data, "couldn't seek config file: %s", *configfile);
        return NULL;
    }

    flen = strlen(*configfile);
    if((config = (char*)malloc(len + 4 + flen)) == NULL)
    {
        errmsg(*configfile, data, "out of memory");
        return NULL;
    }

    /* And read in one block */
    if(fread(config, 1, len, f) != len)
    {
        errmsg(*configfile, data, "couldn't read config file: %s", *configfile);
        return NULL;
    }

    fclose(f);

    /* Null terminate the data */
    config[len] = '\n';
    config[len + 1] = 0;

    /* Remove nasty dos line endings */
    strcln(config, '\r');

    /* Persistent allocation for filename */
    newfilename = config + len + 2;
    strcpy(newfilename, *configfile);
    *configfile = newfilename;

    return config;
}

int
cfg_parse_file(const char* filename, void* data, char** memory)
{
    char* name = NULL;
    char* value = NULL;
    char* config;
    char* next;
    char* header;
    int ret = -1;
    char* p;
    char* t;

    ASSERT(filename);

    config = read_config_file(&filename, data);
    if(!config)
        goto finally;

    next = config;

    /* Go through lines and process them */
    while((t = strchr(next, '\n')) != NULL)
    {
        *t = 0;
        p = next; /* Do this before cleaning below */
        next = t + 1;

        t = strbtrim(p);

        /* Continuation line (had spaces at start) */
        if(p < t && *t)
        {
            if(!value)
            {
                errmsg(filename, data, "%s: invalid continuation in config: %s",
                       filename, p);
                goto finally;
            }

            /* Calculate the end of the current value */
            t = value + strlen(value);
            ASSERT(t < p);

            /* Continuations are separated by spaces */
            *t = ' ';
            t++;

            continue;
        }

        /* No continuation hand off value if necessary */
        if(name && value)
        {
            if(cfg_value(filename, header, name, strtrim(value), data) == -1)
                goto finally;
        }

        name = NULL;
        value = NULL;

        /* Empty lines / comments at start / comments without continuation */
        if(!*t || *p == '#')
            continue;

        /* A header */
        if(*p == '[')
        {
            t = p + strcspn(p, "]");
            if(!*t || t == p + 1)
            {
                errmsg(filename, data, "%s: invalid config header: %s",
                       filename, p);
                goto finally;
            }

            *t = 0;
            header = strtrim(p + 1);
            continue;
        }

        /* Look for the break between name = value on the same line */
        t = p + strcspn(p, ":=");
        if(!*t)
        {
            errmsg(filename, data, "%s: invalid config line: %s",
                   filename, p);
            goto finally;
        }

        /* Null terminate and split value part */
        *t = 0;
        t++;

        name = strtrim(p);
        value = t;
    }

    if(name && value)
    {
        if(cfg_value(filename, header, name, value, data) == -1)
            goto finally;
    }

    ret = 0;


finally:

    if(!memory || ret != 0)
        free(config);
    else if(memory)
        *memory = config;

    return ret;
}

static int
parse_dir_internal(const char* subdir, void* data)
{
    char path[MAXPATHLEN];
    struct dirent* dire;
    struct stat st;
    char* memory;
    DIR* dir;
    int r;

    /* Open specified or current directory */
    dir = opendir(subdir ? subdir : ".");
    if(!dir)
    {
        errmsg(NULL, data, "couldn't list config directory: %s",
               subdir ? subdir : ".");
        return -1;
    }

    while((dire = readdir(dir)) != NULL)
    {
        /* Build a file path to this entry */
        if(subdir)
        {
            strlcpy(path, subdir, MAXPATHLEN);
            strlcat(path, "/", MAXPATHLEN);
            strlcat(path, dire->d_name, MAXPATHLEN);
        }
        else
            strlcpy(path, dire->d_name, MAXPATHLEN);

        /* for non BSD compliant filesystem: stat() only if dirent->d_type is unknown */
        if(dire->d_type == DT_UNKNOWN)
        {
            if(stat(path, &st) < 0)
            {
                errmsg(NULL, data, "couldn't stat path: %s", path);
                return -1;
            }

            if(S_ISREG(st.st_mode))
                dire->d_type = DT_REG;
            else if(S_ISDIR(st.st_mode))
                dire->d_type = DT_DIR;
            else
                continue;
        }

        /* Descend into each sub directory */
        if(dire->d_type == DT_DIR)
        {
            /* No hidden or dot directories */
            if(dire->d_name[0] == '.')
                continue;

            r = parse_dir_internal(path, data);
            if(r < 0)
                return r;

            continue;
        }

        if(dire->d_type != DT_REG && dire->d_type != DT_LNK)
            continue;

        /* Build a happy path name */
        cfg_parse_file(path, data, &memory);

        /* We call it with blanks after files */
        if(cfg_value(path, NULL, NULL, NULL, data) == -1)
            break;

        /* Keep the memory around */
        if(memory)
            atexitv(free, memory);
    }

    closedir(dir);

    return 0;
}

int
cfg_parse_dir(const char* dirname, void* data)
{
    char olddir[MAXPATHLEN];
    int ret;

    ASSERT(dirname != NULL);

    if(!getcwd(olddir, MAXPATHLEN))
        olddir[0] = 0;

    if(chdir(dirname) == -1)
    {
        errmsg(NULL, data, "couldn't list config directory: %s", dirname);
        return -1;
    }

    ret = parse_dir_internal(NULL, data);

    if(olddir[0])
        chdir(olddir);

    return ret;
}

const char*
cfg_parse_uri (char *uri, char** scheme, char** host, char** user,
               char** path, char** query)
{
    char* t;

    *scheme = NULL;
    *host = NULL;
    *user = NULL;
    *path = NULL;
    *query = NULL;

    *scheme = strsep(&uri, ":");
    if(uri == NULL || (uri[0] != '/' && uri[1] != '/'))
        return "invalid uri";

    uri += 2;
    *host = strsep(&uri, "/");
    if(*host[0])
    {
        /* Parse the community out from the host */
        t = strchr(*host, '@');
        if(t)
        {
            *t = 0;
            *user = *host;
            *host = t + 1;
        }
    }

    if(!*host[0])
        return "invalid uri: no host name found";

    if(!uri || !uri[0])
        return "invalid uri: no path found";

    *path = uri;

    while((*path)[0] == '/')
        (*path)++;

    *query = strchr(*path, '?');
    if(*query)
    {
        *(*query) = 0;
        (*query)++;
    }

    return NULL;
}

#define CONFIG_SNMP "snmp"
#define CONFIG_SNMP2 "snmp2"
#define CONFIG_SNMP2C "snmp2c"

/* Parsing snmp, snmp2 snmp2c etc... */
const char*
cfg_parse_scheme(const char *str, enum snmp_version *scheme)
{
    /* Currently we only support SNMP pollers */
    if(strcmp(str, CONFIG_SNMP) == 0)
        *scheme = SNMP_V1;
    else if(strcmp(str, CONFIG_SNMP2) == 0)
        *scheme = SNMP_V2c;
    else if(strcmp(str, CONFIG_SNMP2C) == 0)
        *scheme = SNMP_V2c;
    else
        return "invalid scheme";
    return NULL;
}

const char*
cfg_parse_query (char *query, char **name, char **value, char **remainder)
{
	const char *msg;
	char *x;

	*remainder = NULL;

	if (*query == '&' || *query == '?')
		query++;

	/* Only use the first argument */
	x = strchr (query, '&');
	if (x) {
		*x = 0;
		*remainder = x + 1;
	}

	/* Parse into argument and value */
	*value = strchr (query, '=');
	if (*value) {
		*(*value) = 0;
		(*value)++;
		msg = cfg_parse_url_decode (*value);
		if (msg)
			return msg;
	}

	*name = query;
	return NULL;
}

const static char HEX_CHARS[] = "0123456789abcdef";

const char*
cfg_parse_url_decode (char *value)
{
	char *p, *a, *b;

	/* Change all plusses to spaces */
	for (p = strchr (value, '+'); p; p = strchr (p, '+'))
		*p = ' ';

	/* Now loop through looking for escapes */
	p = value;
	while (*value) {
		/*
		 * A percent sign followed by two hex digits means
		 * that the digits represent an escaped character.
		 */
		if (*value == '%') {
			value++;
			a = strchr (HEX_CHARS, tolower(value[0]));
			b = strchr (HEX_CHARS, tolower(value[1]));
			if (a && b) {
				*p = (a - HEX_CHARS) << 4;
				*(p++) |= (b - HEX_CHARS);
				value += 2;
				continue;
			}
		}

		*(p++) = *(value++);
	}

	*p = 0;
	return NULL;
}
