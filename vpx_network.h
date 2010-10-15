/*
 *  Copyright (c) 2010 The WebM project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#ifndef __vpx_NETWORK_H__
#define __vpx_NETWORK_H__

/* vpx_network version info */
#define vpx_network_version "2.1.1.7"

#define vpx_NETWORK_VERSION_CHIEF 2
#define vpx_NETWORK_VERSION_MAJOR 1
#define vpx_NETWORK_VERSION_MINOR 1
#define vpx_NETWORK_VERSION_PATCH 7
/* end - vpx_network version info */

#include "tctypes.h"

#if defined(_WIN32_WCE) && _WIN32_WCE < 420
# define WIN32_LEAN_AND_MEAN
# include <winsock.h>
# ifndef WINSOCK_VERSION
#  define WINSOCK_VERSION MAKEWORD(1,1)
# endif
#elif defined(WIN32) || defined(_WIN32_WCE)
# define WIN32_LEAN_AND_MEAN
# include <winsock2.h>
# include <ws2tcpip.h>          //for IPv6 structures/functions, IPPROTO_IP options
# ifdef getaddrinfo             //some IPv6 calls/structures are missing without the

//current platform sdk (2/2003)
#  define vpx_NET_SUPPORT_IPV6 1 //support on 2000 is an option, but no production
//release will be made for it. If the support isn't
//there the calls will map to IPv4 calls (i.e. getaddrinfo
//to inet_addr/gethostbyname).
//Starting with XP full support was added.
//XP SP.1+/Server 2003 have a production
//implementation of IPv6.
# endif
#elif defined(LINUX) || defined(__uClinux__) || defined(__SYMBIAN32__)
# include <unistd.h>      //for close if undefined elsewhere
# include <sys/types.h>
# include <sys/time.h>    //for timeval if undefined elsewhere
# include <sys/ioctl.h>   //for ioctl, FIONREAD
# include <sys/socket.h>  //for socket(), bind()...SOCK_STREAM
# include <netinet/in.h>  //sockaddr_in...
# include <arpa/inet.h>   //inet_addr
# include <netdb.h>       //for addrinfo
# include <errno.h>
#elif defined(VXWORKS)
# include <hostLib.h>
# include <sockLib.h>
# include <netinet/in.h>  //for sockaddr_in, socket options, etc.
# include <ioLib.h>       //for FIONREAD
# include <netdb.h>
#elif defined(TI_OMAP)
# define ntohl(x) htonl(x)
# define ntohs(x) htons(x)
#elif defined(vpx_NET_STUBS)
#else
# error "Network support not yet added for this platform!"
#endif

#ifndef vpx_NET_SUPPORT_IPV6
# define vpx_NET_SUPPORT_IPV6 0
#endif

#define vpx_NET_NO_TIMEOUT 0xffffffff

