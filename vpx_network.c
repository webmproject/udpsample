/*
 *  Copyright (c) 2010 The WebM project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "tctypes.h"
#include "rtp.h"
#include <stdio.h>
#include <ctype.h>  //for tolower
#include <string.h>

#if defined(_WIN32_WCE) && defined(WIN32_PLATFORM_WFSP)
//INITGUID needs to be defined before DEFINE_GUID is defined
# define INITGUID
#endif

#include "vpx_network.h"

#if ENABLE_LOGGING
# ifndef vpx_NO_GLOBALS
static int module_loglevel = 0;
# else
#  include "vpx_global_handling.h"
#  define module_loglevel VPXGLOBALm(vpx_network,module_loglevel)
# endif
#endif

#ifndef INVALID_SOCKET
# define INVALID_SOCKET -1
#endif

#ifndef SOCKET_ERROR
# define SOCKET_ERROR -1
#endif

#if vpx_NET_SUPPORT_IPV6
# define supported_network_layer(n) ((n == vpx_IPv4) || (n == vpx_IPv6))
#else
# define supported_network_layer(n) (n == vpx_IPv4)
#endif

#define supported_transport_layer(t) ((t == vpx_TCP) || (t == vpx_UDP))

#if defined(__SYMBIAN32__)
# include <in_sock.h>
/*the prototype for bzero appears in symbian's headers, but bzero is
  not in the libs (Series60 v2.1)*/
# define bzero(p,s) memset(p,0,s)
#endif

#if defined(_WIN32_WCE) && defined(WIN32_PLATFORM_WFSP)
# include <connmgr.h>
static HANDLE g_cxhandle = NULL; //data connection handle
#endif

enum
{
    kInited    = 0x01,
    kBound     = 0x02,
    kConnected = 0x04,
    kListening = 0x08
};

static TCRV socket_option(struct vpxsocket *vpx_sock, tc8 set, tc32 level,
                          tc32 option, void *value, tc32 optlen);

static tc32 set_nonblocking_io(struct vpxsocket *vpx_sock, tc32 on);

/*
 *
 * Exposed library functions
 *
*/

/*
    vpx_net_init()
    Performs any necessary system dependent network initialization
    Return: TC_OK on success, TC_ERROR otherwise
*/
TCRV vpx_net_init()
{
    TCRV rv = TC_OK;
#if defined(WIN32) || defined(_WIN32_WCE)
    WSADATA wsa_data;
#endif

    //module_loglevel = VPXLOG_DEBUG;

    vpxlog_dbg(LOG_PACKET,"vpx_network version: %s\n", vpx_network_version);

#if defined(WIN32) || defined(_WIN32_WCE)
    //smartphone
# if defined(WIN32_PLATFORM_WFSP)

    if (!g_cxhandle)
    {
        CONNMGR_CONNECTIONINFO ci = {0};
        DWORD status;
        HANDLE connection;
        HRESULT hr;

        ci.cbSize      = sizeof(CONNMGR_CONNECTIONINFO);
        ci.dwParams    = CONNMGR_PARAM_GUIDDESTNET; //| CONNMGR_PARAM_MINRCVBW
#ifdef _X86_
        /*the emulator has a proxy connection setup to access the internet;
          w/o this flag set the call will fail*/
        ci.dwFlags     = CONNMGR_FLAG_PROXY_HTTP;
#else
        ci.dwFlags     = 0;
#endif
        ci.dwPriority  = CONNMGR_PRIORITY_USERINTERACTIVE;
        ci.bExclusive  = FALSE; //share connection among apps
        ci.bDisabled   = FALSE;
        ci.guidDestNet = IID_DestNetCorp;
        hr = ConnMgrEstablishConnectionSync(&ci, &connection, 50000, &status);

        if (hr == E_FAIL && status == CONNMGR_STATUS_NOPATHTODESTINATION)
        {
            vpxlog_error("vpx_net_init: connection to IID_DestNetCorp failed."
                         " attempting DestNetInternet\n");
            ci.guidDestNet = IID_DestNetInternet;
            hr = ConnMgrEstablishConnectionSync(&ci, &connection, 50000, &status);
        }

        if (hr == S_OK && status == CONNMGR_STATUS_CONNECTED)
        {
            vpxlog_info("vpx_net_init: ConnMgrEstablishConnectionSync success\n");
        }
        else
        {
            rv = TC_ERROR;
            vpxlog_error("vpx_net_init: ConnMgrEstablishConnectionSync failed,"
                         " hr:%u status:%u\n", hr, status);
        }
    }

# endif

    /*
      Initialize windows networking services
      WINSOCK_VERSION is defined in winsock2.h
      if the initialization fails or the returned version
      is not equal to what we asked for, return an error
    */
    if (WSAStartup(WINSOCK_VERSION, &wsa_data) ||
        (LOBYTE(wsa_data.wVersion) != LOBYTE(WINSOCK_VERSION)) ||
        (HIBYTE(wsa_data.wVersion) != HIBYTE(WINSOCK_VERSION)))
    {
        WSACleanup();
        rv = TC_ERROR;
    }

#endif

    return rv;
}

/*
    vpx_net_destroy()
    Performs any necessary system dependent network deinitialization
*/
void vpx_net_destroy()
{
#if defined(WIN32) || defined(_WIN32_WCE)
# if defined(WIN32_PLATFORM_WFSP)

    if (g_cxhandle)
    {
        //release our data connection immediately
        ConnMgrReleaseConnection(g_cxhandle, 0);
        g_cxhandle = NULL;
    }

# endif
    WSACleanup();
#endif
}


/*
    vpx_net_open(struct vpxsocket* vpx_sock, enum network_layer net_layer,
                 enum transport_layer trans_layer)
      vpx_sock - pointer to an vpxsocket structure that is to hold network info
      net_layer - network layer of the socket to be created
      trans_layer - transport layer of the socket to be created
    Attempts to create a socket with the specified network
    and transport layer. Read and send timeouts default to vpx_NET_NO_TIMEOUT.
    Return:
      TC_OK: on success
      TC_INVALID_PARAMS: if vpx_sock is NULL or if the network/transport
                         layer is not supported.
      TC_ERROR: if a socket could not be created.
*/
TCRV vpx_net_open(struct vpxsocket *vpx_sock,
                  enum network_layer net_layer,
                  enum transport_layer trans_layer)
{
    TCRV rv = TC_INVALID_PARAMS;

    if (vpx_sock && supported_transport_layer(trans_layer) &&
        supported_network_layer(net_layer))
    {

        memset(vpx_sock, 0, sizeof(struct vpxsocket));

        vpx_sock->nl = net_layer;
        vpx_sock->tl = trans_layer;

        switch (vpx_sock->nl)
        {
        case vpx_IPv4:
            vpx_sock->sock = socket(PF_INET, trans_layer, 0);
            break;
        case vpx_IPv6:
#if vpx_NET_SUPPORT_IPV6
            vpx_sock->sock = socket(PF_INET6, trans_layer, 0);
#endif
            break;
        }

        if (vpx_sock->sock != INVALID_SOCKET)
        {
            rv = TC_OK;
            vpx_sock->state = kInited;
            vpx_sock->read_timeout_ms = vpx_NET_NO_TIMEOUT;
            vpx_sock->send_timeout_ms = vpx_NET_NO_TIMEOUT;

        }
        else
            rv = TC_ERROR;
    }


    return rv;
}

/*
    vpx_net_close(struct vpxsocket* vpx_sock)
      vpx_sock - pointer to an vpxsocket structure which
                 holds the socket to be closed
    Attempts to close the socket associated with the vpx_sock structure
    Return:
      TC_OK: on success
      TC_INVALID_PARAMS: if vpx_sock is NULL
      TC_ERROR: if the socket could not be closed
*/
TCRV vpx_net_close(struct vpxsocket *vpx_sock)
{
    TCRV rv = TC_INVALID_PARAMS;

    if (vpx_sock && (vpx_sock->state & kInited))
    {
#if defined(WIN32) || defined(_WIN32_WCE)
        rv = closesocket(vpx_sock->sock) ? TC_ERROR : TC_OK;
#else
        int ret;

        ret = close(vpx_sock->sock);
        rv = (ret) ? TC_ERROR : TC_OK;
        vpxlog_dbg(LOG_PACKET, "vpx_net_close: fd=%d ret=%d eno=%d\n",
                   vpx_sock->sock, ret, errno);
#endif

        vpx_sock->state = 0;
    }

    return rv;
}

/*
    vpx_net_bind(struct vpxsocket* vpx_sock,
                 union vpx_sockaddr_x* vpx_sa_x,
                 tcu16 port)
      vpx_sock - pointer to an vpxsocket structure that contains
                 the socket to be bound
      vpx_sa_x - pointer to an vpx_sockaddr_x struct that contains the
                 interface to bind to or NULL if the user wants to
                 bind to any interface.
      port - the port to bind the socket to
    Attempts to bind vpx_sock to port on the interface specified in vpx_sa_x
    or to any interface if vpx_sa_x is NULL. If provided vpx_sa_x should have
    been filled out by vpx_net_get_addr_info.
    Return:
      TC_OK: on success
      TC_INVALID_PARAMS: if vpx_sock is NULL or does not point to a structure
                         that was initialized via vpx_net_open
      TC_ERROR: if vpx_sock could not be bound to the specified port
                and interface
*/
TCRV vpx_net_bind(struct vpxsocket *vpx_sock,
                  union vpx_sockaddr_x *vpx_sa_x,
                  tcu16 port)
{
    TCRV rv = TC_INVALID_PARAMS;

    if (vpx_sock && (vpx_sock->state & kInited))
    {

        tc32 ret = SOCKET_ERROR;

        switch (vpx_sock->nl)
        {
        case vpx_IPv4:
            vpx_sock->local_addr.sa_in.sin_family      = AF_INET;
            vpx_sock->local_addr.sa_in.sin_addr.s_addr = vpx_sa_x ?
                    vpx_sa_x->sa_in.sin_addr.s_addr :
                    INADDR_ANY;
            vpx_sock->local_addr.sa_in.sin_port        = htons(port);

            ret = bind(vpx_sock->sock, (struct sockaddr *)&vpx_sock->local_addr.sa_in,
                       sizeof(struct sockaddr_in));
            break;
        case vpx_IPv6:
#if vpx_NET_SUPPORT_IPV6
            vpx_sock->local_addr.sa_in6.sin6_family = AF_INET6;
            vpx_sock->local_addr.sa_in6.sin6_addr   = vpx_sa_x ?
                    vpx_sa_x->sa_in6.sin6_addr :
                    in6addr_any;
            vpx_sock->local_addr.sa_in6.sin6_port   = htons(port);

            ret = bind(vpx_sock->sock, (struct sockaddr *)&vpx_sock->local_addr.sa_in6,
                       sizeof(struct sockaddr_in6));
#endif
            break;
        }

        if (ret != SOCKET_ERROR)
        {
            vpx_sock->state |= kBound;
            rv = TC_OK;
        }
        else
            rv = TC_ERROR;
    }

    return rv;
}

