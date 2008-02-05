/*****************************************************************************
 * mmsh.c:
 *****************************************************************************
 * Copyright (C) 2001, 2002 the VideoLAN team
 * $Id$
 *
 * Authors: Laurent Aimar <fenrir@via.ecp.fr>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

/*****************************************************************************
 * Preamble
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <vlc/vlc.h>
#include <vlc_access.h>
#include "vlc_playlist.h"
#include "vlc_strings.h"

#include <vlc_network.h>
#include "vlc_url.h"
#include "asf.h"
#include "buffer.h"

#include "mms.h"
#include "mmsh.h"

/* TODO:
 *  - authentication
 */

/*****************************************************************************
 * Local prototypes
 *****************************************************************************/
int  E_(MMSHOpen)  ( access_t * );
void E_(MMSHClose) ( access_t * );

static ssize_t Read( access_t *, uint8_t *, size_t );
static ssize_t ReadRedirect( access_t *, uint8_t *, size_t );
static int  Seek( access_t *, int64_t );
static int  Control( access_t *, int, va_list );

static int  Describe( access_t  *, char **ppsz_location );
static int  Start( access_t *, int64_t );
static void Stop( access_t * );

static int  GetPacket( access_t *, chunk_t * );
static void GetHeader( access_t *p_access );

static int Restart( access_t * );
static int Reset( access_t * );

//#define MMSH_USER_AGENT "NSPlayer/4.1.0.3856"
#define MMSH_USER_AGENT "NSPlayer/7.10.0.3059"

/****************************************************************************
 * Open: connect to ftp server and ask for file
 ****************************************************************************/
int E_(MMSHOpen)( access_t *p_access )
{
    access_sys_t    *p_sys;
    char            *psz_location = NULL;
    char            *psz_proxy;

    /* init p_sys */

    /* Set up p_access */
    p_access->pf_read = Read;
    p_access->pf_block = NULL;
    p_access->pf_control = Control;
    p_access->pf_seek = Seek;
    p_access->info.i_update = 0;
    p_access->info.i_size = 0;
    p_access->info.i_pos = 0;
    p_access->info.b_eof = VLC_FALSE;
    p_access->info.i_title = 0;
    p_access->info.i_seekpoint = 0;

    p_access->p_sys = p_sys = malloc( sizeof( access_sys_t ) );
    if( !p_sys )
        return VLC_ENOMEM;

    memset( p_sys, 0, sizeof( access_sys_t ) );
    p_sys->i_proto= MMS_PROTO_HTTP;
    p_sys->fd     = -1;
    p_sys->i_start= 0;

    /* Handle proxy */
    p_sys->b_proxy = VLC_FALSE;
    memset( &p_sys->proxy, 0, sizeof(p_sys->proxy) );

    /* Check proxy */
    /* TODO reuse instead http-proxy from http access ? */
    psz_proxy = var_CreateGetString( p_access, "mmsh-proxy" );
    if( !*psz_proxy )
    {
        char *psz_http_proxy = config_GetPsz( p_access, "http-proxy" );
        if( psz_http_proxy && *psz_http_proxy )
        {
            free( psz_proxy );
            psz_proxy = psz_http_proxy;
            var_SetString( p_access, "mmsh-proxy", psz_proxy );
        }
        else
        {
            free( psz_http_proxy );
        }
    }

    if( *psz_proxy )
    {
        p_sys->b_proxy = VLC_TRUE;
        vlc_UrlParse( &p_sys->proxy, psz_proxy, 0 );
    }
#ifdef HAVE_GETENV
    else
    {
        char *psz_proxy = getenv( "http_proxy" );
        if( psz_proxy && *psz_proxy )
        {
            p_sys->b_proxy = VLC_TRUE;
            vlc_UrlParse( &p_sys->proxy, psz_proxy, 0 );
        }
    }
#endif
    free( psz_proxy );

    if( p_sys->b_proxy )
    {
        if( ( p_sys->proxy.psz_host == NULL ) ||
            ( *p_sys->proxy.psz_host == '\0' ) )
        {
            msg_Warn( p_access, "invalid proxy host" );
            vlc_UrlClean( &p_sys->proxy );
            free( p_sys );
            return VLC_EGENERIC;
        }

        if( p_sys->proxy.i_port <= 0 )
            p_sys->proxy.i_port = 80;
        msg_Dbg( p_access, "Using http proxy %s:%d",
                 p_sys->proxy.psz_host, p_sys->proxy.i_port );
    }

    /* open a tcp connection */
    vlc_UrlParse( &p_sys->url, p_access->psz_path, 0 );
    if( ( p_sys->url.psz_host == NULL ) ||
        ( *p_sys->url.psz_host == '\0' ) )
    {
        msg_Err( p_access, "invalid host" );
        vlc_UrlClean( &p_sys->proxy );
        vlc_UrlClean( &p_sys->url );
        free( p_sys );
        return VLC_EGENERIC;
    }
    if( p_sys->url.i_port <= 0 )
        p_sys->url.i_port = 80;

    if( Describe( p_access, &psz_location ) )
    {
        vlc_UrlClean( &p_sys->proxy );
        vlc_UrlClean( &p_sys->url );
        free( p_sys );
        return VLC_EGENERIC;
    }
    /* Handle redirection */
    if( psz_location && *psz_location )
    {
        playlist_t * p_playlist = pl_Yield( p_access );
        msg_Dbg( p_access, "redirection to %s", psz_location );

        /** \bug we do not autodelete here */
        playlist_Add( p_playlist, psz_location, psz_location,
                      PLAYLIST_INSERT | PLAYLIST_GO, PLAYLIST_END, VLC_TRUE,
                      VLC_FALSE );
        vlc_object_release( p_playlist );

        free( psz_location );

        p_access->pf_read = ReadRedirect;
        return VLC_SUCCESS;
    }

    /* Start playing */
    if( Start( p_access, 0 ) )
    {
        msg_Err( p_access, "cannot start stream" );
        free( p_sys->p_header );
        vlc_UrlClean( &p_sys->proxy );
        vlc_UrlClean( &p_sys->url );
        free( p_sys );
        return VLC_EGENERIC;
    }

    if( !p_sys->b_broadcast )
    {
        p_access->info.i_size = p_sys->asfh.i_file_size;
    }

    return VLC_SUCCESS;
}

