#include "proxy_parse.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <semaphore.h>

// how to free up cache

#define MAX_CLIENTS 10 // change later, inc it
#define MAX_BYTES 4096 // max allowed size of http request/response
#define MAX_SIZE 200*(1<<20) // max size of our total LRU cache
#define MAX_ELEMENT_SIZE 10*(1<<20) // max size of a element in cache
typedef struct cache_element cache_element;

struct cache_element
{
    char *data;
    int len;
    char *url;
    time_t lru_time_track;
    cache_element *next;
};

cache_element *find(char *url);
int add_cache_element(char *data, int len, char *url);
void remove_cache_element();

int port_number = 8080;
int proxy_socketId;
pthread_t tid[MAX_CLIENTS]; // store threads IDs in this array
sem_t semaphore;
pthread_mutex_t lock;

cache_element *head;
int cache_size;

int connectRemoteServer(char* host_addr, int port_number){
    int remoteSocket = socket(AF_INET, SOCK_STREAM, 0);
    if(remoteSocket<0){
        printf("Failed in creating remote socket \n");
        return -1;
    }
    struct hostent* host = gethostbyname(host_addr);
    if(host == NULL){
        fprintf(stderr, "No such host exists\n");
        return -1;
    }

    struct sockaddr_in* server_addr;
    bzero((char*)&server_addr, sizeof(server_addr));
    server_addr->sin_family = AF_INET;
    server_addr->sin_port = htons(port_number);
    bcopy((char*)host->h_addr, (char*)&server_addr->sin_addr.s_addr, host->h_length);
    if(connect(remoteSocket, (struct sockaddr*)&server_addr, (size_t)sizeof(server_addr))<0){
        fprintf(stderr, "Error connecting with remote socket\n");
        return -1;
    }
    return remoteSocket;

}

int checkHTTPversion(char* msg){
    int version;
    if(strncmp(msg, "HTTP/1.1", 8 ) == 0){
        version = 1;
    }else if(strncmp(msg, "HTTP/1.0", 8 ) == 0){
        version = 1;
    }else {
        version = -1;
    }
    return version;
}

int sendErrorMessage(int socket, int status_code)
{
	char str[1024];
	char currentTime[50];
	time_t now = time(0);

	struct tm data = *gmtime(&now);
	strftime(currentTime,sizeof(currentTime),"%a, %d %b %Y %H:%M:%S %Z", &data);

	switch(status_code)
	{
		case 400: snprintf(str, sizeof(str), "HTTP/1.1 400 Bad Request\r\nContent-Length: 95\r\nConnection: keep-alive\r\nContent-Type: text/html\r\nDate: %s\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>400 Bad Request</TITLE></HEAD>\n<BODY><H1>400 Bad Rqeuest</H1>\n</BODY></HTML>", currentTime);
				  printf("400 Bad Request\n");
				  send(socket, str, strlen(str), 0);
				  break;

		case 403: snprintf(str, sizeof(str), "HTTP/1.1 403 Forbidden\r\nContent-Length: 112\r\nContent-Type: text/html\r\nConnection: keep-alive\r\nDate: %s\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>403 Forbidden</TITLE></HEAD>\n<BODY><H1>403 Forbidden</H1><br>Permission Denied\n</BODY></HTML>", currentTime);
				  printf("403 Forbidden\n");
				  send(socket, str, strlen(str), 0);
				  break;

		case 404: snprintf(str, sizeof(str), "HTTP/1.1 404 Not Found\r\nContent-Length: 91\r\nContent-Type: text/html\r\nConnection: keep-alive\r\nDate: %s\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>404 Not Found</TITLE></HEAD>\n<BODY><H1>404 Not Found</H1>\n</BODY></HTML>", currentTime);
				  printf("404 Not Found\n");
				  send(socket, str, strlen(str), 0);
				  break;

		case 500: snprintf(str, sizeof(str), "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 115\r\nConnection: keep-alive\r\nContent-Type: text/html\r\nDate: %s\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>500 Internal Server Error</TITLE></HEAD>\n<BODY><H1>500 Internal Server Error</H1>\n</BODY></HTML>", currentTime);
				  //printf("500 Internal Server Error\n");
				  send(socket, str, strlen(str), 0);
				  break;

		case 501: snprintf(str, sizeof(str), "HTTP/1.1 501 Not Implemented\r\nContent-Length: 103\r\nConnection: keep-alive\r\nContent-Type: text/html\r\nDate: %s\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>501 Not Implemented</TITLE></HEAD>\n<BODY><H1>501 Not Implemented</H1>\n</BODY></HTML>", currentTime);
				  printf("501 Not Implemented\n");
				  send(socket, str, strlen(str), 0);
				  break;

		case 505: snprintf(str, sizeof(str), "HTTP/1.1 505 HTTP Version Not Supported\r\nContent-Length: 125\r\nConnection: keep-alive\r\nContent-Type: text/html\r\nDate: %s\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>505 HTTP Version Not Supported</TITLE></HEAD>\n<BODY><H1>505 HTTP Version Not Supported</H1>\n</BODY></HTML>", currentTime);
				  printf("505 HTTP Version Not Supported\n");
				  send(socket, str, strlen(str), 0);
				  break;

		default:  return -1;

	}
	return 1;
}

