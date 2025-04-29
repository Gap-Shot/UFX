/*  
    Daniel Vega
    client.c 
    Sends 10 files to server using UDP. 
    Sends 1-3 lines at a time from a random
    file. 
    Uses ACKs to make sure everything is 
    received in order.
*/

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
#include <time.h>

#define SERVERPORT "7777"
#define MAXBUFLEN 65536
#define numFiles 10
#define MAX_LINES_PER_PACKET 3

struct udp_packet {
    char filename[32];
    int totalLines;
    int currentLineNum;
    int numIncomingLines;
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

int main(int argc, char *argv[]) {

    int sockfd; //socket file descriptor
    struct addrinfo hints, *servinfo, *p; //hints are params to giveaddrinfo, servinfo is a linkedlist of possible addresses, 
                                             //p is a pointer to iteratethrough servinfo
    int rv;//return value for getaddrinfo. used for error checking
    struct sockaddr_storage their_addr;//the address of the server
    socklen_t addr_len; //size of server's address

    if (argc != 2) {
        fprintf(stderr, "must supply server's ipaddress/hostname when executing (./client xxx.xxx.xxx.xxx\n");
        exit(1);
    }

    memset(&hints, 0, sizeof hints);
    //can be either ipv4 or 6 
    hints.ai_family = AF_UNSPEC;
    //UDP 
    hints.ai_socktype = SOCK_DGRAM;

    if ((rv = getaddrinfo(argv[1], SERVERPORT, &hints, &servinfo)) != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
        return 1;
    }


    //loop to create socket 
    for (p = servinfo; p != NULL; p = p->ai_next) {
        if ((sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) == -1) {
            perror("client: socket");
            continue;
        }
        break;
    }

    if (p == NULL) {
        fprintf(stderr, "client: failed to create socket\n");
        return 2;
    }
    //set 500ms timeout
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 500000; 
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);


    FILE *files[numFiles];

    char fileNames[numFiles][32]; //array to hold fileNames
    int numLines[numFiles]; //array to hold total # of lines per file
    int sentLines[numFiles];  //array to hold # of lines sent per file so far

    for (int i = 0; i < numFiles; i++) {

        snprintf(fileNames[i], sizeof(fileNames[i]), "file_%d.txt", i+1);

        files[i] = fopen(fileNames[i], "r");

        //if the file doesn't exist or can't be open
        if (!files[i]) {
            perror("fopen");
            exit(1);
        }

        
        //determine how many lines in current file
        numLines[i] = 0;
        while (!feof(files[i])) {
            char line[256];

            if (fgets(line, sizeof(line), files[i])) {
                numLines[i]++;
            }

        }

        fseek(files[i], 0, SEEK_SET); //set pointer to beginning
        sentLines[i] = 0; 
    }

    srand(time(NULL)); //seed the rng to the current time
    int filesSent[numFiles] = {0};  //array to keep track of which files were fully sent
    int filesCompleted = 0;  //array to keep track of how many files were sent
    int currPacket = 0;

    while (filesCompleted < numFiles) {

        int fileIndex = -1;
        while (fileIndex == -1) {

            int randomFile = rand() % numFiles;  //choose rand file
            //check if the file was fully sent yet
            //if the file was fully sent, try another
            //can be more efficient. Maybe optimize later
            if (numLines[randomFile] > 0 && !filesSent[randomFile]) {
                fileIndex = randomFile;
            }
        }

        //send 1-3 lines from the selected file
        struct udp_packet pkt;
        memset(&pkt, 0, sizeof(pkt));
        pkt.packetNum = currPacket;
        snprintf(pkt.filename, sizeof(pkt.filename), "%s", fileNames[fileIndex]);

        //set the start line. dependent on how many lines were already sent
        pkt.currentLineNum = sentLines[fileIndex];

        //generate a random # from 1-3 to determine how many lines to send
        //could optimize by capping it with the delta of lines sent and total lines in file
        int lines_to_send = rand() % MAX_LINES_PER_PACKET + 1; 

        int i = 0;
        char line[256];

        //nested loop to add the lines from the file to the packet 
        while (i < lines_to_send && fgets(line, sizeof(line), files[fileIndex])) {
            line[strcspn(line, "\n")] = 0; // remove newline
            strncpy(pkt.lines[i], line, sizeof(pkt.lines[i])-1);
            i++;
        }

        pkt.numIncomingLines = i;

        struct ack_packet ack;
        addr_len = sizeof their_addr;
        ack.acki = -1;
        //loop to resend packet until the server acknowledges it
        while(ack.acki != currPacket){
            //send packet
            if (sendto(sockfd, &pkt, sizeof(pkt), 0, p->ai_addr, p->ai_addrlen) == -1) {
                perror("sendto");
                exit(1);
            }
            //did we receive ACK?
            int numbytes = recvfrom(sockfd, &ack, sizeof(ack), 0,
                                    (struct sockaddr *)&their_addr, &addr_len);
            if (numbytes == -1) {
                printf("Timeout. resending %d\n", pkt.packetNum);
                continue;
            }
            //if the server somehow sends an ACK from a packet the client hasn't sent yet
            //should be impossible given our code, unless another client sent packet(s) to the server
            if(ack.acki > currPacket){
                printf("client: Server acknowledged a packet that wasn't sent yet?");
                exit(1);
            }
            printf("client: ACK received for %d\n", ack.acki);
        }
        //increment the lines set var of curr file
        sentLines[fileIndex] += i;

        //check and mark if the file was fully sent
        if (sentLines[fileIndex] == numLines[fileIndex]) {
            filesSent[fileIndex] = 1;
            filesCompleted++;
        } 
        currPacket++;
    }

    /* send the end packet */
    struct udp_packet end_pkt;
    memset(&end_pkt, 0, sizeof(end_pkt));
    snprintf(end_pkt.filename, sizeof(end_pkt.filename), "END");
    sendto(sockfd, &end_pkt, sizeof(end_pkt), 0, p->ai_addr, p->ai_addrlen);

    printf("client: All files sent. Waiting for server to send combined file.\n");

    addr_len = sizeof their_addr;
    char recv_buf[MAXBUFLEN];

    FILE *combined = fopen("combined_from_server.txt", "w");

    while (1) {

        int numbytes = recvfrom(sockfd, recv_buf, MAXBUFLEN-1, 0,
                            (struct sockaddr *)&their_addr, &addr_len);

        recv_buf[numbytes] = '\0';

        if (strcmp(recv_buf, "DONE") == 0) {
            printf("client: received DONE signal, finished receiving file.\n");
            break;
        }
        
        fprintf(combined, "%s", recv_buf);
    }

    //close file and socket and free mem
    fclose(combined);
    freeaddrinfo(servinfo);
    close(sockfd);

    printf("client: file received and saved as combiend_from_server.txt\n");

    return 0;
}


