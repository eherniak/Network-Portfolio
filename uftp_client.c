#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <fcntl.h>

#define BUFSIZE 1024

void error(char *msg) {
    perror(msg);
    exit(0); 
}

void PUT(int sockfd, struct sockaddr_in serveraddr, char *filename) {
    char buffer[BUFSIZE];
    int n, filefd;
    filefd = open(filename, O_RDONLY);
    if (filefd < 0) {
        error("ERROR opening file");
    }

    // send filename
    n = sendto(sockfd, filename, strlen(filename), 0, (struct sockaddr *)&serveraddr, sizeof(serveraddr));
    if (n < 0) error("ERROR");

    // send file content to server
    while ((n = read(filefd, buffer, BUFSIZE)) > 0) {
        n = sendto(sockfd, buffer, n, 0, (struct sockaddr *)&serveraddr, sizeof(serveraddr));
        if (n < 0) error("ERROR");
    }

    close(filefd);
}

void GET(int sockfd, struct sockaddr_in serveraddr, char *filename) {
    char buffer[BUFSIZE];
    int n;
    n = sendto(sockfd, filename, strlen(filename), 0, (struct sockaddr *)&serveraddr, sizeof(serveraddr));
    if (n < 0) error("ERROR");

    // recieve file content fom server
    FILE *file = fopen(filename, "wb");
    if (file == NULL) {
        error("ERROR opening file");
    }

    while ((n = recvfrom(sockfd, buffer, BUFSIZE, 0, NULL, NULL)) > 0) {
        fwrite(buffer, 1, n, file);
    }

    fclose(file);
}

void DELETE(int sockfd, struct sockaddr_in serveraddr, char *filename) {
    int n = sendto(sockfd, filename, strlen(filename), 0, (struct sockaddr *)&serveraddr, sizeof(serveraddr));
    if (n < 0) error("ERROR");
}

void ls(int sockfd, struct sockaddr_in serveraddr) {
    char buffer[BUFSIZE];
    int n = sendto(sockfd, "list_files", strlen("list_files"), 0, (struct sockaddr *)&serveraddr, sizeof(serveraddr));
    if (n < 0) error("ERROR");

    // Receive file list from server
    n = recvfrom(sockfd, buffer, BUFSIZE, 0, NULL, NULL);
    if (n < 0) error("ERROR");

    printf("Files on server:\n%s", buffer);
}

int main(int argc, char **argv) {
    int sockfd, portno, n;
    int serverlen;
    struct sockaddr_in serveraddr;
    struct hostent *server;
    char *hostname;
    char buf[BUFSIZE];

    if (argc != 3) {
       fprintf(stderr,"usage: %s <hostname> <port>\n", argv[0]);
       exit(0);
    }
    hostname = argv[1];
    portno = atoi(argv[2]);

    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) 
        error("ERROR opening socket");

    server = gethostbyname(hostname);
    if (server == NULL) {
        fprintf(stderr,"ERROR, no such host as %s\n", hostname);
        exit(0);
    }

    bzero((char *) &serveraddr, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET;
    bcopy((char *)server->h_addr, (char *)&serveraddr.sin_addr.s_addr, server->h_length);
    serveraddr.sin_port = htons(portno);

    printf("Enter command: PUT, GET, DELETE, ls:\n");
    fgets(buf, BUFSIZE, stdin);

    if (strncmp(buf, "SEND", 4) == 0) {
        char filename[BUFSIZE];
        sscanf(buf, "SEND %s", filename);
        SEND(sockfd, serveraddr, filename);
    } else if (strncmp(buf, "GET", 3) == 0) {
        char filename[BUFSIZE];
        sscanf(buf, "GET %s", filename);
        GET(sockfd, serveraddr, filename);
    } else if (strncmp(buf, "DELETE", 6) == 0) {
        char filename[BUFSIZE];
        sscanf(buf, "DELETE %s", filename);
        DELETE(sockfd, serveraddr, filename);
    } else if (strncmp(buf, "ls", 2) == 0) {
        ls(sockfd, serveraddr);
    }

    return 0;
}
