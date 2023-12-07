//JOSHUA SCHUURMAN - joshschuuman

#include <stdio.h>
#include "csapp.h"
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/wait.h>
#include <pthread.h>

//maximum buffer size
#define BUFSIZE 256

/* Recommended max cache and object sizes */
#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400

/* You won't lose style points for including this long line in your code */
static const char *user_agent_hdr = "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:56.0) Gecko/20100101 Firefox/56.0\r\n";

//wrapper for thread args
struct thread_args {
	int cfd;
	struct cache *cache;
};

//cacheline
struct cacheline{
	int valid;
	char tag[BUFSIZE];
	char data[MAX_OBJECT_SIZE];
	int dataSize;
	int lastUsed;
};

//cache of 10 cachelines
struct cache{
	struct cacheline *lines[10];
};

//lock and timer
pthread_mutex_t mutex;
int timer;

/*	
 *	Checks if the passed string contains a special header
 *	Returns 2 if it is the first line of a request
 *	Returns 1 if it is a default header
 *	Returns 0 otherwise
 */
int isSpecialHeader(char *buf){
	if(strncmp("GET ", buf, 4) == 0)
		return 2;
	else if(!strncmp("User-Agent:", buf, 11)||!strncmp("Host:", buf, 4)||!strncmp("Connection:", buf, 11)||!strncmp("Proxy-Connection:", buf, 17))
		return 1;
	return 0;
}

/*
 * initializes a new cache and all cachelines
 */
struct cache *create_cache(){
	struct cache *cache= malloc(sizeof(struct cache));
	int i;
	for(i=0; i<10; i++){ 
		cache->lines[i] = malloc(sizeof(struct cacheline));
		cache->lines[i]->valid = 0;
		cache->lines[i]->lastUsed = timer++;
	}

	return cache;
}

/*
 * frees all cachelines within a passed cache, alongside the cache itself.
 */
void destroy_cache(struct cache *cache){
	int i;
	for(i=0; i<10; i++) free(cache->lines[i]);
	free(cache);
}

/*
 * tries to read tag from cache, returns the line if there's a hit, otherwise null
 */
struct cacheline *read_cache(struct cache *cache, char tag[BUFSIZE]){
	int i;
	int hit = 0;
	int minlen;
	struct cacheline *curline;

	//iterate through lines, checking for a match
	for(i=0; i<10; i++){
		curline = cache->lines[i];
		if(curline->valid){
			if(strlen(curline->tag) < strlen(tag))
			minlen = strlen(curline->tag);
			else minlen = strlen(tag);
		       	hit = !strncmp(tag, curline->tag, minlen);
		}

		//if there's a hit, lock and increment that line's timer
		if(hit){
			pthread_mutex_lock(&mutex);
			cache->lines[i]->lastUsed = timer++;
			pthread_mutex_unlock(&mutex);
			return cache->lines[i];
		}
	}
	return NULL;
}

/*
 * writes the given data to cache
 */
void write_cache(struct cache *cache, char tag[BUFSIZE], char data[MAX_OBJECT_SIZE], int datasize){
	pthread_mutex_lock(&mutex);		//lock to ensure no double-writes or read-writes
	int LRU = 0;
	int LRUtime = timer;
	int i;
	struct cacheline *curline;

	//iterate through lines, either until finding an empty line or continue finding the LRU
	for(i=0; i<10; i++){
		curline = cache->lines[i];
		if(!curline->valid){
			LRU = i;
			break;
		}
		if(curline->lastUsed < LRUtime){
			LRUtime = curline->lastUsed;
			LRU = i;
		}
	}
	
	//overwrite the LRU cacheline
	curline = cache->lines[LRU];
	curline->valid = 1;
	strncpy(curline->tag, tag, strlen(tag));
	memcpy(curline->data, data, datasize);
	curline->dataSize = datasize;
	curline->lastUsed = timer++;

	pthread_mutex_unlock(&mutex);		//unlock
}

/*
 * 	Takes a fd for a client and will parse and forward the request to a server.
 */