/*****************************************************************************
 * Close: free unused data structures
 *****************************************************************************/
void E_( MMSHClose )( access_t *p_access )
{
    access_sys_t *p_sys = p_access->p_sys;

    Stop( p_access );

    if( p_sys->p_header )
        free( p_sys->p_header  );

    vlc_UrlClean( &p_sys->proxy );
    vlc_UrlClean( &p_sys->url );
    free( p_sys );
}

/*****************************************************************************
 * Control:
 *****************************************************************************/
static int Control( access_t *p_access, int i_query, va_list args )
{
    access_sys_t *p_sys = p_access->p_sys;
    vlc_bool_t   *pb_bool;
    int          *pi_int;
    int64_t      *pi_64;
    int          i_int;

    switch( i_query )
    {
        /* */
        case ACCESS_CAN_SEEK:
            pb_bool = (vlc_bool_t*)va_arg( args, vlc_bool_t* );
            *pb_bool = !p_sys->b_broadcast;
            break;

        case ACCESS_CAN_FASTSEEK:
        case ACCESS_CAN_PAUSE:
            pb_bool = (vlc_bool_t*)va_arg( args, vlc_bool_t* );
            *pb_bool = VLC_FALSE;
            break;

        case ACCESS_CAN_CONTROL_PACE:
            pb_bool = (vlc_bool_t*)va_arg( args, vlc_bool_t* );
            *pb_bool = VLC_TRUE;
            break;

        /* */
        case ACCESS_GET_MTU:
            pi_int = (int*)va_arg( args, int * );
            *pi_int = 3 * p_sys->asfh.i_min_data_packet_size;
            break;

        case ACCESS_GET_PTS_DELAY:
            pi_64 = (int64_t*)va_arg( args, int64_t * );
            *pi_64 = (int64_t)var_GetInteger( p_access, "mms-caching" ) * I64C(1000);
            break;

        case ACCESS_GET_PRIVATE_ID_STATE:
            i_int = (int)va_arg( args, int );
            pb_bool = (vlc_bool_t *)va_arg( args, vlc_bool_t * );

            if( (i_int < 0) || (i_int > 127) )
                return VLC_EGENERIC;
            *pb_bool =  p_sys->asfh.stream[i_int].i_selected ? VLC_TRUE : VLC_FALSE;
            break;

        /* */
        case ACCESS_SET_PAUSE_STATE:
        case ACCESS_GET_TITLE_INFO:
        case ACCESS_SET_TITLE:
        case ACCESS_SET_SEEKPOINT:
        case ACCESS_SET_PRIVATE_ID_STATE:
        case ACCESS_GET_CONTENT_TYPE:
            return VLC_EGENERIC;

        default:
            msg_Warn( p_access, "unimplemented query in control" );
            return VLC_EGENERIC;

    }
    return VLC_SUCCESS;
}