#if defined(__cplusplus)
extern "C" {
#endif

#if !defined(vpx_NET_STUBS)

    /*
        Valid network layers to be passed to vpx_net_ functions.
        Prefixed with vpx_ to prevent naming conflicts.
    */
    enum network_layer {
        vpx_IPv4,
        vpx_IPv6
    };

    /*
        Valid transport layers to be passed to vpx_net_ functions.
        Prefixed with vpx_ to prevent naming conflicts.
    */
    enum transport_layer
    {
        vpx_TCP = SOCK_STREAM,
        vpx_UDP = SOCK_DGRAM
    };

    /*
        Union used in calls to vpx_net_ functions.
        Depending on the network layer in use the correct sockaddr structure
        will be chosen internally by the library.
    */
    union vpx_sockaddr_x
    {
        struct sockaddr_in sa_in;
#if vpx_NET_SUPPORT_IPV6
        struct sockaddr_in6 sa_in6;
#endif
    };

    /*
        vpx_network's socket representation, used in vpx_net_ function calls
    */
    struct vpxsocket
    {

#if defined(WIN32) || defined(_WIN32_WCE)
        SOCKET sock;
#else
        tc32 sock;
#endif

        tc32 state;
        tcu32 read_timeout_ms,
              send_timeout_ms;

        enum network_layer nl;
        enum transport_layer tl;

        union vpx_sockaddr_x local_addr,
                remote_addr;
    };

    /*
        vpx_net_init()
        Performs any necessary system dependent network initialization.
        Should be called before any other vpx_network function.
        Return: TC_OK on success, TC_ERROR otherwise
    */
    TCRV vpx_net_init();

    /*
        vpx_net_destroy()
        Performs any necessary system dependent network deinitialization
    */
    void vpx_net_destroy();

    /*
        vpx_net_set_loglevel
        Sets the log level for this library
    */
    void vpx_net_set_loglevel(tc32 level);


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
                      enum transport_layer trans_layer);

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
    TCRV vpx_net_close(struct vpxsocket *vpx_sock);

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
                      tcu16 port);

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
    TCRV vpx_net_listen(struct vpxsocket *vpx_sock, tc32 backlog);

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
    TCRV vpx_net_accept(struct vpxsocket *vpx_sock, struct vpxsocket *vpx_sock_peer);

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
    TCRV vpx_net_connect(struct vpxsocket *vpx_sock,
                         tc8 *ip_addr,
                         tcu16 port);

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
                               union vpx_sockaddr_x *vpx_sa_x);

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
                      tc32 buf_len, tc32 *bytes_read);

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
                          tc32 *bytes_read, union vpx_sockaddr_x *vpx_sa_from);

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
                      tc32 buf_len, tc32 *bytes_sent);

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
                        tc32 *bytes_sent, union vpx_sockaddr_x vpx_sa_to);

    /*
        vpx_net_is_readable(struct vpxsocket* vpx_sock)
          vpx_sock - pointer to a properly initialized vpxsocket structure to
                     be polled to see if data can be read from it
        Return:
          0: vpx_sock was NULL, did not point to a vpxsocket structure that was
             initialized via vpx_net_open or the socket has no data that can be read
          1: the socket has data that can be read
    */
    tc32 vpx_net_is_readable(struct vpxsocket *vpx_sock);

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
    tc32 vpx_net_amount_readable(struct vpxsocket *vpx_sock, TCRV *rv);

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
    tc32 vpx_net_is_writeable(struct vpxsocket *vpx_sock);

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
    TCRV vpx_net_set_read_timeout(struct vpxsocket *vpx_sock, tcu32 read_timeout);

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
    TCRV vpx_net_set_send_timeout(struct vpxsocket *vpx_sock, tcu32 send_timeout);

    /*
        vpx_net_get_error(tc32* vpx_net_errno)
          vpx_net_errno - pointer to a tc32 to store the last system network
                          error code or NULL if the user does not want it
        Return:
          A string representing the last system network error that occurred. This
          string can only be used until the next call to vpx_net_get_error()
    */
    tc8 *vpx_net_get_error(tc32 *vpx_net_errno);

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
    TCRV vpx_net_recv_buf(struct vpxsocket *vpx_sock, tc8 set, tc32 *value);

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
    TCRV vpx_net_send_buf(struct vpxsocket *vpx_sock, tc8 set, tc32 *value);

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
    TCRV vpx_net_reuse_addr(struct vpxsocket *vpx_sock, tc8 set, tc32 *value);

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
    TCRV vpx_net_linger(struct vpxsocket *vpx_sock, tc8 set, tcu16 *on, tcu16 *sec);

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
    TCRV vpx_net_multicast_ttl(struct vpxsocket *vpx_sock, tc8 set, tcu8 *value);


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
                                union vpx_sockaddr_x *remote_addr);

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
                                     , tcu16 port);


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
    TCRV vpx_net_leave_multicast(struct vpxsocket *vpx_sock);

#else //!defined(vpx_NET_STUBS)
    struct vpxsocket
    {
        int dummy;
    };

# define vpx_net_init() 0
# define vpx_net_destroy() 0
# define vpx_net_set_loglevel(l)
    tc8 *vpx_net_get_error(tc32 *vpx_net_errno);

#endif

#if defined(__cplusplus)
}
#endif

#endif //__vpx_NETWORK_H__
