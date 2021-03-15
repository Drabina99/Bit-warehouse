#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/poll.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <errno.h>
#include <sys/timerfd.h>
#include <fcntl.h>

#define _GNU_SOURCE

#define BLOCK_SIZE 640
#define FULL_PORTION_SIZE 13312
#define MAX_HOST 30
#define PORTION_SIZE 1024 //rozmiar "malej" paczki
#define MAX_CLIENTS 500
#define ALL_PACKS_NB 13 //liczba paczek wyslanych w ciagu 1 polaczenia
#define BILLION 1000000000L

struct timespec ts = { .tv_sec = 0, .tv_nsec = 0 };
struct timespec TS = {.tv_sec = 0, .tv_nsec = 0};

void  childProcess(int* pipe_fd, char* host, int port);
float getPace(int arg_c, char **arg_v); //funkcja pobierajaca tempo
int   getHostnameAndPort(char* addr, char* hostname, int* port); //funkcja rozdzielajaca adres na hosta i port
void  fillWithLetters(char* arr, int* ascii); //wypelnianie mamgazynu trescia
int   createAndConfigureSocket(char* hostname, int port);
void  locRegister(int socket_fd, char *hostname, in_port_t port);
char* getPackToSend(int* fd, int clientFd);
char* readFromPipe(int* fd);
void  parentProcess(int* fd, int pace);
int   isBacklog(); //funkcja sprawdzajaca, czy jest cos jeszcze do wyslania
void  closeConnection(int index);
void  initSocketConnection(int ind, int sd, char *addr);
void  sendOnePack(int index, char* buffer);
void  sendWhenThereIsNoNewClientReady(int* pipe_fd);
char* recvClientAddress(int client_fd, int index);
int   isPipeFilled(int* pipe_fd);
int   acceptConnectionWithNewClient(int socket_fd);
int   initTimer(int ind);
void  printReport(int* pipe_fd);

#define TRUE 1
#define FALSE 0

struct pack {
    int sentPackNb;
    char *buf;
    char addr[20];
};

struct clients {
    struct pack pkg[MAX_CLIENTS];
    struct pollfd fds[MAX_CLIENTS];
};

struct clients clientsSet;

int nfds;
int bytesTakenFromPipe; //bajty odczytane przez potomka
int bytesSent;
int bytesLost;


int main(int argc, char **argv) {
    float pace = getPace(argc, argv);
    ts.tv_nsec = BILLION*(BLOCK_SIZE/(2662*pace));
    if (pace == -1) {
        printf("Podano nieprawidlowe tempo!\n");
        exit(EXIT_FAILURE);
    }
    char host[MAX_HOST];
    int port;
    strcpy(host, argv[optind]);
    if (getHostnameAndPort(argv[optind], host, &port) == -1) {
        printf("Nieprawidlowy adres hosta!\n");
        exit(EXIT_FAILURE);
    }
    printf("%s:%d\n", host, port);
    int pipe_fd[2];
    if (pipe(pipe_fd) == -1) {
        printf("Blad pipe!\n");
        exit(EXIT_FAILURE);
    }
    pid_t p = fork();
    if (p == -1) {
        printf("Blad tworzenia potomka!\n");
        exit(EXIT_FAILURE);
    }
    else if (p == 0)
        childProcess(pipe_fd, host, port);
    else
        parentProcess(pipe_fd, pace);
    exit(0);
}
void childProcess(int* pipe_fd, char* host, int port) {
    int rc = 0, current_size = 0, conn_accepted = TRUE, timeout = 0;
    nfds = 2;
    int socket_fd = createAndConfigureSocket(host, port);
    initSocketConnection(1, socket_fd, "");
    while(!isPipeFilled(pipe_fd));
    initTimer(0);
    do {
        timeout = -1;
        if (isBacklog()) //sprawdzam czy zostalo cos do wyslania
            timeout = 100;
        rc = 0;
        if (nfds < MAX_CLIENTS - 1)
            rc = poll(clientsSet.fds, nfds, timeout);
        if (rc < 0) {
            printf("Blad funkcji poll!\n");
            break;
        }
        else if (rc == 0) //nastapil timeout
            sendWhenThereIsNoNewClientReady(pipe_fd);
        else { //nastapilo zdarzenie w pollu
            if(clientsSet.fds[0].revents == POLLIN) { //obsluga budzika
                char tempBuffer[100];
                read(clientsSet.fds[0].fd, tempBuffer, 8);
                printReport(pipe_fd);
                clientsSet.fds[0].revents=0;
                continue;
            }
            current_size = nfds;
            for (int i = 2; i < current_size; i++) {
                char buf1[32];
                //sprawdzam czy klient sie rozlaczyl
                if (recv(clientsSet.fds[i].fd, buf1, sizeof(buf1), MSG_PEEK | MSG_DONTWAIT) == 0)
                    closeConnection(i);
            }
            for (int i = 1; i < current_size; i++) {
                if (clientsSet.fds[i].revents == 0)
                    continue;
                if (clientsSet.fds[i].revents != POLLIN)
                    continue;
                if (clientsSet.fds[i].fd == socket_fd) {
                    conn_accepted = acceptConnectionWithNewClient(socket_fd);
                }
                else {
                    while(!isPipeFilled(pipe_fd)) {
                        if (isBacklog())
                            sendWhenThereIsNoNewClientReady(pipe_fd);
                        else //czeka na zaladowanie pipe
                            sleep(1);
                    }
                    char* buffer = getPackToSend(pipe_fd, clientsSet.fds[i].fd);
                    sendOnePack(i, buffer);
                }
            }
        }
    } while(conn_accepted == TRUE);
    close(pipe_fd[0]);
}

