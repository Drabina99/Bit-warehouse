#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <time.h>

#define MAX_HOST 30
#define PORTION_SIZE 1024
#define FULL_PORTION_SIZE 13312
#define STORAGE_BLOCK_SIZE 30720
#define BILLION 1000000000L
#define BILLION_FLOAT 1000000000.0

#define TRUE 1
#define FALSE 0

struct timespec ts = {.tv_sec = 0, .tv_nsec = 0};
struct timespec start = {.tv_sec = 0, .tv_nsec = 0};
struct timespec stop = {.tv_sec = 0, .tv_nsec = 0};
struct timespec connection = {.tv_sec = 0, .tv_nsec = 0};
struct timespec firstPortion = {.tv_sec = 0, .tv_nsec = 0};
struct timespec shut = {.tv_sec = 0, .tv_nsec = 0};
struct timespec TS = {.tv_sec = 0, .tv_nsec = 0};

typedef struct {
    char ip[16];
    int port;
    double connection_portion;
    double portion_shutdown;
} reportArgs;

int  getArgs(int arg_c, char** arg_v, int* cap, float* pace, float* deg); //funkcja pobierajaca argumenty pozycyjne
int  getHostnameAndPort(char* addr, char* hostname, int* port);
void printAddressAndPid(int status, void* args);
void createSocketAndConnect(char* hostname, int port, int* socket_fd, struct sockaddr_in* addr);
void printReportHeader();
void onExitReport(reportArgs* args, struct sockaddr_in* addr);
void mainLoop(int capacity, float pace, float deg, char* hostname, int port, reportArgs* args);
int  getPack(float deg, int content, int dl, int* firstPack);
int  getPortion(float pace, float deg, int bytes_recv, int content, int socket_fd, char* buf);

int main(int argc, char** argv) {
    int capacity;
    float pace, deg;
    if(getArgs(argc, argv, &capacity, &pace, &deg) == -1) {
        printf("Blad pobierania argumentow pozycyjnych!\n");
        exit(EXIT_FAILURE);
    }
    char hostname[MAX_HOST];
    int port;
    strcpy(hostname, argv[optind]);
    if(getHostnameAndPort(argv[optind], hostname, &port) == -1) {
        printf("Nieprawidlowy adres hosta!\n");
        exit(EXIT_FAILURE);
    }
    reportArgs* args = calloc(0, sizeof(args));
    mainLoop(capacity, pace, deg, hostname, port, args);
    free(args);
    return 0;
}