/*
    vpx_net_listen(struct vpxsocket* vpx_sock, tc32 backlog)
      vpx_sock - pointer to a properly initialized and bound vpxsocket
                 structure to be setup to listen for incoming connections
      backlog - the maximum length the queue of pending connections can grow to.
                If the value is less than 1, backlog will be set to the system's
                maximum value.
    Attempts to put vpx_sock into a state where it can accept incoming connections.
    Return:
      TC_OK: on success
      TC_INVALID_PARAMS: if vpx_sock is NULL, not properly initialized and bound,
                         or if the transport layer does not support listening
      TC_ERROR: if the vpx_sock could not be put into the listening state
*/
TCRV vpx_net_listen(struct vpxsocket *vpx_sock, tc32 backlog)
{
    TCRV rv = TC_INVALID_PARAMS;

    if (vpx_sock && (vpx_sock->state & kBound) && (vpx_sock->tl == vpx_TCP))
    {

        if (backlog < 1)
            backlog = SOMAXCONN;

        if (listen(vpx_sock->sock, backlog) != SOCKET_ERROR)
        {
            rv = TC_OK;
            vpx_sock->state |= kListening;
        }
        else
            rv = TC_ERROR;
    }

    return rv;
}

/*
    vpx_net_accept(struct vpxsocket* vpx_sock, struct vpxsocket* vpx_sock_peer)
      vpx_sock - pointer to a properly initialized, bound and listening
                 vpxsocket structure
      vpx_sock_peer - result parameter; pointer to an vpxsocket structure that
                      will be filled out with the accepted connection's info
    Attempts to have vpx_sock accept connections on the port it's currently
    listening. If successful, vpx_sock_peer will be filled out with the
    remote peer's info.
    Return:
      TC_OK: on success
      TC_INVALID_PARAMS: if vpx_sock is NULL, was not put into the listening
                         state via vpx_net_listen or vpx_sock_peer is NULL
      TC_ERROR: if there was an error accepting connections on vpx_sock
*/
TCRV vpx_net_accept(struct vpxsocket *vpx_sock, struct vpxsocket *vpx_sock_peer)
{
    TCRV rv = TC_INVALID_PARAMS;

    if (vpx_sock && (vpx_sock->state & kListening) && vpx_sock_peer)
    {

        unsigned int len;

        switch (vpx_sock->nl)
        {
        case vpx_IPv4:
            len = sizeof(struct sockaddr_in);

            vpx_sock_peer->sock = accept(vpx_sock->sock,
                                         (struct sockaddr *)&vpx_sock_peer->remote_addr.sa_in,
                                         &len);
            break;
        case vpx_IPv6:
#if vpx_NET_SUPPORT_IPV6
            len = sizeof(struct sockaddr_in6);

            vpx_sock_peer->sock = accept(vpx_sock->sock,
                                         (struct sockaddr *)&vpx_sock_peer->remote_addr.sa_in6,
                                         &len);
#endif
            break;
        }

        if (vpx_sock_peer->sock != INVALID_SOCKET)
        {

            vpx_sock_peer->nl              = vpx_sock->nl;
            vpx_sock_peer->tl              = vpx_sock->tl;
            vpx_sock_peer->state           = kInited | kConnected;
            vpx_sock_peer->read_timeout_ms = vpx_NET_NO_TIMEOUT;
            vpx_sock_peer->send_timeout_ms = vpx_NET_NO_TIMEOUT;

            rv = TC_OK;
        }
        else
            rv = TC_ERROR;
    }

    return rv;
}

/*
    vpx_net_connect(struct vpxsocket* vpx_sock, tc8* ip_addr, tcu16 port)
      vpx_sock - pointer to an vpxsocket structure that is to be connencted
                 to the endpoint described by ip_addr and port
      ip_addr - pointer to a character string that contains the address to
                attempt to connect to
      port - the port to attempt to connect to on ip_addr
    Attempt to connect vpx_sock to port on ip_addr.
    Return:
        TC_OK: on success
        TC_INVALID_PARAMS: if vpx_sock is NULL, not properly initialized or
                           if ip_addr is NULL
        TC_ERROR: if information could not be obtained about the host or
                  if a connection could not be established
*/
TCRV vpx_net_connect(struct vpxsocket *vpx_sock, tc8 *ip_addr, tcu16 port)
{
    TCRV rv = TC_INVALID_PARAMS;


    if (vpx_sock && (vpx_sock->state & kInited))
    {

        rv = vpx_net_get_addr_info(ip_addr, port, vpx_sock->nl, vpx_sock->tl, &vpx_sock->remote_addr);

        if (!rv)
        {
            tc32 ret = 0;

            switch (vpx_sock->nl)
            {
            case vpx_IPv4:
                ret = connect(vpx_sock->sock,
                              (struct sockaddr *)&vpx_sock->remote_addr.sa_in,
                              sizeof(struct sockaddr_in));
                break;
            case vpx_IPv6:
#if vpx_NET_SUPPORT_IPV6
                ret = connect(vpx_sock->sock,
                              (struct sockaddr *)&vpx_sock->remote_addr.sa_in6,
                              sizeof(struct sockaddr_in6));
#endif
                break;
            }

            if (!ret)
            {
                rv = TC_OK;
                vpx_sock->state |= kConnected;
            }
            else
                rv = TC_ERROR;
        }
    }

    return rv;
}

/*
    vpx_net_get_addr_info(tc8* ip_addr,
                          tcu16 port,
                          enum network_layer net_layer,
                          enum transport_layer trans_layer,
                          union vpx_sockaddr_x* vpx_sa_x)
      ip_addr - address to resolve
      port - port on ip_addr to obtain information for or 0 indicating any
      net_layer - network layer desired on the host machine
      trans_layer - the transport layer desired on the host machine
      vpx_sa_x - pointer to an vpx_sockaddr_x union that will receive host
                 information if obtained
    Attempts to acquire information about ip_addr that can be used in a
    connection attempt. This information will be stored in vpx_sa_x which
    can be used in subsequent vpx_net_ functions
    Return:
      TC_OK: on success
      TC_INVALID_PARAMS: if ip_addr or vpx_sa_x are NULL or if the specified
                         network/transport layers are unsupported by this library
      TC_ERROR: if no information could be obtained about the host
*/
TCRV vpx_net_get_addr_info(tc8 *ip_addr,
                           tcu16 port,
                           enum network_layer net_layer,
                           enum transport_layer trans_layer,
                           union vpx_sockaddr_x *vpx_sa_x)
{
    TCRV rv = TC_INVALID_PARAMS;


