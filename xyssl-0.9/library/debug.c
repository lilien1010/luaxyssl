/*
 *  Debugging routines
 *
 *  Copyright (C) 2006-2007  Christophe Devine
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "xyssl/config.h"

#if defined(XYSSL_DEBUG_C)

#include "xyssl/debug.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#if defined _MSC_VER && !defined  snprintf
#define  snprintf  _snprintf
#endif

#if defined _MSC_VER && !defined vsnprintf
#define vsnprintf _vsnprintf
#endif

char *debug_fmt( const char *format, ... )
{
    va_list argp;
    static char str[512];
    int maxlen = sizeof( str ) - 1;

    va_start( argp, format );
    vsnprintf( str, maxlen, format, argp );
    va_end( argp );

    str[maxlen] = '\0';
    return( str );
}

void debug_print_msg( ssl_context *ssl, int level,
                      char *file, int line, char *text )
{
    char str[512];
    int maxlen = sizeof( str ) - 1;

    if( ssl->f_dbg == NULL )
        return;

    snprintf( str, maxlen, "%s(%04d): %s\n", file, line, text );
    str[maxlen] = '\0';
    ssl->f_dbg( ssl->p_dbg, level, str );
}

void debug_print_ret( ssl_context *ssl, int level,
                      char *file, int line, char *text, int ret )
{
    char str[512];
    int maxlen = sizeof( str ) - 1;

    if( ssl->f_dbg == NULL )
        return;

    snprintf( str, maxlen, "%s(%04d): %s() returned %d (0x%x)\n",
              file, line, text, ret, ret );

    str[maxlen] = '\0';
    ssl->f_dbg( ssl->p_dbg, level, str );
}

void debug_print_buf( ssl_context *ssl, int level,
                      char *file, int line, char *text,
                      unsigned char *buf, int len )
{
    char str[512];
    int i, maxlen = sizeof( str ) - 1;
    int l;

    if( ssl->f_dbg == NULL || len < 0 )
        return;

    snprintf( str, maxlen, "%s(%04d): dumping '%s' (%d bytes)\n",
              file, line, text, len );

    str[maxlen] = '\0';
    ssl->f_dbg( ssl->p_dbg, level, str );

    str[0] = '\0';
    for( i = 0; i < len; i++ )
    {
        if( i >= 4096 )
            break;

        if( i % 16 == 0 )
        {
            if( i > 0 ) {
                snprintf(str+l,maxlen - l, "\n");
                str[maxlen] = '\0';
                ssl->f_dbg( ssl->p_dbg, level, str );
                }

            str[0]='\0';
            snprintf( str, maxlen, "%s(%04d): %04x: ", file, line, i );
            str[maxlen] = '\0';
            l = strlen(str);
        }

        snprintf( str+l, maxlen-l, " %02x", (unsigned int) buf[i] );
        l+=3;
    }

    if (len > 0) {
      int l = strlen(str);
      snprintf(str+l,maxlen - l, "\n");
      str[maxlen] = '\0';
      ssl->f_dbg( ssl->p_dbg, level, str );
      }
}

void debug_print_mpi( ssl_context *ssl, int level,
                      char *file, int line, char *text, mpi *X )
{
    char str[512];
    int i, j, k, n, l, maxlen = sizeof( str ) - 1;
    int block_break;

    if( ssl->f_dbg == NULL || X == NULL )
        return;

    for( n = X->n - 1; n >= 0; n-- )
        if( X->p[n] != 0 )
            break;

    snprintf( str, maxlen, "%s(%04d): value of '%s' (%d bits) is:\n",
              file, line, text, ((n + 1) * sizeof( t_int )) << 3 );

    block_break = 16/sizeof(t_int);
    str[maxlen] = '\0';
    ssl->f_dbg( ssl->p_dbg, level, str );

    str[0] = '\0';
    for( i = n, j = 0; i >= 0; i--, j++ )
    {
        if( j % ( 16 / sizeof( t_int ) ) == 0 )
        {
            if( j > 0 ) {
                snprintf(str+l,maxlen - l, "\n");
                str[maxlen] = '\0';
                ssl->f_dbg( ssl->p_dbg, level, str);
            }

            str[0]='\0';
            snprintf( str, maxlen, "%s(%04d): %04x: ", file, line, j*block_break);
            str[maxlen] = '\0';
            l = strlen(str);
        }

        for( k = sizeof( t_int ) - 1; k >= 0; k-- )
        {
          snprintf( str+l, maxlen-l," %02x", ( X->p[i] >> (k << 3) ) & 0xFF );
          l+=3;
        }
    }
    snprintf(str+l,maxlen - l, "\n");
    ssl->f_dbg( ssl->p_dbg, level, str );
}

void debug_print_crt( ssl_context *ssl, int level,
                      char *file, int line, char *text, x509_cert *crt )
{
    char str[512], prefix[64], *p;
    int i = 0, maxlen = sizeof( prefix ) - 1;

    if( ssl->f_dbg == NULL || crt == NULL )
        return;

    snprintf( prefix, maxlen, "%s(%04d): ", file, line );
    prefix[maxlen] = '\0';
    maxlen = sizeof( str ) - 1;

    while( crt != NULL && crt->next != NULL )
    {
        p = x509parse_cert_info( prefix, crt );

        snprintf( str, maxlen, "%s(%04d): %s #%d:\n%s",
                  file, line, text, ++i, p );

        str[maxlen] = '\0';
        ssl->f_dbg( ssl->p_dbg, level, str );

        debug_print_mpi( ssl, level, file, line,
                         "crt->rsa.N", &crt->rsa.N );

        debug_print_mpi( ssl, level, file, line,
                         "crt->rsa.E", &crt->rsa.E );

        crt = crt->next;
    }
}

#endif