int createAndConfigureSocket(char* hostname, int port) {
    int on = 1;
    int socket1 = socket(AF_INET, SOCK_STREAM, 0);
    if (socket1 == -1) {
        printf("Blad tworzenia gniazda! (producent)\n");
        exit(EXIT_FAILURE);
    }
    if (setsockopt(socket1, SOL_SOCKET, SO_REUSEADDR, (char*)&on, sizeof(on)) < 0) {
        printf("Blac setsockopt!\n");
        close(socket1);
        exit(EXIT_FAILURE);
    }
    if (ioctl(socket1, FIONBIO, (char*) &on) < 0) {
        printf("Blad ioctl!\n");
        close(socket1);
        exit(EXIT_FAILURE);
    }
    if(!strcmp(hostname, "localhost"))
        strcpy(hostname, "127.0.0.1"); //ustawiam adres petli zwrotnej dla localhosta
    locRegister(socket1, hostname, port);
    int queueLen = 100;
    if (listen(socket1, queueLen)) {
        printf("Blad funkcji listen!");
        exit(EXIT_FAILURE);
    }
    return socket1;
}

void locRegister(int socket_fd, char *hostname, in_port_t port) {
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (!inet_aton(hostname, &addr.sin_addr)) {
        printf("Podano nieprawidlowy adres hosta!\n");
        exit(EXIT_FAILURE);
    }
    if (bind(socket_fd, (struct sockaddr*) &addr, sizeof(addr))) {
        printf("Blad wywolania funkcji bind!\n");
        exit(EXIT_FAILURE);
    }
}

float getPace(int arg_c, char **arg_v) {
    char *ptr;
    int opt;
    float pace = -1;
    while((opt = getopt(arg_c, arg_v, "p:")) != -1) {
        switch(opt) {
            case 'p':
                pace = strtof(optarg, &ptr);
                if (*ptr != '\0') {
                    pace = -1;
                    break;
                }
                break;
            case '?':
                printf("Nieznana opcja %c\n", opt);
                break;
        }
    }
    return pace;
}

int getHostnameAndPort(char *addr, char *hostname, int *port) {
    char *pch = strchr(addr, ':');
    char *ptr;
    if (pch == NULL) {
        *port = strtol(hostname, &ptr, 10);
        if (*ptr != '\0')
            return -1;
        sprintf(hostname, "localhost");
        return 0;
    }
    else {
        char postStr[10];
        strcpy(postStr, pch + 1);
        *pch = '\0';
        strcpy(hostname, addr);
        *port = strtol(postStr, &ptr, 10);
        if (*ptr != '\0')
            return -1;
    }
    return 0;
}

void fillWithLetters(char *arr, int *ascii) {
    memset(arr, *ascii, BLOCK_SIZE);
    if (*ascii == 90)
        *ascii = 97;
    else if (*ascii == 122)
        *ascii = 65;
    else
        (*ascii)++;
}

char* getPackToSend(int *pipe_fd, int clientFd) {
    char *portion = NULL;
    if (nfds >= MAX_CLIENTS) {
        printf("Za duza liczba klientow!\n");
        exit(EXIT_FAILURE);
    }
    for(int i = 2; i < nfds; i++) {
        if (clientsSet.fds[i].fd == clientFd) {
            if (strlen(clientsSet.pkg[i].buf) == 0) //jezeli bufor dla klienta jest pusty, to wypelniam go danymi
                strcpy(clientsSet.pkg[i].buf, readFromPipe(pipe_fd));
            portion =  &(clientsSet.pkg[i].buf[clientsSet.pkg[i].sentPackNb * PORTION_SIZE]);
            break;
        }
    }
    return portion;
}

