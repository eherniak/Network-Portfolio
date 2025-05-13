#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <netdb.h>
#include <sys/types.h> 
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>

#define BUFSIZE 1024

void error(char *msg) {
    perror(msg);
    exit(1);
}

void handle_send_file(int sockfd, struct sockaddr_in clientaddr, char *filename) {
    char buffer[BUFSIZE];
    int n, filefd;
    FILE *file = fopen(filename, "wb");
    if (file == NULL) {
        error("ERROR");
    }

    // rceive file content from client
    while ((n = recvfrom(sockfd, buffer, BUFSIZE, 0, (struct sockaddr *)&clientaddr, NULL)) > 0) {
        fwrite(buffer, 1, n, file);
    }

    fclose(file);
    printf("File %s received successfully\n", filename);
}

void handle_get_file(int sockfd, struct sockaddr_in clientaddr, char *filename) {
    char buffer[BUFSIZE];
    int n, filefd;
    filefd = open(filename, O_RDONLY);
    if (filefd < 0) {
        error("ERROR opening file");
    }

    // Send file content to client
    while ((n = read(filefd, buffer, BUFSIZE)) > 0) {
        n = sendto(sockfd, buffer, n, 0, (struct sockaddr *)&clientaddr, sizeof(clientaddr));
        if (n < 0) error("ERROR");
    }

    close(filefd);
}

void handle_delete_file(int sockfd, struct sockaddr_in clientaddr, char *filename) {
    if (remove(filename) == 0) {
        printf("File %s deleted successfully\n", filename);
    } else {
        error("ERROR");
    }
}

void handle_list_files(int sockfd, struct sockaddr_in clientaddr) {
    DIR *d;
    struct dirent *dir;
    char buffer[BUFSIZE];
    d = opendir(".");
    if (d) {
        buffer[0] = '\0'; // clear buffer
        while ((dir = readdir(d)) != NULL) {
            strcat(buffer, dir->d_name);
            strcat(buffer, "\n");
        }
        closedir(d);
    }
    sendto(sockfd, buffer, strlen(buffer), 0, (struct sockaddr *)&clientaddr, sizeof(clientaddr));
}

int main(int argc, char **argv) {
    int sockfd;
    int portno;
    int clientlen;
    struct sockaddr_in serveraddr, clientaddr;
    char buf[BUFSIZE];
    int n;

    if (argc != 2) {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        exit(1);
    }
    portno = atoi(argv[1]);

    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) 
        error("ERROR opening socket");

    bzero((char *) &serveraddr, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET;
    serveraddr.sin_addr.s_addr = htonl(INADDR_ANY);
    serveraddr.sin_port = htons(portno);

    if (bind(sockfd, (struct sockaddr *) &serveraddr, sizeof(serveraddr)) < 0) 
        error("ERROR on binding");

    clientlen = sizeof(clientaddr);

    while (1) {
        bzero(buf, BUFSIZE);
        n = recvfrom(sockfd, buf, BUFSIZE, 0, (struct sockaddr *)&clientaddr, &clientlen);
        if (n < 0) error("ERROR");

        if (strncmp(buf, "SEND", 4) == 0) {
            handle_send_file(sockfd, clientaddr, buf + 5); 
        } else if (strncmp(buf, "GET", 3) == 0) {
            handle_get_file(sockfd, clientaddr, buf + 4);  
        } else if (strncmp(buf, "DELETE", 6) == 0) {
            handle_delete_file(sockfd, clientaddr, buf + 7); 
        } else if (strncmp(buf, "ls", 2) == 0) {
            handle_list_files(sockfd, clientaddr);
        }
    }

    return 0;
}
