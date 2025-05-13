#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>  // unix lib for close
#include <netdb.h>   //for gethostbyname
#include <signal.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/stat.h> //file ops (stat)
#include <sys/types.h>
#include <arpa/inet.h> //internet address stuff (sockaddr_in)
#include <netinet/in.h> //internet protocol
#include <time.h>
#include <openssl/md5.h> //hash function for hash key
#include <errno.h>
#include <fcntl.h> //file operationss
#include <ctype.h>

#define MAX_REQUEST_SIZE 8192  // max size for http req
#define MAX_URL_SIZE 2048  // max url
#define MAX_PATH_SIZE 2048
#define MAX_CACHE_PATH 256      // max size for cache filepath
#define BUFFER_SIZE 8192

//global vars
int server_fd;    //server socket global var
int timeout_seconds; //timeout duration var

//all functions:
void *handleClient(void *client_sock_ptr);
void parseReq(char *request, char *method, char *url, char *host, int *port, char *path);
int is_valid_request(char *method);
char *hashFunction(const char *url);
int checkCache(const char *cache_key, time_t current_time);
void cacheResponse(const char *cache_key, const char *response, int response_size);
void cleanup(int signal);

// signal handler for exit
void cleanup(int signal) {
     printf("\nclosing proxy server\n");
    close(server_fd);  //close socket
exit(0);
}


// generate hash key
char *hashFunction(const char *url) {
    unsigned char digest[MD5_DIGEST_LENGTH];  //create array to put cache  key
    char *cache_key = malloc(33);  //allocate mem for key (32+1)

    MD5((unsigned char*)url, strlen(url), digest);  // generate hash for url

    // convert to string
    for (int i = 0; i < MD5_DIGEST_LENGTH; i++) {
        sprintf(&cache_key[i*2], "%02x", (unsigned int)digest[i]);
    }

    return cache_key;
}

// check if valid response exists + not expires
int checkCache(const char *cache_key, time_t current_time) {
char cache_path[MAX_CACHE_PATH];  // path to cach fille
      struct stat file_stat; //to store file data

    sprintf(cache_path, "./cache/%s", cache_key);  // make full path to cachefile



    // if file exists, get data
        if (stat(cache_path, &file_stat) == 0) {
            //see if expired
            if (current_time - file_stat.st_mtime < timeout_seconds) {
                return 1;  // cache hit
            }
        }
    return 0;  // cache miss
}

// store response in cache
void cacheResponse(const char *cache_key, const char *response, int response_size) {
    char cache_path[MAX_CACHE_PATH];
    FILE *cache_file;
    mkdir("./cache", 0777);  //create dir ide
sprintf(cache_path, "./cache/%s", cache_key);

    //open file
    cache_file = fopen(cache_path, "wb");
    if (cache_file == NULL) {
        perror("failed to open file");
        return;
    }

//write response to cachefile
    fwrite(response, 1, response_size, cache_file);
    fclose(cache_file);
}

// check if http method is GET
int is_valid_request(char *method) {
    return strcmp(method, "GET") == 0;
}

// parse request, get info
void parse_request(char *request, char *method, char *url, char *host, int *port, char *path) {
    char *token, *url_start, *host_token;

    // extract GET method
    token = strtok(request, " ");
    if (token != NULL) {
        strcpy(method, token);  // store in method variable
    }

    // extract URL
    token = strtok(NULL, " ");
    if (token != NULL) {
        strcpy(url, token);  //store in url var
    }

    // default port
    *port = 80;

    // get host, port, path
    if (strncmp(url, "http://", 7) == 0) {
        url_start = url + 7;  // skip over "http://"
    } else {
        url_start = url;
    }

    // find end of host in URL (either / or :)
    char *host_end = strpbrk(url_start, ":/");
    if (host_end == NULL) {
        strcpy(host, url_start);
        strcpy(path, "/");  // default path
    } else {
        int host_len = host_end - url_start;
        strncpy(host, url_start, host_len);  // copy host part
        host[host_len] = '\0';

        if (*host_end == ':') {
            *port = atoi(host_end + 1);  // extract port (if specified)
            char *path_start = strchr(host_end, '/');  // get path start
            if (path_start == NULL) {
                strcpy(path, "/");  // default path
            } else {
                strcpy(path, path_start);  // copy path part
            }
        } else {
            strcpy(path, host_end);  // no port, just path
        }
    }

    // look for host header in reqs (if not in url)
    if (strlen(host) == 0) {
        host_token = strstr(request, "host: ");
        if (host_token != NULL) {
            sscanf(host_token, "host: %s", host);  //get host from header

            char *host_port = strchr(host, ':');
            if (host_port != NULL) {
                *host_port = '\0';  //split host + port
                *port = atoi(host_port + 1);  // get port #
            }
        }
    }
}


////??
int is_dynamic_content(const char *url) {
    return strchr(url, '?') != NULL;
}