char* readFromPipe(int* fd) {
    static char portion[FULL_PORTION_SIZE + 1];
    memset(portion, 0, FULL_PORTION_SIZE + 1);
    int curr_bytes = 0;
    while(curr_bytes < FULL_PORTION_SIZE) { //czyta dopoki nie osiagnie 13KiB
        int bytes_to_read = FULL_PORTION_SIZE - curr_bytes;
        int bytes = read(fd[0], &portion[curr_bytes], bytes_to_read);
        if (bytes > 0) {
            curr_bytes += bytes;
        }
    }
    bytesTakenFromPipe += FULL_PORTION_SIZE;
    return portion;
}

void initSocketConnection(int ind, int sd, char *addr) {
    clientsSet.fds[ind].fd = sd;
    clientsSet.fds[ind].events = POLLIN;
    clientsSet.pkg[ind].sentPackNb = 0;
    clientsSet.pkg[ind].buf = calloc(FULL_PORTION_SIZE + 1, 1);
    strcpy(clientsSet.pkg[ind].addr, addr);
}

int isBacklog() {
    for (int i = 2; i < nfds; i++) {
        if(strlen(clientsSet.pkg[i].addr)>0 && clientsSet.fds[i].fd >0 ) {
            if(clientsSet.pkg[i].sentPackNb < ALL_PACKS_NB) {
                return 1;
            }
        }
    }
    return 0;
}

void sendOnePack(int index, char* buffer) {
    int rc = send(clientsSet.fds[index].fd, buffer, PORTION_SIZE, 0);
    printf(" Do klienta: %s wyslano: %d\n", clientsSet.pkg[index].addr, rc);
    if (rc < 0) {
        printf("Blad funkcji send!\n");
        closeConnection(index);
    }
    else {
        bytesSent += rc;
        clientsSet.pkg[index].sentPackNb++;
        if(clientsSet.pkg[index].sentPackNb >= ALL_PACKS_NB) {
            closeConnection(index);
        }
    }
}

void sendWhenThereIsNoNewClientReady(int* pipe_fd) {
    static int last_ind = 1;
    int ind = -1;
    if(last_ind == nfds-1)
        last_ind = 1;
    for (int i = last_ind + 1; i < nfds; i++) { //wysylam nastepnemu klientowi dane
        if (clientsSet.pkg[i].buf != NULL && clientsSet.fds[i].fd > 0 && clientsSet.pkg[i].sentPackNb < ALL_PACKS_NB) {
            ind = i;
            last_ind = i;
            break;
        }
    }
    if (ind >= 0) {
        char* buffer = getPackToSend(pipe_fd, clientsSet.fds[ind].fd);
        sendOnePack(ind, buffer);
    }
    return;
}

char* recvClientAddress(int client_fd, int index) {
    static char clientMsg[30];
    memset(clientMsg, '\0', sizeof(clientMsg));
    int rc = recv(client_fd, clientMsg, sizeof(clientMsg), 0);
    if (rc < 0) {
        if (errno != EWOULDBLOCK) {
            closeConnection(index);
            printf("Blad funkcji recv!\n");
        }
    }
    else if (rc == 0) {
        printf("Polaczenie zamkniete\n");
        closeConnection(index);
    }
    return clientMsg;
}

int isPipeFilled(int *pipe_fd) {
    int bytesAvailable = 0;
    ioctl(pipe_fd[1], FIONREAD, &bytesAvailable);
    if (bytesAvailable < FULL_PORTION_SIZE) {
        return 0;
    }
    return 1;
}

void closeConnection(int index) {
    close(clientsSet.fds[index].fd);
    clientsSet.fds[index].fd = -1;
    clientsSet.fds[index].events = 0;
    clientsSet.fds[index].revents = 0;
    if (clientsSet.pkg[index].buf != NULL) {
        free(clientsSet.pkg[index].buf);
        clientsSet.pkg[index].buf=NULL;
    }
    int lost = (ALL_PACKS_NB - clientsSet.pkg[index].sentPackNb) * PORTION_SIZE;
    bytesLost += lost;
    if(clock_gettime(CLOCK_REALTIME, &TS) == -1) {
        printf("Blad pobierania czasu (TS)\n");
        exit(EXIT_FAILURE);
    }
    fprintf(stderr,"\nZamykam polaczenie do %s. Stracono %d bajtow\n", clientsSet.pkg[index].addr, lost);
    fprintf(stderr, "TS: %10jd.%03ld\n\n", TS.tv_sec, TS.tv_nsec/1000000);
}