/*****************************************************************************
 * Seek: try to go at the right place
 *****************************************************************************/
static int Seek( access_t *p_access, int64_t i_pos )
{
    access_sys_t *p_sys = p_access->p_sys;
    chunk_t      ck;
    off_t        i_offset;
    off_t        i_packet;

    msg_Dbg( p_access, "seeking to "I64Fd, i_pos );

    i_packet = ( i_pos - p_sys->i_header ) / p_sys->asfh.i_min_data_packet_size;
    i_offset = ( i_pos - p_sys->i_header ) % p_sys->asfh.i_min_data_packet_size;

    Stop( p_access );
    Start( p_access, i_packet * p_sys->asfh.i_min_data_packet_size );

    while( !p_access->b_die )
    {
        msg_Warn( p_access, "GetPacket 1" );
        if( GetPacket( p_access, &ck ) )
            break;

        /* skip headers */
        if( ck.i_type != 0x4824 )
            break;

        msg_Warn( p_access, "skipping header" );
    }

    p_access->info.i_pos = i_pos;
    p_access->info.b_eof = VLC_FALSE;
    p_sys->i_packet_used += i_offset;

    return VLC_SUCCESS;
}

/*****************************************************************************
 * Read:
 *****************************************************************************/
static ssize_t ReadRedirect( access_t *p_access, uint8_t *p, size_t i_len )
{
    return 0;
}

/*****************************************************************************
 * Read:
 *****************************************************************************/
static ssize_t Read( access_t *p_access, uint8_t *p_buffer, size_t i_len )
{
    access_sys_t *p_sys = p_access->p_sys;
    size_t       i_copy;
    size_t       i_data = 0;

    if( p_access->info.b_eof )
        return 0;

    while( i_data < (size_t) i_len )
    {
        if( p_access->info.i_pos < (p_sys->i_start + p_sys->i_header) )
        {
            int i_offset = p_access->info.i_pos - p_sys->i_start;
            i_copy = __MIN( p_sys->i_header - i_offset,
                            (int)((size_t)i_len - i_data) );
            memcpy( &p_buffer[i_data], &p_sys->p_header[i_offset], i_copy );

            i_data += i_copy;
            p_access->info.i_pos += i_copy;
        }
        else if( p_sys->i_packet_used < p_sys->i_packet_length )
        {
            i_copy = __MIN( p_sys->i_packet_length - p_sys->i_packet_used,
                            i_len - i_data );
            memcpy( &p_buffer[i_data],
                    &p_sys->p_packet[p_sys->i_packet_used],
                    i_copy );

            i_data += i_copy;
            p_sys->i_packet_used += i_copy;
            p_access->info.i_pos += i_copy;
        }
        else if( (p_sys->i_packet_length > 0) &&
                 ((int)p_sys->i_packet_used < p_sys->asfh.i_min_data_packet_size) )
        {
            i_copy = __MIN( p_sys->asfh.i_min_data_packet_size - p_sys->i_packet_used,
                            i_len - i_data );
            memset( &p_buffer[i_data], 0, i_copy );

            i_data += i_copy;
            p_sys->i_packet_used += i_copy;
            p_access->info.i_pos += i_copy;
        }
        else
        {
            chunk_t ck;
        msg_Warn( p_access, "GetPacket 2" );
            if( GetPacket( p_access, &ck ) )
            {
                int i_ret = -1;
                if( p_sys->b_broadcast )
                {
                    if( (ck.i_type == 0x4524) && (ck.i_sequence != 0) )
                        i_ret = Restart( p_access );
                    else if( ck.i_type == 0x4324 )
                        i_ret = Reset( p_access );
                }
                if( i_ret )
                {
                    p_access->info.b_eof = VLC_TRUE;
                    return 0;
                }
            }
            if( ck.i_type != 0x4424 )
            {
                p_sys->i_packet_used = 0;
                p_sys->i_packet_length = 0;
            }
        }
    }

    return( i_data );
}

