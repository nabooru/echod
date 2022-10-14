
/********************************************************************************* 
 * echod.c - Simple single-threaded, select-based TCP echo server.
 * 
 *
 *
 **********************************************************************************/

#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
/* #include "log.h" */

#define RECV_BUFSIZE 4096

typedef struct buffer {
   char   data[RECV_BUFSIZE];
   size_t size;
   size_t length;
} buffer_t;

typedef SOCKET socket_t;

/* node_t */
typedef struct node {
   socket_t sfd;      /* Socket descriptor */  
   int      type;     /* Socket type (listen or accept) */
   int      fdclose;
   size_t   rx;       /* Total bytes received */ 
   size_t   sx;       /* Total bytes sent */
   buffer_t *buffer;
   struct sockaddr_in sin;
   struct node *next; /* Linked list */
} node_t;

#define SF_LISTEN    0
#define SF_ACCEPT    1

#define ECHO_DEFAULT_PORT 7
#define MAX_NODE_COUNT    50

#define false        0
#define true         1

/* $fixme */
#define ECHO_EOK      0x000
#define ECHO_ENOMEM   0x100
#define ECHO_ENOENT   0x101
#define ECHO_EINVAL   0x102
#define ECHO_EWSAFAIL 0x103
#define ECHO_ESELECT  0x104
#define ECHO_EDEBUG   0x200

/* node_alloc  */
static node_t *node_alloc(socket_t sfd, int type)
{
   node_t *node = NULL;

   node = (node_t *)malloc(sizeof(node_t));
   if (node == NULL)
       return NULL;

   node->sfd = sfd;
   node->type = type;
   node->fdclose = false; 
   node->rx = 0;
   node->sx = 0;
   node->buffer = NULL;
  
   if (node->type == SF_ACCEPT) {
       node->buffer = (buffer_t *)malloc(sizeof(buffer_t));
       if (node->buffer == NULL) {
           free(node);
           return NULL;
       }         
       memset(node->buffer->data, 0, sizeof(node->buffer->data));
       node->buffer->size = sizeof(node->buffer->data);
       node->buffer->length = 0;    
   }
   memset(&node->sin, 0, sizeof(node->sin));
   node->next = NULL;  
   return node;
}

/* node_append */
static node_t *node_append(node_t *node, socket_t sfd, int type)
{
   node_t *current = NULL;

   if (node != NULL) {
       while (node->next != NULL) 
           node = node->next;
       current = node_alloc(sfd, type);
       if (current != NULL) {
           current->next = NULL; /* Redundant. */
           node->next = current; 
       }  
   }
   return current;
}

/* node_free */
static void node_free(node_t *node)
{
   if (node != NULL) {
       if (node->sfd != INVALID_SOCKET) {
           shutdown(node->sfd, SD_SEND);  
           closesocket(node->sfd);
       }
       if (node->buffer != NULL)
           free(node->buffer);
       free(node);    
   }
}

/* It assumes that the head node can never be removed. $fixme */
static int node_remove(node_t *node, socket_t sfd)
{
   node_t *current = NULL;
   node_t *tmp = NULL;
   int match = false;

   if (node != NULL) {   
       while (node->next != NULL && !match) {
           if (node->next->sfd == sfd) {
               current = node->next;
               node->next = current->next;
               node_free(current);
               match = true;
           } else  
               node = node->next;  
       }
   }
   return match;       
}

static void node_destroy(node_t *node)
{
   node_t *current = NULL;

   while (node != NULL) {
       current = node;
       node = node->next;
       node_free(current);   
   }
}

static int winsock_init(void)
{
   WORD wVersionRequested;
   WSADATA wsaData; 
   int result;
    
   wVersionRequested = MAKEWORD(2, 2);
   result = WSAStartup(wVersionRequested, &wsaData);
   if (result != 0)
       return result;

   if (LOBYTE(wsaData.wVersion) != 2 || HIBYTE(wsaData.wVersion) != 2) {
       WSACleanup();
       return WSAVERNOTSUPPORTED;
   }

   return result;
}