int acceptConnectionWithNewClient(int socket_fd) {
    int new_sd = -1;
    int retv = TRUE;
    do {
        struct sockaddr_in peer;
        socklen_t addr_len = sizeof(peer);
        new_sd = accept(socket_fd, (struct sockaddr*)&peer, &addr_len);
        if (new_sd < 0) {
            if (errno != EWOULDBLOCK) {
                printf("Blad funkcji accept!\n");
                retv = FALSE;
            }
            break;
        }
        else {
            printf("SERWER: Nawiazano polaczenie z klientem o adresie %s:%d\n",
                   inet_ntoa(peer.sin_addr), ntohs(peer.sin_port));
            char adr[30];
            memset(adr, '\0', sizeof(adr));
            sprintf (adr, "%s:%d", inet_ntoa(peer.sin_addr), ntohs(peer.sin_port));
            initSocketConnection(nfds, new_sd , adr);
            nfds++;
        }
    } while (new_sd != -1);
    return retv;
}

void parentProcess(int* fd, int pace) {
    char block[BLOCK_SIZE];
    int ascii = 65;
    float secs = 640.0/(2662.0*pace);
    int intpart = (int)secs;
    struct timespec tim;
    tim.tv_sec = intpart;
    tim.tv_nsec = BILLION * (secs - intpart);
    while (1) {
        fillWithLetters(block, &ascii);
        if(nanosleep(&tim, NULL) < 0) {
            printf("Blad nanosleep!\n");
            exit(0);
        }
        write(fd[1], block, BLOCK_SIZE);
    }
}


int initTimer(int ind)
{
    struct itimerspec timeout;

    int tfd = timerfd_create(CLOCK_MONOTONIC,  0);
    if (tfd <= 0) {
        printf("Blad tworzenia timera!\n");
        exit(EXIT_FAILURE);
    }
    if(fcntl(tfd, F_SETFL, O_NONBLOCK)) {
        printf("Blad funkcji fcntl!\n");
        exit(EXIT_FAILURE);
    }
    timeout.it_value.tv_sec = 5;
    timeout.it_value.tv_nsec = 0;
    timeout.it_interval.tv_sec = 5;
    timeout.it_interval.tv_nsec = 0;
    if(timerfd_settime(tfd, 0, &timeout, NULL)) {
        printf("Blad timerfd_settime!\n");
        exit(EXIT_FAILURE);
    }
    clientsSet.fds[ind].fd = tfd;
    clientsSet.fds[ind].events = POLLIN;
    clientsSet.pkg[ind].sentPackNb = 0;
    clientsSet.pkg[ind].buf = NULL;
    strcpy(clientsSet.pkg[ind].addr, "");
    return tfd;
}

void printReport(int* pipe_fd) {
    fprintf(stderr, "\nMINELO 5 SEKUND!\n--------------------RAPORT--------------------\n");
    int bytesInPipe = 0;
    ioctl(pipe_fd[1], FIONREAD, &bytesInPipe);
    if(clock_gettime(CLOCK_REALTIME, &TS) == -1) {
        printf("Blad pobierania czasu (TS)\n");
        exit(EXIT_FAILURE);
    }
    int clientsNb=0;
    for(int i=2; i<nfds; i++) {
        if(clientsSet.fds[i].fd > 0)
            clientsNb++;
    }
    fprintf(stderr, "TS: %10jd.%03ld\n", TS.tv_sec, TS.tv_nsec/1000000);
    fprintf(stderr, "Liczba podlaczonych klientow: %d\n", clientsNb);
    fprintf(stderr,">  Zajetosc magazynu:      %d B (%.2f%%)\n", bytesInPipe, ((float)bytesInPipe/61440)*100);
    //jako pojemnosc magazynu przyjmuje 61440, bo praktycznie zajetosc nigdy nie osiagnie teoretycznych 65536 B

    fprintf(stderr, ">  Przeplyw materialu:     %d B\n", bytesTakenFromPipe-bytesSent);
    fprintf(stderr,"----------------------------------------------\n\n");
    bytesTakenFromPipe = 0;
    bytesSent = 0;
}
