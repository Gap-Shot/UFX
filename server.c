/* server.c -- updated UDP server */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/wait.h> 
#include <signal.h>

#define PORT "7777"    
#define MAXBUFLEN 65536  
#define MAXFILES 10
#define MAX_LINES_PER_PACKET 3

struct udp_packet {
    char filename[32];
    int total_lines; 
    int start_line_number;
    int num_lines;
    char lines[MAX_LINES_PER_PACKET][256]; 
    int packetNum;
};

struct ack_packet {
    int acki;
};

//returns pointer to address and allows for program to work with ipv4 and 6
void *get_in_addr(struct sockaddr *sa) {
    if (sa->sa_family == AF_INET) 
        return &(((struct sockaddr_in*)sa)->sin_addr);
        
    return &(((struct sockaddr_in6*)sa)->sin6_addr);
}

int main(void) {

    int sockfd; //socket file descriptor
    struct addrinfo hints, *servinfo, *p; //hints are params to giveaddrinfo, servinfo is a linkedlist of possible addresses, p is a pointer to iterate through servinfo
    struct sockaddr_storage their_addr; //the client's address
    socklen_t addr_len; //size of client address
    int rv; //return value for getaddrinfo. used for error checking

    FILE *fileMap[MAXFILES] = {NULL};
    char fileNames[MAXFILES][32];

    memset(&hints, 0, sizeof hints);
    //can be either ipv4 or 6
    hints.ai_family = AF_UNSPEC; 
    //UDP
    hints.ai_socktype = SOCK_DGRAM;
    //passive
    hints.ai_flags = AI_PASSIVE;

    //error handling
    if ((rv = getaddrinfo(NULL, PORT, &hints, &servinfo)) != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
        return 1;
    }

    //loop to create socket and bind it 
    for (p = servinfo; p != NULL; p = p->ai_next) {
        if ((sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) == -1) {
            perror("server: socket");
            continue;
        }
        if (bind(sockfd, p->ai_addr, p->ai_addrlen) == -1) {
            close(sockfd);
            perror("server: bind"); 
            continue;
        }
        break;
    }

    if (p == NULL) {
        fprintf(stderr, "server: failed to bind socket\n");
        return 2;
    }

    freeaddrinfo(servinfo);
    printf("server: waiting to recvfrom...\n");
    addr_len = sizeof their_addr;

    int file_count = 0; //track the number of unique files received
    static int prevAck = -1;
    static int ack_counter = 0;  
    //infinite loop because files can be of any size, and each packet can only contain 3 lines 
    while (1) {

        struct udp_packet pkt;

        int numbytes = recvfrom(sockfd, &pkt, sizeof pkt, 0, (struct sockaddr *)&their_addr, &addr_len);

        if (numbytes == -1) { 
            perror("recvfrom");
            exit(1);
        }
        
        //end loop once an end packet is received
        if (strcmp(pkt.filename, "END") == 0) {
            printf("server: received END signal\n");
            break;
        }

        printf("server: received packet %d from %s, lines %d to %d of %s\n",
            pkt.packetNum,
            inet_ntoa(((struct sockaddr_in*)&their_addr)->sin_addr),
            pkt.start_line_number,
            pkt.start_line_number + pkt.num_lines - 1,
            pkt.filename);

        //if the client resends prev ACk
        //i.e if the client never got the server's prev ACK. 
        //resend prev ack and don't add duplicate lines
        if(pkt.packetNum == prevAck){
            struct ack_packet ack; 
            ack.acki = prevAck;

            if (sendto(sockfd, &ack, sizeof(ack), 0, (struct sockaddr *)&their_addr, addr_len) == -1) {
                perror("server: sendto ack");
                exit(1);
            }
            continue;
        } else if(pkt.packetNum > prevAck + 1){
            printf("packet from the future received? %d %d", pkt.packetNum, prevAck);
            exit(2); //should be impossible with the way client and server are setup
        } else if(pkt.packetNum < prevAck){
            printf("server: received old packet: %d", pkt.packetNum);
            continue; //assume it's an old packet that got lost in traffic and ignore it 
        }

        //check if the filename has been received before
        int fileIndex = -1;
        for (int i = 0; i < file_count; i++) {
            if (strcmp(fileNames[i], pkt.filename) == 0) {
                fileIndex = i; 
                break;
            }
        }

        //if it's a new filename, add it to the list and open a new file for it
        if (fileIndex == -1 && file_count < MAXFILES) {
            fileIndex = file_count;
            snprintf(fileNames[fileIndex], sizeof(fileNames[fileIndex]), "%s", pkt.filename);
            fileMap[fileIndex] = fopen(fileNames[fileIndex], "w+");
            file_count++;  //increment the count of unique files
        }

        //add new lines to the correct file
        for (int i = 0; i < pkt.num_lines; i++) {
            fprintf(fileMap[fileIndex], "%s\n", pkt.lines[i]);
        }
        
        //forces a save to memory 
        fflush(fileMap[fileIndex]);

        /*send ack packet*/
        struct ack_packet ack;
        ack.acki = ack_counter++;
        prevAck++;

        if (sendto(sockfd, &ack, sizeof(ack), 0, (struct sockaddr *)&their_addr, addr_len) == -1) {
            perror("server: sendto ack");
            exit(1);
        }

        
    }

    //close all files
    for (int i = 0; i < MAXFILES; i++) {
        if (fileMap[i]) fclose(fileMap[i]);
    }

    //now that all the files were received, combine them into the file that will be sent back
    FILE *final = fopen("combined.txt", "w+");

    for (int i = 0; i < MAXFILES; i++) {
        if (strlen(fileNames[i]) > 0) {
            FILE *f = fopen(fileNames[i], "r");
   
            char line[256];

            fprintf(final, "\n%s\n", fileNames[i]);
            //insert lines of the current file into the final combined file
            while (fgets(line, sizeof(line), f)) { 
                fputs(line, final); 
            }

            fclose(f);
        }
    }

    fclose(final);

    //send the combined file to the client
    final = fopen("combined.txt", "r");

    char send_buf[512];

    while (fgets(send_buf, sizeof(send_buf), final)) {
        sendto(sockfd, send_buf, strlen(send_buf), 0,
               (struct sockaddr *)&their_addr, addr_len);
    }

    //send DONE so the client knows when the communication is over
    sendto(sockfd, "DONE", 4, 0, (struct sockaddr *)&their_addr, addr_len);

    //close out file and socket and end program
    fclose(final);
    close(sockfd);
    printf("server: finished and exiting\n");

    return 0;
}