/* echo_socket */
static int echo_socket(node_t **node, unsigned short port)
{
   socket_t sfd = INVALID_SOCKET;
   struct addrinfo *result = NULL;
   struct addrinfo hints;
   struct addrinfo *ptr = NULL;
   node_t *current = NULL;
   char buffer[5+1];
   int optval = 1; /* FIONBIO */
   int rc, err;

   memset(&hints, 0, sizeof(hints));
   hints.ai_family   = AF_INET; /* IPv4 address family */
   hints.ai_socktype = SOCK_STREAM;
   hints.ai_protocol = IPPROTO_TCP;
   hints.ai_flags    = AI_PASSIVE;

   memset(buffer, '\0', sizeof(buffer));
   _snprintf(buffer, sizeof(buffer), "%u", port);

   rc = getaddrinfo(NULL, buffer, &hints, &result);
   if (rc != 0) {
       err = WSAGetLastError();
       WSACleanup();
       return err; 
   }

   *node = NULL;
   
   /* For each returned interface, create a listening socket. */
   for (ptr = result; ptr != NULL; ptr = ptr->ai_next) {

       sfd = socket(ptr->ai_family, ptr->ai_socktype, ptr->ai_protocol);
       if (sfd == INVALID_SOCKET)
           continue; 

       rc = bind(sfd, ptr->ai_addr, (int)ptr->ai_addrlen);
       if (rc == SOCKET_ERROR) {
           closesocket(sfd);
           continue;
       }  

       rc = listen(sfd, SOMAXCONN);
       if (rc == SOCKET_ERROR) {
           closesocket(sfd);
           continue;  
       }
  
       rc = ioctlsocket(sfd, FIONBIO, &optval);
       if (rc == SOCKET_ERROR) {
           closesocket(sfd);
           continue;         
       }

       /* Create a linked list and add the descriptor. */
       if (*node == NULL) {
           *node = node_alloc(sfd, SF_LISTEN);
           current = *node; 
       } else 
           current = node_append(*node, sfd, SF_LISTEN);   

       if (current == NULL) {
           closesocket(sfd);
           node_destroy(*node);
           WSACleanup();     
           return ECHO_ENOMEM;
       }     
       memcpy(&current->sin, ptr->ai_addr, ptr->ai_addrlen);
   }

   freeaddrinfo(result);

   /* If *node is still NULL, we don't have a list. */
   if (*node == NULL) {
       if (sfd != INVALID_SOCKET)
           closesocket(sfd);
       WSACleanup();
       return ECHO_ENOENT;
   }

   return ECHO_EOK;
}


/* echo_accept */
static int echo_accept(node_t *current, node_t **client, int fdclose)
{
   socket_t sfd = INVALID_SOCKET;
   unsigned long mode = 1;
   int sinlen;
   int result;
   node_t *cl = NULL;
   
   sfd = accept(current->sfd, NULL, NULL);
   if (sfd == INVALID_SOCKET) { 
       return WSAGetLastError();
   }
 
   if (fdclose) {
       shutdown(sfd, SD_SEND);
       closesocket(sfd);
       return ECHO_EDEBUG;
   }
   
   result = ioctlsocket(sfd, FIONBIO, &mode);
   if (result == SOCKET_ERROR) {
       closesocket(sfd);  
       return WSAGetLastError();
   }   
   
   cl = node_append(current, sfd, SF_ACCEPT);
   if (cl == NULL) { 
       closesocket(sfd);
       return ECHO_ENOMEM;
   }

   sinlen = sizeof(struct sockaddr_in);
   result = getpeername(cl->sfd, &cl->sin, &sinlen);

   *client = cl;
      
   return 0; 
}

/* echo_recv */
static int echo_recv(node_t *current)
{
   char *ptr = NULL;
   int result = SOCKET_ERROR;
   int len; /* remaining len in the buffer */
   int err;
  
   ptr = current->buffer->data + current->buffer->length;
   len = current->buffer->size - current->buffer->length;  
 
   if (len > 0) {
       result = recv(current->sfd, ptr, len, 0);
       if (result == SOCKET_ERROR) {
           err = WSAGetLastError();
           if (err != 10035)
                current->fdclose = true;
       } else if (result == 0) {
           current->fdclose = true;
       } else if (result > 0) {
           current->buffer->length += result;
           current->rx += result; 
       }   
   }

   return result;
}

/* echo_send */
static int echo_send(node_t *current)
{
   char *ptr = current->buffer->data;
   int result = SOCKET_ERROR;
   int rc;

   int i;

   if (current->buffer->length > 0) { 
       int len = current->buffer->length; 
       result = send(current->sfd, ptr, len, 0);
      
       if (result == SOCKET_ERROR) {
           rc = WSAGetLastError();
           if (rc != 10035)
               current->fdclose = true;  
       } else if (result == 0)
           current->fdclose = true;     
       else if (result > 0) {
           if (result != len) { 
               ptr = current->buffer->data + result;
               memcpy(current->buffer->data, ptr, result);
               printf("#DBG_WARNING: Partial send \n");
           }  
           current->buffer->length -= result;
           current->sx += result;
       }
   }   

   return result;
}

/* echo_close */
static void echo_close(node_t *node)
{
    node_destroy(node);
    WSACleanup();  
}

#define log_trace printf
#define log_error printf


static char *unix_epoch(void)
{
   time_t now;
   struct tm *ptr = NULL;
   static char buffer[16];

   time(&now);
   ptr = localtime(&now);
   memset(buffer, '\0', sizeof(buffer));
   strftime(buffer, sizeof(buffer), "%H:%M:%S", ptr);

   return buffer;
}