int handle_request(int clientSocketId, ParsedRequest *request, char *tempReq)
{

    char *buf = (char *)malloc(sizeof(char) * MAX_BYTES);
    strcpy(buf, "GET");
    strcat(buf, request->path);
    strcat(buf, " ");
    strcat(buf, request->version);
    strcat(buf, "\r\n");
    size_t len = strlen(buf);

    if (ParsedHeader_set(request, "Connection", "close") < 0)
    {
        printf("set connection key to close is not working\n");
    }
    if (ParsedHeader_get(request, "HOST") == NULL)
    {
        if (ParsedHeader_set(request, "HOST", request->host) < 0)
        {
            printf("set host header key is not working \n");
        }
    }

    if (ParsedRequest_unparse_headers(request, buf + len, (size_t)MAX_BYTES - len) < 0)
    {
        // take and unparse headers from request and put in buf
        printf("unparse failed \n");
    }

    int server_port;

    if (request->port == NULL)
    {
        server_port = 80;
    }
    else
    {
        server_port = atoi(request->port);
        if (server_port <= 0 || server_port > 65535)
        {
            server_port = 80;
            printf("Invalid port, using default 80\n");
        }
    }
    printf("hello from 12\n");
    int remoteSocketId = connectRemoteServer(request->host, server_port);
    int bytes_send = send(remoteSocketId, buf, strlen(buf),0); // sending http req to server. from buf
    bzero(buf, MAX_BYTES);
    
    printf("hello from 14\n");
    bytes_send = recv(remoteSocketId, buf, MAX_BYTES-1, 0); // last one index stored for null char
    char* temp_buffer = (char*)malloc(sizeof(char)*MAX_BYTES);
    int temp_buffer_size = MAX_BYTES;
    int temp_buffer_index = 0;

    // receive from server, send to client, store in temp_buf for caching and repeat in loop
    while(bytes_send > 0){
        // bytes_send var stores size of data sent or received
        bytes_send = send(clientSocketId, buf, bytes_send, 0);
        for(int i = 0; i<bytes_send; i++){
            temp_buffer[temp_buffer_index] = buf[i];
            temp_buffer_index++;
        }
        temp_buffer_size += MAX_BYTES;
        temp_buffer = (char*)realloc(temp_buffer, temp_buffer_size);
        if(bytes_send<0){
            perror("error in sending data to the client \n");
            break;
        }
        bzero(buf, MAX_BYTES);
        bytes_send = recv(remoteSocketId, buf, MAX_BYTES-1, 0);
    }
    temp_buffer[temp_buffer_index] = '\0';
    free(buf);
    add_cache_element(temp_buffer, strlen(temp_buffer),tempReq); // add temp_buffer data to tempReq and add it to cache
    free(temp_buffer);
    close(remoteSocketId);
    return 0;
}