/* */
static int Restart( access_t *p_access )
{
    access_sys_t *p_sys = p_access->p_sys;
    char *psz_location = NULL;

    msg_Dbg( p_access, "Restart the stream" );
    p_sys->i_start = p_access->info.i_pos;

    /* */
    msg_Dbg( p_access, "stoping the stream" );
    Stop( p_access );

    /* */
    msg_Dbg( p_access, "describe the stream" );
    if( Describe( p_access, &psz_location ) )
    {
        msg_Err( p_access, "describe failed" );
        return VLC_EGENERIC;
    }
    /* */
    if( Start( p_access, 0 ) )
    {
        msg_Err( p_access, "Start failed" );
        return VLC_EGENERIC;
    }
    return VLC_SUCCESS;
}
static int Reset( access_t *p_access )
{
    access_sys_t *p_sys = p_access->p_sys;
    asf_header_t old_asfh = p_sys->asfh;
    int i;

    msg_Dbg( p_access, "Reset the stream" );
    p_sys->i_start = p_access->info.i_pos;

    /* */
    p_sys->i_packet_sequence = 0;
    p_sys->i_packet_used = 0;
    p_sys->i_packet_length = 0;
    p_sys->p_packet = NULL;

    /* Get the next header FIXME memory loss ? */
    GetHeader( p_access );
    if( p_sys->i_header <= 0 )
        return VLC_EGENERIC;

    E_( asf_HeaderParse )( &p_sys->asfh,
                           p_sys->p_header, p_sys->i_header );
    msg_Dbg( p_access, "packet count="I64Fd" packet size=%d",
             p_sys->asfh.i_data_packets_count,
             p_sys->asfh.i_min_data_packet_size );

    E_( asf_StreamSelect)( &p_sys->asfh,
                           var_CreateGetInteger( p_access, "mms-maxbitrate" ),
                           var_CreateGetInteger( p_access, "mms-all" ),
                           var_CreateGetInteger( p_access, "audio" ),
                           var_CreateGetInteger( p_access, "video" ) );

    /* Check we have comptible asfh */
    for( i = 1; i < 128; i++ )
    {
        asf_stream_t *p_old = &old_asfh.stream[i];
        asf_stream_t *p_new = &p_sys->asfh.stream[i];

        if( p_old->i_cat != p_new->i_cat || p_old->i_selected != p_new->i_selected )
            break;
    }
    if( i < 128 )
    {
        msg_Warn( p_access, "incompatible asf header, restart" );
        return Restart( p_access );
    }

    /* */
    p_sys->i_packet_used = 0;
    p_sys->i_packet_length = 0;
    return VLC_SUCCESS;
}