/* echo_listen */
int echo_listen(const char *server, unsigned short port, int timeout, int debug)
{
   socket_t sfd = INVALID_SOCKET; /* max */
   node_t *node = NULL; /* Head of the list */
   node_t *current = NULL;
   fd_set rdfdset, wrfdset;
   int result, nready;
   unsigned long count = 0;
   unsigned short pid;

   result = winsock_init();
   if (result != 0) {
       log_error("[%s] Error: unable to initialize the sockets layer\n", unix_epoch());
       return ECHO_EWSAFAIL;
   }

   result = echo_socket(&node, port);
   if (result != ECHO_EOK) {
       log_error("[%s] Error: could not create socket\n", unix_epoch());
       return ECHO_EDEBUG;
   }   

   pid = GetCurrentProcessId();
   log_trace("[%s] Starting echo daemon v.0.0.1 build 0 pid=0x%04x\n", unix_epoch(), pid);

   /* Print all the interfaces we are bound to. */
   current = node;
   while (current != NULL) {
       log_trace("[%s] Binding to %s:%u id=0x%04x\n", unix_epoch(), 
           inet_ntoa(current->sin.sin_addr), current->sin.sin_port, current->sfd);
       current = current->next; 
   }
    
   log_trace("[%s] Daemon listening on port %u \n", unix_epoch(), port);

   result = 0;

   while (!result) {

       FD_ZERO(&rdfdset);
       FD_ZERO(&wrfdset);
  
       /* Set up descriptors and find max sfd. */ 
       current = node;
       sfd = INVALID_SOCKET;
       count = 0;

       while (current != NULL) {
           FD_SET(current->sfd, &rdfdset); 
           if (current->type == SF_ACCEPT)  {
               if (current->buffer->length > 0) { 
                   FD_SET(current->sfd, &wrfdset);
               } 
           }
           if (current->sfd > sfd)
               sfd = current->sfd; 
           count++;
           current = current->next;      
       }  
      
       nready = select(sfd+1, &rdfdset, &wrfdset, NULL, NULL);
              
       if (nready == SOCKET_ERROR) {
           result = ECHO_ESELECT; 
       } else if (nready == 0) {

           /* Do nothing, for now. */
           printf("#WARNING SELECT\n");

       } else if (nready > 0) {
           current = node;
           while (current != NULL && nready > 0) {
               if (current->type == SF_LISTEN) {

                   if (FD_ISSET(current->sfd, &rdfdset)) {
                       node_t *client = NULL;
                       int rc;
                       nready--;      
                       if (count < (MAX_NODE_COUNT - 1))                                               
                       rc = echo_accept(current, &client, count > (MAX_NODE_COUNT - 1));  
                       if (rc == ECHO_EOK) {
                           log_trace("[%s] Accepted node id=0x%04x %s:%u\n", unix_epoch(), 
                               client->sfd, inet_ntoa(client->sin.sin_addr), client->sin.sin_port);
                       }      
                   } 
               } else if (current->type == SF_ACCEPT) {
                   if (FD_ISSET(current->sfd, &wrfdset)) {
                       int rc;
                       nready--;
                       rc = echo_send(current); 
                       if (rc == SOCKET_ERROR || rc == 0) {
                           log_trace("[%s] Disconnected node id=0x%04x %s:%u %u %u\n", 
                               unix_epoch(),current->sfd,inet_ntoa(current->sin.sin_addr),
                               current->sin.sin_port, current->rx, current->sx);
                       }
                   }             
                    else if (FD_ISSET(current->sfd, &rdfdset)) {
                       int rc;  
                       nready--;
                       rc = echo_recv(current); 
                       if (rc == SOCKET_ERROR || rc == 0) {
                           log_trace("[%s] Disconnected node id=0x%04x %s:%u %u %u\n", 
                               unix_epoch(),current->sfd,inet_ntoa(current->sin.sin_addr), 
                               current->sin.sin_port, current->rx, current->sx); 
                       }                         
                   }
               }

               /* Advance into the list. */
               if (current->fdclose) {
                   node_t *tmp = current->next; 
                   node_remove(node, current->sfd);  
                   current = tmp; 
               } else {
                   current = current->next; 
               }

           } 
       } 
   } 

   echo_close(node);

   return result;
}

/* To do: parse arguments. */
int main(int argc, char *argv[])
{
   int result;
   unsigned short port = 0;
   char *name;
  
   if (argc != 1 && argc != 3) {
       fprintf(stderr, "-%s: incorrect number of arguments\n", argv[0]);
       fprintf(stderr, "\nUsage:  %s [-p port]\n", argv[0]); 
       return EXIT_FAILURE;    
   }

   name = argv[0];

   if (argc == 3) {
       argc--;
       argv++;
       if (strcmp(*argv, "-p") == 0) {
           port = atoi(*(++argv));
       } else {
           fprintf(stderr, "-%s: invalid argument: %s \n", name, *argv);
           return EXIT_FAILURE;    
       }
   }
   
   if (port == 0)
       port = ECHO_DEFAULT_PORT;

   result = echo_listen(NULL, port, 3000, 0);
   if (result != ECHO_EOK) {
       printf("-%s: an error has occurred: %u\n", name, result);
       return EXIT_FAILURE;
   }

   return EXIT_SUCCESS;
}


/* EOF */