void *thread_fn(void *socketNew)
{
    sem_wait(&semaphore); // block execution till semaphore available
    int p;
    sem_getvalue(&semaphore, &p);
    int *t = (int *)socketNew;
    int socket = *t;
    int bytes_received_client, len;

    char *buffer = (char *)calloc(MAX_BYTES, sizeof(char));
    bytes_received_client = recv(socket, buffer, MAX_BYTES, 0);

    while (bytes_received_client > 0)
    {
        len = strlen(buffer);
        if (strstr(buffer, "\r\n\r\n") == NULL)
        {
            bytes_received_client = recv(socket, buffer + len, MAX_BYTES - len, 0);
        }
        else
        {
            break;
        }
    }

    char *tempReq = (char *)calloc(strlen(buffer) + 1, sizeof(char)); // strlen did not count the null terminator in total len. so i had to assign one extra space for null
    for (int i = 0; i < strlen(buffer); i++)
    {
        tempReq[i] = buffer[i];
    }

    struct cache_element *temp = find(tempReq);
    if (temp != NULL)
    {
        int size = temp->len / sizeof(char);
        int pos = 0;
        char response[MAX_BYTES];
        while (pos < size)
        {
            bzero(response, MAX_BYTES);
            for (int i = 0; i < MAX_BYTES; i++)
            {
                response[i] = temp->data[i]; // if it was not cached, i will get HTML response from server and cache it in temp->data and next time send it from cache
                pos++;
            }
            send(socket, response, MAX_BYTES, 0);
        }
        printf("data retrieved from the cache \n");
        printf("%s\n\n", response);
    }
    else if (bytes_received_client > 0)
    {
        len = strlen(buffer);
        ParsedRequest *request = ParsedRequest_create();

        if (ParsedRequest_parse(request, buffer, len) < 0)
        {
            printf("Parsing failed \n");
        }
        else
        {
            bzero(buffer, MAX_BYTES);
            if (strcmp(request->method, "GET") == 0)
            {
                if (request->host && request->path && checkHTTPversion(request->version) == 1)
                {
                    printf("reached here 2 \n");
                    int request_status = handle_request(socket, request, tempReq);
                    printf("reached here 3 \n");
                    if (request_status == -1)
                    {
                        sendErrorMessage(socket, 500);
                    }
                }
                else
                {
                    sendErrorMessage(socket, 500);
                }
            }
            else
            {
                printf("this code doesnt support any method apart from GET\n");
            }
        }
        ParsedRequest_destroy(request);
    }
    else if (bytes_received_client == 0)
    {
        printf("client is disconnected \n");
    }
    shutdown(socket, SHUT_RDWR);
    close(socket);
    free(buffer);
    sem_post(&semaphore);
    sem_getvalue(&semaphore, &p);
    printf("semaphore post value is %d \n", p);
    free(tempReq);
    return NULL;
}

int main(int argc, char *argv[])
{
    int client_socketId, client_len;             // id of client trying to open a socket, len of address of that client
    struct sockaddr_in server_addr, client_addr; // address of socket
    sem_init(&semaphore, 0, MAX_CLIENTS);
    pthread_mutex_init(&lock, NULL);
    if (argc >= 2)
    {
        port_number = atoi(argv[1]);
    }
    else
    {
        printf("too few arguments \n");
        exit(1);
    }

    printf("running proxy server on port number %d\n", port_number);
    proxy_socketId = socket(AF_INET, SOCK_STREAM, 0); // MAKING PROXY SOCKET WHICH IS ALWAYS RUNNING
    int reuse = 1;
    if (setsockopt(proxy_socketId, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse)) < 0)
    {
        perror("set Socket Options failed \n");
    }

    bzero((char *)&server_addr, sizeof(server_addr));

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port_number);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(proxy_socketId, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
    {
        perror("port is not available\n");
        exit(1);
    }
    printf("Binding on port: %d\n", port_number);

    int listen_status = listen(proxy_socketId, MAX_CLIENTS);
    if (listen_status < 0)
    {
        perror("Error in listening to socket \n");
        exit(1);
    }

    int i = 0;
    int connected_socketId[MAX_CLIENTS];

    while (1)
    {
        bzero((char *)&client_addr, sizeof(client_addr));
        client_len = sizeof(client_addr);
        client_socketId = accept(proxy_socketId, (struct sockaddr *)&client_addr, (socklen_t *)&client_len);
        if (client_socketId < 0)
        {
            printf("not able to connect with client socket \n");
            exit(1);
        }
        else
        {
            connected_socketId[i] = client_socketId;
        }

        struct sockaddr_in *client_pt = (struct sockaddr_in *)&client_addr;
        struct in_addr ip_addr = client_pt->sin_addr;
        char ip_addr_str[INET6_ADDRSTRLEN];
        inet_ntop(AF_INET, &ip_addr, ip_addr_str, INET_ADDRSTRLEN);
        printf("client is connected with ip address %s and port number %d\n", ip_addr_str, ntohs(client_addr.sin_port));

        pthread_create(&tid[i], NULL, thread_fn, (void *)&connected_socketId[i]);
        i++;
    }
    close(proxy_socketId);
    return 0;
}