// handle client requests
void *handleClient(void *client_sock_ptr) {
    int client_sock = *((int *)client_sock_ptr);
    free(client_sock_ptr);  // get socket + free mem for pointer

            char request[MAX_REQUEST_SIZE];
            char method[10];
            char url[MAX_URL_SIZE];
            char host[MAX_URL_SIZE];
            char path[MAX_PATH_SIZE];
            int port;

    int bytes_read = recv(client_sock, request, MAX_REQUEST_SIZE - 1, 0);
    if (bytes_read <= 0) {  //if no data read
        close(client_sock);
        return NULL;
    }

    request[bytes_read] = '\0';

    // parse request
    parse_request(request, method, url, host, &port, path);

    if (!is_valid_request(method)) {
        char *error_response = "HTTP/1.1 400 Bad Request\r\nContent-Type: text/html\r\n\r\n<html><body><h1>400 Bad Request</h1><p>only GET method supported.</p></body></html>";
        send(client_sock, error_response, strlen(error_response), 0);
        close(client_sock);
        return NULL;
    }

    //get server addy
    struct hostent *server_host = gethostbyname(host);


    //check url not dynamic (chachable)
    int should_cache = !is_dynamic_content(url);
    char *cache_key = NULL;

    if (should_cache) {
        cache_key = hashFunction(url);  //make hash key
        time_t current_time = time(NULL);


        if (checkCache(cache_key, current_time)) {
            printf("Cache hit for %s\n", url);  // Cache hit

            // read chached files
            //send to client
            char cache_path[MAX_CACHE_PATH];
            sprintf(cache_path, "./cache/%s", cache_key);

            FILE *cache_file = fopen(cache_path, "rb");  //open cache file to read
            if (cache_file != NULL) {
                char buffer[BUFFER_SIZE];
                size_t bytes_read;

                // send data to client sock
                while ((bytes_read = fread(buffer, 1, BUFFER_SIZE, cache_file)) > 0) {
                    send(client_sock, buffer, bytes_read, 0);
                }

                fclose(cache_file);
                free(cache_key);
                close(client_sock);
                return NULL;
            }
        }
    }

    // connect to server
    int server_sock = socket(AF_INET, SOCK_STREAM, 0);  // make server socket
    if (server_sock < 0) {
        perror("server socket creation failed");
        close(client_sock);
        if (cache_key != NULL) free(cache_key);
        return NULL;
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));  //zero out struct
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);//set pot
    memcpy(&server_addr.sin_addr.s_addr, server_host->h_addr, server_host->h_length); //copy ip

    // connect origin server
    if (connect(server_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("failed to connect server");
        close(server_sock);
        close(client_sock);
        if (cache_key != NULL) free(cache_key);
        return NULL;
    }

    // modify to standard http + send to server
    char new_request[MAX_REQUEST_SIZE];
    sprintf(new_request, "GET %s HTTP/1.0\r\nHost: %s\r\n\r\n", path, host);
    send(server_sock, new_request, strlen(new_request), 0);

    // get response
    //forward to client
    char buffer[BUFFER_SIZE];
    char *full_response = NULL;//potentially cache
    int total_bytes = 0;
    int response_capacity = 0;

    while (1) {
        int bytes_received = recv(server_sock, buffer, BUFFER_SIZE, 0); //get data from origin

        if (bytes_received <= 0) {
            break;
        }

        send(client_sock, buffer, bytes_received, 0);  //send data to client

        // store response data
        if (should_cache) {
            if (total_bytes + bytes_received > response_capacity) {
                response_capacity = (total_bytes + bytes_received) * 2;
                full_response = realloc(full_response, response_capacity);//expand buff for full response
                if (full_response == NULL) {
                    should_cache = 0;//if mem allocation fails
                }
            }

            if (full_response != NULL) {
                memcpy(full_response + total_bytes, buffer, bytes_received);  //add data to full response
                total_bytes += bytes_received;
            }
        }
    }

//cache if needed
    if (should_cache && full_response != NULL) {
        cacheResponse(cache_key, full_response, total_bytes);
        free(full_response);
    }

///cleanup
    if (cache_key != NULL) free(cache_key);//
    close(server_sock);
    close(client_sock);
    return NULL;
}

int main(int argc, char *argv[]) {
    // get port + timeout args
    if (argc != 3) {
        printf("usage: %s <port> <timeout>\n", argv[0]);
        return 1;
    }
    int port = atoi(argv[1]);
    timeout_seconds = atoi(argv[2]);

    // signal handler setup
    signal(SIGINT, cleanup);

    // create server socket
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("failed to create socket");
        return 1;
    }

    // allow for sock addr reuse
    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt");
        return 1;
    }

    //bind
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);
        if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
            perror("Failed to bind socket");
            return 1;
        }

    //listen
    if (listen(server_fd, 10) < 0) {
        perror("Failed to listen");
        return 1;
    }

    printf("server running on port %d w/ cache timeout of %d seconds\n", port, timeout_seconds);
    printf("Ctrl+C to stop the server\n");

    // create cache dir (if there isn''t one already)
    mkdir("./cache", 0777);

    // handle client connections
    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        int *client_sock = malloc(sizeof(int));  // Allocate memory for the client socket

        *client_sock = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);  // Accept client connection

        if (*client_sock < 0) {
            perror("Failed to accept connection");
            free(client_sock);
            continue;
        }

        // thread to handle requests
        pthread_t thread_id;
        if (pthread_create(&thread_id, NULL, handleClient, client_sock) != 0) {
            perror("failed to thread");
            close(*client_sock);
            free(client_sock);
        }
        pthread_detach(thread_id);
    }

    return 0;
}
