#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>     //POSIX API
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <dirent.h>
#include <sys/stat.h>
#include <fcntl.h>   //file control
#include <signal.h>

#define BUFFER_SIZE 4096       //size of data buffer during communications
#define MAX_CLIENTS 100       
#define MAX_FILENAME 256       //max filename length

// struct to pass data to threads
typedef struct {
    int client_socket;      //sock descriptor for client
    char server_dir[256];     //directory to store files
} client_args;

// functions
void *handle_client(void *arg);
void handle_put(int client_socket, char *server_dir, char *filename, long chunk_size);
void handle_get(int client_socket, char *server_dir, char *filename, int chunk_num);
void handle_list(int client_socket, char *server_dir);
void handle_check(int client_socket, char *server_dir, char *filename);
void handle_size(int client_socket, char *server_dir, char *filename);

// for shutdown
//set flags to break server loop
volatile sig_atomic_t keep_running = 1;
void handle_signal(int sig) {
    keep_running = 0;
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        printf("usage: %s <directory> <port>\n", argv[0]);
        return 1;
    }
    
    char *server_dir = argv[1];
    int port = atoi(argv[2]);
    
    // create server directory if non existant
    struct stat st = {0};
    if (stat(server_dir, &st) == -1) {
        mkdir(server_dir, 0700);
    }
    
    // signal handling setup
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    
    // create socket
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("error could not create socket");
        return 1;
    }
    
    // set options
    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
        perror("error could not set up socket options");
        return 1;
    }
    
    // bind
    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);
    
    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("error could not bind");
        return 1;
    }
    
    // listen for connections
    if (listen(server_fd, 3) < 0) {
        perror("eerror listening");
        return 1;
    }
    
    printf("dfs server started on port %d in directory %s\n", port, server_dir);
    
    // accept + handle connections
    while (keep_running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        
        int client_socket = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_socket < 0) {
            if (keep_running) {  // report error if not shutiing down
                perror("error accepting connection");
            }
            continue;
        }
        
        //create thread that will handle client
        pthread_t thread_id;
        client_args *args = malloc(sizeof(client_args));
        args->client_socket = client_socket;
        strncpy(args->server_dir, server_dir, sizeof(args->server_dir) - 1);
        args->server_dir[sizeof(args->server_dir) - 1] = '\0';
        
        if (pthread_create(&thread_id, NULL, handle_client, (void *)args) != 0) {
            perror("Error creating thread");
            close(client_socket);
            free(args);
            continue;
        }
        
        // detach
        pthread_detach(thread_id);
    }
    
    // close socket
    close(server_fd);
    printf("DFS server shut down\n");
    
    return 0;
}

// handle client requests
void *handle_client(void *arg) {
    client_args *args = (client_args *)arg;
    int client_socket = args->client_socket;
    char server_dir[256];
    strncpy(server_dir, args->server_dir, sizeof(server_dir));
    free(args);
    
    char buffer[BUFFER_SIZE];
    
    while (1) {
        memset(buffer, 0, BUFFER_SIZE);
        int received = recv(client_socket, buffer, BUFFER_SIZE - 1, 0);
        
        if (received <= 0) {
            break;  //client disconnected
        }
        
        buffer[received] = '\0';
        
        // parse command
        if (strncmp(buffer, "PUT ", 4) == 0) {
            char filename[MAX_FILENAME];
            long chunk_size;
            
            if (sscanf(buffer + 4, "%s %ld", filename, &chunk_size) == 2) {
                handle_put(client_socket, server_dir, filename, chunk_size);
            }
        } else if (strncmp(buffer, "GET ", 4) == 0) {
            char filename[MAX_FILENAME];
            int chunk_num;
            
            if (sscanf(buffer + 4, "%s %d", filename, &chunk_num) == 2) {
                handle_get(client_socket, server_dir, filename, chunk_num);
            }
        } else if (strncmp(buffer, "LIST", 4) == 0) {
            handle_list(client_socket, server_dir);
        } else if (strncmp(buffer, "CHECK ", 6) == 0) {
            char filename[MAX_FILENAME];
            
            if (sscanf(buffer + 6, "%s", filename) == 1) {
                handle_check(client_socket, server_dir, filename);
            }
        } else if (strncmp(buffer, "SIZE ", 5) == 0) {
            char filename[MAX_FILENAME];
            
            if (sscanf(buffer + 5, "%s", filename) == 1) {
                handle_size(client_socket, server_dir, filename);
            }
        }
    }
    
    close(client_socket);
    return NULL;
}