static int OpenConnection( access_t *p_access )
{
    access_sys_t *p_sys = p_access->p_sys;
    vlc_url_t    srv = p_sys->b_proxy ? p_sys->proxy : p_sys->url;

    if( ( p_sys->fd = net_ConnectTCP( p_access,
                                      srv.psz_host, srv.i_port ) ) < 0 )
    {
        msg_Err( p_access, "cannot connect to %s:%d",
                 srv.psz_host, srv.i_port );
        return VLC_EGENERIC;
    }

    if( p_sys->b_proxy )
    {
        net_Printf( VLC_OBJECT(p_access), p_sys->fd, NULL,
                    "GET http://%s:%d%s HTTP/1.0\r\n",
                    p_sys->url.psz_host, p_sys->url.i_port,
                    ( (p_sys->url.psz_path == NULL) ||
                      (*p_sys->url.psz_path == '\0') ) ?
                         "/" : p_sys->url.psz_path );

        /* Proxy Authentication */
        if( p_sys->proxy.psz_username && *p_sys->proxy.psz_username )
        {
            char *buf;
            char *b64;

            asprintf( &buf, "%s:%s", p_sys->proxy.psz_username,
                       p_sys->proxy.psz_password ? p_sys->proxy.psz_password : "" );

            b64 = vlc_b64_encode( buf );
            free( buf );

            net_Printf( VLC_OBJECT(p_access), p_sys->fd, NULL,
                        "Proxy-Authorization: Basic %s\r\n", b64 );
            free( b64 );
        }
    }
    else
    {
        net_Printf( VLC_OBJECT(p_access), p_sys->fd, NULL,
                    "GET %s HTTP/1.0\r\n"
                    "Host: %s:%d\r\n",
                    ( (p_sys->url.psz_path == NULL) ||
                      (*p_sys->url.psz_path == '\0') ) ?
                            "/" : p_sys->url.psz_path,
                    p_sys->url.psz_host, p_sys->url.i_port );
    }
    return VLC_SUCCESS;
}

/*****************************************************************************
 * Describe:
 *****************************************************************************/