    if (ip_addr && vpx_sa_x &&
        supported_network_layer(net_layer) &&
        supported_transport_layer(trans_layer))
    {

#if vpx_NET_SUPPORT_IPV6
        char portstr[6];
        struct addrinfo hints;
        struct addrinfo *host = NULL;

        memset(&hints, 0, sizeof(struct addrinfo));

        hints.ai_socktype = trans_layer;

        switch (net_layer)
        {
        case vpx_IPv4:
            hints.ai_family = PF_INET;
            break;
        case vpx_IPv6:
            hints.ai_family = PF_INET6;
            break;
        }

        sprintf(portstr, "%hu", port);

        if (!getaddrinfo(ip_addr, portstr, &hints, &host))
        {

            memcpy(&vpx_sa_x->sa_in6, host->ai_addr, host->ai_addrlen);
            freeaddrinfo(host);
            rv = TC_OK;
        }
        else
            rv = TC_ERROR;

#else

        tc8 *p = ip_addr;
        tc8 is_host_name = 0;

        //IPv4, unfortunately gethostbyname will fail if given a dotted decimal ip,
        //unlike getaddrinfo. So to avoid any delay's we check to see what
        //representation of an ip we have,
        //if there's a letter assume we have a hostname
        while (*p && !(is_host_name = (tolower(*p) >= 'a' && tolower(*p) <= 'z'))) ++p;

        if (is_host_name)
        {
#if defined(__SYMBIAN32__) && !defined(__WINS__)
            //vpxlog_dbg("Trying to get address by name %s:%d\n", ip_addr, port);
            {

                RSocketServ iSocketServ;
                TNameEntry iNameEntry;
                TNameRecord iNameRecord;
                RHostResolver iResolver;
                TRequestStatus iStatus;

                User::LeaveIfError(iSocketServ.Connect());
                User::LeaveIfError(iResolver.Open(iSocketServ, KAfInet, KProtocolInetUdp));

                TPtrC8 ihostname8((const TUint8 *)ip_addr, strlen(ip_addr));
                TBuf<256> ihostname;
                ihostname.Copy(ihostname8);
                //vpxlog_dbg("ihostname = %s\n", ihostname.Ptr());
                iResolver.GetByName(ihostname, iNameEntry, iStatus);
                User::WaitForRequest(iStatus);

                if (iStatus == KErrNone)
                {
                    iNameRecord = iNameEntry();
                    TInetAddr addr;
                    addr = TInetAddr::Cast(iNameRecord.iAddr);
                    bzero(&vpx_sa_x->sa_in, sizeof(struct sockaddr_in));
                    vpx_sa_x->sa_in.sin_addr.s_addr = ntohl(addr.Address());
                    vpx_sa_x->sa_in.sin_family = AF_INET;
                    vpx_sa_x->sa_in.sin_port   = htons(port);
                    rv = TC_OK;
                    //vpxlog_dbg("I got an address of %x\n",vpx_sa_x->sa_in.sin_addr.s_addr);

                }
                else
                {
                    rv = TC_ERROR;
                    //vpxlog_dbg("I got an error message %d\n",iStatus.Int() );

                }

                iResolver.Close();
                iSocketServ.Close();

            }
#else

#if defined(VXWORKS)
        tc32 host;

        /* The only way VxWorks will be able to resolve a host name
           is if it's in its host table or if the image was built with
           the resolvLib included */
        if ((host = hostGetByName(ip_addr)) != ERROR)
        {
            vpx_sa_x->sa_in.sin_addr.s_addr = host;
#else
        struct hostent *host;

        if ((host = gethostbyname(ip_addr)))
        {
            memcpy(&vpx_sa_x->sa_in.sin_addr, host->h_addr_list[0], sizeof(struct in_addr));
#endif
            vpx_sa_x->sa_in.sin_family = AF_INET;
            vpx_sa_x->sa_in.sin_port   = htons(port);
            rv = TC_OK;
        }
        else
        {
            //could not resolve host name
            rv = TC_ERROR;
        }

        //vpxlog_dbg("gethostbyname returned %d %x\n", rv, host);

#endif

        }
        else
        {
#if defined(__SYMBIAN32__)
            bzero(&vpx_sa_x->sa_in, sizeof(struct sockaddr_in));

            if ((vpx_sa_x->sa_in.sin_addr.s_addr = inet_addr(ip_addr)) != INADDR_NONE)
            {
#elif defined(WIN32) || defined(_WIN32_WCE)

        if ((vpx_sa_x->sa_in.sin_addr.s_addr = inet_addr(ip_addr)) != INADDR_NONE)
        {
#else

        if ((vpx_sa_x->sa_in.sin_addr.s_addr = inet_addr(ip_addr)) > 0)
        {
#endif
                vpx_sa_x->sa_in.sin_family = AF_INET;
                vpx_sa_x->sa_in.sin_port   = htons(port);
                rv = TC_OK;
            }
            else
            {
                vpxlog_dbg(ERRORS, "vpx_network: inet_addr() call returned  INADDR_NONE\n");
                rv = TC_ERROR;
            }
        }

#endif
    }

    return rv;
}

/*
    vpx_net_read(struct vpxsocket* vpx_sock, tc8* buffer,
                 tc32 buf_len, tc32* bytes_read)
      vpx_sock - pointer to a properly initialized vpxsocket structure
      buffer - pointer to a character array where data is to be stored
      buf_len - the max max amount of data to be read into buffer
      bytes_read - pointer to an integer that will receive the actual amount
                   of data read or NULL
    Attempts to read at most buf_len bytes off the socket into buffer. This
    operation can only be done on a connected socket. If a read timeout has
    been set to a non-zero value the operation will fail if it could not be
    completed within the specified time. If the read timeout has been set to
    0 the operation will fail if it could not be completed immediately.
    Return:
      TC_OK: on success
      TC_INVALID_PARAMS: if vpx_sock is NULL, was not properly initialized
                         via vpx_net_open, was not connected via
                         vpx_net_connect, buffer is NULL or buf_len is <= 0
      TC_TIMEDOUT: if a read timeout has been set to non-zero value and the
                   operation could not be completed in the specified time
      TC_WOULDBLOCK: if the read timeout has been set to 0 and the operation
                     could not be completed immediately
      TC_ERROR: if an error other than timed out or would block is encountered
                trying to complete the operation, more information can be
                obtained through calling vpx_net_get_error
*/
TCRV vpx_net_read(struct vpxsocket *vpx_sock, tc8 *buffer,
                  tc32 buf_len, tc32 *bytes_read)
{
    TCRV rv = TC_INVALID_PARAMS;

    if (vpx_sock && (vpx_sock->state & kConnected) && buffer && (buf_len > 0))
    {

        tc32 total_bytes_read = 0,
             ret;

        if (vpx_sock->read_timeout_ms)
        {
            struct timeval tv;
            fd_set read_fds;

            FD_ZERO(&read_fds);
            FD_SET(vpx_sock->sock, &read_fds);

            if (vpx_sock->read_timeout_ms != vpx_NET_NO_TIMEOUT)
            {
                tv.tv_sec  = 0;
                tv.tv_usec = vpx_sock->read_timeout_ms * 1000;

#if !defined(__SYMBIAN32__)
                ret = select(vpx_sock->sock + 1, &read_fds, NULL, NULL, &tv);
#else
                /*jwz 2005-10-04, symbian seems only to define a blocking select
                  via ioctl; as the client doesn't currently hit this path I
                  haven't looked for a replacement*/
                ret = -1;
                vpxassert(vpx_sock->read_timeout_ms == vpx_NET_NO_TIMEOUT);
#endif
            }
            else
            {
#if !defined(__SYMBIAN32__)
                ret = select(vpx_sock->sock + 1, &read_fds, NULL, NULL, NULL);
#else
                /*jwz 2005-10-04, symbian seems only to define a blocking select
                  via ioctl; this select was unnecessary as the socket is set to
                  blocking.*/
                ret = 1;
#endif
            }

            if (ret > 0)
            {
                tc32 n = 0;

#if defined(WIN32) || defined(_WIN32_WCE)
                unsigned long recv_len = 0;
                ioctlsocket(vpx_sock->sock, FIONREAD, &recv_len);
#elif defined(VXWORKS)
                tcu32 recv_len = 0;
                ioctl(vpx_sock->sock, FIONREAD, (tc32)&recv_len);
#elif defined(__SYMBIAN32__)
                tcu32 recv_len = 0;
                ioctl(vpx_sock->sock, E32IONREAD, &recv_len);
#else
                tcu32 recv_len = 0;
                ioctl(vpx_sock->sock, FIONREAD, &recv_len);
#endif

                /* At this point select has said that the socket is readable.
                   We want to try to read once before checking the amount
                   available on the socket in case there is an error pending,
                   in which case recv_len would be 0. */
                do
                {
                    n = recv(vpx_sock->sock, buffer + total_bytes_read, buf_len, 0);
                    total_bytes_read += n;
                    buf_len          -= n;
                }
                while (buf_len && ((tcu32)total_bytes_read < recv_len) && (n > 0));

                rv = (n <= 0) ? TC_ERROR : TC_OK;

            }
            else if (ret < 0)
                rv = TC_ERROR;
            else
                rv = TC_TIMEDOUT;

        }
        else
        {

            //set the socket to non-blocking...
            if (!set_nonblocking_io(vpx_sock, 1))
            {

#if defined(WIN32) || defined(_WIN32_WCE)
                tc32 error = 0;
#endif

                total_bytes_read = recv(vpx_sock->sock, buffer, buf_len, 0);

                if (total_bytes_read < 0)
                {

#if defined(LINUX) || defined(VXWORKS) || defined(__uClinux__) || defined(__SYMBIAN32__)

                    switch (errno)
                    {
                    case EAGAIN:
#if defined(EWOULDBLOCK) && EAGAIN != EWOULDBLOCK
                    case EWOULDBLOCK:
#endif
                        rv = TC_WOULDBLOCK;
                        break;
                    case EINTR:
                    case EFAULT:
                        rv = TC_ERROR;
                        break;
                    default:
                        /* default return is TC_INVALID_PARAMS and
                           covers EBADF, ENOTCONN, ENOTSOCK, EINVAL, etc */
                        break;
                    }

#elif defined(WIN32) || defined(_WIN32_WCE)

                    switch (error = WSAGetLastError())
                    {
                    case WSAEWOULDBLOCK:
                        rv = TC_WOULDBLOCK;
                        break;
                    case WSAEMSGSIZE:
                        rv = TC_MSG_TOO_LARGE;
                        break;
                    case WSAENETDOWN:
                    case WSAEFAULT:
                    case WSAEINTR:
                    case WSAEINPROGRESS:
                    case WSAENETRESET:
                    case WSAESHUTDOWN:
                    case WSAECONNABORTED:
                    case WSAETIMEDOUT:
                    case WSAECONNRESET:
                        rv = TC_ERROR;
                        break;
                    default:
                        /* defaults are approximately equal to those
                           described in the LINUX #if block */
                        break;
                    }

#endif

                }
                else
                    rv = TC_OK;

                //reset the socket to blocking...
                if (set_nonblocking_io(vpx_sock, 0))
                    rv = TC_ERROR;

#if defined(WIN32) || defined(_WIN32_WCE)
                else
                    WSASetLastError(error);

#endif
            }
        }

        if (bytes_read)
            *bytes_read = (total_bytes_read > 0) ? total_bytes_read : 0;
    }

    return rv;
}

/*
    vpx_net_recvfrom(struct vpxsocket* vpx_sock, tc8* buffer, tc32 buf_len,
                     tc32* bytes_read, union vpx_sockaddr_x* vpx_sa_x)
      vpx_sock - pointer to a properly initialized vpxsocket structure
      buffer - pointer to a character array where data is to be stored
      buf_len - max amount of data to be read
      bytes_read - pointer to an integer that will receive the actual amount
                   of data read or NULL
      vpx_sa_from - pointer to a vpx_sockaddr_x union used to store the address
                    of the remote peer the data was received from or NULL
    Attempts to read at most buf_len bytes off the socket into buffer. This
    operation can be done on a connected or unconnected socket. If a
    read timeout has been set to a non-zero value the operation will fail if
    it could not be completed within the specified time. If the send timeout
    has been set to 0 the operation will fail if it could not be completed
    immediately. If data is received and vpx_sa_from is non-NULL the address
    of the sender will be stored in it.
    Return:
      TC_OK: on success
      TC_INVALID_PARAMS: if vpx_sock is NULL, was not properly initialized
                         via vpx_net_open, buffer is NULL or buf_len is <= 0
      TC_TIMEDOUT: if a read timeout has been set to non-zero value and the
                   operation could not be completed in the specified time
      TC_WOULDBLOCK: if the read timeout has been set to 0 and the operation
                     could not be completed immediately
      TC_ERROR: if an error other than timed out or would block is encountered
                trying to complete the operation, more information can be
                obtained through calling vpx_net_get_error
*/
TCRV vpx_net_recvfrom(struct vpxsocket *vpx_sock, tc8 *buffer, tc32 buf_len,
                      tc32 *bytes_read, union vpx_sockaddr_x *vpx_sa_from)
{
    TCRV rv = TC_INVALID_PARAMS;

    if (vpx_sock && (vpx_sock->state & kInited) && buffer && (buf_len > 0))
    {

        tc32 n = -1;
        unsigned int len;

        if (vpx_sock->read_timeout_ms)
        {
            tc32 ret;
            fd_set read_fds;
            struct timeval tv;

            FD_ZERO(&read_fds);
            FD_SET(vpx_sock->sock, &read_fds);

            if (vpx_sock->read_timeout_ms != vpx_NET_NO_TIMEOUT)
            {
                tv.tv_sec  = 0;
                tv.tv_usec = vpx_sock->read_timeout_ms * 1000;

#if !defined(__SYMBIAN32__)
                ret = select(vpx_sock->sock + 1, &read_fds, NULL, NULL, &tv);
#else
                /*jwz 2005-10-04, symbian seems only to define a blocking select
                  via ioctl; as the client doesn't currently hit this path I
                  haven't looked for a replacement*/
                ret = -1;
                vpxassert(vpx_sock->read_timeout_ms == vpx_NET_NO_TIMEOUT);
#endif
            }
            else
            {
#if !defined(__SYMBIAN32__)
                ret = select(vpx_sock->sock + 1, &read_fds, NULL, NULL, NULL);
#else
                /*jwz 2005-10-04, symbian seems only to define a blocking select
                  via ioctl; this select was unnecessary as the socket is set to
                  blocking.*/
                ret = 1;
#endif
            }

            if (ret > 0)
            {
                switch (vpx_sock->nl)
                {
                case vpx_IPv4:
                    len = sizeof(struct sockaddr_in);
                    n = recvfrom(vpx_sock->sock, buffer, buf_len, 0,
                                 (struct sockaddr *)&vpx_sock->remote_addr.sa_in, &len);
#if defined(VXWORKS)
                    vpx_sock->remote_addr.sa_in.sin_family = AF_INET;
#endif
                    break;
                case vpx_IPv6:
#if vpx_NET_SUPPORT_IPV6
                    len = sizeof(struct sockaddr_in6);
                    n = recvfrom(vpx_sock->sock, buffer, buf_len, 0,
                                 (struct sockaddr *)&vpx_sock->remote_addr.sa_in6, &len);
#endif
                    break;
                }

                if (n == SOCKET_ERROR)
                {
                    if (vpx_sock->tl == vpx_UDP)
                    {
#if defined(LINUX) || defined(VXWORKS) || defined(__uClinux__)

                        if (errno == EMSGSIZE)
                            //n = buf_len;
                            rv = TC_MSG_TOO_LARGE;

#elif defined(WIN32) || defined(_WIN32_WCE)

                        if (WSAGetLastError() == WSAEMSGSIZE)
                            //n = buf_len;
                            rv = TC_MSG_TOO_LARGE;

#elif defined(__SYMBIAN32__)

                        /*jwz 2005-10-04, symbian doesn't define EMSGSIZE. I
                          haven't looked for a replacement as the client
                          currently won't be using udp*/
                        if (errno == KErrTooBig) rv = TC_MSG_TOO_LARGE;

#endif
                    }
                    else
                        rv = TC_ERROR;
                }
                else
                {
                    rv = TC_OK;

                    if (vpx_sa_from)
                    {
                        switch (vpx_sock->nl)
                        {
                        case vpx_IPv4:
                            memcpy(&vpx_sa_from->sa_in, &vpx_sock->remote_addr.sa_in,
                                       sizeof(struct sockaddr_in));
                            break;
                        case vpx_IPv6:
#if vpx_NET_SUPPORT_IPV6
                            memcpy(&vpx_sa_from->sa_in6, &vpx_sock->remote_addr.sa_in6,
                                       sizeof(struct sockaddr_in6));
#endif
                            break;
                        }
                    }
                }

            }
            else if (ret < 0)
                rv = TC_ERROR;
            else
                rv = TC_TIMEDOUT;

        }
        else
        {

            //set the socket to non-blocking...
            if (!set_nonblocking_io(vpx_sock, 1))
            {

#if defined(WIN32) || defined(_WIN32_WCE)
                tc32 error = 0;
#endif

                switch (vpx_sock->nl)
                {
                case vpx_IPv4:
                    len = sizeof(struct sockaddr_in);
                    n = recvfrom(vpx_sock->sock, buffer, buf_len, 0,
                                 (struct sockaddr *)&vpx_sock->remote_addr.sa_in, &len);
#if defined(VXWORKS)
                    vpx_sock->remote_addr.sa_in.sin_family = AF_INET;
#endif
                    break;
                case vpx_IPv6:
#if vpx_NET_SUPPORT_IPV6
                    len = sizeof(struct sockaddr_in6);
                    n = recvfrom(vpx_sock->sock, buffer, buf_len, 0,
                                 (struct sockaddr *)&vpx_sock->remote_addr.sa_in6, &len);
#endif
                    break;
                }

                if (n < 0)
                {
#if defined(LINUX) || defined(VXWORKS) || defined(__uClinux__) || defined(__SYMBIAN32__)

                    switch (errno)
                    {
                    case EAGAIN:
#if defined(EWOULDBLOCK) && EAGAIN != EWOULDBLOCK
                    case EWOULDBLOCK:
#endif
                        rv = TC_WOULDBLOCK;
                        break;
                    case EINTR:
                    case EFAULT:
                        rv = TC_ERROR;
                        break;
                    default:
                        /* default return is TC_INVALID_PARAMS and
                           covers EBADF, ENOTCONN, ENOTSOCK, EINVAL, etc */
                        break;
                    }

#elif defined(WIN32) || defined(_WIN32_WCE)

                    switch (error = WSAGetLastError())
                    {
                    case WSAEWOULDBLOCK:
                        rv = TC_WOULDBLOCK;
                        break;
                    case WSAEMSGSIZE:
                        rv = TC_MSG_TOO_LARGE;
                        break;
                    case WSAENETDOWN:
                    case WSAEFAULT:
                    case WSAEINTR:
                    case WSAEINPROGRESS:
                    case WSAENETRESET:
                    case WSAESHUTDOWN:
                    case WSAECONNABORTED:
                    case WSAETIMEDOUT:
                    case WSAECONNRESET:
                        rv = TC_ERROR;
                        break;
                    default:
                        /* defaults are approximately equal to those
                           described in the LINUX #if block */
                        break;
                    }

#endif
                }
                else
                {
                    rv = TC_OK;

                    if (vpx_sa_from)
                    {
                        switch (vpx_sock->nl)
                        {
                        case vpx_IPv4:
                            memcpy(&vpx_sa_from->sa_in, &vpx_sock->remote_addr.sa_in,
                                       sizeof(struct sockaddr_in));
                            break;
                        case vpx_IPv6:
#if vpx_SUPPORT_IPV6
                            memcpy(&vpx_sa_from->sa_in6, &vpx_sock->remote_addr.sa_in6,
                                       sizeof(struct sockaddr_in6));
#endif
                            break;
                        }
                    }
                }

                //reset the socket to blocking...
                if (set_nonblocking_io(vpx_sock, 0))
                    rv = TC_ERROR;

#if defined(WIN32) || defined(_WIN32_WCE)
                else
                    WSASetLastError(error);

#endif
            }
        }

        if (bytes_read)
            *bytes_read = n;
    }

    return rv;
}

/*
    vpx_net_send(struct vpxsocket* vpx_sock, tc8* buffer,
                 tc32 buf_len, tc32* bytes_sent)
      vpx_sock - pointer to a properly initialized vpxsocket structure
      buffer - pointer to a character array containing data to be sent
      buf_len - the length of the data in buffer
      bytes_sent - pointer to an integer that will receive the actual amount
                   of data sent or NULL
    Attempts to send buffer to vpx_sock's connected peer. If a send timeout
    has been set to a non-zero value the operation will fail if it could not
    be completed within the specified time. If the send timeout has been set
    to 0 the operation will fail if it could not be completed immediately.
    Return:
      TC_OK: on success
      TC_INVALID_PARAMS: if vpx_sock is NULL, was not properly initialized
                         via vpx_net_open, was not connected via
                         vpx_net_connect, buffer is NULL or buf_len is <= 0
      TC_TIMEDOUT: if a send timeout has been set to non-zero value and the
                   operation could not be completed in the specified time
      TC_WOULDBLOCK: if the send timeout has been set to 0 and the operation
                     could not be completed immediately
      TC_ERROR: if an error other than timed out or would block is encountered
                trying to complete the operation, more information can be
                obtained through calling vpx_net_get_error
*/
TCRV vpx_net_send(struct vpxsocket *vpx_sock, tc8 *buffer,
                  tc32 buf_len, tc32 *bytes_sent)
{
    TCRV rv = TC_INVALID_PARAMS;

    if (vpx_sock && (vpx_sock->state & kConnected) && buffer && (buf_len > 0))
    {

        tc32 n = 0;

        if (vpx_sock->send_timeout_ms)
        {
            tc32 ret;
            struct timeval tv;
            fd_set write_fds;

            FD_ZERO(&write_fds);
            FD_SET(vpx_sock->sock, &write_fds);

            if (vpx_sock->send_timeout_ms != vpx_NET_NO_TIMEOUT)
            {
                tv.tv_sec  = 0;
                tv.tv_usec = vpx_sock->send_timeout_ms * 1000;

#if !defined(__SYMBIAN32__)
                ret = select(vpx_sock->sock + 1, NULL, &write_fds, NULL, &tv);
#else
                /*jwz 2005-10-04, symbian seems only to define a blocking select
                  via ioctl; as the client doesn't currently hit this path I
                  haven't looked for a replacement*/
                ret = -1;
                vpxassert(vpx_sock->send_timeout_ms == vpx_NET_NO_TIMEOUT);
#endif
            }
            else
            {
#if !defined(__SYMBIAN32__)
                ret = select(vpx_sock->sock + 1, NULL, &write_fds, NULL, NULL);
#else
                /*jwz 2005-10-04, symbian seems only to define a blocking select
                  via ioctl; this select was unnecessary as the socket is set to
                  blocking.*/
                ret = 1;
#endif
            }

            if (ret > 0)
                n = send(vpx_sock->sock, buffer, buf_len, 0);
            else if (ret < 0)
                rv = TC_ERROR;
            else
                rv = TC_TIMEDOUT;

            rv = (n == SOCKET_ERROR) ? TC_ERROR : TC_OK;
        }
        else
        {

            //set the socket to non-blocking...
            if (!set_nonblocking_io(vpx_sock, 1))
            {

#if defined(WIN32) || defined(_WIN32_WCE)
                tc32 error = 0;
#endif

                n = send(vpx_sock->sock, buffer, buf_len, 0);

                if (n < 0)
                {
#if defined(LINUX) || defined(VXWORKS) || defined(__uClinux__) || defined(__SYMBIAN32__)

                    switch (errno)
                    {
                    case EAGAIN:
#if defined(EWOULDBLOCK) && EAGAIN != EWOULDBLOCK
                    case EWOULDBLOCK:
#endif
                        rv = TC_WOULDBLOCK;
                        break;
                    case EFAULT:
                    case EINTR:
#ifdef EMSGSIZE
                    case EMSGSIZE:
#endif
#ifdef ENOBUFS
                    case ENOBUFS:
#endif
                    case ENOMEM:
                    case EPIPE:
                        rv = TC_ERROR;
                        break;
                    default:
                        /* default return is TC_INVALID_PARAMS and
                           covers EBADF, ENOTSOCK, EINVAL, etc */
                        break;
                    }

#elif defined(WIN32) || defined(_WIN32_WCE)

                    switch (error = WSAGetLastError())
                    {
                    case WSAEWOULDBLOCK:
                        rv = TC_WOULDBLOCK;
                        break;
                    case WSAEMSGSIZE:
                        rv = TC_MSG_TOO_LARGE;
                        break;
                    case WSAENETDOWN:
                    case WSAEACCES:
                    case WSAEINTR:
                    case WSAEINPROGRESS:
                    case WSAEFAULT:
                    case WSAENETRESET:
                    case WSAENOBUFS:
                    case WSAESHUTDOWN:
                    case WSAEHOSTUNREACH:
                    case WSAECONNABORTED:
                    case WSAECONNRESET:
                    case WSAETIMEDOUT:
                        rv = TC_ERROR;
                        break;
                    default:
                        /* defaults are approximately equal to those
                           described in the LINUX #if block */
                        break;
                    }

#endif
                }
                else
                    rv = TC_OK;

                //reset the socket to blocking...
                if (set_nonblocking_io(vpx_sock, 0))
                    rv = TC_ERROR;

#if defined(WIN32) || defined(_WIN32_WCE)
                else
                    WSASetLastError(error);

#endif

            }
        }

        if (bytes_sent)
            *bytes_sent = n;
    }

    return rv;
}

/*
    vpx_net_sendto(struct vpxsocket* vpx_sock, tc8* buffer, tc32 buf_len,
                   tc32* bytes_sent, union vpx_sockaddr_x vpx_sa_to)
      vpx_sock - pointer to a properly initialized vpxsocket structure
      buffer - pointer to a character array containing data to be sent
      buf_len - the length of the data in buffer
      bytes_sent - pointer to an integer that will receive the actual amount
                   of data sent or NULL
      vpx_sa_to - vpx_sockaddr_x containing the address of the target
    Attempts to send buffer to vpx_sockaddr_to. This operation can be done on
    connected and unconnected sockets. If a send timeout has been set
    to a non-zero value the operation will fail if it could not be completed
    within the specified time. If the send timeout has been set to 0 the
    operation will fail if it could not be completed immediately.
    Return:
      TC_OK: on success
      TC_INVALID_PARAMS: if vpx_sock is NULL, was not properly initialized
                         via vpx_net_open, buffer is NULL or buf_len is <= 0
      TC_TIMEDOUT: if a send timeout has been set to non-zero value and the
                   operation could not be completed in the specified time
      TC_WOULDBLOCK: if the send timeout has been set to 0 and the operation
                     could not be completed immediately
      TC_ERROR: if an error other than timed out or would block is encountered
                trying to complete the operation, more information can be
                obtained through calling vpx_net_get_error
*/
TCRV vpx_net_sendto(struct vpxsocket *vpx_sock, tc8 *buffer, tc32 buf_len,
                    tc32 *bytes_sent, union vpx_sockaddr_x vpx_sa_to)
{
    TCRV rv = TC_INVALID_PARAMS;

    if (vpx_sock && (vpx_sock->state & kInited) && buffer && (buf_len > 0))
    {

        tc32 n = 0;

        if (vpx_sock->send_timeout_ms)
        {
            tc32 ret;
            fd_set write_fds;
            struct timeval tv;

            FD_ZERO(&write_fds);
            FD_SET(vpx_sock->sock, &write_fds);


            if (vpx_sock->send_timeout_ms != vpx_NET_NO_TIMEOUT)
            {
                tv.tv_sec  = 0;
                tv.tv_usec = vpx_sock->send_timeout_ms * 1000;

#if !defined(__SYMBIAN32__)
                ret = select(vpx_sock->sock + 1, NULL, &write_fds, NULL, &tv);
#else
                /*jwz 2005-10-04, symbian seems only to define a blocking select
                  via ioctl; as the client doesn't currently hit this path I
                  haven't looked for a replacement*/
                ret = -1;
                vpxassert(vpx_sock->send_timeout_ms == vpx_NET_NO_TIMEOUT);
#endif
            }
            else
            {
#if !defined(__SYMBIAN32__)
                ret = select(vpx_sock->sock + 1, NULL, &write_fds, NULL, NULL);
#else
                /*jwz 2005-10-04, symbian seems only to define a blocking select
                  via ioctl; this select was unnecessary as the socket is set to
                  blocking.*/
                ret = 1;
#endif
            }

            if (ret > 0)
            {
                switch (vpx_sock->nl)
                {
                case vpx_IPv4:
                    n = sendto(vpx_sock->sock, buffer, buf_len, 0,
                               (struct sockaddr *)&vpx_sa_to.sa_in,
                               sizeof(struct sockaddr_in));
                    break;
                case vpx_IPv6:
#if vpx_SUPPORT_IPV6
                    n = sendto(vpx_sock->sock, buffer, buf_len, 0,
                               (struct sockaddr *)&vpx_sa_to.sa_in6,
                               sizeof(struct sockaddr_in6));
#endif
                    break;
                }

                rv = (n == SOCKET_ERROR) ? TC_ERROR : TC_OK;

            }
            else if (ret < 0)
            {
                rv = TC_ERROR;
            }
            else
                rv = TC_TIMEDOUT;


        }
        else
        {

            //set the socket to non-blocking...
            if (!set_nonblocking_io(vpx_sock, 1))
            {

#if defined(WIN32) || defined(_WIN32_WCE)
                tc32 error = 0;
#endif

                switch (vpx_sock->nl)
                {
                case vpx_IPv4:
                    n = sendto(vpx_sock->sock, buffer, buf_len, 0,
                               (struct sockaddr *)&vpx_sa_to.sa_in,
                               sizeof(struct sockaddr_in));
                    break;
                case vpx_IPv6:
#if vpx_SUPPORT_IPV6
                    n = sendto(vpx_sock->sock, buffer, buf_len, 0,
                               (struct sockaddr *)&vpx_sa_to.sa_in6,
                               sizeof(struct sockaddr_in6));
#endif
                    break;
                }

                if (n < 0)
                {
#if defined(LINUX) || defined(VXWORKS) || defined(__uClinux__) || defined(__SYMBIAN32__)

                    switch (errno)
                    {
                    case EAGAIN:
#if defined(EWOULDBLOCK) && EAGAIN != EWOULDBLOCK
                    case EWOULDBLOCK:
#endif
                        rv = TC_WOULDBLOCK;
                        break;
                    case EFAULT:
                    case EINTR:
#ifdef EMSGSIZE
                    case EMSGSIZE:
#endif
#ifdef ENOBUFS
                    case ENOBUFS:
#endif
                    case ENOMEM:
                    case EPIPE:
                        rv = TC_ERROR;
                        break;
                    default:
                        /* default return is TC_INVALID_PARAMS and
                           covers EBADF, ENOTSOCK, EINVAL, etc */
                        break;
                    }

#elif defined(WIN32) || defined(_WIN32_WCE)

                    switch (error = WSAGetLastError())
                    {
                    case WSAEWOULDBLOCK:
                        rv = TC_WOULDBLOCK;
                        break;
                    case WSAEMSGSIZE:
                        rv = TC_MSG_TOO_LARGE;
                        break;
                    case WSAENETDOWN:
                    case WSAEACCES:
                    case WSAEINTR:
                    case WSAEINPROGRESS:
                    case WSAEFAULT:
                    case WSAENETRESET:
                    case WSAENOBUFS:
                    case WSAESHUTDOWN:
                    case WSAEHOSTUNREACH:
                    case WSAECONNABORTED:
                    case WSAECONNRESET:
                    case WSAETIMEDOUT:
                        rv = TC_ERROR;
                        break;
                    default:
                        /* defaults are approximately equal to those
                           described in the LINUX #if block */
                        break;
                    }

#endif
                }
                else
                    rv = TC_OK;

                //reset the socket to blocking...
                if (set_nonblocking_io(vpx_sock, 0))
                    rv = TC_ERROR;

#if defined(WIN32) || defined(_WIN32_WCE)
                else
                    WSASetLastError(error);

#endif

            }
        }

        if (bytes_sent)
            *bytes_sent = n;
    }

    return rv;
}

/*
    vpx_net_is_readable(struct vpxsocket* vpx_sock)
      vpx_sock - pointer to a properly initialized vpxsocket structure to
                 be polled to see if data can be read from it
    Return:
      0: vpx_sock was NULL, did not point to a vpxsocket structure that was
         initialized via vpx_net_open or the socket has no data that can be read
      1: the socket has data that can be read
*/
tc32 vpx_net_is_readable(struct vpxsocket *vpx_sock)
{
    tc32 is_readable = 0;

    if (vpx_sock && (vpx_sock->state & kInited))
    {
        tc32 ret;
#ifndef __SYMBIAN32__
        struct timeval tv = {0, 0};
        fd_set read_fds;

        FD_ZERO(&read_fds);
        FD_SET(vpx_sock->sock, &read_fds);

        ret = select(vpx_sock->sock + 1, &read_fds, NULL, NULL, &tv);

        if ((ret > 0) && FD_ISSET(vpx_sock->sock, &read_fds))
            is_readable = 1;

#else
        /*jwz 2005-10-04, we do use this in the client to detect an error
          condition. we'll have to see if this partial implementation is
          enough; perhaps we'll have to write a wrapper for the RSocket class*/
        /*jwz 2008-01-31, not sure whether this select workaround is necessary
          now, but current builds have been using it w/no visible ill effects*/
        char b;

        ret = set_nonblocking_io(vpx_sock, 1);
        vpxassert(!ret);
        ret = (!ret) ? recv(vpx_sock->sock, &b, 1, MSG_PEEK) : ret;

        if (ret > 0) is_readable = 1;
        else if (errno != EAGAIN && errno != KErrWouldBlock/*-1000*/)
        {
            vpxlog_error("vpx_net_is_readable, ret=%d eno=%d\n", ret, errno);
            is_readable = 1;
        }
        else is_readable = 0;

        ret = set_nonblocking_io(vpx_sock, 0);
        vpxassert(!ret);
#endif
    }

    return is_readable;
}

/*
    vpx_net_amount_readable(struct vpxsocket* vpx_sock, TCRV* rv)
      vpx_sock - pointer to a properly initialized vpxsocket structure to
                 be polled to see if data can be read from it
      rv - TCRV pointer to receive the result of the function. This parameter
           may be NULL. rv will be set to TC_OK on success and TC_ERROR
           on error.
    Return:
        The amount of data in bytes able to be read off the socket
*/
tc32 vpx_net_amount_readable(struct vpxsocket *vpx_sock, TCRV *rv)
{
    tc32 ret;
    tc32 amount = 0;

    if (vpx_sock && (vpx_sock->state & kInited))
    {
#if defined(WIN32) || defined(_WIN32_WCE)
        unsigned long amt;
        ret = ioctlsocket(vpx_sock->sock, FIONREAD, &amt);
        amount = (tc32)amt;
#elif defined(LINUX) || defined(__uClinux__)
        ret = ioctl(vpx_sock->sock, FIONREAD, &amount);
#elif defined(VXWORKS)
        ret = ioctl(vpx_sock->sock, FIONREAD, (tc32)&amount);
#elif defined(__SYMBIAN32__)
        ret = ioctl(vpx_sock->sock, E32IONREAD, &amount);
#endif

        if (rv)
            *rv = (ret == SOCKET_ERROR) ? TC_ERROR : TC_OK;
    }

    return amount;
}

/*
    vpx_net_is_writeable(struct vpxsocket* vpx_sock)
      vpx_sock - pointer to a properly initialized vpxsocket structure to
                 be polled to see if data can be written to it
    Return:
      0: vpx_sock was NULL, did not point to a vpxsocket structure that was
         initialized via vpx_net_open or the socket cannot be written to
         without blocking
      1: the socket can be written to without blocking
*/
tc32 vpx_net_is_writeable(struct vpxsocket *vpx_sock)
{
    tc32 is_writeable = 0;

    if (vpx_sock && (vpx_sock->state & kInited))
    {
        tc32 ret;
#if !defined(__SYMBIAN32__)
        struct timeval tv = {0, 0};
#endif
        fd_set write_fds;

        FD_ZERO(&write_fds);
        FD_SET(vpx_sock->sock, &write_fds);

#if !defined(__SYMBIAN32__)
        ret = select(vpx_sock->sock + 1, NULL, &write_fds, NULL, &tv);
#else
        ret = 0;
#endif

        if ((ret > 0) &&
            (FD_ISSET(vpx_sock->sock, &write_fds)))
            is_writeable = 1;
    }

    return is_writeable;
}

/*
    vpx_net_set_read_timeout(struct vpxsocket* vpx_sock, tcu32 read_timeout)
      vpx_sock - pointer to a properly initialized vpxsocket structure
      read_timeout - time to wait in milliseconds before giving up on a read
                     operation. 0 indicates that a non-blocking attempt to read
                     should be made. vpx_NET_NO_TIMEOUT - indicates the socket
                     should never timeout.
    Return:
      TC_OK: on success
      TC_INVALID_PARAMS: if vpx_sock was NULL or did not point to an vpxsocket
                         that was initialized via vpx_net_open
*/
TCRV vpx_net_set_read_timeout(struct vpxsocket *vpx_sock, tcu32 read_timeout)
{
    TCRV rv = TC_INVALID_PARAMS;

    if (vpx_sock && (vpx_sock->state & kInited))
    {

        vpx_sock->read_timeout_ms = read_timeout;
        rv = TC_OK;

    }

    return rv;
}

/*
    vpx_net_set_send_timeout(struct vpxsocket* vpx_sock, tcu32 send_timeout)
      vpx_sock - pointer to a properly initialized vpxsocket structure
      read_timeout - time to wait in milliseconds before giving up on a send
                     operation. 0 indicates that a non-blocking attempt to send
                     should be made. vpx_NET_NO_TIMEOUT - indicates the socket
                     should never timeout.
    Return:
      TC_OK: on success
      TC_INVALID_PARAMS: if vpx_sock was NULL or did not point to an vpxsocket
                         that was initialized via vpx_net_open
*/
TCRV vpx_net_set_send_timeout(struct vpxsocket *vpx_sock, tcu32 send_timeout)
{
    TCRV rv = TC_INVALID_PARAMS;

    if (vpx_sock && (vpx_sock->state & kInited))
    {

        vpx_sock->send_timeout_ms = send_timeout;
        rv = TC_OK;

    }

    return rv;
}

/*
    vpx_net_get_error(tc32* vpx_net_errno)
      vpx_net_errno - pointer to a tc32 to store the last system network
                      error code or NULL if the user does not want it
    Return:
      A string representing the last system network error that occurred. This
      string can only be used until the next call to vpx_net_get_error()
*/
tc8 *vpx_net_get_error(tc32 *vpx_net_errno)
{
#if defined(LINUX) || defined(__uClinux__) || defined(__SYMBIAN32__)

    if (vpx_net_errno)
        *vpx_net_errno = errno;

    return strerror(errno);
#elif defined(VXWORKS)
    tc32 error = errno;
    tc8 *errStr = "No Error";

    switch (error)
    {
    case ENOTSUP:
        errStr = "ENOTSUP";
        break;
    case EMSGSIZE:
        errStr = "EMSGSIZE";
        break;
    case EDESTADDRREQ:
        errStr = "EDESTADDRREQ";
        break;
    case EPROTOTYPE:
        errStr = "EPROTOTYPE";
        break;
    case ENOPROTOOPT:
        errStr = "ENOPROTOOPT";
        break;
    case EPROTONOSUPPORT:
        errStr = "EPROTONOSUPPORT";
        break;
    case ESOCKTNOSUPPORT:
        errStr = "ESOCKTNOSUPPORT";
        break;
    case EOPNOTSUPP:
        errStr = "EOPNOTSUPP";
        break;
    case EPFNOSUPPORT:
        errStr = "EPFNOSUPPORT";
        break;
    case EAFNOSUPPORT:
        errStr = "EAFNOSUPPORT";
        break;
    case EADDRINUSE:
        errStr = "EADDRINUSE";
        break;
    case EADDRNOTAVAIL:
        errStr = "EADDRNOTAVAIL";
        break;
    case ENOTSOCK:
        errStr = "ENOTSOCK";
        break;
    case ENETUNREACH:
        errStr = "ENETUNREACH";
        break;
    case ENETRESET:
        errStr = "ENETRESET";
        break;
    case ECONNABORTED:
        errStr = "ECONNABORTED";
        break;
    case ECONNRESET:
        errStr = "ECONNRESET";
        break;
    case ENOBUFS:
        errStr = "ENOBUFS";
        break;
    case EISCONN:
        errStr = "EISCONN";
        break;
    case ENOTCONN:
        errStr = "ENOTCONN";
        break;
    case ESHUTDOWN:
        errStr = "ESHUTDOWN";
        break;
    case ETOOMANYREFS:
        errStr = "ETOOMANYREFS";
        break;
    case ETIMEDOUT:
        errStr = "ETIMEDOUT";
        break;
    case ECONNREFUSED:
        errStr = "ECONNREFUSED";
        break;
    case ENETDOWN:
        errStr = "ENETDOWN";
        break;
    case ETXTBSY:
        errStr = "ETXTBSY";
        break;
    case ELOOP:
        errStr = "ELOOP";
        break;
    case EHOSTUNREACH:
        errStr = "EHOSTUNREACH";
        break;
    case ENOTBLK:
        errStr = "ENOTBLK";
        break;
    case EHOSTDOWN:
        errStr = "EHOSTDOWN";
        break;
    }

    if (vpx_net_errno)
        *vpx_net_errno = error;

    return errStr;
#elif defined(WIN32) || defined(_WIN32_WCE)
    tc8 *errstr = "No error";
    tc32 error = WSAGetLastError();

    /* there isn't much use in doing this, but it might give
       enough info without having to use net helpmsg */
    switch (error)
    {
    case WSAEWOULDBLOCK:
        errstr = "WSAEWOULDBLOCK";
        break;
    case WSAEINPROGRESS:
        errstr = "WSAEINPROGRESS";
        break;
    case WSAEALREADY:
        errstr = "WSAEALREADY";
        break;
    case WSAENOTSOCK:
        errstr = "WSAENOTSOCK";
        break;
    case WSAEDESTADDRREQ:
        errstr = "WSAEDESTADDRREQ";
        break;
    case WSAEMSGSIZE:
        errstr = "WSAEMSGSIZE";
        break;
    case WSAEPROTOTYPE:
        errstr = "WSAEPROTOTYPE";
        break;
    case WSAENOPROTOOPT:
        errstr = "WSAENOPROTOOPT";
        break;
    case WSAEPROTONOSUPPORT:
        errstr = "WSAEPROTONOSUPPORT";
        break;
    case WSAESOCKTNOSUPPORT:
        errstr = "WSAESOCKTNOSUPPORT";
        break;
    case WSAEOPNOTSUPP:
        errstr = "WSAEOPNOTSUPP";
        break;
    case WSAEPFNOSUPPORT:
        errstr = "WSAEPFNOSUPPORT";
        break;
    case WSAEAFNOSUPPORT:
        errstr = "WSAEAFNOSUPPORT";
        break;
    case WSAEADDRINUSE:
        errstr = "WSAEADDRINUSE";
        break;
    case WSAEADDRNOTAVAIL:
        errstr = "WSAEADDRNOTAVAIL";
        break;
    case WSAENETDOWN:
        errstr = "WSAENETDOWN";
        break;
    case WSAENETUNREACH:
        errstr = "WSAENETUNREACH";
        break;
    case WSAENETRESET:
        errstr = "WSAENETRESET";
        break;
    case WSAECONNABORTED:
        errstr = "WSAECONNABORTED";
        break;
    case WSAECONNRESET:
        errstr = "WSAECONNRESET";
        break;
    case WSAENOBUFS:
        errstr = "WSAENOBUFS";
        break;
    case WSAEISCONN:
        errstr = "WSAEISCONN";
        break;
    case WSAENOTCONN:
        errstr = "WSAENOTCONN";
        break;
    case WSAESHUTDOWN:
        errstr = "WSAESHUTDOWN";
        break;
    case WSAETOOMANYREFS:
        errstr = "WSAETOOMANYREFS";
        break;
    case WSAETIMEDOUT:
        errstr = "WSAETIMEDOUT";
        break;
    case WSAECONNREFUSED:
        errstr = "WSAECONNREFUSED";
        break;
    case WSAELOOP:
        errstr = "WSAELOOP";
        break;
    case WSAENAMETOOLONG:
        errstr = "WSAENAMETOOLONG";
        break;
    case WSAEHOSTDOWN:
        errstr = "WSAEHOSTDOWN";
        break;
    case WSAEHOSTUNREACH:
        errstr = "WSAEHOSTUNREACH";
        break;
    case WSAENOTEMPTY:
        errstr = "WSAENOTEMPTY";
        break;
    case WSAEPROCLIM:
        errstr = "WSAEPROCLIM";
        break;
    case WSAEUSERS:
        errstr = "WSAEUSERS";
        break;
    case WSAEDQUOT:
        errstr = "WSAEDQUOT";
        break;
    case WSAESTALE:
        errstr = "WSAESTALE";
        break;
    case WSAEREMOTE:
        errstr = "WSAEREMOTE";
        break;
    case WSASYSNOTREADY:
        errstr = "WSASYSNOTREADY";
        break;
    case WSAVERNOTSUPPORTED:
        errstr = "WSAVERNOTSUPPORTED";
        break;
    case WSANOTINITIALISED:
        errstr = "WSANOTINITIALISED";
        break;
    case WSAEDISCON:
        errstr = "WSAEDISCON";
        break;
    case WSAENOMORE:
        errstr = "WSAENOMORE";
        break;
    case WSAECANCELLED:
        errstr = "WSAECANCELLED";
        break;
    case WSAEINVALIDPROCTABLE:
        errstr = "WSAEINVALIDPROCTABLE";
        break;
    case WSAEINVALIDPROVIDER:
        errstr = "WSAEINVALIDPROVIDER";
        break;
    case WSAEPROVIDERFAILEDINIT:
        errstr = "WSAEPROVIDERFAILEDINIT";
        break;
    case WSASYSCALLFAILURE:
        errstr = "WSASYSCALLFAILURE";
        break;
    case WSASERVICE_NOT_FOUND:
        errstr = "WSASERVICE_NOT_FOUND";
        break;
    case WSATYPE_NOT_FOUND:
        errstr = "WSATYPE_NOT_FOUND";
        break;
    case WSA_E_NO_MORE:
        errstr = "WSA_E_NO_MORE";
        break;
    case WSA_E_CANCELLED:
        errstr = "WSA_E_CANCELLED";
        break;
    case WSAEREFUSED:
        errstr = "WSAEREFUSED";
        break;
    case WSAHOST_NOT_FOUND:
        errstr = "WSAHOST_NOT_FOUND";
        break;
    }

    if (vpx_net_errno)
        *vpx_net_errno = error;

    return errstr;
#endif
}

/*
    vpx_net_recv_buf(struct vpxsocket* vpx_sock, tc8 set, tc32* value)
      vpx_sock - a pointer to a properly initialized vpxsocket structure
      set - Value indicating whether the option should be set or queried.
            1 indicates the option should be set using the value stored
            in value. 0 indicates the current value of the option should
            be returned in value.
      value - depending on the value of set, either contains the size
              to set the socket's receive buffer to or will receive the
              current size of the socket's receive buffer
    Return:
      TC_OK: on success
      TC_INVALID_PARAMS: if vpx_sock is NULL, wasn't initialized via
                         vpx_net_open or value is NULL
      TC_ERROR: if the option could not be queried/set
*/
TCRV vpx_net_recv_buf(struct vpxsocket *vpx_sock, tc8 set, tc32 *value)
{
    tc32 optlen = sizeof(tc32);
    return socket_option(vpx_sock, set, SOL_SOCKET, SO_RCVBUF, value, optlen);
}

/*
    vpx_net_send_buf(struct vpxsocket* vpx_sock, tc8 set, tc32* value)
     vpx_sock - a pointer to a properly initialized vpxsocket structure
      set - Value indicating whether the option should be set or queried.
            1 indicates the option should be set using the value stored
            in value. 0 indicates the current value of the option should
            be returned in value.
      value - depending on the value of set, either contains the size
              to set the socket's send buffer to or will receive the
              current size of the socket's send buffer
    Return:
      TC_OK: on success
      TC_INVALID_PARAMS: if vpx_sock is NULL, wasn't initialized via
                         vpx_net_open or value is NULL
      TC_ERROR: if the option could not be queried/set
*/
TCRV vpx_net_send_buf(struct vpxsocket *vpx_sock, tc8 set, tc32 *value)
{
    tc32 optlen = sizeof(tc32);
    return socket_option(vpx_sock, set, SOL_SOCKET, SO_SNDBUF, value, optlen);
}

/*
    vpx_net_reuse_addr(struct vpxsocket* vpx_sock, tc8 set, tc32* value)
     vpx_sock - a pointer to a properly initialized vpxsocket structure
      set - Value indicating whether the option should be set or queried.
            1 indicates the option should be set using the value stored
            in value. 0 indicates the current value of the option should
            be returned in value.
      value - depending on the value of set, either contains an integer
              0/1 to indicate whether the socket's reuse address option
              should be turned on or off or will receive the current setting
    Return:
      TC_OK: on success
      TC_INVALID_PARAMS: if vpx_sock is NULL, wasn't initialized via
                         vpx_net_open or value is NULL
      TC_ERROR: if the option could not be queried/set
*/
TCRV vpx_net_reuse_addr(struct vpxsocket *vpx_sock, tc8 set, tc32 *value)
{
    tc32 optlen = sizeof(tc32);
    return socket_option(vpx_sock, set, SOL_SOCKET, SO_REUSEADDR, value, optlen);
}

/*
    vpx_net_linger(struct vpxsocket* vpx_sock, tc8 set, tcu16* on, tcu16* sec)
     vpx_sock - a pointer to a properly initialized vpxsocket structure
      set - Value indicating whether the option should be set or queried.
            1 indicates the option should be set using the values stored in
            on and sec. 0 indicates the current value of the option should
            be returned in on and sec.
      on - depending on the value of set indicates whether to turn on/off (1/0)
            the linger option or will receive the current setting
      sec - depending on the value of set indicates the amount of time in
            seconds for the socket to linger or will receive the current value
    Return:
      TC_OK: on success
      TC_INVALID_PARAMS: if vpx_sock is NULL, wasn't initialized via
                         vpx_net_open or on or sec are NULL
      TC_ERROR: if the option could not be queried/set
*/
TCRV vpx_net_linger(struct vpxsocket *vpx_sock, tc8 set, tcu16 *on, tcu16 *sec)
{
    TCRV rv = TC_INVALID_PARAMS;

    /*on symbian SO_LINGER is defined but there's no linger struct*/
#if !defined(__SYMBIAN32__)
    tc32 optlen = sizeof(struct linger);
    struct linger l;

    if (set && on && sec)
    {
        l.l_onoff  = *on;
        l.l_linger = *sec;

        rv = socket_option(vpx_sock, set, SOL_SOCKET, SO_LINGER, &l, optlen);
    }
    else if (on && sec)
    {
        rv = socket_option(vpx_sock, set, SOL_SOCKET, SO_LINGER, &l, optlen);

        *on  = l.l_onoff;
        *sec = l.l_linger;
    }

#else
    (void)vpx_sock;
    (void)set;
    (void)on;
    (void)sec;
#endif

    return rv;
}


/* udp only functions */

/*
    vpx_net_multicast_ttl(struct vpxsocket* vpx_sock, tc8 set, tcu8* value)
      vpx_sock - pointer to a properly initialized vpxsocket structure
      set - flag indicating whether to set (non-zero value) or
            query (0) the option
      value - depending on the value of set, sets the ttl to the value stored
              in value or receives the current value of ttl
    Attempts to set/query the multicast ttl value of the socket
    represented by vpx_sock
    Return:
      TC_OK: on success
      TC_INVALID_PARAMS: if vpx_sock is NULL, was not initialized via
                         vpx_net_open, the socket is not a udp socket or
                         value is NULL
      TC_ERROR: if the option could not be queried/set
*/
TCRV vpx_net_multicast_ttl(struct vpxsocket *vpx_sock, tc8 set, tcu8 *value)
{
    TCRV rv = TC_INVALID_PARAMS;
    tc32 optlen = sizeof(tcu8);

    if (vpx_sock && (vpx_sock->state & kInited) && (vpx_sock->tl == vpx_UDP))
    {
        switch (vpx_sock->nl)
        {
        case vpx_IPv4:
            rv = socket_option(vpx_sock, set, IPPROTO_IP, IP_MULTICAST_TTL, value, optlen);
            break;
        case vpx_IPv6:
#if vpx_NET_SUPPORT_IPV6
            rv = socket_option(vpx_sock, set, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, value, optlen);
#endif
            break;
        }
    }

    return rv;
}

/*
    vpx_net_join_multicast(struct vpxsocket* vpx_sock,
                           union vpx_sockaddr_x* remote_addr)
      vpx_sock - pointer to a properly initialized vpxsocket structure
      local_addr - an vpx_sockaddr_x structure containing the address of the
                   local interface to use for the multicast session or NULL
                   indicating any interface can be used. (Currently ignored
                   for IPv6).
      remote_addr - an vpx_sockaddr_x structure containing the multicast address
    Attempts to add vpx_sock to the multicast session indicated by remote_addr.
    On success the reuse_addr option will be set on vpx_sock so others may
    join the session and vpx_sock will be added to the session.
    Return:
      TC_OK: on success
      TC_INVALID_PARAMS: if vpx_sock is NULL, was not properly initialized via
                         vpx_net_open, vpx_sock does not represent a UDP socket,
                         or remote_addr is NULL
      TC_ERROR: if the reuse_addr option could not be set or the socket could
                not be added to the multicast session
*/
TCRV vpx_net_join_multicast(struct vpxsocket *vpx_sock,
                            union vpx_sockaddr_x *local_addr,
                            union vpx_sockaddr_x *remote_addr)
{
    TCRV rv = TC_INVALID_PARAMS;

    if (vpx_sock && (vpx_sock->state & kInited) &&
        (vpx_sock->tl == vpx_UDP) && remote_addr)
    {

        switch (vpx_sock->nl)
        {
        case vpx_IPv4:
        {
            tc32 on = 1;
            struct ip_mreq mreq;

            //allow others to bind to the same multicast port
            rv = vpx_net_reuse_addr(vpx_sock, 1, &on);

            if (!rv)
            {

                mreq.imr_interface.s_addr = local_addr ?
                                            local_addr->sa_in.sin_addr.s_addr :
                                            INADDR_ANY;
                mreq.imr_multiaddr.s_addr = remote_addr->sa_in.sin_addr.s_addr;

                memset(&vpx_sock->local_addr.sa_in, 0, sizeof(struct sockaddr_in));
                memset(&vpx_sock->remote_addr.sa_in, 0, sizeof(struct sockaddr_in));

                vpx_sock->local_addr.sa_in.sin_addr.s_addr  = mreq.imr_interface.s_addr;
                vpx_sock->remote_addr.sa_in.sin_addr.s_addr = mreq.imr_multiaddr.s_addr;

                rv = socket_option(vpx_sock, 1, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                                   &mreq, sizeof(struct ip_mreq));
            }
        }
        break;
        case vpx_IPv6:
#if vpx_NET_SUPPORT_IPV6
            {
                tc32 on = 1;
                struct ipv6_mreq mreq;

                //allow others to bind to the same multicast port
                rv = vpx_net_reuse_addr(vpx_sock, 1, &on);

                if (!rv)
                {

                    struct in6_addr in6 = in6addr_any;

                    mreq.ipv6mr_interface = 0;

                    memset(&vpx_sock->local_addr.sa_in6, 0, sizeof(struct sockaddr_in6));
                    memset(&vpx_sock->remote_addr.sa_in6, 0, sizeof(struct sockaddr_in6));

                    memcpy(&mreq.ipv6mr_multiaddr, &remote_addr->sa_in6.sin6_addr,
                               sizeof(struct in6_addr));

                    memcpy(&vpx_sock->local_addr.sa_in6.sin6_addr,
                               &in6, sizeof(struct in6_addr));

                    memcpy(&vpx_sock->remote_addr.sa_in6.sin6_addr,
                               &mreq.ipv6mr_multiaddr, sizeof(struct in6_addr));

                    rv = socket_option(vpx_sock, 1, IPPROTO_IPV6, IPV6_ADD_MEMBERSHIP,
                                       &mreq, sizeof(struct ipv6_mreq));
                }
            }
#endif
            break;
        }
    }


    return rv;
}


/*
    vpx_net_join_multicast_addr(struct vpxsocket* vpx_sock
                                 , tc8* ip_addr
                                 , tcu16 port)
      vpx_sock - pointer to an vpxsocket structure that is to be connencted
                 to the endpoint described by ip_addr and port
      ip_addr - pointer to a character string that contains the multicast
                address to attempt to join
      port - the port to attempt to join on ip_addr
    Attempt to join vpx_sock to port on ip_addr.
    Return:
        TC_OK: on success
        TC_INVALID_PARAMS: if vpx_sock is NULL, was not properly initialized via
                         vpx_net_open, vpx_sock does not represent a UDP socket,
                         or remote_addr is NULL
        TC_ERROR: if the reuse_addr option could not be set or the socket could
                not be added to the multicast session
*/
TCRV vpx_net_join_multicast_addr(struct vpxsocket *vpx_sock
                                 , tc8 *ip_addr
                                 , tcu16 port)
{
    TCRV rv = TC_INVALID_PARAMS;

    if (vpx_sock && (vpx_sock->state & kInited) &&
        (vpx_sock->tl == vpx_UDP) && ip_addr)
    {
        rv = vpx_net_get_addr_info(ip_addr, port, vpx_sock->nl,
                                   vpx_sock->tl, &vpx_sock->remote_addr);

        if (!rv)
        {
            tc32 ret = 0;

            ret = vpx_net_bind(vpx_sock,
                               NULL,
                               port);

            if (!ret)
            {
                ret = vpx_net_join_multicast(vpx_sock,
                                             NULL,
                                             &vpx_sock->remote_addr);

                if (!ret)
                {
                    rv = TC_OK;
                    vpx_sock->state |= kConnected;
                }
                else
                    rv = TC_ERROR;
            }
            else
            {
                rv = TC_ERROR;
            }
        }
    }

    return rv;
}

/*
    vpx_net_leave_multicast(struct vpxsocket* vpx_sock)
      vpx_sock - pointer to a properly initialized vpxsocket structure to be
                 removed from the multicast session
    Attempts to remove vpx_sock from the multicast session it was previously
    added to via vpx_net_join_multicast
    Return:
      TC_OK: on success
      TC_INVALID_PARAMS: if vpx_sock is NULL, was not properly initialized via
                         vpx_net_open or the socket is not a UDP socket
      TC_ERROR: if the socket could not be removed from the session
*/
TCRV vpx_net_leave_multicast(struct vpxsocket *vpx_sock)
{
    TCRV rv = TC_INVALID_PARAMS;

    if (vpx_sock && (vpx_sock->state & kInited) && (vpx_sock->tl == vpx_UDP))
    {

        switch (vpx_sock->nl)
        {
        case vpx_IPv4:
        {
            struct ip_mreq mreq;

            mreq.imr_interface.s_addr = vpx_sock->local_addr.sa_in.sin_addr.s_addr;
            mreq.imr_multiaddr.s_addr = vpx_sock->remote_addr.sa_in.sin_addr.s_addr;

            rv = socket_option(vpx_sock, 1, IPPROTO_IP, IP_DROP_MEMBERSHIP,
                               &mreq, sizeof(struct ip_mreq));
        }
        break;
        case vpx_IPv6:
#if vpx_NET_SUPPORT_IPV6
            {
                struct ipv6_mreq mreq;

                mreq.ipv6mr_interface = 0;

                memcpy(&mreq.ipv6mr_multiaddr, &vpx_sock->remote_addr.sa_in6.sin6_addr,
                           sizeof(struct in6_addr));

                rv = socket_option(vpx_sock, 1, IPPROTO_IPV6, IPV6_DROP_MEMBERSHIP,
                                   &mreq, sizeof(struct ipv6_mreq));
            }
#endif
            break;
        }
    }

    return rv;
}

/* END - udp only functions */

/*
 *
 * END - Exposed library functions
 *
*/

/*
 *
 * Internal library functions
 *
*/

/*
    socket_option(struct vpxsocket* vpx_sock, tc8 set, tc32 level,
                  tc32 option, void* value, tc32 optlen)
    Generic function used to wrap set/getsockopt. Used by exposed library
    functions to set/get socket options
*/
static TCRV socket_option(struct vpxsocket *vpx_sock, tc8 set, tc32 level,
                          tc32 option, void *value, tc32 optlen)
{
    TCRV rv = TC_INVALID_PARAMS;
    tc32 ret;

    if (vpx_sock && (vpx_sock->state & kInited) && value)
    {
        if (set)
            ret = setsockopt(vpx_sock->sock, level, option, value, optlen);
        else
            ret = getsockopt(vpx_sock->sock, level, option, value, (tcu32 *)&optlen);

        rv = (ret == SOCKET_ERROR) ? TC_ERROR : TC_OK;
    }

    return rv;
}

/*
    set_nonblocking_io(struct vpxsocket* vpx_sock, tc32 on)
      vpx_sock - pointer to an vpxsocket structure
      on - turn non-blocking io on (non-zero value) / off (0)
    Internal library function used to turn non-blocking io
    on/off for the specified socket
*/
static tc32 set_nonblocking_io(struct vpxsocket *vpx_sock, tc32 on)
{
#if defined(WIN32) || defined(_WIN32_WCE)
    return ioctlsocket(vpx_sock->sock, FIONBIO, &((unsigned long)on));
#elif defined(VXWORKS)
    return ioctl(vpx_sock->sock, FIONBIO, (tc32)&on);
#elif defined(__SYMBIAN32__)
    /*jwz 2005-10-04, FIONBIO is undefined on symbian and I haven't looked for
      a replacement. For now this is acceptable as the only use of
      non-blocking io in the client is rtp(udp)*/
    //(void)vpx_sock; (void)on;
    //return -1;
    //const int dbg_on=on;
    int ret;

    if (on) ret = setsockopt(vpx_sock->sock, SOL_SOCKET, KSONonBlockingIO, &on, sizeof(on));
    else ret = setsockopt(vpx_sock->sock, SOL_SOCKET, KSOBlockingIO, (on = 1, &on), sizeof(on));

    //vpxlog_dbg("set_nbio(%d): ret:%d eno:%d\n",dbg_on,ret,eno);
    return ret;
#else
    return ioctl(vpx_sock->sock, FIONBIO, &on);
#endif
}