// PUT command- receive and store file chunks
void handle_put(int client_socket, char *server_dir, char *filename, long chunk_size) {
    // get first chunk
    char chunk_header[64];
    int received = recv(client_socket, chunk_header, sizeof(chunk_header) - 1, 0);
    if (received <= 0) {
        return;
    }
    chunk_header[received] = '\0';
    
    int chunk_num = 0;
    if (sscanf(chunk_header, "CHUNK %d", &chunk_num) != 1) {
        return;
    }
    
    // create filepath
    char file_path[512];
    snprintf(file_path, sizeof(file_path), "%s/%s.%d", server_dir, filename, chunk_num);
    
    // open for writing
    FILE *file = fopen(file_path, "wb");
    if (!file) {
        perror("Error creating file");
        return;
    }
    
    // recieve+wtite data from chunk
    char *chunk_data = malloc(chunk_size);
    if (!chunk_data) {
        perror("Memory allocation failed");
        fclose(file);
        return;
    }
    
    size_t remaining = chunk_size;
    char *ptr = chunk_data;
    
    while (remaining > 0) {
        received = recv(client_socket, ptr, remaining, 0);
        if (received <= 0) {
            break;
        }
        ptr += received;
        remaining -= received;
    }
    
    fwrite(chunk_data, 1, chunk_size, file);
    fclose(file);
    free(chunk_data);
    
    // second chunk
    received = recv(client_socket, chunk_header, sizeof(chunk_header) - 1, 0);
    if (received <= 0) {
        return;
    }
    chunk_header[received] = '\0';
    
    chunk_num = 0;
    if (sscanf(chunk_header, "CHUNK %d", &chunk_num) != 1) {
        return;
    }

    snprintf(file_path, sizeof(file_path), "%s/%s.%d", server_dir, filename, chunk_num);
    
    file = fopen(file_path, "wb");
    if (!file) {
        perror("Error creating file");
        return;
    }

    chunk_data = malloc(chunk_size);
    if (!chunk_data) {
        perror("Memory allocation failed");
        fclose(file);
        return;
    }
    
    remaining = chunk_size;
    ptr = chunk_data;
    
    while (remaining > 0) {
        received = recv(client_socket, ptr, remaining, 0);
        if (received <= 0) {
            break;
        }
        ptr += received;
        remaining -= received;
    }
    
    fwrite(chunk_data, 1, chunk_size, file);
    fclose(file);
    free(chunk_data);
    
    // send acknowledgment
    char response[] = "ok";
    send(client_socket, response, strlen(response), 0);
}

//GET command- send file to client
void handle_get(int client_socket, char *server_dir, char *filename, int chunk_num) {
    //make filepath
    char file_path[512];
    snprintf(file_path, sizeof(file_path), "%s/%s.%d", server_dir, filename, chunk_num);
    
    // open for reading
    FILE *file = fopen(file_path, "rb");
    if (!file) {
        char response[] = "error file not found";
        send(client_socket, response, strlen(response), 0);
        return;
    }
    
    // get size of file
    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    rewind(file);
    
    //allocate memory for file
    char *file_data = malloc(file_size);
    if (!file_data) {
        perror("memory allocation failed");
        fclose(file);
        return;
    }
    
    // read file to memory
    if (fread(file_data, 1, file_size, file) != file_size) {
        perror("error reading file");
        fclose(file);
        free(file_data);
        return;
    }
    
    fclose(file);
    
    //send data
    send(client_socket, file_data, file_size, 0);
    free(file_data);
}

// LIST command - send list of files to client
void handle_list(int client_socket, char *server_dir) {
    DIR *dir;
    struct dirent *ent;
    char response[BUFFER_SIZE] = "";
    
    dir = opendir(server_dir);
    if (dir == NULL) {
        perror("error opening directory");
        return;
    }
    
    // keep track of files already seen, avoid dupes
    char files[100][MAX_FILENAME];
    int file_count = 0;
    
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_type == DT_REG) {  // regular file
            char *filename = ent->d_name;
            char *dot = strrchr(filename, '.');
            
            if (dot && dot != filename) {
                *dot = '\0';  // remove extension
                
                // check if file has been see before
                int found = 0;
                for (int i = 0; i < file_count; i++) {
                    if (strcmp(files[i], filename) == 0) {
                        found = 1;
                        break;
                    }
                }
                
                if (!found && file_count < 100) {
                    strncpy(files[file_count], filename, MAX_FILENAME - 1);
                    files[file_count][MAX_FILENAME - 1] = '\0';
                    file_count++;
                    
                    if (strlen(response) + strlen(filename) + 2 < BUFFER_SIZE) {
                        strcat(response, filename);
                        strcat(response, "\n");
                    }
                }
            }
        }
    }
    
    closedir(dir);
    
    // send response
    send(client_socket, response, strlen(response), 0);
}

//CHECK command - check if file exists
void handle_check(int client_socket, char *server_dir, char *filename) {
    DIR *dir;
    struct dirent *ent;
    int found = 0;
    
dir = opendir(server_dir);
    if (dir == NULL) {
        perror("error opening directory");
        return;
    }
    
    // search for file chunks
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_type == DT_REG) {
            char base_filename[MAX_FILENAME];
            int chunk_num;
            
            if (sscanf(ent->d_name, "%[^.].%d", base_filename, &chunk_num) == 2) {
                if (strcmp(base_filename, filename) == 0) {
                    found = 1;
                    break;
                }
            }
        }
    }
    
    closedir(dir);
    
    // send response
    if (found) {
        char response[] = "EXISTS";
        send(client_socket, response, strlen(response), 0);
    } else {
        char response[] = "NOT_FOUND";
        send(client_socket, response, strlen(response), 0);
    }
}

//SIZE command - send file chunk size
void handle_size(int client_socket, char *server_dir, char *filename) {
    DIR *dir;
    struct dirent *ent;
    long file_size = 0;
    
        dir = opendir(server_dir);
        if (dir == NULL) {
            perror("error opening directory");
            return;
        }
    
    // get andy chunk of file to get size
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_type == DT_REG) {
            char base_filename[MAX_FILENAME];
            int chunk_num;
            
            if (sscanf(ent->d_name, "%[^.].%d", base_filename, &chunk_num) == 2) {
                if (strcmp(base_filename, filename) == 0) {
                    char file_path[512];
                    snprintf(file_path, sizeof(file_path), "%s/%s", server_dir, ent->d_name);
                    
                    struct stat st;
                    if (stat(file_path, &st) == 0) {
                        file_size = st.st_size;
                        break;
                    }
                }
            }
        }
    }
    
    closedir(dir);
    
    // send response
    char response[64];
    sprintf(response, "SIZE %ld", file_size);
    send(client_socket, response, strlen(response), 0);
}