cache_element* find(char* url){
    cache_element* site = NULL;
    int temp_lock_val = pthread_mutex_lock(&lock);
    printf("find cache lock acquired %d\n", temp_lock_val);
    if(head != NULL){
        site = head;
        while(site !=NULL){
            if(strcmp(site->url, url) == 0){
                printf("This site was last used at(as per LRU time track): %ld", site->lru_time_track);
                printf("url found in lru cache \n");
                site->lru_time_track = time(NULL);
                printf("LRU site access time updated after find: %ld", site->lru_time_track);
                break;
            }
            site=site->next;
        }
    }else{
        printf("url not found in LRU cache \n");
    }
    temp_lock_val = pthread_mutex_unlock(&lock);
    printf("mutex lock is unlocked \n");
    return site;
};

int add_cache_element(char* data, int size, char* url){
    int temp_lock_var = pthread_mutex_lock(&lock);
    printf("Mutex lock acquired for adding element to cache \n");
    int element_size = size+1+strlen(url) +1+sizeof(cache_element); // size of data + null terminator for data + size of url + null terminator for url + struct size
    if(element_size>MAX_ELEMENT_SIZE){
        temp_lock_var = pthread_mutex_unlock(&lock);
        printf("cache element too big, cant be added to cache so mutex lock is unlocked\n");
        return 0;
    }
    else{
        while(cache_size+element_size > MAX_SIZE){
            remove_cache_element();
        }
        cache_element* element = (cache_element*)malloc(sizeof(cache_element)); // allocate struct equivalent memory
        element->data = (char*)malloc(size+1); // one extra space for null terminator
        element->url = (char*)malloc(strlen(url)*sizeof(char) + 1);
        strcpy(element->data, data); // strcpy automatically adds null terminator
        strcpy(element->url, url);
        element->lru_time_track = time(NULL);
        element->next = head;
        element->len = size;
        head = element;
        cache_size+=element_size;
        temp_lock_var=pthread_mutex_unlock(&lock);
        printf("add cache element mutex lock is now unlocked\n");
        return 1;
    }
    return 0;
}

void remove_cache_element(){
    cache_element* p;
    cache_element* q;
    cache_element* temp;
    int temp_lock_val = pthread_mutex_lock(&lock);
    printf("mutex lock is acquired from removing from cache \n");
    if(head != NULL){
        p = head;
        q=head;
        temp = head;
        while(q->next !=NULL){
            if(q->next->lru_time_track < temp->lru_time_track){
                temp = q->next;
                p=q;
            }
            q=q->next;
        }
        if(temp==head){
            head=head->next;
        }else{
            p->next = temp->next;
        }

        cache_size = cache_size - temp->len - 1 - strlen(temp->url) - 1 - sizeof(cache_element);
        free(temp->data);
        free(temp->url);
        free(temp); 
    }

    temp_lock_val = pthread_mutex_unlock(&lock);
    printf("released mutex lock for cache removal \n");
    return;
}