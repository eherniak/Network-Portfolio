#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <openssl/md5.h>
#include <time.h>
#include <sys/time.h>

#define MAX_SERVERS 10
#define MAX_FILENAME 256
#define BUFFER_SIZE 4096
#define CONFIG_FILE "dfc.conf"  //reads to config servers; adds each server to list
#define TIMEOUT_SEC 1

//struct to hold server information
typedef struct {
    char hostname[64];
    char ip[16];
    int port;
    int connected;
    int socket;
} Server;

typedef struct {
    int count;
    Server servers[MAX_SERVERS];
} ServerList;

// functions
void read_config(ServerList *server_list);
void connect_to_servers(ServerList *server_list);
void put_file(ServerList *server_list, char *filename);
void get_file(ServerList *server_list, char *filename);
void list_files(ServerList *server_list);
unsigned int get_file_hash(char *filename);
int check_file_completeness(ServerList *server_list, char *filename);

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("usage: %s <command> [filename] ... [filename]\n", argv[0]);
        return 1;
    }

    ServerList server_list;
    server_list.count = 0;

    //read config file
    read_config(&server_list);
    
    // connect to available servers
    connect_to_servers(&server_list);
    
    // execute command
    if (strcmp(argv[1], "list") == 0) {
        list_files(&server_list);
    } else if (strcmp(argv[1], "get") == 0) {
        if (argc < 3) {
            printf("Error: missing filename for 'get' command\n");
            return 1;
        }
        for (int i = 2; i < argc; i++) {
            get_file(&server_list, argv[i]);
        }
    } else if (strcmp(argv[1], "put") == 0) {
        if (argc < 3) {
            printf("Error: missing filename for 'put' command\n");
            return 1;
        }
        for (int i = 2; i < argc; i++) {
            put_file(&server_list, argv[i]);
        }
    } else {
        printf("Error: unknown command '%s'\n", argv[1]);
        return 1;
    }
    
    // close all connections
    for (int i = 0; i < server_list.count; i++) {
        if (server_list.servers[i].connected) {
            close(server_list.servers[i].socket);
        }
    }
    
    return 0;
}

// read from the dfc.conf file
void read_config(ServerList *server_list) {
    char config_path[256];
    snprintf(config_path, sizeof(config_path), "%s/%s", getenv("HOME"), CONFIG_FILE);
    
    FILE *config = fopen(config_path, "r");
    if (!config) {
        perror("error opening config file");
        exit(1);
    }
    
    //add each server to list
    char line[256];
    while (fgets(line, sizeof(line), config)) {
        if (strncmp(line, "server", 6) == 0) {
            char server_name[64];
            char server_address[256];
            
            if (sscanf(line, "server %s %s", server_name, server_address) == 2) {
                Server *server = &server_list->servers[server_list->count];
                strncpy(server->hostname, server_name, sizeof(server->hostname));
                
                // Parse IP and port
                char *colon = strchr(server_address, ':');
                if (colon) {
                    *colon = '\0';
                    strncpy(server->ip, server_address, sizeof(server->ip));
                    server->port = atoi(colon + 1);
                    server->connected = 0;
                    server_list->count++;
                }
            }
        }
    }
    
    fclose(config);
    
    printf("loaded %d servers from config\n", server_list->count);
}

