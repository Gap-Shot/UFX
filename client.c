/* client.c -- updated UDP client */

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
#define MAXFILES 10
#define MAX_LINES_PER_PACKET 3

struct udp_packet {
    char filename[32];
    int total_lines;
    int start_line_number;
    int num_lines;
    char lines[MAX_LINES_PER_PACKET][256];
};

//returns pointer to address and allows for program to work with ipv4 and 6
void *get_in_addr(struct sockaddr *sa) {
    if (sa->sa_family == AF_INET)
        return &(((struct sockaddr_in*)sa)->sin_addr); 
 
    return &(((struct sockaddr_in6*)sa)->sin6_addr);
}

int main(int argc, char *argv[]) {

    int sockfd; //socket file descriptor
    struct addrinfo hints, *servinfo, *p; //hints are params to giveaddrinfo, servinfo is a linkedlist of possible addresses, p is a pointer to iteratethrough servinfo
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

    //error checking 
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

    FILE *files[MAXFILES];

    char fileNames[MAXFILES][32]; //array to hold fileNames
    int numLines[MAXFILES]; //array to hold total # of lines per file
    int sentLines[MAXFILES];  //array to hold # of lines sent per file so far

    for (int i = 0; i < MAXFILES; i++) {

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

        sentLines[i] = 0;  //set the sent lines to 0 
    }

    srand(time(NULL)); //seed the rng to the current time
    int filesSent[MAXFILES] = {0};  //array to keep track of which files were fully sent
    int total_files_sent = 0;  //array to keep track of how many files were sent

    while (total_files_sent < MAXFILES) {

        int fileIndex = -1;
        while (fileIndex == -1) {

            int random_file = rand() % MAXFILES;  //choose rand file
            //protect against empty files and check if the file was fully sent yet
            //if the file was fully sent, try another
            //can be more efficient. Maybe optimize later
            if (numLines[random_file] > 0 && !filesSent[random_file]) {
                fileIndex = random_file;
            }
        }

        //send 1-3 lines from the selected file
        struct udp_packet pkt;
        memset(&pkt, 0, sizeof(pkt));
        snprintf(pkt.filename, sizeof(pkt.filename), "%s", fileNames[fileIndex]);

        //set the start line. dependent on how many lines were already sent
        pkt.start_line_number = sentLines[fileIndex];

        //generate a random # from 1-3 to determine how many lines to send
        //could optimize by capping it with the delta of lines sent and total lines in file
        int lines_to_send = rand() % MAX_LINES_PER_PACKET + 1; 

        int count = 0;
        char line[256];

        //nested loop to add the lines from the file to the packet 
        while (count < lines_to_send && fgets(line, sizeof(line), files[fileIndex])) {
            line[strcspn(line, "\n")] = 0; // remove newline
            strncpy(pkt.lines[count], line, sizeof(pkt.lines[count])-1);
            count++;
        }

        pkt.num_lines = count;

        //sends the packet and throws an error if one occurs 
        if (sendto(sockfd, &pkt, sizeof(pkt), 0, p->ai_addr, p->ai_addrlen) == -1) {
            perror("sendto");
            exit(1);
        }

        //increment the lines set var of curr file
        sentLines[fileIndex] += count;

        usleep(5000); //delay 
        /*TODO: add logic to wait for an ACK packet before sending the next packet*/

        //check and mark if the file was fully sent
        if (sentLines[fileIndex] == numLines[fileIndex]) {
            filesSent[fileIndex] = 1;
            total_files_sent++;
        } 
    }

    /* send the end packet */
    struct udp_packet end_pkt;
    memset(&end_pkt, 0, sizeof(end_pkt));
    snprintf(end_pkt.filename, sizeof(end_pkt.filename), "END");
    sendto(sockfd, &end_pkt, sizeof(end_pkt), 0, p->ai_addr, p->ai_addrlen);

    printf("client: finished sending files. Waiting to receive combined file...\n");

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
        //write data to file 
        fprintf(combined, "%s", recv_buf);
    }

    //close file and socket and free mem
    fclose(combined);
    freeaddrinfo(servinfo);
    close(sockfd);

    printf("client: file received and saved as combiend_from_server.txt\n");

    return 0;
}