void *proxy(void *varg){
	//cast, get connfd, and free
	struct thread_args *arg = varg;
	int connfd = arg->cfd;
	struct cache *cache = arg->cache;
	free(varg);

	//File Descriptor for connection where the proxy is the client (ctos = client to server)
	int ctosfd;

	//Whether or not this request results in a cache hit or miss
	int hit;
	char tag[BUFSIZE];
	char data[MAX_OBJECT_SIZE];
	struct cacheline *line;

	//Buffers for host and port for parsing the first line of a request
	char host[BUFSIZE];
	char port[BUFSIZE];

	//variables used for reading both the request from the client, and the response from the server. n can be reused, but buf and rio are for the client, response and rio2 are for the server
	size_t n;
	char buf[BUFSIZE];
	char response[BUFSIZE];
	rio_t rio;
	rio_t rio2;

	//initialize rio for the client fd
	Rio_readinitb(&rio, connfd);

	//Parse request from client
	while((n = Rio_readlineb(&rio, buf, BUFSIZE)) != 0 && (buf[0] != '\r' && buf[1] != '\n')) {	
		//If this is the first line
		if(isSpecialHeader(buf) == 2){
			//calculate the beginning of the hostname by finding "//"
			int i = 0;
			while(buf[i] != '/' || buf[i+1] != '/') i++;
			i += 2;

			//j for indexing into piece buffers, buffers for request and path pieces
			int j = 0;	
			char request[BUFSIZE];		
			char path[BUFSIZE];			

			//copy the host until beginning of port or path
			while(buf[i] != '/' && buf[i] != ':'){	
				host[j] = buf[i];
				j++;
				i++;
			}
			host[j] = '\0';
			
			//if provided a port, copy it, else use default port 80
			if(buf[i] == ':'){			
				i++;
				j = 0;
				while(buf[i] != '/'){
					port[j] = buf[i];
					j++;
					i++;
				}
				port[j] = '\0';			
			}
			else{					
			       	strcpy(port, "80\0");
			}

			//copy the path one char short and manually write 0 to ensure HTTP/1.0
			j = 0;
			while(buf[i+1] != '\r'){
				path[j] = buf[i];
				j++;
				i++;
			}
			path[j] = '0';
			path[j+1] = '\0';

			//concatenates all of the pieces - ("GET ", hostname, port, path) into request and adds our default headers
			strcpy(request, "GET ");	
			strcat(request, path);
			strcat(request, "\r\n");
			strcat(request, "Host: ");
			strcat(request, host);
			strcat(request, ":");
			strcat(request, port);
			strcat(request, "\r\n");
			strcat(request, "Connection: close\r\nProxy-Connection: close\r\n");
			strcat(request, user_agent_hdr);

			//check for this request's tag in the cache
			strcpy(tag, host);
			strcat(tag, port);
			strcat(tag, path);
			line = read_cache(cache, tag);
			hit = (line!=NULL);
			
			//if cache hit, get the line. otherwise, open a connection to server and write the request thusfar
			if(hit) break;
			else{
				ctosfd = Open_clientfd(host, port);
				Rio_writen(ctosfd, request, strlen(request));
			}
		}
		//If an arbitrary header, write to server
		else if(!isSpecialHeader(buf)){
			Rio_writen(ctosfd, buf, strlen(buf));
		}
	}
	//if there is a miss, we have to read from the server and try to write to the cache. otherwise, just write the data from the line in the cache
	if(!hit){
		Rio_writen(ctosfd, "\r\n", 2);	//write empty line to server, indicating end of request

		//initialize rio2 and write to client in BUFSIZEd chunks
		int datasize = 0;
		char *datapos = data;
		Rio_readinitb(&rio2, ctosfd);

		//keep track of all we write in data, as long as the read isn't longer then MAX_OBJECT_SIZE
		while((n = Rio_readnb(&rio2, response, BUFSIZE)) != 0){
			Rio_writen(connfd, response, n);
			if(datasize + n <= MAX_OBJECT_SIZE) memcpy(datapos, response, n);
			datapos += n;
			datasize += n;
		}

		//as long as the write was less than MAX_OBJECT_SIZE, write it to cache
		if(datasize <= MAX_OBJECT_SIZE) write_cache(cache, tag, data, datasize);

		//close the client->server fd
		close(ctosfd);
	}
	else Rio_writen(connfd, line->data, line->dataSize);

	//close cfd and return
	close(connfd);
	return NULL;
}

int main(int argc, char **argv)
{ 
	//initialize mutex, timer, and cache
	struct cache *cache = create_cache();
	timer = 0;
	pthread_mutex_init(&mutex, NULL);

	//file descriptors for lfd and cfd
	int listenfd, connfd;

	//check if passed port is valid and open an lfd if so
	if(argc != 2){
		fprintf(stderr, "usage: %s <port>\n", argv[0]);
		exit(1);
	}
	listenfd = Open_listenfd(argv[1]);

	while(1){
		//accept connection	
		connfd = accept(listenfd, NULL, NULL);
		if(connfd<0){
			perror("accept");
			close(listenfd);
			exit(1);
		}

		//handle in thread
		pthread_t thread;
		struct thread_args *arg = malloc(sizeof(*arg));
		arg->cfd = connfd;
		arg->cache = cache;
		int rv = pthread_create(&thread, NULL, proxy, arg);
		if(rv<0) perror("pthread_create");
	}

	//close lfd, destroy mutex & cache, and exit normally
	Close(listenfd);
	pthread_mutex_destroy(&mutex);
	destroy_cache(cache);
	exit(0);
}
