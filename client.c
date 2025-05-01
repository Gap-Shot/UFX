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
    //int totalLines;
    int currentLineNum;
    int numIncomingLines;
    char lines[MAX_LINES_PER_PACKET][256];
    int packetNum;
};

struct ack_packet {
    int acki;
};

int main(int argc, char *argv[]) {

    int sockfd; 
    struct addrinfo hints, *servinfo, *p;
    int rv;
    struct sockaddr_storage their_addr;
    socklen_t addr_len; 

    if (argc != 2) {
        fprintf(stderr, "must supply server's ipaddress/hostname when executing (./client xxx.xxx.xxx.xxx\n");
        exit(1);
    }

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
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

        if (!files[i]) {
            perror("fopen");
            exit(2);
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
    int currPacket = 0; //index to keep track of how many packets were successfully sent by comparing to ACKs

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
            line[strcspn(line, "\n")] = 0;
            strncpy(pkt.lines[i], line, sizeof(pkt.lines[i])-1);
            i++;
        }

        pkt.numIncomingLines = i;
        //ACK packet
        struct ack_packet ack;
        addr_len = sizeof their_addr;
        ack.acki = -1; //set to -1 to guarentee the packet data is sent once
        //loop to resend packet until the server acknowledges it
        while(ack.acki != currPacket){
            //send/resend packet
            if (sendto(sockfd, &pkt, sizeof(pkt), 0, p->ai_addr, p->ai_addrlen) == -1) {
                perror("sendto");
                exit(1);
            }
            //did we receive ACK?
            int numbytes = recvfrom(sockfd, &ack, sizeof(ack), 0,
                                    (struct sockaddr *)&their_addr, &addr_len);
            if (numbytes == -1) {
                printf("Timeout. resending packet# %d\n", pkt.packetNum);
                continue;
            }
            //if the server somehow sends an ACK from a packet the client hasn't sent yet, error out
            //should be impossible since we are sending one at a time, unless another client sent packet(s) to the server
            if(ack.acki > currPacket){
                printf("client: Server acknowledged a packet that wasn't sent yet?");
                exit(1);
            }
            printf("client: ACK received for packet# %d\n", ack.acki);
        }
        //increment the lines set var of current file
        sentLines[fileIndex] += i;

        //check and docunent if the file was fully sent
        if (sentLines[fileIndex] == numLines[fileIndex]) {
            filesSent[fileIndex] = 1;
            filesCompleted++;
        } 
        currPacket++;
    }

    //send the end packet
    struct ack_packet ack;
    struct udp_packet end_pkt;
    memset(&end_pkt, 0, sizeof(end_pkt));
    snprintf(end_pkt.filename, sizeof(end_pkt.filename), "END");
    while(ack.acki != -2){

        sendto(sockfd, &end_pkt, sizeof(end_pkt), 0, p->ai_addr, p->ai_addrlen);

        printf("client: All files sent. Waiting for server to acknowledge and send the combined file.\n");
        int numbytes = recvfrom(sockfd, &ack, sizeof(ack), 0,
                                (struct sockaddr *)&their_addr, &addr_len);
        if (numbytes == -1) {
              printf("Timeout. resending END packet\n");
              continue;
        } 
        
    }

    printf("client: server acknowledged all files were sent. Awaiting combined file");
    
    addr_len = sizeof their_addr;
    char recv_buf[MAXBUFLEN];

    FILE *combined = fopen("combined_from_server.txt", "w");
    //receive and write lines to file until END packet is received 
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