void mainLoop(int capacity, float pace, float deg, char* hostname, int port, reportArgs* args) {
    int bytes_recv = 0;
    int content = 0;
    while(1) {
        int socket_fd;
        struct sockaddr_in addr;
        createSocketAndConnect(hostname, port, &socket_fd, &addr);
        struct sockaddr_in client_addr;
        socklen_t client_addr_len = sizeof(addr);
        if(getsockname(socket_fd, (struct sockaddr*)&client_addr, &client_addr_len)) {
            printf("Blad client getsockname!\n");
            exit(EXIT_FAILURE);
        }
        printf("KLIENT: Moj adres: %s:%d\n", inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
        if(clock_gettime(CLOCK_MONOTONIC, &connection) == -1) {
            printf("Blad pobierania czasu (connection)\n");
            exit(EXIT_FAILURE);
        }
        printf("KLIENT: Nawiazano polaczenie z serwerem o adresie %s:%d\n", hostname, port);
        char buf[FULL_PORTION_SIZE+1];
        content = getPortion(pace, deg, bytes_recv, content, socket_fd, buf);
        if(shutdown(socket_fd, SHUT_RDWR)) {
            printf("Nie udalo sie zamknac polaczenia!\n");
            exit(EXIT_FAILURE);
        }
        if(clock_gettime(CLOCK_MONOTONIC, &shut) == -1) {
            printf("Blad pobierania czasu (shutdown)\n");
            exit(EXIT_FAILURE);
        }
        socklen_t addr_len = sizeof(addr);
        if(getsockname(socket_fd, (struct sockaddr*)&addr, &addr_len)) {
            printf("Blad getsockname!\n");
            exit(EXIT_FAILURE);
        }
        onExitReport(args, &addr);
        bytes_recv = 0;
        if(content + FULL_PORTION_SIZE > capacity) {
            if(clock_gettime(CLOCK_REALTIME, &TS) == -1) {
                printf("Blad pobierania czasu (TS)\n");
                exit(EXIT_FAILURE);
            }
            printReportHeader();
            break;
        }
    }
}

int getPortion(float pace, float deg, int bytes_recv, int content, int socket_fd, char* buf) {
    int dl;
    int firstPack = TRUE;
    while((dl = recv(socket_fd, buf, PORTION_SIZE , 0)) > -1) {
        buf[dl] = '\0';
        bytes_recv += strlen(buf);
        content = getPack(deg, content, dl, &firstPack);
        if(clock_gettime(CLOCK_REALTIME, &start) == -1) {
            printf("Blad pobierania czasu (start)\n");
            exit(EXIT_FAILURE);
        }
        double sleep_time = (dl/(4435*pace));
        ts.tv_sec = (int)sleep_time;
        ts.tv_nsec = BILLION*(sleep_time-ts.tv_sec);
        if(nanosleep(&ts, NULL) == -1) {
            printf("Nanosleep error!\n");
            exit(EXIT_FAILURE);
        }
        printf("KLIENT:  pobralem razem: %d\n\n", bytes_recv);
        if (bytes_recv >= FULL_PORTION_SIZE) {
            printf("KLIENT:  pobralem cala porcje!\n\n");
            break;
        }
    }
    return content;
}

int getPack(float deg, int content, int dl, int* firstPack) {
    if(!(*firstPack)) {
        if(clock_gettime(CLOCK_REALTIME, &stop) == -1) {
            printf("Blad pobierania czasu (stop)\n");
            exit(EXIT_FAILURE);
        }
        double time = (stop.tv_sec-start.tv_sec)+(stop.tv_nsec-start.tv_nsec)/BILLION_FLOAT;
        content -= 819*deg*time;
        if(content < 0)
            content = 0;
    }
    else {
        if(clock_gettime(CLOCK_MONOTONIC, &firstPortion) == -1) {
            printf("Blad pobierania czasu (firstPortion)\n");
            exit(EXIT_FAILURE);
        }
        *firstPack = FALSE;
    }
    content += dl;
    printf("KLIENT: zajetosc magazynu: %d\n", content);
    return content;
}

void onExitReport(reportArgs* args, struct sockaddr_in* addr) {
    args = malloc(sizeof(reportArgs));
    strcpy(args->ip, inet_ntoa((*addr).sin_addr));
    args->port = ntohs((*addr).sin_port);
    args->connection_portion = (firstPortion.tv_sec-connection.tv_sec)+(firstPortion.tv_nsec-connection.tv_nsec)/BILLION_FLOAT;
    args->portion_shutdown = (shut.tv_sec-firstPortion.tv_sec)+(shut.tv_nsec-firstPortion.tv_nsec)/BILLION_FLOAT;
    on_exit(printAddressAndPid, (void*)args);
}

void printReportHeader() {
    fprintf(stderr, "\n\n ---------- RAPORT ---------- \n\n");
    fprintf(stderr, "TS: %10jd.%03ld\n", TS.tv_sec, TS.tv_nsec/1000000);
    fprintf(stderr, "Dane dla kazdego bloku (od ostatniego do pierwszego):\n");
}

void createSocketAndConnect(char* hostname, int port, int* socket_fd, struct sockaddr_in* addr) {
    (*socket_fd) = socket(AF_INET, SOCK_STREAM, 0);
    if((*socket_fd) == -1) {
        printf("Blad tworzenia gniazda! (producent)\n");
        exit(EXIT_FAILURE);
    }
    (*addr).sin_family = AF_INET;
    (*addr).sin_port = htons(port);
    if(!strcmp(hostname, "localhost"))
        strcpy(hostname, "127.0.0.1");
    if(!inet_aton(hostname, &(*addr).sin_addr)) {
        printf("Podano nieprawidlowy adres hosta!\n");
        exit(EXIT_FAILURE);
    }
    int it;
    for(it = 0; it < 10; it++) {
        if(connect((*socket_fd), (struct sockaddr*) addr, sizeof((*addr))) != -1)
            break;
    }
    if(it == 10) {
        printf("Polaczenie nie zostalo zaakceptowane!\n");
        exit(EXIT_FAILURE);
    }
}

int getArgs(int arg_c, char** arg_v, int* cap, float* pace, float* deg) {
    char* ptr;
    int opt;
    while((opt = getopt(arg_c, arg_v, "c:p:d:")) != -1) {
        switch(opt) {
            case 'c':
                *cap = strtol(optarg, &ptr, 10);
                if(*ptr != '\0')
                    return -1;
                *cap *= STORAGE_BLOCK_SIZE;
                break;
            case 'p':
                *pace = strtof(optarg, &ptr);
                if(*ptr != '\0')
                    return -1;
                break;
            case 'd':
                *deg = strtof(optarg, &ptr);
                if(*ptr != '\0')
                    return -1;
                break;
            case '?':
                printf("Nieznana opcja %c\n", opt);
                return -1;
        }
    }
    return 0;
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

void printAddressAndPid(int status, void* args) {
    reportArgs argsStruct = *(reportArgs*)args;
    fprintf(stderr, "\t---\n\tPID: %d\n\tAdres: %s:%d\n\tOpoznienia:\n\t\t> polaczenie - 1. porcja: %lf\n\t\t> 1. porcja - zamkniecie polaczenia: %lf\n",
            getpid(), argsStruct.ip, argsStruct.port, argsStruct.connection_portion, argsStruct.portion_shutdown);

    exit(status);
}