static int Describe( access_t  *p_access, char **ppsz_location )
{
    access_sys_t *p_sys = p_access->p_sys;
    char         *psz_location = NULL;
    char         *psz;
    int          i_code;

    /* Reinit context */
    p_sys->b_broadcast = VLC_TRUE;
    p_sys->i_request_context = 1;
    p_sys->i_packet_sequence = 0;
    p_sys->i_packet_used = 0;
    p_sys->i_packet_length = 0;
    p_sys->p_packet = NULL;
    E_( GenerateGuid )( &p_sys->guid );

    if( OpenConnection( p_access ) )
        return VLC_EGENERIC;

    net_Printf( VLC_OBJECT(p_access), p_sys->fd, NULL,
                "Accept: */*\r\n"
                "User-Agent: "MMSH_USER_AGENT"\r\n"
                "Pragma: no-cache,rate=1.000000,stream-time=0,stream-offset=0:0,request-context=%d,max-duration=0\r\n"
                "Pragma: xClientGUID={"GUID_FMT"}\r\n"
                "Connection: Close\r\n",
                p_sys->i_request_context++,
                GUID_PRINT( p_sys->guid ) );

    if( net_Printf( VLC_OBJECT(p_access), p_sys->fd, NULL, "\r\n" ) < 0 )
    {
        msg_Err( p_access, "failed to send request" );
        goto error;
    }

    /* Receive the http header */
    if( ( psz = net_Gets( VLC_OBJECT(p_access), p_sys->fd, NULL ) ) == NULL )
    {
        msg_Err( p_access, "failed to read answer" );
        goto error;
    }

    if( strncmp( psz, "HTTP/1.", 7 ) )
    {
        msg_Err( p_access, "invalid HTTP reply '%s'", psz );
        free( psz );
        goto error;
    }
    i_code = atoi( &psz[9] );
    if( i_code >= 400 )
    {
        msg_Err( p_access, "error: %s", psz );
        free( psz );
        goto error;
    }

    msg_Dbg( p_access, "HTTP reply '%s'", psz );
    free( psz );
    for( ;; )
    {
        char *psz = net_Gets( p_access, p_sys->fd, NULL );
        char *p;

        if( psz == NULL )
        {
            msg_Err( p_access, "failed to read answer" );
            goto error;
        }

        if( *psz == '\0' )
        {
            free( psz );
            break;
        }

        if( ( p = strchr( psz, ':' ) ) == NULL )
        {
            msg_Err( p_access, "malformed header line: %s", psz );
            free( psz );
            goto error;
        }
        *p++ = '\0';
        while( *p == ' ' ) p++;

        /* FIXME FIXME test Content-Type to see if it's a plain stream or an
         * asx FIXME */
        if( !strcasecmp( psz, "Pragma" ) )
        {
            if( strstr( p, "features" ) )
            {
                /* FIXME, it is a bit badly done here ..... */
                if( strstr( p, "broadcast" ) )
                {
                    msg_Dbg( p_access, "stream type = broadcast" );
                    p_sys->b_broadcast = VLC_TRUE;
                }
                else if( strstr( p, "seekable" ) )
                {
                    msg_Dbg( p_access, "stream type = seekable" );
                    p_sys->b_broadcast = VLC_FALSE;
                }
                else
                {
                    msg_Warn( p_access, "unknow stream types (%s)", p );
                    p_sys->b_broadcast = VLC_FALSE;
                }
            }
        }
        else if( !strcasecmp( psz, "Location" ) )
        {
            psz_location = strdup( p );
        }

        free( psz );
    }

    /* Handle the redirection */
    if( ( (i_code == 301) || (i_code == 302) ||
          (i_code == 303) || (i_code == 307) ) &&
        psz_location && *psz_location )
    {
        msg_Dbg( p_access, "redirection to %s", psz_location );
        net_Close( p_sys->fd ); p_sys->fd = -1;

        *ppsz_location = psz_location;
        return VLC_SUCCESS;
    }

    /* Read the asf header */
    GetHeader( p_access );
    if( p_sys->i_header <= 0 )
    {
        msg_Err( p_access, "header size == 0" );
        goto error;
    }
    /* close this connection */
    net_Close( p_sys->fd );
    p_sys->fd = -1;

    /* *** parse header and get stream and their id *** */
    /* get all streams properties,
     *
     * TODO : stream bitrates properties(optional)
     *        and bitrate mutual exclusion(optional) */
    E_( asf_HeaderParse )( &p_sys->asfh,
                           p_sys->p_header, p_sys->i_header );
    msg_Dbg( p_access, "packet count="I64Fd" packet size=%d",
             p_sys->asfh.i_data_packets_count,
             p_sys->asfh.i_min_data_packet_size );

    E_( asf_StreamSelect)( &p_sys->asfh,
                           var_CreateGetInteger( p_access, "mms-maxbitrate" ),
                           var_CreateGetInteger( p_access, "mms-all" ),
                           var_CreateGetInteger( p_access, "audio" ),
                           var_CreateGetInteger( p_access, "video" ) );
    return VLC_SUCCESS;

error:
    if( p_sys->fd > 0 )
    {
        net_Close( p_sys->fd  );
        p_sys->fd = -1;
    }
    return VLC_EGENERIC;
}

static void GetHeader( access_t *p_access )
{
    access_sys_t *p_sys = p_access->p_sys;

    /* Read the asf header */
    p_sys->i_header = 0;
    if( p_sys->p_header )
        free( p_sys->p_header  );
    p_sys->p_header = NULL;
    for( ;; )
    {
        chunk_t ck;
        if( GetPacket( p_access, &ck ) || ck.i_type != 0x4824 )
            break;

        if( ck.i_data > 0 )
        {
            p_sys->i_header += ck.i_data;
            p_sys->p_header = realloc( p_sys->p_header, p_sys->i_header );
            memcpy( &p_sys->p_header[p_sys->i_header - ck.i_data],
                    ck.p_data, ck.i_data );
        }
    }
    msg_Dbg( p_access, "complete header size=%d", p_sys->i_header );
}


/*****************************************************************************
 * Start stream
 ****************************************************************************/
