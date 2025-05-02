/*  
    Daniel Vega
    server.c 
    Takes 10 files from the client. Then,
    combines them into one file, in order 
    and sends the combined file to the client.
    Uses ACKs and Seq#s to make sure everything is received
    in order.
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
#include <sys/wait.h> 
#include <signal.h>
#include <time.h>

#define PORT "7777"    
#define MAXBUFLEN 1024  
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

struct combined_data_packet {
    int packetNum;
    char data[MAXBUFLEN];
};

//will be used with qsort to sort files by name
int compare_strings(const void *a, const void *b) {
    return strcmp((const char *)a, (const char *)b);
}

int main(void) {

    int sockfd; 
    struct addrinfo hints, *servinfo, *p; 
    struct sockaddr_storage their_addr; 
    socklen_t addr_len; 
    int rv; 
    FILE *fileMap[numFiles] = {NULL};
    char fileNames[numFiles][32];

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    //UDP
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = AI_PASSIVE; //use my IP

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
    //set 500ms timeout
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 500000; 
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);
    int currPacket = 0;

    //infinite loop because files can be of any size, and each packet can only contain 3 lines 
    while (1) {

        struct udp_packet pkt;
        struct ack_packet ack;

        int numbytes = recvfrom(sockfd, &pkt, sizeof pkt, 0, (struct sockaddr *)&their_addr, &addr_len);

        if (numbytes == -1) { 
            printf("timeout. waiting for packet to be resent \n");
            continue; //wait for the next packet. The client will keep resending
        }
        
        //end loop once an end packet is received
        if (strcmp(pkt.filename, "END") == 0) {
            printf("server: received END signal\n");
            ack.acki = -2;

            if (sendto(sockfd, &ack, sizeof(ack), 0, (struct sockaddr *)&their_addr, addr_len) == -1) {
                perror("server: sendto ack");
                exit(1);
            }
                //loop to wait to see if client resends end packet.
                //if the client resends it, that means it didn't receive the server's ACK for the end packet
                //so resend the ACK for the end packet
                time_t start_time = time(NULL);
                while (time(NULL) - start_time < 1) { //wait
                    struct udp_packet end_retry;
                    int numbytes = recvfrom(sockfd, &end_retry, sizeof end_retry, 0,
                                            (struct sockaddr *)&their_addr, &addr_len);
                    if (numbytes != -1 && strcmp(end_retry.filename, "END") == 0) {
                        //resend ACK
                        ack.acki = -2;
                        sendto(sockfd, &ack, sizeof(ack), 0, (struct sockaddr *)&their_addr, addr_len);
                    }
                }
            break;
        }

        printf("server: received packet %d from %s, lines %d to %d of %s\n",
            pkt.packetNum,
            inet_ntoa(((struct sockaddr_in*)&their_addr)->sin_addr),
            pkt.currentLineNum,pkt.currentLineNum + pkt.numIncomingLines- 1,
            pkt.filename);

        //if the client resends previously received packet, resend prev ack...
        //assuming the previous ack got lost in transmission
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
            exit(2); //should be impossible since one packet is sent at a time
        } else if(pkt.packetNum < prevAck){//if a packet is received that is 2 or more iterations old
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
        if (fileIndex == -1 && file_count < numFiles) {
            fileIndex = file_count;
            snprintf(fileNames[fileIndex], sizeof(fileNames[fileIndex]), "%s", pkt.filename);
            fileMap[fileIndex] = fopen(fileNames[fileIndex], "w+");
            file_count++;  //increment the count of unique files
        }

        //add new lines to the correct file
        if(pkt.packetNum == currPacket){
            for (int i = 0; i < pkt.numIncomingLines; i++) {
                fprintf(fileMap[fileIndex], "%s\n", pkt.lines[i]);
            }
            //forces a save to memory 
            fflush(fileMap[fileIndex]);
            currPacket++;
            ack.acki = ack_counter++;
            prevAck++;
        
            if (sendto(sockfd, &ack, sizeof(ack), 0, (struct sockaddr *)&their_addr, addr_len) == -1) {
                perror("server: sendto ack");
                exit(1);
            }
        }

        
    }

    //close all files
    for (int i = 0; i < numFiles; i++) {
        if (fileMap[i]) fclose(fileMap[i]);
    }

    //now that all the files were received, combine them into the file that will be sent back
    FILE *final = fopen("combined.txt", "w+");
    //sort fileNames[] to get files in order
    qsort(fileNames, numFiles, sizeof(fileNames[0]), compare_strings);

    //fill file
    for (int i = 0; i < numFiles; i++) {

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

    struct combined_data_packet send_pkt;
    struct ack_packet ack;
    //int currPacket = ack_counter+1; //start at the next packet number in case of reordered packets
    ack.acki = -1;
    while (fgets(send_pkt.data, sizeof(send_pkt.data) - sizeof(int), final)) {
        send_pkt.packetNum = currPacket; 
    
        while (ack.acki != currPacket) {
            //printf("server: sending packet# %d ACK rcvd: %d \n", send_pkt.packetNum, ack.acki);
            //printf("currpacket is %d\n", currPacket);
            // Send the packet
            if (sendto(sockfd, &send_pkt, sizeof(send_pkt), 0, (struct sockaddr *)&their_addr, addr_len) == -1) {
                perror("server: sendto");
                exit(1);
            }

            //ack.acki = -1;
    
            // Wait for ACK
            int numbytes = recvfrom(sockfd, &ack, sizeof(ack), 0, (struct sockaddr *)&their_addr, &addr_len);
            if (numbytes == -1) {
                printf("Timeout. Resending packet# %d\n", send_pkt.packetNum);
                continue;
            }
    
            if (ack.acki > currPacket) {
                printf("server: client acknowledged a packet that wasn't sent yet?\n");
                exit(1);
            }else if (ack.acki == currPacket - 1)
            {
                printf("server: client acknowledged a packet that was already sent\n");
                continue; //ignore the ack, it was already received previously
            }  
    
            printf("server: ACK received for packet# %d\n", ack.acki);
        }
    
        currPacket++;
    }
    
    //send the END packet
    send_pkt.packetNum = -1;
    memset(send_pkt.data, 0, sizeof(send_pkt.data)); // Clear the data field
    while (1) {
        if (sendto(sockfd, &send_pkt, sizeof(send_pkt), 0, (struct sockaddr *)&their_addr, addr_len) == -1) {
            perror("server: sendto END");
            exit(1);
        }
    
        int numbytes = recvfrom(sockfd, &ack, sizeof(ack), 0, (struct sockaddr *)&their_addr, &addr_len);
        if (numbytes == -1) {
            printf("Timeout. Resending END packet\n");
            continue;
        }
    
        if (ack.acki == -1) {
            printf("server: client acknowledged END packet\n");
            break;
        }
    }
    
    fclose(final);
    close(sockfd);
    printf("server: finished and exiting\n");

    return 0;
}