// connect to available servers (w/ timeout)
void connect_to_servers(ServerList *server_list) {
    struct timeval tv;
    tv.tv_sec = TIMEOUT_SEC;
    tv.tv_usec = 0;
    
    for (int i = 0; i < server_list->count; i++) {
        Server *server = &server_list->servers[i];
        
        // create socket
        server->socket = socket(AF_INET, SOCK_STREAM, 0);
        if (server->socket < 0) {
            perror("Error creating socket");
            continue;
        }
        
        //set 1sec timeout for connection
        setsockopt(server->socket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);
        setsockopt(server->socket, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof tv);
        
        // connect to server
        struct sockaddr_in serv_addr;
        memset(&serv_addr, 0, sizeof(serv_addr));
        serv_addr.sin_family = AF_INET;
        serv_addr.sin_port = htons(server->port);
        
        if (inet_pton(AF_INET, server->ip, &serv_addr.sin_addr) <= 0) {
            printf("invalid address: %s\n", server->ip);
            close(server->socket);
            continue;
        }
        
        if (connect(server->socket, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
            printf("server %s (%s:%d) not available\n", server->hostname, server->ip, server->port);
            close(server->socket);
            continue;
        }
        
        server->connected = 1;
        printf("connected to server %s (%s:%d)\n", server->hostname, server->ip, server->port);
    }
}

// hash the filename
//for distributing chunks
unsigned int get_file_hash(char *filename) {
    unsigned char digest[MD5_DIGEST_LENGTH];
    MD5_CTX md5_context;
    
    MD5_Init(&md5_context);
    MD5_Update(&md5_context, filename, strlen(filename));
    MD5_Final(digest, &md5_context);
    
    // use first 4 bytes
    unsigned int hash_value = 0;
    memcpy(&hash_value, digest, sizeof(unsigned int));
    
    return hash_value;
}

// implementing PUT command
void put_file(ServerList *server_list, char *filename) {
    // check if file exists & readable
    FILE *file = fopen(filename, "rb");
    if (!file) {
        perror("Error opening file");
        return;
    }
    
    // get file size
    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    rewind(file);
    
    // calculate chunk size (ciel)
    long chunk_size = (file_size + 3) / 4;
    
    // allocate memory for file chunks
    char *chunks[4];
    for (int i = 0; i < 4; i++) {
        chunks[i] = malloc(chunk_size);
        if (!chunks[i]) {
            perror("memory allocation failed");
            fclose(file);
            for (int j = 0; j < i; j++) {
                free(chunks[j]);
            }
            return;
        }
        memset(chunks[i], 0, chunk_size);
    }
    
    // read file into chunks
    for (int i = 0; i < 4; i++) {
        size_t read_size = fread(chunks[i], 1, chunk_size, file);
        if (read_size < chunk_size && i < 3 && !feof(file)) {
            perror("error reading file");
            fclose(file);
            for (int j = 0; j < 4; j++) {
                free(chunks[j]);
            }
            return;
        }
    }
    
    fclose(file);
    
    // calculate which server gets which chunk(hash)
    unsigned int hash = get_file_hash(filename);
    int x = hash % server_list->count;
    printf("file %s maps to %d\n", filename, x);
    
    // check if enough servers are connected
    int available_servers = 0;
    for (int i = 0; i < server_list->count; i++) {
        if (server_list->servers[i].connected) {
            available_servers++;
        }
    }
    
    if (available_servers < 3) { //need at least 3
        printf("%s put failed\n", filename);
        for (int i = 0; i < 4; i++) {
            free(chunks[i]);
        }
        return;
    }
    
    // define chunk pairs
    int chunk_pairs[4][2] = {
        {0, 1}, {1, 2}, {2, 3}, {3, 0} 
    };
    
    // adjust based on hash
    for (int i = 0; i < x; i++) {
        //rotate
        int temp1 = chunk_pairs[0][0];
        int temp2 = chunk_pairs[0][1];
        
        for (int j = 0; j < 3; j++) {
            chunk_pairs[j][0] = chunk_pairs[j+1][0];
            chunk_pairs[j][1] = chunk_pairs[j+1][1];
        }
        
        chunk_pairs[3][0] = temp1;
        chunk_pairs[3][1] = temp2;
    }
    
    // send chunks to the servers
    for (int i = 0; i < server_list->count; i++) {
        if (!server_list->servers[i].connected) {
            continue;
        }
        
        // send PUT command
        char command[BUFFER_SIZE];
        sprintf(command, "PUT %s %ld", filename, chunk_size);
        send(server_list->servers[i].socket, command, strlen(command), 0);
        
        //send first chunk
        char chunk_header[64];
        sprintf(chunk_header, "CHUNK %d", chunk_pairs[i][0] + 1);
        send(server_list->servers[i].socket, chunk_header, strlen(chunk_header), 0);
        
        send(server_list->servers[i].socket, chunks[chunk_pairs[i][0]], chunk_size, 0);
        
        //send second
        sprintf(chunk_header, "CHUNK %d", chunk_pairs[i][1] + 1);
        send(server_list->servers[i].socket, chunk_header, strlen(chunk_header), 0);
        
        send(server_list->servers[i].socket, chunks[chunk_pairs[i][1]], chunk_size, 0);
        
        // get acknowledgment
        char buffer[BUFFER_SIZE];
        int received = recv(server_list->servers[i].socket, buffer, BUFFER_SIZE, 0);
        if (received > 0) {
            buffer[received] = '\0';
            if (strncmp(buffer, "OK", 2) == 0) {
                printf("server %s: chunks %d and %d uploaded successfully\n", 
                       server_list->servers[i].hostname, 
                       chunk_pairs[i][0] + 1, 
                       chunk_pairs[i][1] + 1);
            } else {
                printf("server %s: upload failed - %s\n", server_list->servers[i].hostname, buffer);
            }
        }
    }
    
    // free memory
    for (int i = 0; i < 4; i++) {
        free(chunks[i]);
    }
    
    printf("file %s uploaded successfully\n", filename);
}

// check if there are enough chunks to reconstruct the file
int check_file_completeness(ServerList *server_list, char *filename) {
    int chunk_present[4] = {0, 0, 0, 0};
    
    // find where each chunk is
    //which servers have which chunks
    unsigned int hash = get_file_hash(filename);
    int x = hash % server_list->count;
    
    // define chunk pairs for each server
    int chunk_pairs[4][2] = {
        {0, 1}, {1, 2}, {2, 3}, {3, 0} 
    };
    
 // adjust
            for (int i = 0; i < x; i++) {
                //rotate
                int temp1 = chunk_pairs[0][0];
                int temp2 = chunk_pairs[0][1];
                
                for (int j = 0; j < 3; j++) {
                    chunk_pairs[j][0] = chunk_pairs[j+1][0];
                    chunk_pairs[j][1] = chunk_pairs[j+1][1];
                }
                
                chunk_pairs[3][0] = temp1;
                chunk_pairs[3][1] = temp2;
            }
    
    // check available chunks
    for (int i = 0; i < server_list->count; i++) {
        if (server_list->servers[i].connected) {
            // send CHECK cmd
            char command[BUFFER_SIZE];
            sprintf(command, "CHECK %s", filename);
            send(server_list->servers[i].socket, command, strlen(command), 0);
            
            // receive response
            char buffer[BUFFER_SIZE];
            int received = recv(server_list->servers[i].socket, buffer, BUFFER_SIZE, 0);
            if (received > 0) {
                buffer[received] = '\0';
                if (strncmp(buffer, "EXISTS", 6) == 0) {
                    chunk_present[chunk_pairs[i][0]] = 1;
                    chunk_present[chunk_pairs[i][1]] = 1;
                }
            }
        }
    }
    
    // see if all chunks present
    for (int i = 0; i < 4; i++) {
        if (!chunk_present[i]) {
            return 0;
        }
    }
    
    return 1;
}

// implememt GET command
void get_file(ServerList *server_list, char *filename) {
    // check if enough chunks to reconstruct the file
    if (!check_file_completeness(server_list, filename)) {
        printf("%s is incomplete\n", filename);
        return;
    }
    
    unsigned int hash = get_file_hash(filename);
    int x = hash % server_list->count;
    
    int chunk_pairs[4][2] = {
        {0, 1}, {1, 2}, {2, 3}, {3, 0} 
    };
    
   
    for (int i = 0; i < x; i++) {
        int temp1 = chunk_pairs[0][0];
        int temp2 = chunk_pairs[0][1];
        
        for (int j = 0; j < 3; j++) {
            chunk_pairs[j][0] = chunk_pairs[j+1][0];
            chunk_pairs[j][1] = chunk_pairs[j+1][1];
        }
        
        chunk_pairs[3][0] = temp1;
        chunk_pairs[3][1] = temp2;
    }
    
    // get chunk size
    //get size from first server with file
    long chunk_size = 0;
    for (int i = 0; i < server_list->count; i++) {
        if (server_list->servers[i].connected) {
            // send SIZE command
            char command[BUFFER_SIZE];
            sprintf(command, "SIZE %s", filename);
            send(server_list->servers[i].socket, command, strlen(command), 0);
            
            // get response
            char buffer[BUFFER_SIZE];
            int received = recv(server_list->servers[i].socket, buffer, BUFFER_SIZE, 0);
            if (received > 0) {
                buffer[received] = '\0';
                if (strncmp(buffer, "SIZE ", 5) == 0) {
                    chunk_size = atol(buffer + 5);
                    break;
                }
            }
        }
    }
    
    if (chunk_size == 0) {
        printf("error: could not determine chunk size\n");
        return;
    }
    
    // allocate memory for chunks
    char *chunks[4] = {NULL, NULL, NULL, NULL};
    for (int i = 0; i < 4; i++) {
        chunks[i] = malloc(chunk_size);
        if (!chunks[i]) {
            perror("memory allocation failed");
            for (int j = 0; j < i; j++) {
                free(chunks[j]);
            }
            return;
        }
    }
    
    // retrieve chunks from servers
    for (int i = 0; i < server_list->count; i++) {
        if (!server_list->servers[i].connected) {
            continue;
        }
        
        // GET command for first chunk
        char command[BUFFER_SIZE];
        sprintf(command, "GET %s %d", filename, chunk_pairs[i][0] + 1);
        send(server_list->servers[i].socket, command, strlen(command), 0);
        
        // get chunk
        char *chunk_ptr = chunks[chunk_pairs[i][0]];
        size_t remaining = chunk_size;
        
        while (remaining > 0) {
            int received = recv(server_list->servers[i].socket, chunk_ptr, remaining, 0);
            if (received <= 0) {
                break;
            }
            chunk_ptr += received;
            remaining -= received;
        }
        
        //GET command for second chunk
        sprintf(command, "GET %s %d", filename, chunk_pairs[i][1] + 1);
        send(server_list->servers[i].socket, command, strlen(command), 0);
        
        // get chunk
        chunk_ptr = chunks[chunk_pairs[i][1]];
        remaining = chunk_size;
        
        while (remaining > 0) {
            int received = recv(server_list->servers[i].socket, chunk_ptr, remaining, 0);
            if (received <= 0) {
                break;
            }
            chunk_ptr += received;
            remaining -= received;
        }
    }
    
    // write chunks to output file
    FILE *output = fopen(filename, "wb");
    if (!output) {
        perror("error creating output file");
        for (int i = 0; i < 4; i++) {
            free(chunks[i]);
        }
        return;
    }
    
    for (int i = 0; i < 4; i++) {
        fwrite(chunks[i], 1, chunk_size, output);
    }
    
    fclose(output);
    
    // free memore
    for (int i = 0; i < 4; i++) {
        free(chunks[i]);
    }
    
    printf("file %s downloaded successfully\n", filename);
}

// implememting LIST command
void list_files(ServerList *server_list) {
    //avoid duplicates, keep track of files already seen
    char files[100][MAX_FILENAME];
    int file_count = 0;
    
    // get file list from each server
    for (int i = 0; i < server_list->count; i++) {
        if (!server_list->servers[i].connected) {
            continue;
        }
        
        // send LIST cmd
        char command[] = "LIST";
        send(server_list->servers[i].socket, command, strlen(command), 0);
        
        // get response
        char buffer[BUFFER_SIZE];
        int received = recv(server_list->servers[i].socket, buffer, BUFFER_SIZE, 0);
        if (received > 0) {
            buffer[received] = '\0';
            
            char *line = strtok(buffer, "\n");
            while (line) {
                // check if file already in list
                int found = 0;
                for (int j = 0; j < file_count; j++) {
                    if (strcmp(files[j], line) == 0) {
                        found = 1;
                        break;
                    }
                }
                
                if (!found && file_count < 100) {
                    strncpy(files[file_count], line, MAX_FILENAME - 1);
                    files[file_count][MAX_FILENAME - 1] = '\0';
                    file_count++;
                }
                
                line = strtok(NULL, "\n");
            }
        }
    }
    
    // display files
    for (int i = 0; i < file_count; i++) {
        if (check_file_completeness(server_list, files[i])) {
            printf("%s\n", files[i]);
        } else {
            printf("%s [incomplete]\n", files[i]);
        }
    }
    
    if (file_count == 0) {
        printf("no files found\n");
    }
}