static int Start( access_t *p_access, int64_t i_pos )
{
    access_sys_t *p_sys = p_access->p_sys;
    int  i_streams = 0;
    int  i_streams_selected = 0;
    int  i;
    char *psz = NULL;

    msg_Dbg( p_access, "starting stream" );

    for( i = 1; i < 128; i++ )
    {
        if( p_sys->asfh.stream[i].i_cat == ASF_STREAM_UNKNOWN )
            continue;
        i_streams++;
        if( p_sys->asfh.stream[i].i_selected )
            i_streams_selected++;
    }
    if( i_streams_selected <= 0 )
    {
        msg_Err( p_access, "no stream selected" );
        return VLC_EGENERIC;
    }

    if( OpenConnection( p_access ) )
        return VLC_EGENERIC;

    net_Printf( VLC_OBJECT(p_access), p_sys->fd, NULL,
                "Accept: */*\r\n"
                "User-Agent: "MMSH_USER_AGENT"\r\n" );
    if( p_sys->b_broadcast )
    {
        net_Printf( VLC_OBJECT(p_access), p_sys->fd, NULL,
                    "Pragma: no-cache,rate=1.000000,request-context=%d\r\n",
                    p_sys->i_request_context++ );
    }
    else
    {
        net_Printf( VLC_OBJECT(p_access), p_sys->fd, NULL,
                    "Pragma: no-cache,rate=1.000000,stream-time=0,stream-offset=%u:%u,request-context=%d,max-duration=0\r\n",
                    (uint32_t)((i_pos >> 32)&0xffffffff),
                    (uint32_t)(i_pos&0xffffffff),
                    p_sys->i_request_context++ );
    }
    net_Printf( VLC_OBJECT(p_access), p_sys->fd, NULL,
                "Pragma: xPlayStrm=1\r\n"
                "Pragma: xClientGUID={"GUID_FMT"}\r\n"
                "Pragma: stream-switch-count=%d\r\n"
                "Pragma: stream-switch-entry=",
                GUID_PRINT( p_sys->guid ),
                i_streams);

    for( i = 1; i < 128; i++ )
    {
        if( p_sys->asfh.stream[i].i_cat != ASF_STREAM_UNKNOWN )
        {
            int i_select = 2;
            if( p_sys->asfh.stream[i].i_selected )
            {
                i_select = 0;
            }
            net_Printf( VLC_OBJECT(p_access), p_sys->fd, NULL,
                        "ffff:%d:%d ", i, i_select );
        }
    }
    net_Printf( VLC_OBJECT(p_access), p_sys->fd, NULL, "\r\n" );
    net_Printf( VLC_OBJECT(p_access), p_sys->fd, NULL,
                "Connection: Close\r\n" );

    if( net_Printf( VLC_OBJECT(p_access), p_sys->fd, NULL, "\r\n" ) < 0 )
    {
        msg_Err( p_access, "failed to send request" );
        return VLC_EGENERIC;
    }

    psz = net_Gets( VLC_OBJECT(p_access), p_sys->fd, NULL );
    if( psz == NULL )
    {
        msg_Err( p_access, "cannot read data 0" );
        return VLC_EGENERIC;
    }

    if( atoi( &psz[9] ) >= 400 )
    {
        msg_Err( p_access, "error: %s", psz );
        free( psz );
        return VLC_EGENERIC;
    }
    msg_Dbg( p_access, "HTTP reply '%s'", psz );
    free( psz );

    /* FIXME check HTTP code */
    for( ;; )
    {
        char *psz = net_Gets( p_access, p_sys->fd, NULL );
        if( psz == NULL )
        {
            msg_Err( p_access, "cannot read data 1" );
            return VLC_EGENERIC;
        }
        if( *psz == '\0' )
        {
            free( psz );
            break;
        }
        msg_Dbg( p_access, "%s", psz );
        free( psz );
    }

    p_sys->i_packet_used   = 0;
    p_sys->i_packet_length = 0;

    return VLC_SUCCESS;
}

/*****************************************************************************
 * closing stream
 *****************************************************************************/
static void Stop( access_t *p_access )
{
    access_sys_t *p_sys = p_access->p_sys;

    msg_Dbg( p_access, "closing stream" );
    if( p_sys->fd > 0 )
    {
        net_Close( p_sys->fd );
        p_sys->fd = -1;
    }
}

/*****************************************************************************
 * get packet
 *****************************************************************************/
static int GetPacket( access_t * p_access, chunk_t *p_ck )
{
    access_sys_t *p_sys = p_access->p_sys;
    int restsize;

    /* chunk_t */
    memset( p_ck, 0, sizeof( chunk_t ) );

    /* Read the chunk header */
    /* Some headers are short, like 0x4324. Reading 12 bytes will cause us
     * to lose synchronization with the stream. Just read to the length
     * (4 bytes), decode and then read up to 8 additional bytes to get the
     * entire header.
     */
    if( net_Read( p_access, p_sys->fd, NULL, p_sys->buffer, 4, VLC_TRUE ) < 4 )
    {
       msg_Err( p_access, "cannot read data 2" );
       return VLC_EGENERIC;
    }

    p_ck->i_type = GetWLE( p_sys->buffer);
    p_ck->i_size = GetWLE( p_sys->buffer + 2);

    restsize = p_ck->i_size;
    if( restsize > 8 )
        restsize = 8;

    if( net_Read( p_access, p_sys->fd, NULL, p_sys->buffer + 4, restsize, VLC_TRUE ) < restsize )
    {
        msg_Err( p_access, "cannot read data 3" );
        return VLC_EGENERIC;
    }
    p_ck->i_sequence  = GetDWLE( p_sys->buffer + 4);
    p_ck->i_unknown   = GetWLE( p_sys->buffer + 8);

    /* Set i_size2 to 8 if this header was short, since a real value won't be
     * present in the buffer. Using 8 avoid reading additional data for the
     * packet.
     */
    if( restsize < 8 )
        p_ck->i_size2 = 8;
    else
        p_ck->i_size2 = GetWLE( p_sys->buffer + 10);

    p_ck->p_data      = p_sys->buffer + 12;
    p_ck->i_data      = p_ck->i_size2 - 8;

    if( p_ck->i_type == 0x4524 )   // Transfer complete
    {
        if( p_ck->i_sequence == 0 )
        {
            msg_Warn( p_access, "EOF" );
            return VLC_EGENERIC;
        }
        else
        {
            msg_Warn( p_access, "next stream following" );
            return VLC_EGENERIC;
        }
    }
    else if( p_ck->i_type == 0x4324 )
    {
        /* 0x4324 is CHUNK_TYPE_RESET: a new stream will follow with a sequence of 0 */
        msg_Warn( p_access, "next stream following (reset) seq=%d", p_ck->i_sequence  );
        return VLC_EGENERIC;
    }
    else if( (p_ck->i_type != 0x4824) && (p_ck->i_type != 0x4424) )
    {
        msg_Err( p_access, "invalid chunk FATAL (0x%x)", p_ck->i_type );
        return VLC_EGENERIC;
    }

    if( (p_ck->i_data > 0) &&
        (net_Read( p_access, p_sys->fd, NULL, &p_sys->buffer[12],
                   p_ck->i_data, VLC_TRUE ) < p_ck->i_data) )
    {
        msg_Err( p_access, "cannot read data 4" );
        return VLC_EGENERIC;
    }

#if 0
    if( (p_sys->i_packet_sequence != 0) &&
        (p_ck->i_sequence != p_sys->i_packet_sequence) )
    {
        msg_Warn( p_access, "packet lost ? (%d != %d)", p_ck->i_sequence, p_sys->i_packet_sequence );
    }
#endif

    p_sys->i_packet_sequence = p_ck->i_sequence + 1;
    p_sys->i_packet_used   = 0;
    p_sys->i_packet_length = p_ck->i_data;
    p_sys->p_packet        = p_ck->p_data;

    return VLC_SUCCESS;
}
